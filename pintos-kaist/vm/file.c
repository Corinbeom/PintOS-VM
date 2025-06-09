#include "vm/vm.h"
#include "threads/vaddr.h"
#include "threads/mmu.h"
#include "userprog/process.h"
#include "userprog/syscall.h"

static bool file_backed_swap_in(struct page *page, void *kva);
static bool file_backed_swap_out(struct page *page);
static void file_backed_destroy(struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations file_ops = {
	.swap_in = file_backed_swap_in,
	.swap_out = file_backed_swap_out,
	.destroy = file_backed_destroy,
	.type = VM_FILE,
};

/* 파일 기반 페이지 시스템 초기화 (현재는 구현 없음) */
void vm_file_init(void)
{
}

/* 파일 기반 페이지(file-backed page)를 초기화하는 함수
 *
 * [역할]
 * - UNINIT → FILE 타입으로 전환될 때 호출됨
 * - aux 구조체에 담긴 파일 정보(file, ofs, read_bytes 등)를 page->file에 복사
 * - 페이지의 pml4, operations 설정 등 기본 구성 수행
 *
 * @param page 초기화할 대상 페이지 (uninit 상태에서 호출됨)
 * @param type VM_FILE 타입 (명시적 전달되지만 사용되지 않음)
 * @param kva 아직 연결된 물리 주소이지만 여기선 사용 안함
 * @return true 항상 성공
 */
bool file_backed_initializer(struct page *page, enum vm_type type, void *kva)
{
	// 이 페이지는 이제 file-backed 페이지가 되었음을 등록
	page->operations = &file_ops;

	// lazy_load_segment 등에서 넘겨준 파일 관련 정보(aux) 해석
	struct file_load *aux = page->uninit.aux;

	// file_page 구조체에 파일 관련 메타데이터 설정
	struct file_page *file_page = &page->file;
	file_page->file = aux->file;               // 매핑된 파일
	file_page->ofs = aux->ofs;                 // 읽기 시작할 오프셋
	file_page->read_bytes = aux->read_bytes;   // 실제 읽을 바이트 수
	file_page->zero_bytes = aux->zero_bytes;   // 나머지 0으로 채울 바이트 수
	file_page->file_length = aux->file_length; // 전체 매핑 길이

	// 페이지의 pml4 등록 (현재 스레드와 연결됨)
	page->pml4 = thread_current()->pml4;

	// 프레임의 page_list에 연결 → 해당 프레임이 여러 페이지와 연결될 수 있음 (mmap 공유)
	list_push_back(&page->frame->page_list, &page->out_elem);

	return true;
}

/* 파일 기반 페이지(file-backed page)를 swap-in 하는 함수
 *
 * [역할]
 * - 해당 페이지가 물리 메모리에서 제거되어 swap-out된 이후,
 *   다시 파일에서 내용을 읽어와 복구
 * - file-backed 페이지들은 여러 VA에서 mmap으로 공유될 수 있으므로
 *   연결된 다른 페이지들도 함께 프레임에 매핑 복원
 *
 * @param page 복원할 대상 페이지
 * @param kva  페이지가 매핑될 커널 주소 (frame->kva와 동일)
 * @return     true always (읽기 실패 등은 별도로 처리하지 않음)
 */
static bool file_backed_swap_in(struct page *page, void *kva)
{
	struct file_page *file_page = &page->file;
	struct list *file_list = file_page->file_list; // 이 프레임을 공유했던 페이지 목록
	struct frame *frame = page->frame;

	// 파일 시스템 보호를 위해 락 획득
	bool is_lock_held = lock_held_by_current_thread(&filesys_lock);
	if (!is_lock_held)
		lock_acquire(&filesys_lock);

	// 실제 파일로부터 데이터를 읽어와 frame에 로드
	file_read_at(file_page->file,
	             page->frame->kva,
	             file_page->read_bytes,
	             file_page->ofs);

	if (!is_lock_held)
		lock_release(&filesys_lock);

	// 공유된 다른 가상 페이지들도 이 프레임에 다시 연결
	while (!list_empty(file_list))
	{
		struct page *in_page = list_entry(list_pop_front(file_list), struct page, out_elem);

		// 다시 frame에 연결 (page_list에 복원)
		list_push_back(&frame->page_list, &in_page->out_elem);

		// 해당 페이지를 MMU에 다시 매핑 (VA → frame->kva)
		pml4_set_page(in_page->pml4, in_page->va, frame->kva, in_page->writable);
	}

	// file_list는 재사용하지 않으므로 해제
	free(file_list);
	return true;
}

/* 파일 기반 페이지(file-backed page)를 swap-out 하는 함수
 *
 * [역할]
 * - 프레임에 존재하던 file-backed 페이지들을 모두 제거
 * - dirty 상태인 경우: 파일에 변경 내용 write-back
 * - 이후 프레임과 페이지 간 연결 해제
 * - 해당 프레임과 연결된 모든 페이지는 file_list에 저장됨 (swap-in용)
 *
 * @param page 현재 swap-out을 유도한 대표 페이지
 * @return true 항상 true 반환
 */
static bool file_backed_swap_out(struct page *page)
{
	struct file_page *file_page = &page->file;

	// 1. 해당 프레임을 공유하는 모든 페이지를 모아둘 리스트 생성
	struct list *file_list = malloc(sizeof(struct list));
	list_init(file_list);
	file_page->file_list = file_list;

	struct frame *frame = page->frame;

	// 2. 파일 시스템 동기화를 위한 락 획득
	bool is_lock_held = lock_held_by_current_thread(&filesys_lock);
	if (!is_lock_held)
		lock_acquire(&filesys_lock);

	// 3. 해당 프레임을 공유 중인 모든 페이지 처리
	while (!list_empty(&frame->page_list))
	{
		struct page *out_page = list_entry(list_pop_front(&frame->page_list), struct page, out_elem);

		// dirty 페이지인 경우만 파일에 write-back 수행
		if (pml4_is_dirty(out_page->pml4, out_page->va))
		{
			file_write_at(out_page->file.file,
			              out_page->frame->kva,
			              out_page->file.read_bytes,
			              out_page->file.ofs);
		}

		// 공유 리스트(file_list)에 추가 (swap-in 시 복원용)
		list_push_back(file_list, &out_page->out_elem);

		// 해당 가상 주소와 물리 주소의 매핑 해제
		pml4_clear_page(out_page->pml4, out_page->va);
	}

	// 4. 락 반환
	if (!is_lock_held)
		lock_release(&filesys_lock);

	return true;
}

/* 파일 기반 페이지(file-backed page) 제거 함수
 *
 * [역할]
 * - 해당 페이지가 dirty한 경우 파일에 write-back
 * - 파일 close
 * - 프레임의 참조 수 감소
 * - 필요한 경우 MMU 매핑 해제
 *
 * 이 함수는 vm_dealloc_page() → destroy() → file_backed_destroy() 순으로 호출된다.
 */
static void file_backed_destroy(struct page *page)
{
	struct file_page *file_page = &page->file;

	// 1. 프레임 참조 카운트 감소 (공유된 프레임이므로 한 페이지 제거 시마다 감소)
	page->frame->cnt_page -= 1;

	// 2. 파일 동기화를 위한 락 획득 (중첩 락 방지)
	bool is_lock_held = lock_held_by_current_thread(&filesys_lock);
	if (!is_lock_held)
		lock_acquire(&filesys_lock);

	// 3. 페이지가 dirty 상태라면 파일에 write-back
	if (pml4_is_dirty(thread_current()->pml4, page->va))
	{
		file_write_at(page->file.file,
		              page->frame->kva,
		              page->file.read_bytes,
		              page->file.ofs);
	}

	// 4. 해당 페이지가 참조하던 파일 닫기 (ref count 감소)
	file_close(page->file.file);

	// 5. 락 해제
	if (!is_lock_held)
		lock_release(&filesys_lock);

	// 6. 아직 다른 페이지들이 같은 프레임을 공유 중이면 현재 페이지만 매핑 해제
	if (page->frame->cnt_page > 0)
		pml4_clear_page(thread_current()->pml4, page->va);
}


/* lazy loading을 위한 페이지 초기화 함수
 *
 * [역할]
 * - 초기화 시 등록된 파일 정보를 이용하여
 *   파일로부터 데이터를 읽고, 나머지 공간은 0으로 채운다.
 * - 페이지 폴트가 발생했을 때 최초로 호출되며,
 *   해당 페이지의 프레임에 실제 내용을 로딩한다.
 */
static bool lazy_load(struct page *page, void *aux_)
{
	// 1. 인자로 전달된 보조 정보 구조체 파싱
	struct file_load *aux = (struct file_load *)aux_;
	struct file *file = aux->file;
	off_t ofs = aux->ofs;					// 파일에서 읽기 시작할 오프셋
	uint32_t read_bytes = aux->read_bytes; // 읽어야 할 바이트 수
	uint32_t zero_bytes = aux->zero_bytes; // 남은 공간을 0으로 채울 바이트 수

	free(aux); // aux는 더 이상 필요 없으므로 해제

	// 2. 파일 시스템 락 획득 (다중 스레드 환경에서의 race condition 방지)
	bool is_lock_held = lock_held_by_current_thread(&filesys_lock);
	if (!is_lock_held)
		lock_acquire(&filesys_lock);

	// 3. 파일 위치를 오프셋으로 이동
	file_seek(file, ofs);

	// 4. 파일에서 실제 내용 읽기
	read_bytes = file_read(file, page->frame->kva, read_bytes);

	// 5. 락 해제
	if (!is_lock_held)
		lock_release(&filesys_lock);

	// 6. 남은 영역을 0으로 초기화 (zero-fill)
	memset(page->frame->kva + read_bytes, 0, zero_bytes);

	return true;
}


/* mmap() 시스템 콜 구현: 파일을 메모리에 매핑하고 lazy load 등록
 *
 * [역할]
 * - 파일의 offset부터 length만큼의 데이터를 주소 addr부터 가상 메모리에 매핑
 * - 실제 데이터는 lazy_load() 함수로 지연 로딩됨 (페이지 폴트 발생 시 load)
 * - 페이지 단위로 매핑하고 이미 존재하는 주소일 경우 실패
 *
 * @param addr      : 매핑할 시작 가상 주소 (유저 스택 영역과 겹치면 안 됨)
 * @param length    : 매핑할 총 크기 (바이트 단위)
 * @param writable  : 해당 매핑된 메모리 영역이 쓰기 가능한지 여부
 * @param file      : 매핑할 파일의 포인터
 * @param offset    : 파일 내에서 시작할 위치
 * @return 성공 시 addr, 실패 시 NULL
 */
void *do_mmap(void *addr, size_t length, int writable,
              struct file *file, off_t offset)
{
	// 매핑할 총 페이지 수 계산 (page-aligned가 아니면 올림 처리)
	int cnt_page = length % PGSIZE ? length / PGSIZE + 1 : length / PGSIZE;
	size_t length_ = length;

	// 파일의 길이보다 offset이 크면 유효하지 않음
	if (file_length(file) < offset)
		return NULL;

	// (1) 매핑하려는 주소 공간에 이미 페이지가 존재하면 실패
	for (int i = 0; i < cnt_page; i++) {
		if (spt_find_page(&thread_current()->spt, addr + i * PGSIZE) != NULL)
			return NULL;
	}

	// (2) 파일 시스템 락을 필요 시 획득 (동기화)
	bool is_lock_held = lock_held_by_current_thread(&filesys_lock);
	if (!is_lock_held)
		lock_acquire(&filesys_lock);

	// (3) 각 페이지에 대해 lazy loading 방식으로 매핑 등록
	for (int i = 0; i < cnt_page; i++) {
		// 파일을 reopen해서 각 페이지마다 독립적인 참조 확보
		struct file *file_ = file_reopen(file);

		// 파일 로딩 정보 전달용 구조체 생성
		struct file_load *aux = malloc(sizeof(struct file_load));
		aux->file_length = length; // 전체 길이 (munmap 대비)
		aux->file = file_;
		aux->ofs = offset + i * PGSIZE; // 이 페이지가 읽을 파일 offset

		// 이번 페이지에서 읽어야 할 바이트 계산
		if (length_ >= PGSIZE) {
			aux->read_bytes = PGSIZE;
			length_ -= PGSIZE;
		} else {
			aux->read_bytes = length_;
		}

		// 나머지는 zero-fill 영역
		aux->zero_bytes = PGSIZE - aux->read_bytes;

		// lazy loader를 지정하여 VM_FILE 타입으로 페이지 할당
		vm_alloc_page_with_initializer(VM_FILE, addr + i * PGSIZE, writable, lazy_load, aux);
	}

	// (4) 락 해제
	if (!is_lock_held)
		lock_release(&filesys_lock);

	return addr;
}

/* munmap() 시스템 콜 구현: 주어진 addr부터 시작하는 매핑 해제
 *
 * [역할]
 * - addr에서 시작된 파일 매핑을 해제 (mmap으로 등록된 영역)
 * - dirty 상태인 경우 파일에 write-back 수행
 * - 프레임의 참조 수 감소 및 페이지 테이블에서 제거
 *
 * @param addr : 매핑 해제할 시작 주소 (반드시 페이지 정렬된 주소)
 */
void do_munmap(void *addr)
{
	size_t length;
	int cnt_page;

	// (1) addr는 반드시 페이지 기준이어야 함 (0x...000 형태)
	if (pg_ofs(addr) != 0)
		return;

	// (2) 해당 주소의 페이지를 보조 페이지 테이블에서 찾음
	struct page *page = spt_find_page(&thread_current()->spt, addr);
	if (page == NULL)
		return;

	// (3) mmap된 총 길이와 해제할 페이지 수 계산
	length = page->file.file_length;
	cnt_page = length % PGSIZE ? length / PGSIZE + 1 : length / PGSIZE;

	// (4) 파일 시스템 락을 필요 시 획득
	bool is_lock_held = lock_held_by_current_thread(&filesys_lock);
	if (!is_lock_held)
		lock_acquire(&filesys_lock);

	// (5) 페이지 하나씩 순회하며 해제
	for (int i = 0; i < cnt_page; i++)
	{
		// 현재 해제 대상 페이지
		page = spt_find_page(&thread_current()->spt, addr + i * PGSIZE);

		// dirty이면 현재 메모리 내용을 파일에 기록
		if (pml4_is_dirty(thread_current()->pml4, page->va))
		{
			file_write_at(page->file.file, page->va, page->file.read_bytes, page->file.ofs);
		}

		// 프레임 참조 수 감소
		page->frame->cnt_page -= 1;

		// 파일 핸들 닫기
		file_close(page->file.file);

		// 보조 페이지 테이블에서 제거
		hash_delete(&thread_current()->spt.spt_hash, &page->page_elem);

		// 페이지 테이블에서도 해당 매핑 제거
		pml4_clear_page(thread_current()->pml4, page->va);
	}

	// (6) 락을 획득했으면 해제
	if (!is_lock_held)
		lock_release(&filesys_lock);
}

