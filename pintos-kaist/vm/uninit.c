/* uninit.c: 초기화되지 않은(uninit) 페이지 구현부
 *
 * 모든 페이지는 처음에 uninit 페이지로 생성됩니다. 첫 번째 페이지 폴트가 발생하면,
 * 핸들러 체인은 uninit_initialize(page->operations.swap_in)를 호출합니다.
 * 이 함수는 페이지를 특정 타입의 페이지 객체(anon, file, page_cache 등)로 변환하고,
 * vm_alloc_page_with_initializer 함수로부터 전달된 초기화 콜백을 호출합니다.
 */

#include "vm/vm.h"
#include "vm/uninit.h"

static bool uninit_initialize (struct page *page, void *kva);
static void uninit_destroy (struct page *page);

/* 이 구조체는 수정하지 마세요 */
static const struct page_operations uninit_ops = {
	.swap_in = uninit_initialize,
	.swap_out = NULL,
	.destroy = uninit_destroy,
	.type = VM_UNINIT,
};

/* 이 함수는 수정하지 마세요 */
void
uninit_new (struct page *page, void *va, vm_initializer *init,
		enum vm_type type, void *aux,
		bool (*initializer)(struct page *, enum vm_type, void *)) {
	ASSERT (page != NULL);

	*page = (struct page) {
		.operations = &uninit_ops,
		.va = va,
		.frame = NULL, /* 아직 프레임 없음 */
		.uninit = (struct uninit_page) {
			.init = init,
			.type = type,
			.aux = aux,
			.page_initializer = initializer,
		}
	};
}

/* 첫 번째 페이지 폴트 시 페이지를 초기화합니다 */
static bool
uninit_initialize (struct page *page, void *kva) {
	struct uninit_page *uninit = &page->uninit;

	/* 먼저 보관 → page_initializer가 값을 덮어쓸 수 있음 */
	vm_initializer *init = uninit->init;
	void *aux = uninit->aux;

	/* TODO: 필요 시 이 함수의 로직을 보완하세요 */
	return uninit->page_initializer (page, uninit->type, kva) &&
		(init ? init (page, aux) : true);
}

/* uninit_page가 보유한 리소스를 해제합니다.
 * 대부분의 페이지는 다른 타입으로 전환되지만,
 * 실행 중 한 번도 참조되지 않은 채 프로세스가 종료되면 uninit 페이지가 남아있을 수 있습니다.
 * 페이지 자체는 호출자에 의해 해제됩니다. */
static void
uninit_destroy (struct page *page) {
	struct uninit_page *uninit UNUSED = &page->uninit;
	/* TODO: 처리할 작업이 없다면 그냥 return 하세요. */
}
