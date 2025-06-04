/* file.c: 메모리 기반 파일 객체(mmap된 객체)의 구현 */

#include "vm/vm.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "userprog/process.h"

static bool file_backed_swap_in (struct page *page, void *kva);
static bool file_backed_swap_out (struct page *page);
static void file_backed_destroy (struct page *page);

/* 이 구조체는 수정하지 마세요 */
static const struct page_operations file_ops = {
	.swap_in = file_backed_swap_in,
	.swap_out = file_backed_swap_out,
	.destroy = file_backed_destroy,
	.type = VM_FILE,
};

/* 파일 기반 페이지 서브시스템을 초기화합니다. */
void
vm_file_init (void) {
}

/* 파일 기반 페이지를 초기화합니다. */
bool
file_backed_initializer (struct page *page, enum vm_type type, void *kva) {
	/* 핸들러 설정 */
	page->operations = &file_ops;

	struct file_page *file_page = &page->file;

	struct load_info *load_info = (struct load_info *)page->uninit.aux;

	file_page->file = load_info->file;
	file_page->ofs = load_info->ofs;
	file_page->read_bytes = load_info->read_bytes;
	file_page->zero_bytes = load_info->zero_bytes;
}

/* 파일에서 내용을 읽어 페이지를 메모리에 로드합니다. */
static bool
file_backed_swap_in (struct page *page, void *kva) {
	struct file_page *file_page UNUSED = &page->file;
}

/* 페이지의 내용을 파일에 write-back 하여 스왑 아웃합니다. */
static bool
file_backed_swap_out (struct page *page) {
	struct file_page *file_page UNUSED = &page->file;
}

/* 파일 기반 페이지를 제거합니다. PAGE는 호출자에 의해 해제됩니다. */
static void
file_backed_destroy (struct page *page) {
	struct file_page *file_page UNUSED = &page->file;
	if (pml4_is_dirty(thread_current()->pml4, page->va)) {
		file_write_at(file_page->file, page->va, file_page->read_bytes, file_page->ofs);
		pml4_set_dirty(thread_current()->pml4, page->va, 0);
	}
	pml4_clear_page(thread_current()->pml4, page->va);
}

/* mmap을 수행합니다. */
void *
do_mmap (void *addr, size_t length, int writable,
		struct file *file, off_t offset) {
			struct file *f = file_reopen(file);
    void *start_addr = addr; // 매핑 성공 시 파일이 매핑된 가상 주소 반환하는 데 사용
    // 이 매핑을 위해 사용한 총 페이지 수
    int total_page_count = length <= PGSIZE ? 1 : length % PGSIZE ? length / PGSIZE + 1 : length / PGSIZE; 

    size_t read_bytes = file_length(f) < length ? file_length(f) : length;
    size_t zero_bytes = PGSIZE - read_bytes % PGSIZE;

    ASSERT((read_bytes + zero_bytes) % PGSIZE == 0);
    ASSERT(pg_ofs(addr) == 0);      // upage가 페이지 정렬되어 있는지 확인
    ASSERT(offset % PGSIZE == 0); // ofs가 페이지 정렬되어 있는지 확인

    while (read_bytes > 0 || zero_bytes > 0)
    {
        /* 이 페이지를 채우는 방법을 계산합니다.
        파일에서 PAGE_READ_BYTES 바이트를 읽고
        최종 PAGE_ZERO_BYTES 바이트를 0으로 채웁니다. */
        size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
        size_t page_zero_bytes = PGSIZE - page_read_bytes;

        struct load_info *load_info = (struct load_info *)malloc(sizeof(struct load_info));
        load_info->file = f;
        load_info->ofs = offset;
        load_info->read_bytes = page_read_bytes;
        load_info->zero_bytes = page_zero_bytes;

        // vm_alloc_page_with_initializer를 호출하여 대기 중인 객체를 생성합니다.
        if (!vm_alloc_page_with_initializer(VM_FILE, addr,
                                            writable, lazy_load_segment, load_info))
            return NULL;

        struct page *p = spt_find_page(&thread_current()->spt, start_addr);
        p->mapped_page_count = total_page_count;

        /* Advance. */
        // 읽은 바이트와 0으로 채운 바이트를 추적하고 가상 주소를 증가시킵니다.
        read_bytes -= page_read_bytes;
        zero_bytes -= page_zero_bytes;
        addr += PGSIZE;
        offset += page_read_bytes;
    }

    return start_addr;
}

/* munmap을 수행합니다. */
void
do_munmap (void *addr) {
	struct supplemental_page_table *spt = &thread_current()->spt;
    struct page *p = spt_find_page(spt, addr);
    int count = p->mapped_page_count;
    for (int i = 0; i < count; i++)
    {
        if (p)
            destroy(p);
        addr += PGSIZE;
        p = spt_find_page(spt, addr);
    }
}
