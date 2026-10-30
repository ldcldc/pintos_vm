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

    struct segment_aux *aux = (struct segment_aux *) page->uninit.aux;

    if (aux != NULL) {
        file_page->file = aux->file;
        file_page->offset = aux->offset;
        file_page->read_bytes = aux->page_read_bytes;
        file_page->num_page = aux->num_page;
    }
    else {
        file_page->file = NULL;
        file_page->offset = 0;
        file_page->read_bytes = 0;
        file_page->num_page = 0;
    }
	
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
		sys_file_lock_acquire ();
        file_write_at (file_page->file, page->va, file_page->read_bytes, file_page->offset);
        sys_file_lock_release ();

        /* dirty bit 지움 */
        pml4_set_dirty (curr->pml4, page->va, false);
    }

    if (page->frame != NULL) {
        vm_remove_frame_from_table (page->frame);

        pml4_clear_page (thread_current ()->pml4, page->va);

        palloc_free_page (page->frame->kva);
        free (page->frame);
        page->frame = NULL;
    }

	/* 이제 file_close */
	sys_file_lock_acquire (); 
    file_close (file_page->file);
    sys_file_lock_release ();
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
    struct thread *curr = thread_current ();
    struct page *head_page = spt_find_page (&curr->spt, addr);

    if (head_page == NULL) return;

    /* unmap할 페이지 뭉치 개수 */
    size_t num_page = head_page->file.num_page;

    /* 반복문 돌면서 spt_remove_page */
    for (size_t i = 0; i < num_page; i++) {
        void *curr_addr = addr + (i * PGSIZE);
        struct page *page = spt_find_page (&curr->spt, curr_addr);

        if (page != NULL) {
            /* spt 지우면서 page->operations->destroy (file_backed_destroy)호출 */
            spt_remove_page (&curr->spt, page);
        }
    }
}
