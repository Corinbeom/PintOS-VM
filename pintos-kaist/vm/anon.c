/* anon.c: 디스크 이미지가 아닌 페이지(즉, 익명 페이지)를 위한 구현입니다. */

#include "vm/vm.h"
#include "devices/disk.h"

/* 이 아래 줄은 수정하지 마세요 */
static struct disk *swap_disk;
static bool anon_swap_in (struct page *page, void *kva);
static bool anon_swap_out (struct page *page);
static void anon_destroy (struct page *page);

/* 이 구조체는 수정하지 마세요 */
static const struct page_operations anon_ops = {
	.swap_in = anon_swap_in,
	.swap_out = anon_swap_out,
	.destroy = anon_destroy,
	.type = VM_ANON,
};

/* 익명 페이지에 필요한 데이터를 초기화합니다. */
void
vm_anon_init (void) {
	/* TODO: swap_disk를 설정하세요. */
	swap_disk = disk_get(1, 0);
}

/* 익명 페이지 매핑을 초기화합니다. */
bool
anon_initializer (struct page *page, enum vm_type type, void *kva) {
	/* 핸들러를 설정합니다. */
	page->operations = &anon_ops;

	struct anon_page *anon_page = &page->anon;
}

/* 스왑 디스크에서 내용을 읽어와 페이지를 메모리에 로드합니다. */
static bool
anon_swap_in (struct page *page, void *kva) {
	struct anon_page *anon_page = &page->anon;
}

/* 페이지 내용을 스왑 디스크에 저장하여 스왑 아웃합니다. */
static bool
anon_swap_out (struct page *page) {
	struct anon_page *anon_page = &page->anon;
}

/* 익명 페이지를 제거합니다. PAGE는 호출자에 의해 해제됩니다. */
static void
anon_destroy (struct page *page) {
	struct anon_page *anon_page = &page->anon;
}
