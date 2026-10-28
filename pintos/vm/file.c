/* file.c: Implementation of memory backed file object (mmaped object). */

#include "vm/vm.h"
#include "userprog/process.h"
#include "threads/vaddr.h"

static bool file_backed_swap_in (struct page *page, void *kva);
static bool file_backed_swap_out (struct page *page);
static void file_backed_destroy (struct page *page);

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

	return true;
}

/* Swap in the page by read contents from the file. */
static bool
file_backed_swap_in (struct page *page, void *kva) {
	struct file_page *file_page UNUSED = &page->file;
}

/* Swap out the page by writeback contents to the file. */
static bool
file_backed_swap_out (struct page *page) {
	struct file_page *file_page UNUSED = &page->file;
}

/* Destory the file backed page. PAGE will be freed by the caller. */
static void
file_backed_destroy (struct page *page) {
	struct file_page *file_page UNUSED = &page->file;
    struct thread *curr = thread_current ();

    /* dirty bit 확인 */
    if (pml4_is_dirty (curr->pml4, page->va)) {
        /* 파일에 변경 내용 write-back */
        file_write_at (file_page->file, page->va, file_page->read_bytes, file_page->offset);
        
        /* dirty bit 지움 */
        pml4_set_dirty (curr->pml4, page->va, false);
    }
}

/* Do the mmap */
void *
do_mmap (void *addr, size_t length, int writable,
		struct file *file, off_t offset) {
    /* read_bytes, zero_bytes 계산 */
    uint32_t read_bytes = file_length(file) - offset;

	read_bytes = (read_bytes > length) ? length : read_bytes;

    uint32_t zero_bytes = pg_round_up(addr + length) - (addr + read_bytes);

    if (!load_segment_from_file(file, offset, addr, read_bytes, zero_bytes, writable)) {
        return NULL;
    }
    
    return addr;
}

/* Do the munmap */
void
do_munmap (void *addr) {
	
}
