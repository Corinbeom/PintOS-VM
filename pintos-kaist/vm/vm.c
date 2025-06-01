/* vm.c: 가상 메모리 객체들을 위한 일반 인터페이스 */

#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include "lib/kernel/hash.h"
#include "threads/mmu.h"

/* 각 서브시스템의 초기화 코드를 호출하여 가상 메모리 서브시스템을 초기화합니다. */
void
vm_init (void) {
	vm_anon_init ();
	vm_file_init ();
#ifdef EFILESYS  /* Project 4용 */
	pagecache_init ();
#endif
	register_inspect_intr ();
	/* 이 위쪽은 수정하지 마세요 !! */
	/* TODO: 이 아래쪽부터 코드를 추가하세요 */
}

/* 페이지의 타입을 가져옵니다. 이 함수는 페이지가 초기화된 후 타입을 알고 싶을 때 유용합니다.
 * 이 함수는 이미 완전히 구현되어 있습니다. */
enum vm_type
page_get_type (struct page *page) {
	int ty = VM_TYPE (page->operations->type);
	switch (ty) {
		case VM_UNINIT:
			return VM_TYPE (page->uninit.type);
		default:
			return ty;
	}
}

/* 헬퍼 함수들 */
static struct frame *vm_get_victim (void);
static bool vm_do_claim_page (struct page *page);
static struct frame *vm_evict_frame (void);

/* 초기화 함수와 함께 새 페이지를 생성합니다. malloc과 page 구조체를 직접 사용해 페이지를 만들지 마세요. */
bool
vm_alloc_page_with_initializer (enum vm_type type, void *upage, bool writable,
		vm_initializer *init, void *aux) {
	ASSERT (VM_TYPE(type) != VM_UNINIT)

	struct supplemental_page_table *spt = &thread_current ()->spt;
	
	/* 이미 해당 page가 SPT에 존재하는지 확인합니다 */
	if (spt_find_page (spt, upage) == NULL) {
		/* TODO: VM 타입에 따라 페이지를 생성하고, 초기화 함수를 가져온 뒤,
		 * spt_find_page()로 중복 확인
		 * uninit_new()로 struct page 생성
		 * spt_insert_page()로 SPT에 삽입
		 *  */
		struct page *p = (struct page *)malloc(sizeof(struct page));
		// VM 유형에 따라 초기화 함수를 가져와서
		bool (*page_initializer)(struct page *, enum vm_type, void *);

		switch (VM_TYPE(type))
		{
		case VM_ANON:
			page_initializer = anon_initializer;
			break;
		case VM_FILE:
			page_initializer = file_backed_initializer;
			break;
		}
		// uninit_new를 호출해 "uninit" 페이지 구조체를 생성하세요.
		uninit_new(p, upage, init, type, aux, page_initializer);
		// uninit_new를 호출한 후에는 필드를 수정해야 합니다.
		p->writable = writable;

		/* TODO: Insert the page into the spt. */
		return spt_insert_page(spt, p);
	}
err:
	return false;
}

/* SPT에서 해당 가상 주소(VA)를 찾아 페이지를 반환합니다. 에러 발생 시 NULL 반환 */
struct page *
spt_find_page (struct supplemental_page_table *spt UNUSED, void *va UNUSED) {
	/* TODO: Fill this function. */
	struct page p;
	struct hash_elem *e;

	// va에 해당하는 hash_elem 찾기
	p.va = pg_round_down(va); // page의 시작 주소 할당
	e = hash_find(&spt->spt_hash, &p.hash_elem);
	// 있으면 e에 해당하는 페이지 반환
	return e != NULL ? hash_entry(e, struct page, hash_elem) : NULL;
}

/* 검증을 거쳐 SPT에 PAGE를 삽입합니다. */
bool
spt_insert_page (struct supplemental_page_table *spt UNUSED,
		struct page *page UNUSED) {
		return hash_insert(&spt->spt_hash, &page->hash_elem) == NULL ? true : false;
}

void
spt_remove_page (struct supplemental_page_table *spt, struct page *page) {
	vm_dealloc_page (page);
	return true;
}

/* 제거(eviction) 대상이 될 프레임을 선택하여 반환합니다. */
static struct frame *
vm_get_victim (void) {
	struct frame *victim = NULL;
	 /* TODO: 제거 정책은 자유롭게 구현 가능합니다. */
	
	return victim;
}

/* 페이지 하나를 제거하고, 해당 프레임을 반환합니다. 에러가 발생하면 NULL 반환 */
static struct frame *
vm_evict_frame (void) {
	struct frame *victim UNUSED = vm_get_victim ();
	/* TODO: victim을 스왑 아웃하고 해당 프레임을 반환하세요. */

	return NULL;
}

/* palloc()을 이용해 프레임을 할당합니다. 사용 가능한 페이지가 없으면 페이지를 제거한 뒤 프레임을 반환합니다.
 * 항상 유효한 주소를 반환해야 합니다. 즉, 사용자 풀 메모리가 꽉 찬 경우에도 제거를 통해 프레임 공간을 확보합니다. */
static struct frame *
vm_get_frame (void) {
	struct frame *frame = NULL;
	/* TODO: 이 함수를 구현하세요. */
	void *kva = palloc_get_page(PAL_USER);
	
	if (kva == NULL) {
		PANIC("todo");
	}

	frame = (struct frame *)malloc(sizeof(struct frame));
	frame->kva = kva;
	frame->page = NULL;

	ASSERT (frame != NULL);
	ASSERT (frame->page == NULL);
	return frame;
}

