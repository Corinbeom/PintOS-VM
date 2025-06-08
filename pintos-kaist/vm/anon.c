/* anon.c: Implementation of page for non-disk image (a.k.a. anonymous page). */

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

/* 익명 페이지 초기화 (스왑 디스크 및 슬롯 구성) */
void vm_anon_init(void)
{
	swap_disk = disk_get(1, 1); // 디스크 1:1 (SATA Controller 1의 디스크 1) 사용
	list_init(&swap_slot_list);
	lock_init(&swap_lock);

	// 디스크 전체 공간을 SLOT_SIZE 단위로 나누어 swap_slot 리스트 구성
	for (int i = 0; i < disk_size(swap_disk); i += SLOT_SIZE)
	{
		struct swap_slot *slot = malloc(sizeof(struct swap_slot));
		list_init(&slot->page_list);
		slot->start_sector = i;
		list_push_back(&swap_slot_list, &slot->slot_elem);
	}

	// 모든 sector를 0으로 초기화
	memset(zero_set, 0, PGSIZE);
}

/* 익명 페이지용 초기화자
 * - anonymous 페이지 초기화 및 page handler 등록 */
bool anon_initializer(struct page *page, enum vm_type type, void *kva)
{
	page->operations = &anon_ops;
	struct anon_page *anon_page = &page->anon;
	page->pml4 = thread_current()->pml4;
	anon_page->slot = NULL; // 아직 스왑 슬롯 없음
	list_push_back(&page->frame->page_list, &page->out_elem); // 프레임의 페이지 목록에 등록
	return true;
}

/* swap_disk로부터 데이터를 읽어와 메모리로 복구 (swap-in)
 * - 하나의 프레임을 공유했던 모든 페이지에 대해 매핑 복구 */
static bool anon_swap_in(struct page *page, void *kva)
{
	struct anon_page *anon_page = &page->anon;
	struct list *page_list = &anon_page->slot->page_list;
	struct swap_slot *slot = anon_page->slot;
	int read = 0;

	while (!list_empty(page_list))
	{
		struct page *in_page = list_entry(list_pop_front(page_list), struct page, out_elem);
		pml4_set_page(in_page->pml4, in_page->va, page->frame->kva, in_page->writable);

		if (read++ == 0)
		{
			// 실제 데이터 복구는 단 한 번 수행 (공유된 첫 페이지 기준)
			for (int i = 0; i < SLOT_SIZE; i++)
			{
				disk_read(swap_disk, slot->start_sector + i, in_page->va + DISK_SECTOR_SIZE * i);
				disk_write(swap_disk, slot->start_sector + i, zero_set + DISK_SECTOR_SIZE * i);
			}
		}
		in_page->frame = page->frame;
		page->frame->cnt_page += 1;
		list_push_back(&page->frame->page_list, &in_page->out_elem);
	}

	// 사용한 슬롯은 다시 swap_slot_list로 반환
	list_push_back(&swap_slot_list, &slot->slot_elem);
	return true;
}

/* 메모리 내용을 swap_disk로 저장하고 페이지 매핑 제거 (swap-out) */
static bool anon_swap_out(struct page *page)
{
	struct anon_page *anon_page = &page->anon;
	// 빈 swap 슬롯 할당
	anon_page->slot = list_entry(list_pop_front(&swap_slot_list), struct swap_slot, slot_elem);
	struct frame *frame = page->frame;
	int dirty = 0;

	// 해당 프레임을 공유하는 모든 페이지를 슬롯에 기록
	while (!list_empty(&frame->page_list))
	{
		struct page *out_page = list_entry(list_pop_front(&frame->page_list), struct page, out_elem);
		frame->cnt_page -= 1;
		list_push_back(&anon_page->slot->page_list, &out_page->out_elem);
		out_page->anon.slot = anon_page->slot;

		for (int i = 0; i < SLOT_SIZE; i++)
		{
			disk_write(swap_disk, out_page->anon.slot->start_sector + i, out_page->va + DISK_SECTOR_SIZE * i);
		}
		pml4_clear_page(out_page->pml4, out_page->va); // 페이지 매핑 제거
	}
	return true;
}

/* 익명 페이지 제거: 프레임 참조 수 감소만 수행 */
static void anon_destroy(struct page *page)
{
	struct anon_page *anon_page = &page->anon;
	page->frame->cnt_page -= 1;
}
