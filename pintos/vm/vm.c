/* vm.c: Generic interface for virtual memory objects. */

#include <list.h>
#include <hash.h>
#include <string.h>

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
static struct frame * vm_scan_victim (void);
static bool vm_frame_is_accessed (struct frame *frame);
static struct frame *vm_evict_frame (void);
static struct frame *vm_get_frame (void);
static bool vm_handle_wp (struct page *page UNUSED);
static bool vm_stack_growth (void *addr);
static bool vm_do_claim_page (struct page *page);
static uint64_t page_hash (const struct hash_elem *e, void *aux UNUSED);
static bool page_less (const struct hash_elem *a, const struct hash_elem *b, void *aux UNUSED);
static bool page_copy_uninit (struct page *src_page);
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

	if (upage == NULL || !is_user_vaddr(upage)|| pg_ofs (upage) != 0)	//코드영역 아래랑 커널베이스~스택 사이는?
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

	if (&((&round_down_page)->hash_elem) == NULL)
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

    /* 현제 frame_elem이 NULL이거나 끝이면 */
    if (curr_frame_elem == NULL || curr_frame_elem == list_end (&frame_list)) {
        curr_frame_elem = list_begin (&frame_list);
    }

	victim = vm_scan_victim ();

    if (victim != NULL) {
        list_remove (&victim->frame_elem);
    }

    lock_release (&frame_lock);
    return victim;
}

/* accessed bit가 0인 프레임 탐색 */
static struct frame *
vm_scan_victim (void) {
	/* frame_list * 2 만큼 순회 */
    size_t loop_count = list_size (&frame_list) * 2;
    
    for (size_t i = 0; i < loop_count; i++) {
        struct frame *curr_frame = list_entry (curr_frame_elem, struct frame, frame_elem);

        curr_frame_elem = list_next_circular (curr_frame_elem, &frame_list);

		/* 비어 있거나 eviction 금지 상태면 후보에서 제외 */
        if (curr_frame->page == NULL || curr_frame->no_evict) {
            continue;
        }

        if (!vm_frame_is_accessed (curr_frame)) {
            return curr_frame;
        }
    }

    return NULL;
}

