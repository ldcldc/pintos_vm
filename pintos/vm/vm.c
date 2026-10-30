/* vm.c: Generic interface for virtual memory objects. */

#include <list.h>
#include <hash.h>

#include "threads/vaddr.h"
#include "threads/mmu.h"
#include "threads/malloc.h"

#include "userprog/syscall.h"

#include "vm/vm.h"
#include "vm/inspect.h"
#include "vm/file.h"
#include "vm/anon.h"

#define MAXSTACK (1 << 20)		/* 1mb */

struct list frame_list;				/* 모든 frame list */
struct lock frame_lock;				/* frame_lock */
struct list_elem *curr_frame_elem; 	/* 마지막으로 검사한 위치 */

/* Initializes the virtual memory subsystem by invoking each subsystem's
 * intialize codes. */
void
vm_init (void) {
	vm_anon_init ();
	vm_file_init ();

    list_init (&frame_list);
    lock_init (&frame_lock);
    curr_frame_elem = NULL;
#ifdef EFILESYS  /* For project 4 */
	pagecache_init ();
#endif
	register_inspect_intr ();
	/* DO NOT MODIFY UPPER LINES. */
	/* TODO: Your code goes here. */
}

/* Get the type of the page. This function is useful if you want to know the
 * type of the page after it will be initialized.
 * This function is fully implemented now. */
enum vm_type
page_get_type (struct page *page) {
	int ty = VM_TYPE (page->operations->type);
	switch (ty) {
		case VM_UNINIT:
			return VM_TYPE (page->uninit.type);
		default:
			return ty;
	}
}

/* Helpers */
static struct page *page_create_uninit (enum vm_type type, void *upage,
						vm_initializer *init, void *aux, bool writable);
static struct frame *vm_get_victim (void);
static struct frame *vm_evict_frame (void);
static struct frame *vm_get_frame (void);
static bool vm_handle_wp (struct page *page UNUSED);
static bool vm_stack_growth (void *addr);
static bool vm_do_claim_page (struct page *page);
static unsigned page_hash (const struct hash_elem *e, void *aux UNUSED);
static bool page_less (const struct hash_elem *a, const struct hash_elem *b, void *aux UNUSED);
static bool page_copy_uninit (struct supplemental_page_table *dst, struct page *src_page);
static bool page_copy_initialized (struct supplemental_page_table *dst, struct page *src_page);
static struct segment_aux * duplicate_segment_aux (struct segment_aux *src_aux);
static bool duplicate_file_page_meta (struct page *dst_page, struct page *src_page);
static void destructor (struct hash_elem *e, void *aux UNUSED);

/* Create the pending page object with initializer. If you want to create a
 * page, do not create it directly and make it through this function or
 * `vm_alloc_page`. */

bool
vm_alloc_page_with_initializer (enum vm_type type, void *upage, bool writable,
		vm_initializer *init, void *aux) {

	if (upage == NULL || !is_user_vaddr(upage)|| pg_ofs (upage) != 0)	//pg_ofs (upage)이것도 지금해야할까 코드 아래랑 커널베이스~스택 사이는?
		goto err;

	ASSERT (VM_TYPE(type) != VM_UNINIT)

	struct supplemental_page_table *spt = &thread_current ()->spt;

	/* Check wheter the upage is already occupied or not. */
	if (spt_find_page (spt, upage) == NULL) {
		struct page *page = page_create_uninit (type, upage, init, aux, writable);
		if (page == NULL) {
			goto err;
		}

		/* spt에 삽입 */
		if (!spt_insert_page (spt, page)) {
			free (page);
			goto err;
		}
		return true;
	}
err:
    return false;
}

static struct page *
page_create_uninit (enum vm_type type, void *upage, vm_initializer *init, void *aux, bool writable) {
    struct page *page = malloc(sizeof(*page));
    if (page == NULL)
        return NULL;

    /* 타입별 uninit_new 세팅 */
    switch (VM_TYPE (type)) {
    case VM_ANON:
        uninit_new (page, upage, init, type, aux, anon_initializer);
        break;
    case VM_FILE:
        uninit_new (page, upage, init, type, aux, file_backed_initializer);
        break;
    default:
        free (page);
        return NULL;
    }
    
    page->writable = writable;
	page->p_thread = thread_current ();
	 
    return page;
}

