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

static struct frame *vm_get_victim(void);
static bool vm_do_claim_page(struct page *page);
static struct frame *vm_evict_frame(void);
static bool install_page(void *upage, void *kpage, bool writable);
bool hash_page_less(const struct hash_elem *a, const struct hash_elem *b, void *aux);
unsigned int hash_va(const struct hash_elem *p, void *aux UNUSED);
void clear_page_hash(struct hash_elem *h, void *aux);

/* lazy loading을 위한 초기화 함수와 함께 페이지를 예약 등록
 * - 실 페이지 할당은 page fault 시점에 수행됨
 * - 유저 주소 upage에 대한 가상 페이지를 생성하고 SPT에 등록
 * - 타입에 맞는 initializer 함수와 보조 데이터(aux)를 사용 */
bool vm_alloc_page_with_initializer(enum vm_type type, void *upage, bool writable,
                                    vm_initializer *init, void *aux)
{
	ASSERT(VM_TYPE(type) != VM_UNINIT); // UNINIT는 직접 호출 금지

	struct supplemental_page_table *spt = &thread_current()->spt;
	upage = pg_round_down(upage); // PGSIZE로 페이지 정렬 

	// 페이지 타입에 따라 해당 초기화 함수 설정
	bool (*page_initializer)(struct page *, enum vm_type, void *kva);
	switch (VM_TYPE(type))
	{
	case VM_ANON: // 익명 페이지인 경우
		page_initializer = anon_initializer;
		break;
	case VM_FILE: // 파일 기반 페이지인 경우
		page_initializer = file_backed_initializer;
		break;
	default:
		return false; // 지원되지 않는 타입
	}

	// 중복 페이지 존재 여부 확인
	if (spt_find_page(spt, upage) == NULL)
	{
		// (1) 새로운 page 구조체 생성
		struct page *newpage = calloc(1, sizeof(struct page)); 

		// (2) 초기화 타입은 UNINIT으로 설정, 이후 page_initializer로 lazy init
		uninit_new(newpage, upage, init, type, aux, page_initializer); 
		newpage->writable = writable;

		// (3) 보조 페이지 테이블에 등록
		spt_insert_page(spt, newpage); 
		return true;
	}
	return false; // 이미 해당 주소에 페이지 존재
}

/* 주어진 가상 주소 va에 해당하는 페이지를 SPT에서 검색
 * - 해시 기반 탐색 */
struct page *spt_find_page(struct supplemental_page_table *spt UNUSED, void *va UNUSED)
{
	struct page tmp;
	tmp.va = va; // 임시 구조체로 비교용 키 구성
	struct hash_elem *h = hash_find(&spt->spt_hash, &tmp.page_elem);
	if (h == NULL)
		return NULL; // 찾지 못했을 시 NULL 반환
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

		// 해당 프레임의 페이지가 최근 접근되지 않았으면 바로 victim으로 선택
		if (!pml4_is_accessed(frame->page->pml4, frame->page->va)) 
			return frame;
		