/* frame의 accessed bit가 1이면 0으로 바꾸고 true 반환 */
static bool
vm_frame_is_accessed (struct frame *frame) {
    bool accessed = false;

    struct list_elem *e;
    for (e = list_begin (&frame->page_list); e != list_end (&frame->page_list); e = list_next (e)) {
        struct page *p = list_entry (e, struct page, cow_elem);
        uint64_t *pml4 = p->p_thread->pml4;

        if (pml4_is_accessed (pml4, p->va)) {
            pml4_set_accessed (pml4, p->va, false);
            accessed = true;
        }
    }
    
    return accessed;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *
vm_evict_frame (void) {
    struct frame *victim = vm_get_victim ();
    if (victim == NULL) {
        return NULL;
    }

    /* cow로 여러 페이지가 공유중이어도 페이지 하나만 swap out */
    struct page *front_page = list_entry (list_front (&victim->page_list), struct page, cow_elem);
    if (!swap_out (front_page)) {
        return NULL; 
    }

    /* 연결 다 끊고, front_page 스왑 위치 공유 (lock필요) */
    lock_acquire (&frame_lock);
    while (!list_empty (&victim->page_list)) {
        struct list_elem *e = list_pop_front (&victim->page_list);
        struct page *page = list_entry (e, struct page, cow_elem);
        
        pml4_clear_page (page->p_thread->pml4, page->va);
        
        /* list 순회하면서 page수 만큼 swap_ref_inc*/
        if (page != front_page && VM_TYPE(page->operations->type) == VM_ANON) {
            page->anon.swap_index = front_page->anon.swap_index;
            swap_ref_inc (page->anon.swap_index);
        }
        
        page->frame = NULL;
    }
    
    list_init (&victim->page_list);
    victim->page = NULL;
    lock_release (&frame_lock);

    return victim;
}

/* palloc() and get frame. If there is no available page, evict the page
 * and return it. This always return valid address. That is, if the user pool
 * memory is full, this function evicts the frame to get the available memory
 * space.*/
static struct frame *
vm_get_frame (void) {
	/* TODO: Fill this function. */
    void *kva = palloc_get_page (PAL_USER);
    struct frame *frame = NULL;

    /* palloc실패 => evict */
    if (kva == NULL) {
        frame = vm_evict_frame ();
        if (frame == NULL) {
            return NULL;
        }
    } 
    /* palloc성공 => frame할당, 초기화 */
    else {
        frame = malloc (sizeof (*frame));
        if (frame == NULL) {
            palloc_free_page (kva);
            return NULL;
        }
        frame->kva = kva;
        frame->page = NULL;
    }

    lock_acquire (&frame_lock);
    list_push_back (&frame_list, &frame->frame_elem);
    lock_release (&frame_lock);

    list_init (&frame->page_list);

    /* 작업 끝날 때 까지 evict 후보에서 제외 */
    frame->no_evict = true;

    return frame;
}

/* lock_acquire (&frame_lock)로 더이상 evict못하게 하고
 * 그 전에 evict 됐으면 그냥 나가서 not present fault로 다시 swap in 시켜야 함
 * 공유중 아니면 pml4_set_page로 writable = true 
 * vm_get_frame ()하기 전엔 lock_release (&frame_lock)해야 하니 
 * share_frame->no_evict = true 로 evict방어
 * vm_get_frame ()위아래로 release했다가 acquire하는 사이에 지워질 수도 있어
 * 한번더 검사 후 마저 진행 */


/* Handle the fault on write_protected page */
static bool
vm_handle_wp (struct page *page) {
    struct frame *share_frame = page->frame;
    struct frame *private_frame = NULL;

    lock_acquire (&frame_lock);

    /* fault나서 여기왔는데 그 사이 evict됐으면 나가서 not present fault로 다시 swap in */
    if (page->frame != share_frame) {
        lock_release (&frame_lock);
        return true;
    }

    /* 공유 중 아니면 writable = true */
    if (list_size (&share_frame->page_list) <= 1) {
        pml4_set_page (thread_current ()->pml4, page->va, share_frame->kva, true);
        lock_release (&frame_lock);
        return true;
    }

    /* vm_get_frame()대비 evict방어 */
    share_frame->no_evict = true;
    lock_release (&frame_lock);

    private_frame = vm_get_frame ();
    if (private_frame == NULL) {
        lock_acquire (&frame_lock);
        share_frame->no_evict = false;
        lock_release (&frame_lock);
        return false;
    }

    lock_acquire (&frame_lock);
 
    /* 한번더 검사 후 마저 진행 */
    if (page->frame != share_frame) {
        share_frame->no_evict = false;
        lock_release (&frame_lock);
        vm_remove_frame_from_table (private_frame);
        palloc_free_page (private_frame->kva);
        free (private_frame);
        return true;
    }

    memcpy (private_frame->kva, share_frame->kva, PGSIZE);
    list_remove (&page->cow_elem);

    page->frame = private_frame;
    private_frame->page = page;
    list_push_back (&private_frame->page_list, &page->cow_elem);

    share_frame->no_evict = false;
    lock_release (&frame_lock);

    /* pml4에 frame이랑 writable 변경*/
    pml4_set_page (thread_current ()->pml4, page->va, private_frame->kva, true);
    private_frame->no_evict = false;

    return true;
}

bool
vm_claim_if_valid (struct page *page, void *addr, void *rsp, bool write) {
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

    if (addr == NULL || is_kernel_vaddr (addr)) {
        return false;
	}

	struct page *page = spt_find_page (spt, addr);

    /* ram에 있는 주소에서의 fault면 */
	if (!not_present) {
		/* writable에 쓰려고 하면 */
		if (page != NULL && page->writable && write) {
			return vm_handle_wp (page);
		}
    	return false;
	}

	/* 유저모드인지 커널모드인지에 따라 rsp설정 */
	void *rsp = (void *)(user ? f->rsp : curr->ursp);

	return vm_claim_if_valid(page, addr, rsp, write);
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

	/* frame<->page 연결 */
	frame->page = page;
	page->frame = frame;

    /* 내용 채우기 */
    if (!swap_in (page, frame->kva)) {
        frame->page = NULL;
        page->frame = NULL;
        vm_remove_frame_from_table (frame);
        palloc_free_page (frame->kva);
        free (frame);
        return false;
    }

	/* pml4에 입력 */
	if (!pml4_set_page (thread_current ()->pml4, page->va, frame->kva, page->writable)) {
        frame->page = NULL;
        page->frame = NULL;
        vm_remove_frame_from_table (frame);
        palloc_free_page (frame->kva);
        free (frame);
        return false;
	}

    /* 할거 다 끝나면 evict가능 */
    list_push_back (&frame->page_list, &page->cow_elem);
    frame->no_evict = false;

    return true;
}

/* Initialize new supplemental page table */
void
supplemental_page_table_init (struct supplemental_page_table *spt) {
	hash_init(&spt->hash, page_hash, page_less, NULL);
}

static uint64_t
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
            success = page_copy_uninit (src_page);
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
page_copy_uninit (struct page *src_page) {
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
    if (!vm_alloc_page (src_page->operations->type, src_page->va, src_page->writable)) {
        return false;
    }

	/* 방금 spt에 넣은 dst_page 탐색 */
    struct page *dst_page = spt_find_page (dst, src_page->va);
    if (dst_page == NULL) {
		return false;
	}

	/* VM_FILE 이면 file_page로 바꾸고 file_page맴버 복사 */
    if (VM_TYPE(src_page->operations->type) == VM_FILE) {
        file_backed_initializer (dst_page, VM_FILE, NULL);
        if (!duplicate_file_page_meta (dst_page, src_page)) {
            return false;
        }
    }

    /* 부모가 ram에 있으면 cow */
    if (src_page->frame != NULL) {
        /* pml4수정해서 dirty날아가기 전에 write back */
        if (VM_TYPE(src_page->operations->type) == VM_FILE && pml4_is_dirty (src_page->p_thread->pml4, src_page->va)) {
            sys_file_lock_acquire ();
            file_write_at (src_page->file.file, src_page->frame->kva, src_page->file.read_bytes, src_page->file.offset);
            sys_file_lock_release ();
            
            pml4_set_dirty (src_page->p_thread->pml4, src_page->va, false);
        }

        /* VM_ANON이면 미리 anon_page로 변경 */
        if (VM_TYPE(src_page->operations->type) == VM_ANON) {
            anon_initializer (dst_page, VM_ANON, NULL);
        }

		lock_acquire (&frame_lock);
		list_push_back (&src_page->frame->page_list, &dst_page->cow_elem);
		dst_page->frame = src_page->frame;
		lock_release (&frame_lock);

        /* pml4에 writable false로 set */
        pml4_set_page (thread_current ()->pml4, dst_page->va, dst_page->frame->kva, false);
        pml4_set_page (src_page->p_thread->pml4, src_page->va, src_page->frame->kva, false);
    } 
	/* 부모가 ram에 없으면 */
    else {
        if (!vm_do_claim_page (dst_page)) {
			return false;
		}
        
        /* anon이면 부모의 스왑 데이터 복사 */
        if (VM_TYPE(src_page->operations->type) == VM_ANON) {
            anon_read_from_swap (src_page->anon.swap_index, dst_page->frame->kva);
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