/* Find VA from spt and return page. On error, return NULL. */
/* 유저 영역인지 커널 영역인지 확인이 필요한지 알아보기 */
/* 꼭 여기서 확인이 필요하지는 않지만, spt는 해당 프로세스의 '유저 가상 주소'를 관리
	* 같은 유저 가상주소라도 프로세스마다 의미가 다르기 때문에 SPT를 각각 관리
	*/
/* pg_round_down: 입력된 주소 보다 작은 가장 가까운 페이지의 시작 주소 반환 */
/* | | | |va| | | |...| 
	*  ^
	*  |
	* start
	* addr
	*/
struct page *
spt_find_page (struct supplemental_page_table *spt, void *va) {
    struct page round_down_page;
    round_down_page.va = pg_round_down (va);

	if (&round_down_page == NULL || &((&round_down_page)->hash_elem) == NULL)
		return NULL;

    /* hash_find로 탐색 */
    struct hash_elem *h_e = hash_find (&spt->hash, &round_down_page.hash_elem);
    if (h_e == NULL){
        return NULL;
	}

	return hash_entry (h_e, struct page, hash_elem);
}

/* Insert PAGE into spt with validation. */
bool
spt_insert_page (struct supplemental_page_table *spt,
		struct page *page) {
	return hash_insert (&spt->hash, &page->hash_elem) == NULL;
}

void
spt_remove_page (struct supplemental_page_table *spt, struct page *page) {
	hash_delete (&spt->hash, &page->hash_elem);
	vm_dealloc_page (page);
}

