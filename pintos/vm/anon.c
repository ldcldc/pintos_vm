/* anon.c: Implementation of page for non-disk image (a.k.a. anonymous page). */

#include "vm/vm.h"
#include "devices/disk.h"
#include "threads/vaddr.h"
#include <bitmap.h>

#define NUM_SEC_IN_PAGE (PGSIZE / DISK_SECTOR_SIZE)	//8

static struct bitmap *swap_bitmap;

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

	swap_bitmap = bitmap_create (disk_size (swap_disk) / NUM_SEC_IN_PAGE);
}

/* Initialize the file mapping */
bool
anon_initializer (struct page *page, enum vm_type type, void *kva) {
	/* Set up the handler */
	page->operations = &anon_ops;

	struct anon_page *anon_page = &page->anon;

	anon_page->swap_index = BITMAP_ERROR;
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

    /* 스왑 공간 비우기 */
    bitmap_set (swap_bitmap, swap_index, false);
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

    return true;
}

/* Destroy the anonymous page. PAGE will be freed by the caller. */
static void
anon_destroy (struct page *page) {
	struct anon_page *anon_page = &page->anon;
	
	if (anon_page->swap_index != BITMAP_ERROR) {
        bitmap_set (swap_bitmap, anon_page->swap_index, false); // 스왑 슬롯 비우기
    }
}
