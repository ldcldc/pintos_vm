/* file.c: Implementation of memory backed file object (mmaped object). */

#include "vm/vm.h"
#include "userprog/process.h"
#include "lib/round.h"
#include "threads/vaddr.h"
#include "threads/mmu.h"

static bool file_backed_swap_in (struct page *page, void *kva);
static bool file_backed_swap_out (struct page *page);
static void file_backed_destroy (struct page *page);
static void check_page_record (void *addr);
/* DO NOT MODIFY this struct */
static const struct page_operations file_ops = {
	.swap_in = file_backed_swap_in,
	.swap_out = file_backed_swap_out,
	.destroy = file_backed_destroy,
	.type = VM_FILE,
};

/* The initializer of file vm */
void
vm_file_init (void) {
	
}

/* Initialize the file backed page */
bool
file_backed_initializer (struct page *page, enum vm_type type, void *kva) {
	/* Set up the handler */
	page->operations = &file_ops;
	struct file_page *file_page = &page->file;
	struct segment_aux *aux = (struct segment_aux *)page->uninit.aux;
	
	file_page->file = aux->file;
	file_page->offset = aux->offset;
	file_page->length = aux->page_read_bytes;
	file_page->page_cnt = aux->page_cnt;

	return true;
}

/* Swap in the page by read contents from the file. */
static bool
file_backed_swap_in (struct page *page, void *kva) {
	struct file_page *file_page = &page->file;
}

/* Swap out the page by writeback contents to the file. */
static bool
file_backed_swap_out (struct page *page) {
	struct file_page *file_page = &page->file;
}

/* Destory the file backed page. PAGE will be freed by the caller. */
static void
file_backed_destroy (struct page *page) {
	struct file_page *file_page = &page->file;
	
	if (pml4_is_dirty (thread_current ()->pml4, page->va)) {		
		file_write_at (file_page->file, page->va, file_page->length, file_page->offset);
		pml4_set_dirty (thread_current ()->pml4, page->va, 0);
	}

	file_close (file_page->file);
}

/* Do the mmap */
void *
do_mmap (void *addr, size_t length, int writable,
		struct file *file, off_t offset) {
	off_t file_len = file_length (file);
	uint32_t standard_length = file_len - offset;

	uint32_t read_bytes = standard_length >= (offset + length) ? length : (file_len - offset);
	uint32_t zero_bytes = read_bytes <= PGSIZE ? (PGSIZE - read_bytes) 
											   : pg_round_up (addr + read_bytes) - (addr + read_bytes);
	
	if (!load_segment_file (file, offset, addr, read_bytes, zero_bytes, writable, VM_FILE)) {
		return NULL;
	}

	return addr;
}

/* Do the munmap */
void
do_munmap (void *addr) {
	check_page_record (addr);
}

static void
check_page_record (void *addr) {
	struct page *page = spt_find_page (&thread_current ()->spt, addr);
	size_t count = page->file.page_cnt;

	char *start = (char *) addr;
    
	for (char *curr = start; curr < start + (count * PGSIZE); curr += PGSIZE) {
        struct page *page = spt_find_page (&thread_current ()->spt, curr);
		
		hash_delete (&thread_current ()->spt.hash, &page->hash_elem);
		vm_dealloc_page (page);
    }
}