/* vm.c: 가상 메모리 객체들을 위한 일반 인터페이스 */

#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"

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
		 * TODO: uninit_new를 호출하여 "uninit" 페이지 구조체를 생성하세요.
		 * TODO: uninit_new 호출 후에는 필요한 필드를 수정해야 합니다.
		 * spt_find_page()로 중복 확인
		 * uninit_new()로 struct page 생성
		 * spt_insert_page()로 SPT에 삽입
		 *  */

	}
err:
	return false;
}

/* SPT에서 해당 가상 주소(VA)를 찾아 페이지를 반환합니다. 에러 발생 시 NULL 반환 */
struct page *
spt_find_page (struct supplemental_page_table *spt UNUSED, void *va UNUSED) {
	struct page *page = NULL;
	/* TODO: 해시 테이블에서 va에 해당하는 page 찾아 반환 */

	return page;
}

/* 검증을 거쳐 SPT에 PAGE를 삽입합니다. */
bool
spt_insert_page (struct supplemental_page_table *spt UNUSED,
		struct page *page UNUSED) {
	int succ = false;
	/* TODO: hash_insert() 사용 */

	return succ;
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

	return vm_do_claim_page (page);
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

	return swap_in (page, frame->kva);
}

/* 새로운 보조 페이지 테이블을 초기화합니다. */
void
supplemental_page_table_init (struct supplemental_page_table *spt UNUSED) {
	/* hash_init() 사용 */
}

/* 보조 페이지 테이블을 src에서 dst로 복사합니다. */
bool
supplemental_page_table_copy (struct supplemental_page_table *dst UNUSED,
		struct supplemental_page_table *src UNUSED) {
}

/* 보조 페이지 테이블이 보유하고 있는 자원을 해제합니다. */
void
supplemental_page_table_kill (struct supplemental_page_table *spt UNUSED) {
	/* TODO: 스레드가 가지고 있는 모든 보조 페이지 테이블을 제거하고
	 * TODO: 수정된 내용을 저장소에 모두 반영(writeback)해야 합니다. */
}

bool load_file(void *kaddr, struct vm_entry *vme) {

}