/* 스택을 확장합니다. */
static void
vm_stack_growth (void *addr UNUSED) {
	/* vm_try_handle_fault() 안에서
		스택 확장 조건 만족 시 호출 */
}

/* 쓰기 보호된 페이지에서의 폴트를 처리합니다. */
static bool
vm_handle_wp (struct page *page UNUSED) {
}

/* 성공 시 true를 반환합니다. */
bool
vm_try_handle_fault (struct intr_frame *f UNUSED, void *addr UNUSED,
		bool user UNUSED, bool write UNUSED, bool not_present UNUSED) {
	struct supplemental_page_table *spt UNUSED = &thread_current ()->spt;
	struct page *page = NULL;
	/* TODO: 폴트를 검증하고 처리하세요. 
	 * spt_find_page() → vm_claim_page()
	 * → 유저 접근 여부, write 여부 등 확인
	*/
	/* TODO: 여기에 코드를 작성하세요 */
	if (addr == NULL) {
		return false;
	}

	if (is_kernel_vaddr(addr)) {
		return false;
	}

	if (not_present) {
		page = spt_find_page(spt, addr);
		if (page == NULL) {
			return false;
		}
		if (write == 1 && page->writable == 0) {
			return false;
		}
		return vm_do_claim_page(page);
	}

	return false;
}

/* 페이지를 해제합니다. 이 함수는 수정하지 마세요. */
void
vm_dealloc_page (struct page *page) {
	destroy (page);
	free (page);
}

/* 해당 가상 주소(VA)에 할당된 페이지를 확보(claim)합니다. */
bool
vm_claim_page (void *va UNUSED) {
	struct page *page = NULL;
	/* TODO: 이 함수를 구현하세요. */
	page = spt_find_page(&thread_current()->spt, va);
	if (page == NULL)
		return false;
	return vm_do_claim_page (page);
}

/* 주어진 PAGE를 확보하고 MMU에 매핑합니다. */
static bool
vm_do_claim_page (struct page *page) {
	struct frame *frame = vm_get_frame ();

	/* 연결 설정 */
	frame->page = page;
	page->frame = frame;

	/* TODO: 페이지의 VA를 프레임의 PA에 매핑하기 위한 페이지 테이블 엔트리를 삽입하세요. */
	struct thread *current = thread_current();

	pml4_set_page(current->pml4, page->va, frame->kva, page->writable);
	
	return swap_in(page, frame->kva);
}

/* 새로운 보조 페이지 테이블을 초기화합니다. */
void
supplemental_page_table_init (struct supplemental_page_table *spt UNUSED) {
	hash_init(&spt->spt_hash, page_hash, page_less, NULL);
}

unsigned page_hash(const struct hash_elem *p_, void *aux UNUSED) {
	const struct page *p = hash_entry(p_, struct page, hash_elem);
	return hash_bytes(&p->va, sizeof p->va);
}

bool page_less(const struct hash_elem *a_, const struct hash_elem *b_, void *aux UNUSED) {
	const struct page *a = hash_entry(a_, struct page, hash_elem);
	const struct page *b = hash_entry(b_, struct page, hash_elem);

	return a->va < b->va;
}

/* 보조 페이지 테이블을 src에서 dst로 복사합니다. */
bool
supplemental_page_table_copy (struct supplemental_page_table *dst UNUSED, struct supplemental_page_table *src UNUSED) 
{
	struct hash_iterator i;
	hash_first(&i, &src->spt_hash);

    while (hash_next(&i))
	{
		// src_page 정보
		struct page *src_page = hash_entry(hash_cur(&i), struct page, hash_elem);
		enum vm_type type = src_page->operations->type;
		void *upage = src_page->va;
		bool writable = src_page->writable;

		/* 1) type이 uninit이면 */
		if (type == VM_UNINIT)
		{ // uninit page 생성 & 초기화
			vm_initializer *init = src_page->uninit.init;
			void *aux = src_page->uninit.aux;
			vm_alloc_page_with_initializer(VM_ANON, upage, writable, init, aux);
			continue;
		}

		/* 2) type이 uninit이 아니면 */
		if (!vm_alloc_page_with_initializer(type, upage, writable, NULL, NULL)) // uninit page 생성 & 초기화
			// init(lazy_load_segment)는 page_fault가 발생할때 호출됨
			// 지금 만드는 페이지는 page_fault가 일어날 때까지 기다리지 않고 바로 내용을 넣어줘야 하므로 필요 없음
			return false;

		// vm_claim_page으로 요청해서 매핑 & 페이지 타입에 맞게 초기화
		if (!vm_claim_page(upage))
			return false;

		// 매핑된 프레임에 내용 로딩
		struct page *dst_page = spt_find_page(dst, upage);
		memcpy(dst_page->frame->kva, src_page->frame->kva, PGSIZE);
	}
	return true;

}

void clear_page_hash(struct hash_elem *h, void *aux)
{
	struct page *page = hash_entry(h, struct page, hash_elem);
	destroy(page);
	free(page);
}

/* 보조 페이지 테이블이 보유하고 있는 자원을 해제합니다. */
void
supplemental_page_table_kill (struct supplemental_page_table *spt UNUSED) {
	/* TODO: 스레드가 가지고 있는 모든 보조 페이지 테이블을 제거하고
	 * TODO: 수정된 내용을 저장소에 모두 반영(writeback)해야 합니다. */
	hash_clear(&spt->spt_hash, clear_page_hash);
}