/* Get the struct frame, that will be evicted. */
static struct frame *
vm_get_victim (void) {
    struct frame *victim = NULL;
    
    lock_acquire (&frame_lock);
    
    if (list_empty (&frame_list)) {
        lock_release (&frame_lock);
        return NULL;
    }

    /* 시계바늘이 NULL이거나 끝에 도달했으면 맨 처음으로 리셋 */
    if (curr_frame_elem == NULL || curr_frame_elem == list_end (&frame_list)) {
        curr_frame_elem = list_begin (&frame_list);
    }

	/* frame_list * 2 만큼 순회 */
	size_t loop_count = list_size (&frame_list) * 2;
    for (size_t i = 0; i < loop_count; i++) {
        struct frame *curr_frame = list_entry (curr_frame_elem, struct frame, frame_elem);
        struct page *curr_page = curr_frame->page;

		if (curr_page == NULL) continue;

        curr_frame_elem = list_next_circular (curr_frame_elem, &frame_list);
        
        uint64_t *curr_pml4 = curr_page->p_thread->pml4;
        
        if (pml4_is_accessed (curr_pml4, curr_page->va)) {
            pml4_set_accessed (curr_pml4, curr_page->va, false);
        } else {
            victim = curr_frame;
            break;
        }
    }

    if (victim != NULL) {
        list_remove (&victim->frame_elem);
    }
    
    lock_release (&frame_lock);
    return victim;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *
vm_evict_frame (void) {
    struct frame *victim = vm_get_victim ();
    if (victim == NULL) {
        return NULL;
    }

    if (!swap_out (victim->page)) {
        return NULL; 
    }
    
    pml4_clear_page (victim->page->p_thread->pml4, victim->page->va);
    
    victim->page->frame = NULL;

    return victim;
}

/* palloc() and get frame. If there is no available page, evict the page
 * and return it. This always return valid address. That is, if the user pool
 * memory is full, this function evicts the frame to get the available memory
 * space.*/
static struct frame *
vm_get_frame (void) {
	/* TODO: Fill this function. */
	struct frame *frame = malloc (sizeof (struct frame));
	if (frame == NULL) {
		return NULL;
	}

	/* frame 맴버에 값 대입 */
	frame->kva = palloc_get_page (PAL_USER);
	if (frame->kva == NULL) {
		free (frame);
		struct frame *victim = vm_evict_frame ();
        if (victim == NULL) {
            return NULL;
        }

        lock_acquire (&frame_lock);
        list_push_back (&frame_list, &victim->frame_elem);
        lock_release (&frame_lock);

		return victim; 
	}

	frame->page = NULL;

    lock_acquire (&frame_lock);
    list_push_back (&frame_list, &frame->frame_elem);
    lock_release (&frame_lock);

	ASSERT (frame != NULL);
	ASSERT (frame->page == NULL);
	return frame;
}

/* Handle the fault on write_protected page */
static bool
vm_handle_wp (struct page *page UNUSED) {
}

bool
vm_claim_if_valid (struct supplemental_page_table *spt, void *addr, void *rsp, bool write) {
	struct page *page = spt_find_page (spt, addr);
	if (page == NULL) {
		/* 스택인지, 늘릴 수 있는지 확인 */
		if (addr >= rsp - 8 && addr < (void *) USER_STACK && addr >= (void *) (USER_STACK - (MAXSTACK))) {
            return vm_stack_growth (addr);
        }
		return false;
	}

	/* 권한 검사 */
   	if (write && !page->writable) {
        return false;
	}

	if (page->frame != NULL) {
        return true;
	}

	return vm_do_claim_page (page);
}

static bool
vm_alloc_page_immediate (enum vm_type type, void *pg_addr, bool writable) {
	/* lazy load필요없으니 vm_alloc_page */
    if (!vm_alloc_page (type, pg_addr, writable)) {
        return false;
    }

	/* 즉시 할당 */
    if (!vm_claim_page (pg_addr)) {
        return false;
    }

	return true;
}

/* Growing the stack. */
static bool
vm_stack_growth (void *addr) {
    void *pg_addr = pg_round_down (addr);
	return vm_alloc_page_immediate (VM_ANON | VM_MARKER_0, pg_addr, true);
}

/* Return true on success */
bool
vm_try_handle_fault (struct intr_frame *f, void *addr, 
			bool user, bool write, bool not_present) {
	struct thread *curr = thread_current ();
	struct supplemental_page_table *spt= &curr->spt;

	/* 유효성 확인 */
	if (addr == NULL || is_kernel_vaddr (addr) || !not_present)
        return false;

	/* 유저모드인지 커널모드인지에 따라 rsp설정 */
	void *rsp = user ? f->rsp : curr->ursp;

	return vm_claim_if_valid(spt, addr, rsp, write);
}

/* Free the page.
 * DO NOT MODIFY THIS FUNCTION. */
void
vm_dealloc_page (struct page *page) {
	destroy (page);
	free (page);
}

/* Claim the page that allocate on VA. */
bool
vm_claim_page (void *va) {
	/* TODO: Fill this function */
	struct page *page = spt_find_page (&thread_current ()->spt, va);
	if(page == NULL) {
		return false;
	}
	return vm_do_claim_page (page);
}

/* Claim the PAGE and set up the mmu. */
/* 가상 페이지에 실제 물리 프레임을 연결 */
static bool
vm_do_claim_page (struct page *page) {
	struct frame *frame = vm_get_frame ();
    if (frame == NULL) {
        return false;
	}

	/* Set links */
	frame->page = page;
	page->frame = frame;

	/* pml4에 입력 */
	if (!pml4_set_page (thread_current ()->pml4, page->va, frame->kva, page->writable)) {
        return false;
	}

	return swap_in (page, frame->kva);
}

/* Initialize new supplemental page table */
void
supplemental_page_table_init (struct supplemental_page_table *spt) {
	hash_init(&spt->hash, page_hash, page_less, NULL);
}

static unsigned
page_hash (const struct hash_elem *e, void *aux UNUSED) {
    const struct page *p = hash_entry (e, struct page, hash_elem);
    return hash_bytes (&p->va, sizeof p->va);
}

static bool
page_less (const struct hash_elem *a, const struct hash_elem *b, void *aux UNUSED) {
    const struct page *p_a = hash_entry (a, struct page, hash_elem);
    const struct page *p_b = hash_entry (b, struct page, hash_elem);

    return p_a->va < p_b->va;
}

/* Copy supplemental page table from src to dst */
/*
	src의 supplemental page table 안에 있는 각 page를 순회하면서 
	dst의 supplemental page table에 정확한 복사본 entry를 만들어야 합니다. 
	uninit page를 할당하고 곧바로 claim해야 합니다.	
*/
bool
supplemental_page_table_copy (struct supplemental_page_table *dst,
		struct supplemental_page_table *src) {
    struct hash_iterator i;

    hash_first (&i, &src->hash);

	/* spt를 순회하며 page_copy */
    while (hash_next (&i)) {
        struct page *src_page = hash_entry (hash_cur(&i), struct page, hash_elem);
		bool success = false;

		/* uninit page */
		if (VM_TYPE (src_page->operations->type) == VM_UNINIT) {
            success = page_copy_uninit (dst, src_page);
        }
		/* initialized page */
        else {
            success = page_copy_initialized (dst, src_page);
        }

		if (!success){
			return false;
		}
    }
    return true;
}

/* uninit page를 copy할 때 */
static bool
page_copy_uninit (struct supplemental_page_table *dst,
		struct page *src_page) {
	struct segment_aux *src_aux = (struct segment_aux *) src_page->uninit.aux;

	/* aux 복사 */
	struct segment_aux *dst_aux = duplicate_segment_aux (src_aux);
	if (src_aux != NULL && dst_aux == NULL) {
    	return false;
	}
	
	/* 페이지 초기화 */
	if (!vm_alloc_page_with_initializer (src_page->uninit.type, src_page->va,
				src_page->writable, src_page->uninit.init, dst_aux)) {
		free (dst_aux);
		return false;
	}
	return true;
}

/* 부모의 aux가 존재하면 자식 aux에 대입*/
static struct segment_aux *
duplicate_segment_aux (struct segment_aux *src_aux) {
    if (src_aux == NULL) return NULL;

    struct segment_aux *dst_aux = malloc (sizeof (*dst_aux));
    if (dst_aux == NULL) return NULL;
    
    *dst_aux = *src_aux;

	/* file이 존재하는 page면 파일도 한번 더 열려야 하니 file_duplicate */
    if (src_aux->file != NULL) {
        sys_file_lock_acquire ();
        dst_aux->file = file_duplicate (src_aux->file);
        sys_file_lock_release ();
        
        if (dst_aux->file == NULL) {
            free (dst_aux);
            return NULL;
        }
    }
    return dst_aux;
}

/* uninit page말고 copy할 때 */
static bool
page_copy_initialized (struct supplemental_page_table *dst,
		struct page *src_page) {
	if (!vm_alloc_page_immediate (src_page->operations->type, src_page->va, src_page->writable)) {
		return false;
	}

	/* 위에서 spt에 넣은 dst_page 탐색 */
	struct page *dst_page = spt_find_page (dst, src_page->va);
	if (dst_page == NULL)
        return false;

    /* VM_FILE 일 때 file_page맴버 복사 */
    if (VM_TYPE(src_page->operations->type) == VM_FILE) {
        if (!duplicate_file_page_meta (dst_page, src_page)) {
            return false;
        }
    }

    /* 부모가 ram에 있으면 */
    if (src_page->frame != NULL) {
		/* 물리 frame 데이터 복사 */
        memcpy (dst_page->frame->kva, src_page->frame->kva, PGSIZE);
    } 
	/* 부모가 ram에 없으면 */
    else {
        if (VM_TYPE(src_page->operations->type) == VM_ANON) {
            anon_read_from_swap (src_page->anon.swap_index, dst_page->frame->kva);
        }
		else if (VM_TYPE(src_page->operations->type) == VM_FILE) {
            sys_file_lock_acquire ();
            file_read_at (dst_page->file.file, dst_page->frame->kva, dst_page->file.read_bytes, dst_page->file.offset);
            sys_file_lock_release ();

            memset (dst_page->frame->kva + dst_page->file.read_bytes, 0, PGSIZE - dst_page->file.read_bytes);
        }
    }

	return true;
}

/* 부모의 file_page맴버 복사 */
static bool
duplicate_file_page_meta (struct page *dst_page, struct page *src_page) {
    sys_file_lock_acquire ();
    dst_page->file.file = file_duplicate (src_page->file.file);
    sys_file_lock_release ();

    if (dst_page->file.file == NULL) {
        return false;
    }

    dst_page->file.offset = src_page->file.offset;
    dst_page->file.read_bytes = src_page->file.read_bytes;
    dst_page->file.num_page = src_page->file.num_page;

    return true;
}

/* Free the resource hold by the supplemental page table */
void
supplemental_page_table_kill (struct supplemental_page_table *spt) {
	/* TODO: Destroy all the supplemental_page_table hold by thread and
	 * TODO: writeback all the modified contents to the storage. */
    hash_clear (&spt->hash, destructor);
}

static void destructor (struct hash_elem *e, void *aux UNUSED) {
    struct page *page = hash_entry (e, struct page, hash_elem);
    vm_dealloc_page (page);
}

void
vm_remove_frame_from_table (struct frame *frame) {
    lock_acquire (&frame_lock);
    
    /* curr_frame_elem가 삭제할 프레임이면 이동 */
    if (curr_frame_elem == &frame->frame_elem) {
        curr_frame_elem = list_next_circular (curr_frame_elem, &frame_list);
        if (curr_frame_elem == &frame->frame_elem) {
            curr_frame_elem = NULL;
        }
    }
    
    list_remove (&frame->frame_elem);
    lock_release (&frame_lock);
}