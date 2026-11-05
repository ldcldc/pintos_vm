/* anon.c: Implementation of page for non-disk image (a.k.a. anonymous page). */

#include "vm/vm.h"
#include "devices/disk.h"
#include "threads/vaddr.h"
#include "threads/malloc.h"
#include "threads/mmu.h"
#include <bitmap.h>
#include <string.h>

#define NUM_SEC_IN_PAGE (PGSIZE / DISK_SECTOR_SIZE)	//8

static struct bitmap *swap_bitmap;

static int *swap_ref;           /* 각 스왑공간에 연관된 page수(cow) */
static size_t swap_slot_cnt;    /* 스왑공간 수(page단위) */

/* DO NOT MODIFY BELOW LINE */
static struct disk *swap_disk;
static bool anon_swap_in (struct page *page, void *kva);
static bool anon_swap_out (struct page *page);
static void anon_destroy (struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations anon_ops = {
	.swap_in = anon_swap_in,
	.swap_out = anon_swap_out,
	.destroy = anon_destroy,
	.type = VM_ANON,
};

/* Initialize the data for anonymous pages */
void
vm_anon_init (void) {
	swap_disk = disk_get(1, 1);
	if (swap_disk == NULL)
        return;

	swap_slot_cnt = disk_size (swap_disk) / NUM_SEC_IN_PAGE;
	swap_bitmap = bitmap_create (swap_slot_cnt);
	if (swap_bitmap == NULL)
		return;

    swap_ref = calloc (swap_slot_cnt, sizeof (int));
    
    ASSERT(swap_ref != NULL);   // 여기 ASSERT는 좀 아닌가
}

/* swap_ref++(cow) */
void
swap_ref_inc (size_t swap_index) {
	if (swap_ref == NULL || swap_index == BITMAP_ERROR) {
		return;
    }
	swap_ref[swap_index]++;
}

/* swap_ref-- 0되면 스왑공간 비움(cow) */
bool
swap_ref_dec (size_t swap_index) {
	if (swap_ref == NULL || swap_index == BITMAP_ERROR) {
		return false;
    }

	if (--swap_ref[swap_index] > 0) {
		return false;
    }

	bitmap_set (swap_bitmap, swap_index, false);
	return true;
}

/* Initialize the file mapping */
bool
anon_initializer (struct page *page, enum vm_type type, void *kva) {
	/* Set up the handler */
	page->operations = &anon_ops;

	struct anon_page *anon_page = &page->anon;

	anon_page->swap_index = BITMAP_ERROR;

    return true;
}

void
anon_read_from_swap (size_t swap_index, void *kva) {
    /* 스왑 공간에서 8섹터 read */
    for (int i = 0; i < NUM_SEC_IN_PAGE; i++) {
        disk_read (swap_disk, swap_index * NUM_SEC_IN_PAGE + i, kva + (i * DISK_SECTOR_SIZE));
    }
}

/* Swap in the page by read contents from the swap disk. */
static bool
anon_swap_in (struct page *page, void *kva) {
	struct anon_page *anon_page = &page->anon;

	size_t swap_index = anon_page->swap_index;

    if (swap_index == BITMAP_ERROR) {
        return false;
    }
	/* 디스크 스왑 공간에서 한 SECTOR 읽기 8번 반복 */
    for (int i = 0; i < NUM_SEC_IN_PAGE; i++) {
        disk_read (swap_disk, swap_index * NUM_SEC_IN_PAGE + i, kva + (i * DISK_SECTOR_SIZE));
    }

    /* swap_ref_dec */
    swap_ref_dec (swap_index);
    anon_page->swap_index = BITMAP_ERROR;

    return true;

}

/* Swap out the page by writing contents to the swap disk. */
static bool
anon_swap_out (struct page *page) {
	struct anon_page *anon_page = &page->anon;

    /* 비트맵으로 남는공간 찾아서 마킹하고 인덱스 반환 */
    size_t swap_index = bitmap_scan_and_flip (swap_bitmap, 0, 1, false);
    if (swap_index == BITMAP_ERROR) {
        return false;
    }

    /* 디스크 스왑 공간에 한 SECTOR 쓰기 8번 반복 */
    for (int i = 0; i < NUM_SEC_IN_PAGE; i++) {
        disk_write (swap_disk, swap_index * NUM_SEC_IN_PAGE + i, page->frame->kva + (i * DISK_SECTOR_SIZE));
    }

    anon_page->swap_index = swap_index;

    /*  swap_ref 초기화(cow) */
    if (swap_ref != NULL)
        swap_ref[swap_index] = 1;

    return true;
}


/* Destroy the anonymous page. PAGE will be freed by the caller. */
static void
anon_destroy (struct page *page) {
    struct anon_page *anon_page = &page->anon;
    
    /* 스왑 공간 비우기: 공유 중이면 참조 수만 감소 */
    swap_ref_dec (anon_page->swap_index);
    anon_page->swap_index = BITMAP_ERROR;

    /* 해제 */
    if (page->frame != NULL) {
        lock_acquire (&frame_lock);
        list_remove (&page->cow_elem);
        bool should_free = list_empty (&page->frame->page_list);
        lock_release (&frame_lock);

        pml4_clear_page (page->p_thread->pml4, page->va);

        if (should_free) {
            vm_remove_frame_from_table (page->frame);
            palloc_free_page (page->frame->kva);
            free (page->frame);
        }
        page->frame = NULL;
    }
}
