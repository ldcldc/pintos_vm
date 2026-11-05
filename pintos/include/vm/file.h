#ifndef VM_FILE_H
#define VM_FILE_H
#include "filesys/file.h"
#include "vm/vm.h"

struct page;
enum vm_type;

struct file_page {
    struct file *file;      /* 파일 */
    off_t offset;           /* 파일에서의 offset */
    uint32_t read_bytes;    /* 읽어온 크기 == write back할 때 쓸 크기 */
	size_t num_page;		/* 이 파일에서 읽어온 페이지 수(첫 페이지만 값 넣고 나머진 0) */
};

void vm_file_init (void);
bool file_backed_initializer (struct page *page, enum vm_type type, void *kva);
void *do_mmap(void *addr, size_t length, int writable,
		struct file *file, off_t offset);
void do_munmap (void *va);
#endif
