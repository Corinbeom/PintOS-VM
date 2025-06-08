/* vm.c: Generic interface for virtual memory objects. */

#include "threads/vaddr.h"
#include "threads/malloc.h"
#include "threads/mmu.h"
#include "userprog/syscall.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include "vm/file.h"

/* 가상 메모리 서브시스템 초기화 함수
 * - 익명 페이지, 파일 기반 페이지 등 각 서브시스템을 초기화
 * - frame_table 리스트도 초기화함 */
void vm_init(void)
{
	vm_anon_init();              // 익명 페이지용 초기화 함수 호출
	vm_file_init();              // 파일 기반 페이지용 초기화 함수 호출
#ifdef EFILESYS
	pagecache_init();            // Project 4에서 사용하는 페이지 캐시 초기화
#endif
	register_inspect_intr();    // 디버깅용 인터럽트 등록
	list_init(&frame_table);    // 프레임 테이블 리스트 초기화
}

/* 주어진 페이지가 어떤 타입인지 반환 (UNINIT인 경우 내부 타입까지 반환)
 * - 페이지의 실제 타입 정보를 알아내기 위한 함수 */
enum vm_type page_get_type(struct page *page)
{
	int ty = VM_TYPE(page->operations->type);
	switch (ty)
	{
	case VM_UNINIT:
		return VM_TYPE(page->uninit.type); // 초기화 안된 페이지는 내부 타입 반환
	default:
		return ty;
	}
}

/* 페이지를 초기화자와 함께 생성
 * - 유저 주소 upage에 대한 가상 페이지를 생성하고 SPT에 등록
 * - 타입에 맞는 initializer 함수와 보조 데이터(aux)를 사용 */
bool vm_alloc_page_with_initializer(enum vm_type type, void *upage, bool writable,
                                    vm_initializer *init, void *aux)
{
	ASSERT(VM_TYPE(type) != VM_UNINIT); // UNINIT는 직접 호출 금지
	struct supplemental_page_table *spt = &thread_current()->spt;
	upage = pg_round_down(upage); // 페이지 정렬

	bool (*page_initializer)(struct page *, enum vm_type, void *kva);
	switch (VM_TYPE(type))
	{
	case VM_ANON:
		page_initializer = anon_initializer;
		break;
	case VM_FILE:
		page_initializer = file_backed_initializer;
		break;
	default:
		return false; // 지원되지 않는 타입
	}

	// 중복 페이지 존재 여부 확인
	if (spt_find_page(spt, upage) == NULL)
	{
		struct page *newpage = calloc(1, sizeof(struct page)); // 페이지 구조체 동적 할당
		uninit_new(newpage, upage, init, type, aux, page_initializer); // UNINIT 상태로 생성
		newpage->writable = writable;
		spt_insert_page(spt, newpage); // 보조 페이지 테이블에 등록
		return true;
	}
	return false; // 이미 존재함
}

/* 주어진 가상 주소 va에 해당하는 페이지를 SPT에서 검색
 * - 해시 기반 탐색 */
struct page *spt_find_page(struct supplemental_page_table *spt UNUSED, void *va UNUSED)
{
	struct page tmp;
	tmp.va = va; // 임시 구조체로 비교용 키 구성
	struct hash_elem *h = hash_find(&spt->spt_hash, &tmp.page_elem);
	if (h == NULL)
		return NULL; // 찾지 못함
	return hash_entry(h, struct page, page_elem); // 실제 페이지 반환
}

/* 보조 페이지 테이블(SPT)에 새로운 페이지를 삽입 */
bool spt_insert_page(struct supplemental_page_table *spt UNUSED, struct page *page UNUSED)
{
	return (hash_insert(&spt->spt_hash, &page->page_elem) == NULL); // 중복 없는 경우 true
}

/* 주어진 페이지를 해제 (SPT에서 제거하고 free 처리) */
void spt_remove_page(struct supplemental_page_table *spt, struct page *page)
{
	vm_dealloc_page(page); // destroy() 호출 후 메모리 해제
}

