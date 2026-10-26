/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include "threads/mmu.h"

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

/* Returns a hash value for page p. */
/* Ljw_01 */
unsigned
page_hash (const struct hash_elem *p_, void *aux UNUSED) {
  const struct page *p = hash_entry (p_, struct page, hash_elem);
  return hash_bytes (&p->va, sizeof p->va);
}

/* Returns true if page a precedes page b. */
/* Ljw_01 */
bool
page_less (const struct hash_elem *a_,
           const struct hash_elem *b_, void *aux UNUSED) {
  const struct page *a = hash_entry (a_, struct page, hash_elem);
  const struct page *b = hash_entry (b_, struct page, hash_elem);

  return a->va < b->va;
}
/* Create the pending page object with initializer. If you want to create a
 * page, do not create it directly and make it through this function or
 * `vm_alloc_page`. */
bool
vm_alloc_page_with_initializer (enum vm_type type, void *upage, bool writable,
		vm_initializer *init, void *aux) {

	ASSERT (VM_TYPE(type) != VM_UNINIT)

	struct supplemental_page_table *spt = &thread_current ()->spt;

	/* Check wheter the upage is already occupied or not. */
	if (spt_find_page (spt, upage) == NULL) {
		/* TODO: Create the page, fetch the initialier according to the VM type,
		 * TODO: and then create "uninit" page struct by calling uninit_new. You
		 * TODO: should modify the field after calling the uninit_new. */

		/* TODO: Insert the page into the spt. */
	}
err:
	return false;
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
/* Ljw_01 */
struct page *
spt_find_page (struct supplemental_page_table *spt, void *va) {
	struct page *page;
	page->va = pg_round_down (va);
	if (page == NULL || &page->hash_elem == NULL)
		return NULL;

	struct hash_elem *elem = hash_find (spt->hash, &page->hash_elem);
	if (elem == NULL)
		return NULL;

	page = hash_entry (elem, struct page, hash_elem);
	return page;
}

/* Insert PAGE into spt with validation. */
/* Ljw_01 */
bool
spt_insert_page (struct supplemental_page_table *spt ,
		struct page *page ) {
	int succ = false;
	/* TODO: Fill this function. */
	/* 이미 spt에 page가 있다면 false 반환 */
	if (page == spt_find_page (spt, page->va))
		return succ;

	/* hash에 hash_elem 삽입. 나중에 hash_elem을 통해 페이지 찾기 */
	hash_insert (spt->hash, &page->hash_elem);
	succ = true;

	return succ;
}

void
spt_remove_page (struct supplemental_page_table *spt, struct page *page) {
	vm_dealloc_page (page);
	return true;
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
/* Ljw_01 */
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
	struct supplemental_page_table *spt = &thread_current ()->spt;
	struct page *page = NULL;
	/* TODO: Validate the fault */
	/* TODO: Your code goes here */

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
/* Ljw_01 */
bool
vm_claim_page (void *va) {
	struct page *page = NULL;
	/* TODO: Fill this function */		
	struct thread *curr = thread_current ();
	if (page == NULL)
		return false;

	page = spt_find_page (&curr->spt, va);
	return vm_do_claim_page (page);
}

/* Claim the PAGE and set up the mmu. */
/* 가상 페이지에 실제 물리 프레임을 연결 */
/* Ljw_01 */
static bool
vm_do_claim_page (struct page *page) {
	struct frame *frame = vm_get_frame ();
	if (frame == NULL)
		return false;

	/* Set links */
	frame->page = page;
	page->frame = frame;

	/* TODO: Insert page table entry to map page's VA to frame's PA. */
	uint64_t *pml4 = thread_current ()->pml4;	
	bool success = pml4_set_page (pml4, page->va, frame->kva, page->writable);

	if (!success)
		return false;

	return swap_in (page, frame->kva);
}

/* Initialize new supplemental page table */
/* Ljw_01 */
void
supplemental_page_table_init (struct supplemental_page_table *spt) {
	hash_init (spt->hash, page_hash, page_less, NULL);
}

/* Copy supplemental page table from src to dst */
bool
supplemental_page_table_copy (struct supplemental_page_table *dst UNUSED,
		struct supplemental_page_table *src UNUSED) {
}

/* Free the resource hold by the supplemental page table */
void
supplemental_page_table_kill (struct supplemental_page_table *spt UNUSED) {
	/* TODO: Destroy all the supplemental_page_table hold by thread and
	 * TODO: writeback all the modified contents to the storage. */
}
