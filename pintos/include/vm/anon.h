#ifndef VM_ANON_H
#define VM_ANON_H
#include "vm/vm.h"
struct page;
enum vm_type;

struct anon_page {
    size_t swap_index;
};

void vm_anon_init (void);
bool anon_initializer (struct page *page, enum vm_type type, void *kva);
void anon_read_from_swap (size_t swap_index, void *kva);

/* COW로 공유된 swap 슬롯의 참조 수 관리 */
void swap_ref_inc (size_t swap_index);
bool swap_ref_dec (size_t swap_index);

#endif
