/* file.c: Implementation of memory backed file object (mmaped object). */

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

/* 파일 기반 페이지를 초기화하는 함수
 * - uninit 페이지를 실제 file-backed 페이지로 변환
 * - page->file에 file_load(aux) 정보를 저장 */
bool file_backed_initializer(struct page *page, enum vm_type type, void *kva)
{
	page->operations = &file_ops;
	struct file_load *aux = page->uninit.aux; // 파일 정보 담긴 구조체

	struct file_page *file_page = &page->file;
	file_page->file = aux->file;
	file_page->ofs = aux->ofs;
	file_page->read_bytes = aux->read_bytes;
	file_page->zero_bytes = aux->zero_bytes;
	file_page->file_length = aux->file_length;
	page->pml4 = thread_current()->pml4;
	list_push_back(&page->frame->page_list, &page->out_elem); // 프레임의 공유 페이지 리스트에 등록
	return true;
}

/* 파일로부터 내용을 읽어와 페이지를 메모리에 로드 (swap-in)
 * - file_page의 file_list에 있는 모든 페이지를 해당 프레임에 연결 */
static bool file_backed_swap_in(struct page *page, void *kva)
{
	struct file_page *file_page = &page->file;
	struct list *file_list = file_page->file_list;
	struct frame *frame = page->frame;
	bool is_lock_held = lock_held_by_current_thread(&filesys_lock);
	if (!is_lock_held)
		lock_acquire(&filesys_lock);
	file_read_at(file_page->file, page->frame->kva, file_page->read_bytes, file_page->ofs);
	if (!is_lock_held)
		lock_release(&filesys_lock);

	// 같은 프레임을 공유하는 페이지들을 다시 연결
	while (!list_empty(file_list))
	{
		struct page *in_page = list_entry(list_pop_front(file_list), struct page, out_elem);
		list_push_back(&frame->page_list, &in_page->out_elem);
		pml4_set_page(in_page->pml4, in_page->va, frame->kva, in_page->writable);
	}
	free(file_list);
	return true;
}

/* 페이지를 파일에 저장하고 (dirty일 경우) 연결 해제 (swap-out) */
static bool file_backed_swap_out(struct page *page)
{
	struct file_page *file_page = &page->file;
	struct list *file_list = malloc(sizeof(struct list));
	list_init(file_list);
	file_page->file_list = file_list;
	struct frame *frame = page->frame;

	bool is_lock_held = lock_held_by_current_thread(&filesys_lock);
	if (!is_lock_held)
		lock_acquire(&filesys_lock);

	while (!list_empty(&frame->page_list))
	{
		struct page *out_page = list_entry(list_pop_front(&frame->page_list), struct page, out_elem);
		if (pml4_is_dirty(out_page->pml4, out_page->va))
			file_write_at(out_page->file.file, out_page->frame->kva, out_page->file.read_bytes, out_page->file.ofs);
		list_push_back(file_list, &out_page->out_elem);
		pml4_clear_page(out_page->pml4, out_page->va);
	}
	if (!is_lock_held)
		lock_release(&filesys_lock);
	return true;
}

/* 파일 기반 페이지 제거: dirty면 write-back 수행하고 참조 수 감소 */
static void file_backed_destroy(struct page *page)
{
	struct file_page *file_page = &page->file;
	page->frame->cnt_page -= 1;
	bool is_lock_held = lock_held_by_current_thread(&filesys_lock);
	if (!is_lock_held)
		lock_acquire(&filesys_lock);
	if (pml4_is_dirty(thread_current()->pml4, page->va))
	{
		file_write_at(page->file.file, page->frame->kva, page->file.read_bytes, page->file.ofs);
	}
	file_close(page->file.file);
	if (!is_lock_held)
		lock_release(&filesys_lock);
	if (page->frame->cnt_page > 0)
		pml4_clear_page(thread_current()->pml4, page->va);
}

/* lazy loading: 파일에서 필요한 바이트만 읽고 나머지는 zero-fill */
static bool lazy_load(struct page *page, void *aux_)
{
	struct file_load *aux = (struct file_load *)aux_;
	struct file *file = aux->file;
	off_t ofs = aux->ofs;
	uint32_t read_bytes = aux->read_bytes;
	uint32_t zero_bytes = aux->zero_bytes;
	free(aux);
	bool is_lock_held = lock_held_by_current_thread(&filesys_lock);
	if (!is_lock_held)
		lock_acquire(&filesys_lock);
	file_seek(file, ofs);
	read_bytes = file_read(file, page->frame->kva, read_bytes);
	if (!is_lock_held)
		lock_release(&filesys_lock);

	memset(page->frame->kva + read_bytes, 0, zero_bytes);
	return true;
}

/* mmap 구현: 파일을 메모리에 매핑하고 lazy loading 등록 */
void *do_mmap(void *addr, size_t length, int writable,
              struct file *file, off_t offset)
{
	int cnt_page = length % PGSIZE ? length / PGSIZE + 1 : length / PGSIZE;
	size_t length_ = length;
	off_t ofs = file_length(file);
	if (ofs < offset) return NULL;
	for (int i = 0; i < cnt_page; i++)
	{
		if (spt_find_page(&thread_current()->spt, addr + i * PGSIZE) != NULL)
			return NULL;
	}

	bool is_lock_held = lock_held_by_current_thread(&filesys_lock);
	if (!is_lock_held)
		lock_acquire(&filesys_lock);
	for (int i = 0; i < cnt_page; i++)
	{
		struct file *file_ = file_reopen(file);
		struct file_load *aux = malloc(sizeof(struct file_load));
		aux->file_length = length;
		aux->file = file_;
		aux->ofs = offset + i * PGSIZE;
		if (length_ >= PGSIZE)
		{
			aux->read_bytes = PGSIZE;
			length_ -= PGSIZE;
		}
		else
			aux->read_bytes = length_;
		aux->zero_bytes = PGSIZE - aux->read_bytes;
		vm_alloc_page_with_initializer(VM_FILE, addr + i * PGSIZE, writable, lazy_load, aux);
	}
	if (!is_lock_held)
		lock_release(&filesys_lock);

	return addr;
}

/* munmap 구현: 해당 addr로부터 매핑된 페이지를 제거 */
void do_munmap(void *addr)
{
	size_t length;
	int cnt_page;
	if (pg_ofs(addr) != 0) return;

	struct page *page = spt_find_page(&thread_current()->spt, addr);
	if (page == NULL) return;

	length = page->file.file_length;
	cnt_page = length % PGSIZE ? length / PGSIZE + 1 : length / PGSIZE;

	bool is_lock_held = lock_held_by_current_thread(&filesys_lock);
	if (!is_lock_held)
		lock_acquire(&filesys_lock);
	for (int i = 0; i < cnt_page; i++)
	{
		page = spt_find_page(&thread_current()->spt, addr + i * PGSIZE);
		if (pml4_is_dirty(thread_current()->pml4, page->va))
		{
			file_write_at(page->file.file, page->va, page->file.read_bytes, page->file.ofs);
		}
		page->frame->cnt_page -= 1;
		file_close(page->file.file);
		hash_delete(&thread_current()->spt.spt_hash, &page->page_elem);
		pml4_clear_page(thread_current()->pml4, page->va);
	}
	if (!is_lock_held)
		lock_release(&filesys_lock);
}
