/* vm.c: Generic interface for virtual memory objects. */

#include <list.h>
#include "threads/mmu.h"
#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include <hash.h>
#include "threads/vaddr.h"

/* Initializes the virtual memory subsystem by invoking each subsystem's
 * intialize codes. */
void
vm_init (void) {
	vm_anon_init ();
	vm_file_init ();
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
static struct frame *vm_get_victim (void);
static bool vm_do_claim_page (struct page *page);
static struct frame *vm_evict_frame (void);
static unsigned page_hash (const struct hash_elem *e, void *aux UNUSED);
static bool page_less (const struct hash_elem *a, const struct hash_elem *b, void *aux UNUSED);
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
		/* TODO: Create the page, fetch the initialier according to the VM type,
		 * TODO: and then create "uninit" page struct by calling uninit_new. You
		 * TODO: should modify the field after calling the uninit_new. */
		/* TODO: Insert the page into the spt. */
		struct page *page = malloc(sizeof(*page));
		if (page == NULL)
			goto err;

		/* page type에 따라 다른 initializer 대입 */
		switch (VM_TYPE (type)) {
		case VM_ANON:
			uninit_new (page, upage, init, type, aux, anon_initializer);
			break;
		case VM_FILE:
			uninit_new (page, upage, init, type, aux, file_backed_initializer);
			break;
		default:
			free (page);
			goto err;
		}
		page->writable = writable;

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
	vm_dealloc_page (page);
}

/* Get the struct frame, that will be evicted. */
static struct frame *
vm_get_victim (void) {
	struct frame *victim = NULL;
	 /* TODO: The policy for eviction is up to you. */

	return victim;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *
vm_evict_frame (void) {
	struct frame *victim UNUSED = vm_get_victim ();
	/* TODO: swap out the victim and return the evicted frame. */

	return NULL;
}

/* palloc() and get frame. If there is no available page, evict the page
 * and return it. This always return valid address. That is, if the user pool
 * memory is full, this function evicts the frame to get the available memory
 * space.*/
static struct frame *
vm_get_frame (void) {
	struct frame *frame = NULL;
	/* TODO: Fill this function. */
	frame = malloc (sizeof (struct frame));
	if (frame == NULL)
		return NULL;

	frame->kva = palloc_get_page (PAL_USER);
	if (frame->kva == NULL) 
		PANIC ("todo");

	frame->page = NULL;

	ASSERT (frame != NULL);
	ASSERT (frame->page == NULL);
	return frame;
}

/* Growing the stack. */
static void
vm_stack_growth (void *addr UNUSED) {
}

/* Handle the fault on write_protected page */
static bool
vm_handle_wp (struct page *page UNUSED) {
}

/* Return true on success */
bool
vm_try_handle_fault (struct intr_frame *f, void *addr, 
			bool user, bool write, bool not_present) {
	struct supplemental_page_table *spt= &thread_current ()->spt;

	/* 유효성 확인 //이걸로 되나??? */
	if (addr == NULL || is_kernel_vaddr (addr) || !not_present)
        return false;

	/* spt에 있는지 확인 */
	struct page *page = spt_find_page (spt, addr);
	if (page == NULL) {
		return false;
		/* 스택인지확인 */
		if (true) {
			PANIC("스택늘리기");
		}

		return false;
	}

   if (write && !page->writable){
        return false;
   }
	return vm_do_claim_page (page);
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
	if(frame == NULL) {
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

/* uninit page를 copy할 때 */
bool
page_copy_uninit (struct supplemental_page_table *dst,
		struct page *src_page) {
	struct segment_aux *src_aux = (struct segment_aux *) src_page->uninit.aux;
	struct segment_aux *dst_aux = NULL;

	/* 부모의 aux가 존재하면 자식 aux에 대입*/
	if (src_aux != NULL) {
        dst_aux = malloc (sizeof (*dst_aux));
        if (dst_aux == NULL)
            return false;
        *dst_aux = *src_aux;

		/* file이 존재하는 page면 파일도 한번 더 열려야 하니 file_duplicate */
		if (src_aux->file != NULL) {
            dst_aux->file = file_duplicate (src_aux->file);
            if (dst_aux->file == NULL) {
                free (dst_aux);
                return false;
            }
        }
    }

	/* 페이지 초기화 */
	if (!vm_alloc_page_with_initializer (src_page->uninit.type, src_page->va,
				src_page->writable, src_page->uninit.init, dst_aux)) {
		free (dst_aux);
		return false;
	}
	return true;
}

/* uninit page말고 copy할 때 */
bool
page_copy_initialized (struct supplemental_page_table *dst,
		struct page *src_page) {
	/* lazy load필요없으니 vm_alloc_page */
	if (!vm_alloc_page (src_page->operations->type, src_page->va, src_page->writable))
		return false;

	/* 즉시 할당 */
	if (!vm_claim_page (src_page->va))
		return false;

	/* page도 복사 */
	struct page *dst_page = spt_find_page (dst, src_page->va);
	if (dst_page == NULL)
        return false;

	memcpy (dst_page->frame->kva, src_page->frame->kva, PGSIZE);
	return true;
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
        
		bool success = (src_page->frame == NULL) ? page_copy_uninit (dst, src_page)
												 : page_copy_ununinit (dst, src_page);

		if (!success){
			return false;
		}
    }
    return true;
}
static void destructor (struct hash_elem *e, void *aux UNUSED) {
    struct page *page = hash_entry (e, struct page, hash_elem);
	/* free (aux) 생각해보기 */
    vm_dealloc_page (page);
}

/* Free the resource hold by the supplemental page table */
void
supplemental_page_table_kill (struct supplemental_page_table *spt) {
	/* TODO: Destroy all the supplemental_page_table hold by thread and
	* TODO: writeback all the modified contents to the storage. */
    hash_clear (&spt->hash, destructor);
}