/* 페이지 교체 알고리즘: victim frame 선택
 * - Clock 방식 (accessed 비트 확인 및 unset 반복) */
static struct frame *vm_get_victim(void)
{
	for (struct list_elem *e = list_begin(&frame_table); e != list_end(&frame_table); e = list_next(e))
	{
		struct frame *frame = list_entry(e, struct frame, frame_elem);
		if (!pml4_is_accessed(frame->page->pml4, frame->page->va)) // accessed == false
			return frame;
		else
			pml4_set_accessed(frame->page->pml4, frame->page->va, 0); // accessed reset
	}
	// 모든 페이지가 accessed된 경우 맨 앞 반환
	return list_entry(list_front(&frame_table), struct frame, frame_elem);
}

/* 교체할 frame을 선택하고 swap-out까지 수행 (swap_out은 TODO) */
static struct frame *vm_evict_frame(void)
{
	struct frame *victim = vm_get_victim();
	// TODO: swap_out(victim->page)
	return victim;
}

/* 새로운 프레임을 확보함. 메모리가 부족할 경우 프레임 교체 발생 */
static struct frame *vm_get_frame(void)
{
	struct frame *frame = calloc(1, sizeof(struct frame));
	void *upage = palloc_get_page(PAL_USER | PAL_ZERO); // 유저 페이지 확보
	if (upage == NULL)
	{
		// 프레임 할당 실패 시 victim 선정 및 swap-out
		free(frame);
		frame = vm_evict_frame();
		swap_out(frame->page);
		list_remove(&frame->frame_elem);
		list_push_back(&frame_table, &frame->frame_elem);
		frame->page = NULL;
	}
	else
	{
		frame->kva = upage;
		frame->page = NULL;
		list_push_back(&frame_table, &frame->frame_elem);
		list_init(&frame->page_list);
		frame->cnt_page = 1;
	}
	return frame;
}

/* 유저 스택 확장용 함수
 * - 접근한 주소를 기준으로 anonymous 페이지 할당 및 claim */
static void vm_stack_growth(void *addr UNUSED)
{
	vm_alloc_page_with_initializer(VM_ANON, pg_round_down(addr), 1, NULL, NULL);
	vm_claim_page(pg_round_down(addr));
}

/* 페이지 폴트 핸들러
 * - not_present fault: stack 확장 또는 lazy loading 처리
 * - protection fault: 종료 */
bool vm_try_handle_fault(struct intr_frame *f, void *addr, bool user, bool write, bool not_present)
{
	struct supplemental_page_table *spt = &thread_current()->spt;
	struct page *page = NULL;
	uint64_t user_rsp = user ? f->rsp : thread_current()->user_rsp;

	if (not_present)
	{
		// 스택 확장 조건
		if (user_rsp - 8 == addr || (USER_STACK - (1 << 20) <= user_rsp && user_rsp < addr && addr < USER_STACK))
		{
			vm_stack_growth(addr);
			return true;
		}
		page = spt_find_page(spt, pg_round_down(addr));
		if (page == NULL || (write && !page->writable))
			exit(-1); // 존재하지 않거나 쓰기 불가
	}
	else if (write)
		exit(-1); // protection fault

	return vm_do_claim_page(page); // lazy loading 수행
}

/* 주어진 page에 대해 물리 프레임을 확보하고 매핑을 구성 */
static bool vm_do_claim_page(struct page *page)
{
	struct frame *frame = vm_get_frame();
	frame->page = page;
	page->frame = frame;

	switch (VM_TYPE(page->operations->type))
	{
	case VM_UNINIT:
		if (!install_page(page->va, frame->kva, page->writable))
			PANIC("FAIL");
		break;
	case VM_ANON:
		break; // anonymous는 swap_in()에서 처리됨
	case VM_FILE:
		break;
	}
	return swap_in(page, frame->kva);
}