		// 최근 접근되었다면 accessed 비트만 초기화하고 다음 기회 부여
		pml4_set_accessed(frame->page->pml4, frame->page->va, 0); 
	}
	// 모든 페이지가 accessed == true였던 경우: 2바퀴 째에서 선택하게 됨
	// → 일단 임시로 맨 앞 프레임을 반환
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
	// 프레임 구조체 자체는 항상 먼저 확보
	struct frame *frame = calloc(1, sizeof(struct frame));

	// 유저 영역용 물리 페이지 1개 확보 (0으로 초기화됨)
	void *upage = palloc_get_page(PAL_USER | PAL_ZERO); 

	if (upage == NULL)
	{
		// 메모리가 부족해서 할당 실패 시:
		// 1. frame 구조체는 해제
		free(frame);

		// 2. victim frame을 교체 정책으로 선정
		frame = vm_evict_frame();             // 교체 대상 선정
		swap_out(frame->page);                // 해당 프레임의 페이지를 디스크로 내보냄

		// 3. 프레임 리스트에서 제거 후 다시 추가 (순서 재정의 목적)
		list_remove(&frame->frame_elem);
		list_push_back(&frame_table, &frame->frame_elem);

		// 4. 이 프레임은 재사용될 것이므로 기존 페이지 링크 해제
		frame->page = NULL;
	}
	else
	{
		// 페이지 확보 성공 시:
		frame->kva = upage;                   // 실제 물리 주소 저장
		frame->page = NULL;                   // 아직 연결된 페이지 없음
		list_push_back(&frame_table, &frame->frame_elem); // 프레임 테이블 등록

		list_init(&frame->page_list);         // 공유하는 페이지 리스트 초기화
		frame->cnt_page = 1;                  // 현재는 페이지 1개만 연결
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
 * [핸들링 대상]
 * - ① 페이지가 존재하지 않는 경우 (not_present = true):
 *     → 스택 자동 확장 or lazy loading 처리
 * - ② 페이지는 존재하지만 protection fault (not_present = false):
 *     → 예: read-only 페이지에 write 요청 → 즉시 종료
 */
bool vm_try_handle_fault(struct intr_frame *f, void *addr, bool user, bool write, bool not_present)
{
	struct supplemental_page_table *spt = &thread_current()->spt;
	struct page *page = NULL;
	// 현재 커널/유저 모드에 따라 올바른 RSP를 선택
	uint64_t user_rsp = user ? f->rsp : thread_current()->user_rsp;

	if (not_present)
	{
		// [1] Stack growth: 접근 주소가 RSP 아래거나, 전체 스택 범위 내이면 자동 확장 허용
		if (user_rsp - 8 == addr ||
			(USER_STACK - (1 << 20) <= user_rsp && user_rsp < addr && addr < USER_STACK))
		{
			vm_stack_growth(addr);
			return true;
		}

		// [2] Lazy loading: SPT에서 해당 주소에 등록된 페이지가 있는지 확인
		page = spt_find_page(spt, pg_round_down(addr));
		
		// (1) 없는 주소이거나, (2) 쓰기 요청인데 read-only 페이지일 경우 → 강제 종료
		if (page == NULL || (write && !page->writable))
			exit(-1);
	}
	else if (write)
	{
		// Protection fault: 존재하는 페이지지만 쓰기 허용되지 않은 경우 → 종료
		exit(-1);
	}

	// 정상적인 페이지 접근 → 물리 메모리에 매핑 시도 (lazy load 수행)
	return vm_do_claim_page(page);
}

/* 페이지를 실제로 확보(claim)하여 물리 메모리에 연결하는 함수
 *
 * [역할]
 * - 페이지에 대응되는 프레임을 확보 (vm_get_frame)
 * - 해당 프레임과 페이지를 상호 연결
 * - 페이지 타입에 따라 처리 방식 결정
 *   - UNINIT: lazy initializer 호출 전, MMU에 가상-물리 매핑 먼저 구성
 *   - ANON / FILE: swap-in에 모든 역할 위임
 * - 마지막으로 swap_in() 호출하여 실제 데이터 메모리에 적재
 *
 * @param page  확보할 가상 페이지 구조체
 * @return true on success, false on failure
 */
static bool vm_do_claim_page(struct page *page)
{
	// 1. 새로운 유저 프레임을 확보
	struct frame *frame = vm_get_frame();

	// 2. 페이지 <-> 프레임 연결
	frame->page = page;
	page->frame = frame;

	// 3. 페이지 타입별로 처리 분기
	switch (VM_TYPE(page->operations->type))
	{
	case VM_UNINIT:
		// Lazy loading용 페이지는 먼저 MMU에 가상-물리 매핑이 필요함
		if (!install_page(page->va, frame->kva, page->writable))
			PANIC("FAIL");  // 매핑 실패는 심각한 오류로 간주
		break;

	case VM_ANON:
		// Anonymous page는 swap_in()에서 로딩 처리
		break;

	case VM_FILE:
		// File-backed page도 swap_in()에서 로딩 처리
		break;
	}

	// 4. swap_in()을 통해 실제 데이터를 프레임에 적재
	//    - UNINIT이면 lazy initializer 호출
	//    - ANON이면 swap 디스크에서 복구
	//    - FILE이면 mmap된 파일에서 복구
	return swap_in(page, frame->kva);
}

/* 보조 페이지 테이블 초기화 (해시 테이블 구성) */
void supplemental_page_table_init(struct supplemental_page_table *spt UNUSED)
{
	hash_init(&spt->spt_hash, hash_va, hash_page_less, NULL);
}

/* 부모 프로세스의 보조 페이지 테이블(SPT)을 자식 프로세스로 복사하는 함수
 *
 * [용도]
 * - process_fork() 호출 시 부모의 SPT를 자식에게 복제
 * - 페이지 타입별로 복제 방식이 다름
 *   - UNINIT: aux 구조체 deep copy 후 lazy initializer 복제
 *   - ANON: 물리 메모리에 존재하는 내용까지 복사
 *   - FILE: mmap된 파일은 공유하지만 page 구조체는 별도 할당
 *
 * @param dst 자식 프로세스의 보조 페이지 테이블
 * @param src 부모 프로세스의 보조 페이지 테이블
 * @return true on success
 */
bool supplemental_page_table_copy(struct supplemental_page_table *dst, struct supplemental_page_table *src)
{
	struct hash_iterator i;
	hash_first(&i, &src->spt_hash);

	// 파일 시스템 관련 페이지가 포함될 수 있으므로 락 획득
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
			// Lazy loading 방식의 페이지
			// → aux 복사해서 자식도 동일한 방식으로 초기화되게 함
			aux = malloc(sizeof(struct load));
			memcpy(aux, page->uninit.aux, sizeof(struct load));

			vm_alloc_page_with_initializer(
				page->uninit.type,
				page->va,
				page->writable,
				page->uninit.init,
				aux);
			break;

		case VM_ANON:
			// 익명 페이지는 실제 물리 메모리에 존재하므로 바로 복제 필요
			vm_alloc_page_with_initializer(
				page->operations->type,
				page->va,
				page->writable,
				NULL, NULL);

			// 페이지 확보 (claim)
			vm_claim_page(page->va);

			// 복제한 자식의 페이지 가져와서 물리 메모리 내용 복사
			newpage = spt_find_page(dst, page->va);
			memcpy(newpage->frame->kva, page->frame->kva, PGSIZE);
			break;

		case VM_FILE:
			// mmap()된 파일 기반 페이지
			// → 파일은 공유하지만 page 구조체는 별도로 복사
			newpage = malloc(sizeof(struct page));

			newpage->va = page->va;
			newpage->writable = page->writable;
			newpage->operations = page->operations;

			spt_insert_page(dst, newpage);

			// 기존 프레임 공유 (참조 수 증가)
			newpage->frame = page->frame;
			newpage->frame->cnt_page++;

			// 파일 정보 복사 (파일은 duplicate해서 사용)
			newpage->file.file = file_duplicate(page->file.file);
			newpage->file.file_length = page->file.file_length;
			newpage->file.ofs = page->file.ofs;
			newpage->file.read_bytes = page->file.read_bytes;
			newpage->file.zero_bytes = page->file.zero_bytes;

			// MMU에 페이지 등록 (물리 메모리 공유)
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
