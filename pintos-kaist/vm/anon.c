#include "vm/vm.h"
#include "threads/vaddr.h"
#include "devices/disk.h"

/* DO NOT MODIFY BELOW LINE */
static struct disk *swap_disk;                  // 스왑 디스크 핸들
static struct list swap_slot_list;              // 빈 스왑 슬롯 리스트
static struct lock swap_lock;                   // 스왑 슬롯 접근 보호 락
static char *zero_set[PGSIZE];                  // 디스크 클리어용 zero 패턴

static bool anon_swap_in(struct page *page, void *kva);
static bool anon_swap_out(struct page *page);
static void anon_destroy(struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations anon_ops = {
	.swap_in = anon_swap_in,
	.swap_out = anon_swap_out,
	.destroy = anon_destroy,
	.type = VM_ANON,
};

/* 익명 페이지용 초기화 함수
 *
 * [역할]
 * - Swap 영역으로 사용할 디스크를 지정하고,
 *   그 디스크를 SLOT_SIZE 단위로 나누어 swap slot 리스트를 구성
 * - 각 슬롯은 나중에 anonymous 페이지를 swap-out할 때 사용됨
 */
void vm_anon_init(void)
{
	// (1) 스왑 디스크 설정 (디스크 컨트롤러 1번, 디스크 1번)
	swap_disk = disk_get(1, 1);  // pintos에서는 하드코딩된 디스크 번호 사용

	// (2) swap slot 리스트 및 락 초기화
	list_init(&swap_slot_list); // 사용 가능한 슬롯을 리스트 형태로 관리
	lock_init(&swap_lock);      // swap-in/out 중 동기화 필요

	// (3) 디스크 전체를 SLOT_SIZE(=PGSIZE / DISK_SECTOR_SIZE) 단위로 분할
	for (int i = 0; i < disk_size(swap_disk); i += SLOT_SIZE)
	{
		// 새 swap_slot 구조체 할당 및 초기화
		struct swap_slot *slot = malloc(sizeof(struct swap_slot));
		list_init(&slot->page_list);     // 해당 슬롯에 연결된 페이지들 목록
		slot->start_sector = i;          // 이 슬롯의 시작 섹터 번호

		// 전역 swap_slot_list에 추가
		list_push_back(&swap_slot_list, &slot->slot_elem);
	}

	// (4) 모든 sector를 0으로 초기화할 수 있는 zero buffer 준비
	memset(zero_set, 0, PGSIZE); // disk_write 시 zero-fill에 사용
}


/* 익명 페이지 초기화 함수
 *
 * [역할]
 * - 페이지에 익명 페이지용 핸들러 등록 (anon_ops)
 * - 익명 페이지 메타데이터 설정 (swap slot 비워둠)
 * - 해당 페이지를 현재 스레드의 페이지 테이블에 등록
 * - 프레임의 공유 페이지 리스트에 이 페이지를 추가 (참조 수 증가 목적)
 */
bool anon_initializer(struct page *page, enum vm_type type, void *kva)
{
	// (1) 익명 페이지는 anon_ops에 의해 관리됨 (swap_in, swap_out, destroy 함수 등록)
	page->operations = &anon_ops;

	// (2) 익명 페이지 내부 구조체 가져옴
	struct anon_page *anon_page = &page->anon;

	// (3) 페이지의 pml4 설정 (현재 스레드 기준)
	page->pml4 = thread_current()->pml4;

	// (4) 아직 swap-out 되지 않았으므로 slot은 NULL로 초기화
	anon_page->slot = NULL;

	// (5) 프레임의 공유 페이지 리스트에 등록 (나중에 frame->page_list 순회 시 사용됨)
	list_push_back(&page->frame->page_list, &page->out_elem);

	return true;
}


/* 스왑 디스크에서 데이터를 읽어와 메모리로 복구 (anonymous 페이지 swap-in)
 *
 * [역할]
 * - 스왑 슬롯에 저장된 데이터를 하나의 프레임으로 복구
 * - 이 프레임을 공유하던 모든 페이지에 대해 pml4 매핑 복원
 * - 스왑 슬롯은 복구 이후 재사용 가능하도록 반환
 *
 * @param page : 복구 대상 페이지 (공유 프레임 기준)
 * @param kva  : 매핑할 커널 가상 주소 (unused, frame->kva 사용)
 * @return true if success
 */
static bool anon_swap_in(struct page *page, void *kva)
{
	struct anon_page *anon_page = &page->anon;
	struct swap_slot *slot = anon_page->slot;
	struct list *page_list = &slot->page_list;
	int read = 0;

	// (1) swap-out 당시 이 프레임을 공유하던 모든 페이지에 대해 복구 수행
	while (!list_empty(page_list))
	{
		// (2) 해당 페이지를 꺼내 pml4에 다시 매핑
		struct page *in_page = list_entry(list_pop_front(page_list), struct page, out_elem);
		pml4_set_page(in_page->pml4, in_page->va, page->frame->kva, in_page->writable);

		// (3) 디스크에서 실제 데이터 복구는 단 한 번만 수행 (첫 번째 페이지 기준)
		if (read++ == 0)
		{
			for (int i = 0; i < SLOT_SIZE; i++)
			{
				// 디스크로부터 sector 단위로 읽어서 프레임에 로드
				disk_read(swap_disk, slot->start_sector + i, in_page->va + DISK_SECTOR_SIZE * i);

				// 해당 sector는 zero_set으로 덮어서 "지운다" (중복 쓰기 방지)
				disk_write(swap_disk, slot->start_sector + i, zero_set + DISK_SECTOR_SIZE * i);
			}
		}

		// (4) 복구된 페이지를 다시 frame과 연결
		in_page->frame = page->frame;
		page->frame->cnt_page += 1;
		list_push_back(&page->frame->page_list, &in_page->out_elem);
	}

	// (5) 사용한 swap 슬롯을 다시 swap_slot_list에 반환 (재사용 가능)
	list_push_back(&swap_slot_list, &slot->slot_elem);
	return true;
}


/* 익명 페이지를 스왑 아웃하는 함수
 *
 * [역할]
 * - 현재 프레임을 공유 중인 모든 페이지를 스왑 디스크에 저장
 * - 각 페이지에 대한 매핑(pml4)을 제거하여 메모리에서 제거
 * - 저장된 페이지 정보를 해당 스왑 슬롯에 기록
 *
 * @param page : 스왑 아웃 대상 페이지 (공유 프레임 중 하나)
 * @return true if success
 */
static bool anon_swap_out(struct page *page)
{
	struct anon_page *anon_page = &page->anon;
	struct frame *frame = page->frame;

	// (1) 비어 있는 swap 슬롯을 하나 꺼내 현재 페이지에 할당
	anon_page->slot = list_entry(list_pop_front(&swap_slot_list), struct swap_slot, slot_elem);

	// (2) 해당 프레임에 연결된 모든 페이지를 순회하며 swap-out
	while (!list_empty(&frame->page_list))
	{
		struct page *out_page = list_entry(list_pop_front(&frame->page_list), struct page, out_elem);

		// (3) 프레임 참조 수 감소
		frame->cnt_page -= 1;

		// (4) 스왑 슬롯에 해당 페이지 정보 저장
		list_push_back(&anon_page->slot->page_list, &out_page->out_elem);
		out_page->anon.slot = anon_page->slot;

		// (5) 페이지 내용을 스왑 디스크에 sector 단위로 저장
		for (int i = 0; i < SLOT_SIZE; i++)
		{
			disk_write(swap_disk,
					   out_page->anon.slot->start_sector + i,
					   out_page->va + DISK_SECTOR_SIZE * i);
		}

		// (6) 현재 프로세스의 pml4에서 이 페이지에 대한 매핑 제거
		pml4_clear_page(out_page->pml4, out_page->va);
	}
	return true;
}

/* 익명 페이지 제거 (anonymous page destroy)
 *
 * [역할]
 * - 현재 페이지를 참조 중인 프레임의 참조 수(cnt_page)를 1 감소
 * - 스왑 슬롯 반환이나 메모리 해제는 수행하지 않음
 *
 * @param page : 제거할 익명 페이지
 */
static void anon_destroy(struct page *page)
{
	struct anon_page *anon_page = &page->anon;

	// (1) 이 페이지가 연결된 프레임 참조 수 감소
	//     실제 frame->kva는 다른 페이지가 여전히 참조 중일 수 있으므로 바로 해제하지 않음
	page->frame->cnt_page -= 1;
}