/* 보조 페이지 테이블 초기화 (해시 테이블 구성) */
void supplemental_page_table_init(struct supplemental_page_table *spt UNUSED)
{
	hash_init(&spt->spt_hash, hash_va, hash_page_less, NULL);
}

/* spt 복사: 부모→자식 페이지 복제 */
bool supplemental_page_table_copy(struct supplemental_page_table *dst, struct supplemental_page_table *src)
{
	struct hash_iterator i;
	hash_first(&i, &src->spt_hash);
	bool is_lock_held = lock_held_by_current_thread(&filesys_lock);
	if (!is_lock_held) lock_acquire(&filesys_lock);

	while (hash_next(&i))
	{
		struct page *page = hash_entry(hash_cur(&i), struct page, page_elem);
		void *aux;
		struct page *newpage;
		switch (VM_TYPE(page->operations->type))
		{
		case VM_UNINIT:
			aux = malloc(sizeof(struct load));
			memcpy(aux, page->uninit.aux, sizeof(struct load));
			vm_alloc_page_with_initializer(page->uninit.type, page->va, page->writable, page->uninit.init, aux);
			break;
		case VM_ANON:
			vm_alloc_page_with_initializer(page->operations->type, page->va, page->writable, NULL, NULL);
			vm_claim_page(page->va);
			newpage = spt_find_page(dst, page->va);
			memcpy(newpage->frame->kva, page->frame->kva, PGSIZE);
			break;
		case VM_FILE:
			newpage = malloc(sizeof(struct page));
			newpage->va = page->va;
			newpage->writable = page->writable;
			newpage->operations = page->operations;
			spt_insert_page(dst, newpage);
			newpage->frame = page->frame;
			newpage->frame->cnt_page++;
			newpage->file.file = file_duplicate(page->file.file);
			newpage->file.file_length = page->file.file_length;
			newpage->file.ofs = page->file.ofs;
			newpage->file.read_bytes = page->file.read_bytes;
			newpage->file.zero_bytes = page->file.zero_bytes;
			pml4_set_page(thread_current()->pml4, newpage->va, page->frame->kva, page->writable);
			break;
		}
	}
	if (!is_lock_held) lock_release(&filesys_lock);
	return true;
}

/* 보조 페이지 테이블의 모든 페이지 제거 */
void supplemental_page_table_kill(struct supplemental_page_table *spt UNUSED)
{
	hash_clear(&spt->spt_hash, clear_page_hash);
}

/* 페이지 할당 해제: 메모리 파괴 및 free() */
void vm_dealloc_page(struct page *page)
{
	destroy(page);
	free(page);
}

/* 특정 주소에 해당하는 페이지를 확보 (lazy loading) */
bool vm_claim_page(void *va UNUSED)
{
	struct thread *curr = thread_current();
	struct page *page = spt_find_page(&curr->spt, va);
	if (page == NULL)
		PANIC("TODO");
	return vm_do_claim_page(page);
}

/* MMU에 가상주소→물리주소 매핑 */
static bool install_page(void *upage, void *kpage, bool writable)
{
	struct thread *t = thread_current();
	return (pml4_get_page(t->pml4, upage) == NULL && pml4_set_page(t->pml4, upage, kpage, writable));
}

/* 해시 테이블 정렬용 비교 함수 (va 주소 비교) */
bool hash_page_less(const struct hash_elem *a, const struct hash_elem *b, void *aux)
{
	struct page *page_a = hash_entry(a, struct page, page_elem);
	struct page *page_b = hash_entry(b, struct page, page_elem);
	return page_a->va < page_b->va;
}

/* 해시 함수: va 기반 해시값 계산 */
unsigned int hash_va(const struct hash_elem *p, void *aux UNUSED)
{
	struct page *page = hash_entry(p, struct page, page_elem);
	return hash_bytes(&page->va, sizeof(page->va));
}

/* 페이지 해제용 콜백 함수 */
void clear_page_hash(struct hash_elem *h, void *aux)
{
	struct page *page = hash_entry(h, struct page, page_elem);
	vm_dealloc_page(page);
}