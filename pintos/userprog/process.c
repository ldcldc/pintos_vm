#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#include "threads/malloc.h"

#include "userprog/syscall.h"
#ifdef VM
#include "vm/vm.h"
#endif

#define STDIN_FILE ((struct file *) 1)
#define STDOUT_FILE ((struct file *) 2)

static void process_cleanup (void);
static bool load (const char *file_name, struct intr_frame *if_);
static void initd (void *f_name);
static void __do_fork (void *);

/* 자식에게 넘겨줄 data 구조체 */
struct parent_data {
	struct thread *parent;
	struct intr_frame *if_;
};

/* General process initializer for initd and other process. */
static void
process_init (void) {
	//struct thread *current = thread_current ();
}

/* Starts the first userland program, called "initd", loaded from FILE_NAME.
 * The new thread may be scheduled (and may even exit)
 * before process_create_initd() returns. Returns the initd's
 * thread id, or TID_ERROR if the thread cannot be created.
 * Notice that THIS SHOULD BE CALLED ONCE. */
tid_t
process_create_initd (const char *file_name) {
	char *fn_copy;
	tid_t tid;

	/* Make a copy of FILE_NAME.
	 * Otherwise there's a race between the caller and load(). */
	fn_copy = palloc_get_page (0);
	if (fn_copy == NULL)
		return TID_ERROR;
	strlcpy (fn_copy, file_name, PGSIZE);
	/* 입력값에서 파일 이름만 잘라서 thread_create */
	char file_name_copy[128]; 
	strlcpy(file_name_copy, file_name, sizeof(file_name_copy));

	char *save_ptr;
	char *parsed_file_name = strtok_r (file_name_copy, " ", &save_ptr); 

	/* Create a new thread to execute FILE_NAME. */
	tid = thread_create (parsed_file_name, PRI_DEFAULT, initd, fn_copy);
	if (tid == TID_ERROR)
		palloc_free_page (fn_copy);
	return tid;
}

/* A thread function that launches first user process. */
static void
initd (void *f_name) {
#ifdef VM
	supplemental_page_table_init (&thread_current ()->spt);
#endif

	process_init ();

	if (process_exec (f_name) < 0)
		PANIC("Fail to launch initd\n");
	NOT_REACHED ();
}




/* Clones the current process as `name`. Returns the new process's thread id, or
 * 실패 시 TID_ERROR 반환 */
tid_t
process_fork (const char *name, struct intr_frame *if_) {
    struct thread *curr = thread_current ();
    
    struct parent_data p; 
    p.parent = curr;
    p.if_ = if_;

    tid_t tid = thread_create (name, PRI_DEFAULT, __do_fork, &p);
    if (tid == TID_ERROR) return TID_ERROR;

	/* tid로 thread_create확인 하면서 child_status 추출 */
    struct child_status *child_status = NULL;
    for (struct list_elem *e = list_begin (&curr->children); e != list_end (&curr->children); e = list_next (e)) {
        struct child_status *s = list_entry (e, struct child_status, elem);
        if (s->tid == tid) {
            child_status = s;
            break;
        }
    }
    if (child_status == NULL) return TID_ERROR;
	/* 자식 __do_fork 대기 */
    sema_down (&child_status->fork_sema);
        
    if (!child_status->fork_success) {
        return TID_ERROR;
    }

    return tid;
}

#ifndef VM
/* Duplicate the parent's address space by passing this function to the
 * pml4_for_each. This is only for the project 2. */
static bool
duplicate_pte (uint64_t *pte, void *va, void *aux) {
	struct thread *current = thread_current ();
	struct thread *parent = (struct thread *) aux;
	void *parent_page;
	void *newpage;
	bool writable;

	/* 1. TODO: If the parent_page is kernel page, then return immediately. */
    if (is_kernel_vaddr (va)){
        return true;
	}

	/* 2. Resolve VA from the parent's page map level 4. */
	parent_page = pml4_get_page (parent->pml4, va);
    if (parent_page == NULL)
        return true;

	/* 3. TODO: Allocate new PAL_USER page for the child and set result to
	 *    TODO: NEWPAGE. */
    newpage = palloc_get_page (PAL_USER);
    if (newpage == NULL)
        return false;
	/* 4. TODO: Duplicate parent's page to the new page and
	 *    TODO: check whether parent's page is writable or not (set WRITABLE
	 *    TODO: according to the result). */
    memcpy (newpage, parent_page, PGSIZE);
	/* 부모 writable 확인 */
	writable = is_writable (pte);

	/* 5. Add new page to child's page table at address VA with WRITABLE
	 *    permission. */
	if (!pml4_set_page (current->pml4, va, newpage, writable)) {
		/* 6. TODO: if fail to insert page, do error handling. */
        palloc_free_page (newpage);
        return false;
	}
	return true;
}
#endif

/* A thread function that copies parent's execution context.
 * Hint) parent->tf does not hold the userland context of the process.
 *       That is, you are required to pass second argument of process_fork to
 *       this function. */
static void
__do_fork (void *aux) {
	struct intr_frame if_;
	struct thread *current = thread_current ();

    struct parent_data *p = (struct parent_data *) aux;
    struct thread *parent = p->parent;
    struct intr_frame *parent_if = p->if_;

	bool succ = true;

	/* 1. Read the cpu context to local stack. */
	memcpy (&if_, parent_if, sizeof (struct intr_frame));

	/* 자식 rax = 0 */
    if_.R.rax = 0;
	/* 2. Duplicate PT */
	current->pml4 = pml4_create();
	if (current->pml4 == NULL)
		goto error;

	process_activate (current);
#ifdef VM
	supplemental_page_table_init (&current->spt);
	if (!supplemental_page_table_copy (&current->spt, &parent->spt))
		goto error;
#else
	if (!pml4_for_each (parent->pml4, duplicate_pte, parent))
		goto error;
#endif

	/* 부모 parent->file_list 복사 */
    if (!sys_copy_file_list (current, parent)) {
        goto error;
    }

	process_init ();

    /* fork_success = true 후 부모 대기용 sema_up */
    current->child_status->fork_success = true;
    sema_up (&current->child_status->fork_sema);

	/* Finally, switch to the newly created process. */
	if (succ)
		do_iret (&if_);
error:

    /* fork_success = true 후 부모 대기용 sema_up */
    current->child_status->fork_success = false;
    sema_up (&current->child_status->fork_sema);
	thread_exit ();
}

/* Switch the current execution context to the f_name.
 * Returns -1 on fail. */
int
process_exec (void *f_name) {
	char *file_name = f_name;
	bool success;
	/* 입력값 너무 길면 return -1 */
	if (strlen(file_name) > 128) {
        palloc_free_page (f_name);
        return -1;
    }

	/* We cannot use the intr_frame in the thread structure.
	 * This is because when current thread rescheduled,
	 * it stores the execution information to the member. */
	struct intr_frame _if;
	_if.ds = _if.es = _if.ss = SEL_UDSEG;
	_if.cs = SEL_UCSEG;
	_if.eflags = FLAG_IF | FLAG_MBS;

	char *argv[64];
	int argc = 0;

	char *save_ptr;    /* strtok_r이 자르다 어디서 멈췄는지 기억 */
	char *token = strtok_r(file_name, " ", &save_ptr);

	while (token != NULL) {
		argv[argc] = token;    /* 저장 */
		token = strtok_r(NULL, " ", &save_ptr);    /* 다음단어로 덮어씀 */
		argc++;
	}
	if (argc == 0) {
		palloc_free_page (file_name);
		return -1;
    }

	struct thread *curr = thread_current ();
    if (curr->running_file != NULL) {
        sys_file_lock_acquire();
        file_allow_write(curr->running_file); /* 쓰기 금지 해제 */
        file_close(curr->running_file);       /* 이전 파일 닫기 */
        sys_file_lock_release();
        curr->running_file = NULL;
    }

	/* We first kill the current context */
	process_cleanup ();

	/* And then load the binary */
	success = load (argv[0], &_if);

	if (success == true) {
		/* argc 전달 */
		_if.R.rdi = argc;  
		/* 문자열을 뒤에서부터 스택에 복사 */
		for (int i = argc - 1; i >= 0; i--) {
			size_t len = strlen (argv[i]) + 1;
			_if.rsp -= len;
			memcpy ((void *) _if.rsp, argv[i], len);
			argv[i] = (char *) _if.rsp;
		}
	
		/* rsp를 8의 배수로 */
		while (_if.rsp % 8 != 0) {
			_if.rsp -= 1;
			*(uint8_t *) _if.rsp = 0;
		}
	
		/* argv[argc] = NULL */
		_if.rsp -= 8;
		*(uint64_t *) _if.rsp = 0;
	
		/* 포인터 배열 argv[argc-1] ~ argv[0] 순서 */
		for (int i = argc - 1; i >= 0; i--) {
			_if.rsp -= 8;
			*(uint64_t *) _if.rsp = (uint64_t) argv[i];
		}

		_if.R.rsi = _if.rsp;
		_if.rsp -= 8;
		*(uint64_t *) _if.rsp = 0;
	}

	/* If load failed, quit. */
	palloc_free_page (file_name);
	if (!success)
		return -1;

	/* Start switched process. */
	do_iret (&_if);
	NOT_REACHED ();
}


/* Waits for thread TID to die and returns its exit status.  If
 * it was terminated by the kernel (i.e. killed due to an
 * exception), returns -1.  If TID is invalid or if it was not a
 * child of the calling process, or if process_wait() has already
 * been successfully called for the given TID, returns -1
 * immediately, without waiting.
 *
 * This function will be implemented in problem 2-2.  For now, it
 * does nothing. */
int
process_wait (tid_t child_tid) {
	struct thread *curr = thread_current ();
    struct child_status *child_status = NULL;
    struct list_elem *e;

    /* 자식 리스트에서 tid 검색 */
    for (e = list_begin (&curr->children); e != list_end (&curr->children); e = list_next (e)) {
        struct child_status *s = list_entry (e, struct child_status, elem);
        if (s->tid == child_tid) {
            child_status = s;
            break;
        }
    }
	/* 내 자식 아니면 return -1 */
	if (child_status == NULL) {
		return -1;
	}
	/* 자식 process_exit 대기 */
	sema_down (&child_status->wait_sema);
	/* 자식 정보 읽어오고 process_exit 완료 허가 (sema_up) */
	int status = child_status->exit_status;
	list_remove (&child_status->elem);
	/* 자식 리스트에서 지우면서 ref_cnt-- */
    enum intr_level old_level = intr_disable ();
	child_status->ref_cnt--;

    /* 자식도 exit했으면 free (child_status) */
    if (child_status->ref_cnt == 0) {
        free (child_status);
    }
	intr_set_level (old_level); 

	return status;
}

void
process_exit (void) {
	struct thread *curr = thread_current ();

    /* 파일 쓰기 잠금 해제 */
    if (curr->running_file != NULL) {
		sys_file_lock_acquire ();
        file_close (curr->running_file);
		sys_file_lock_release ();
    }

	/* 열었던 파일 free (중복 포인터 NULL) */
    if (curr->file_list != NULL) {

        for (int i = 0; i < curr->fd_capacity; i++) {
            struct file *f = curr->file_list[i];

            if (f != NULL && f != STDIN_FILE && f != STDOUT_FILE) {
				sys_file_lock_acquire ();
                file_close (f);
				sys_file_lock_release ();

                /* (중복 포인터 NULL) */
                for (int j = i + 1; j < curr->fd_capacity; j++) {
                    if (curr->file_list[j] == f) {
                        curr->file_list[j] = NULL;
                    }
                }
            }
            curr->file_list[i] = NULL; /* 자기 자신도 NULL로 초기화 */
        }

        free (curr->file_list);
        curr->file_list = NULL;
    }

	if (curr->pml4 != NULL) {
        printf ("%s: exit(%d)\n", curr->name, curr->exit_status);
    }

	/* 자식 리스트 비우면서 ref_cnt-- */
    while (!list_empty (&curr->children)) {
        struct list_elem *e = list_pop_front (&curr->children);
        struct child_status *child_status = list_entry (e, struct child_status, elem);

        enum intr_level old_level = intr_disable ();
        child_status->ref_cnt--;
        /* 자식도 exit했으면 free (child_status) */
        if (child_status->ref_cnt == 0) {
            free (child_status);
        }
		intr_set_level (old_level); 
    }

	process_cleanup (); 

    if (curr->child_status != NULL) {
        curr->child_status->exit_status = curr->exit_status;
        /* wait중인 부모 sema_up */
        sema_up (&curr->child_status->wait_sema);
        
        /* ref_cnt-- */
		enum intr_level old_level = intr_disable ();
        curr->child_status->ref_cnt--;
        /* 부모도 exit했으면 free (child_status) */
        if (curr->child_status->ref_cnt == 0) {
            free (curr->child_status);
        }
		intr_set_level (old_level); 
    }
}

/* Free the current process's resources. */
static void
process_cleanup (void) {
	struct thread *curr = thread_current ();

#ifdef VM
	supplemental_page_table_kill (&curr->spt);
#endif

	uint64_t *pml4;
	/* Destroy the current process's page directory and switch back
	 * to the kernel-only page directory. */
	pml4 = curr->pml4;
	if (pml4 != NULL) {
		/* Correct ordering here is crucial.  We must set
		 * cur->pagedir to NULL before switching page directories,
		 * so that a timer interrupt can't switch back to the
		 * process page directory.  We must activate the base page
		 * directory before destroying the process's page
		 * directory, or our active page directory will be one
		 * that's been freed (and cleared). */
		curr->pml4 = NULL;
		pml4_activate (NULL);
		pml4_destroy (pml4);
	}
}

/* Sets up the CPU for running user code in the nest thread.
 * This function is called on every context switch. */
void
process_activate (struct thread *next) {
	/* Activate thread's page tables. */
	pml4_activate (next->pml4);

	/* Set thread's kernel stack for use in processing interrupts. */
	tss_update (next);
}

/* We load ELF binaries.  The following definitions are taken
 * from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
#define EI_NIDENT 16

#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
 * This appears at the very beginning of an ELF binary. */
struct ELF64_hdr {
	unsigned char e_ident[EI_NIDENT];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint64_t e_entry;
	uint64_t e_phoff;
	uint64_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
};

struct ELF64_PHDR {
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
};

/* Abbreviations */
#define ELF ELF64_hdr
#define Phdr ELF64_PHDR

static bool setup_stack (struct intr_frame *if_);
static bool validate_segment (const struct Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes,
		bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
 * Stores the executable's entry point into *RIP
 * and its initial stack pointer into *RSP.
 * Returns true if successful, false otherwise. */
static bool
load (const char *file_name, struct intr_frame *if_) {
	struct thread *t = thread_current ();
	struct ELF ehdr;
	struct file *file = NULL;
	off_t file_ofs;
	bool success = false;
	int i;

	/* Allocate and activate page directory. */
	t->pml4 = pml4_create ();
	if (t->pml4 == NULL)
		goto done;
	process_activate (thread_current ());

	/* Open executable file. */
	sys_file_lock_acquire ();
	file = filesys_open (file_name);
	sys_file_lock_release ();
	if (file == NULL) {
		printf ("load: %s: open failed\n", file_name);
		goto done;
	}

	sys_file_lock_acquire ();
    /* 파일 쓰기 잠금, 스레드에 저장 */
    file_deny_write (file);
    thread_current ()->running_file = file;

	/* Read and verify executable header. */
	if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
			|| memcmp (ehdr.e_ident, "\177ELF\2\1\1", 7)
			|| ehdr.e_type != 2
			|| ehdr.e_machine != 0x3E // amd64
			|| ehdr.e_version != 1
			|| ehdr.e_phentsize != sizeof (struct Phdr)
			|| ehdr.e_phnum > 1024) {
		printf ("load: %s: error loading executable\n", file_name);
		sys_file_lock_release ();
		goto done;
	}
	sys_file_lock_release ();

	/* Read program headers. */
	file_ofs = ehdr.e_phoff;

	sys_file_lock_acquire ();
	off_t f_len = file_length (file); 
	sys_file_lock_release ();	
 
	for (i = 0; i < ehdr.e_phnum; i++) {
		struct Phdr phdr;

		if (file_ofs < 0 || file_ofs > f_len){
			goto done;
		}
		sys_file_lock_acquire ();
		file_seek (file, file_ofs);

		if (file_read (file, &phdr, sizeof phdr) != sizeof phdr){
			sys_file_lock_release ();
			goto done;
		}
		sys_file_lock_release ();

		file_ofs += sizeof phdr;
		switch (phdr.p_type) {
			case PT_NULL:
			case PT_NOTE:
			case PT_PHDR:
			case PT_STACK:
			default:
				/* Ignore this segment. */
				break;
			case PT_DYNAMIC:
			case PT_INTERP:
			case PT_SHLIB:
				goto done;
			case PT_LOAD:
				if (validate_segment (&phdr, file)) {
					bool writable = (phdr.p_flags & PF_W) != 0;
					uint64_t file_page = phdr.p_offset & ~PGMASK;
					uint64_t mem_page = phdr.p_vaddr & ~PGMASK;
					uint64_t page_offset = phdr.p_vaddr & PGMASK;
					uint32_t read_bytes, zero_bytes;
					if (phdr.p_filesz > 0) {
						/* Normal segment.
						 * Read initial part from disk and zero the rest. */
						read_bytes = page_offset + phdr.p_filesz;
						zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
								- read_bytes);
					} else {
						/* Entirely zero.
						 * Don't read anything from disk. */
						read_bytes = 0;
						zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
					}
					if (!load_segment (file, file_page, (void *) mem_page,
								read_bytes, zero_bytes, writable)) {
						goto done;
					}
				}
				else {
					goto done;
				}
				break;
		}
	}

	/* Set up stack. */
	if (!setup_stack (if_)) {
		goto done;
	}

	/* Start address. */
	if_->rip = ehdr.e_entry;

	success = true;

done:
	/* We arrive here whether the load is successful or not. */
	//file_close (file);
	return success;
}


/* Checks whether PHDR describes a valid, loadable segment in
 * FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Phdr *phdr, struct file *file) {
	/* p_offset and p_vaddr must have the same page offset. */
	if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
		return false;

	/* p_offset must point within FILE. */
	if (phdr->p_offset > (uint64_t) file_length (file))
		return false;

	/* p_memsz must be at least as big as p_filesz. */
	if (phdr->p_memsz < phdr->p_filesz)
		return false;

	/* The segment must not be empty. */
	if (phdr->p_memsz == 0)
		return false;

	/* The virtual memory region must both start and end within the
	   user address space range. */
	if (!is_user_vaddr ((void *) phdr->p_vaddr))
		return false;
	if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
		return false;

	/* The region cannot "wrap around" across the kernel virtual
	   address space. */
	if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
		return false;

	/* Disallow mapping page 0.
	   Not only is it a bad idea to map page 0, but if we allowed
	   it then user code that passed a null pointer to system calls
	   could quite likely panic the kernel by way of null pointer
	   assertions in memcpy(), etc. */
	if (phdr->p_vaddr < PGSIZE)
		return false;

	/* It's okay. */
	return true;
}

#ifndef VM
/* Codes of this block will be ONLY USED DURING project 2.
 * If you want to implement the function for whole project 2, implement it
 * outside of #ifndef macro. */

/* load() helpers. */
static bool install_page (void *upage, void *kpage, bool writable);

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	sys_file_lock_acquire ();
	file_seek (file, ofs);
	sys_file_lock_release ();
	
	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* Get a page of memory. */
		uint8_t *kpage = palloc_get_page (PAL_USER);
		if (kpage == NULL)
			return false;

		/* Load this page. */
		sys_file_lock_acquire ();
		if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes) {
			palloc_free_page (kpage);
			sys_file_lock_release ();
			return false;
		}
		sys_file_lock_release ();
		memset (kpage + page_read_bytes, 0, page_zero_bytes);

		/* Add the page to the process's address space. */
		if (!install_page (upage, kpage, writable)) {
			printf("fail\n");
			palloc_free_page (kpage);
			return false;
		}

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a minimal stack by mapping a zeroed page at the USER_STACK */
static bool
setup_stack (struct intr_frame *if_) {
	uint8_t *kpage;
	bool success = false;

	kpage = palloc_get_page (PAL_USER | PAL_ZERO);
	if (kpage != NULL) {
		success = install_page (((uint8_t *) USER_STACK) - PGSIZE, kpage, true);
		if (success)
			if_->rsp = USER_STACK;
		else
			palloc_free_page (kpage);
	}
	return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
 * virtual address KPAGE to the page table.
 * If WRITABLE is true, the user process may modify the page;
 * otherwise, it is read-only.
 * UPAGE must not already be mapped.
 * KPAGE should probably be a page obtained from the user pool
 * with palloc_get_page().
 * Returns true on success, false if UPAGE is already mapped or
 * if memory allocation fails. */
static bool
install_page (void *upage, void *kpage, bool writable) {
	struct thread *t = thread_current ();

	/* Verify that there's not already a page at that virtual
	 * address, then map our page there. */
	return (pml4_get_page (t->pml4, upage) == NULL
			&& pml4_set_page (t->pml4, upage, kpage, writable));
}
#else
/* From here, codes will be used after project 3.
 * If you want to implement the function for only project 2, implement it on the
 * upper block. */

static bool
lazy_load_segment (struct page *page, void *aux) {
	struct segment_aux *s_aux = (struct segment_aux *) aux;
    /* file offset에서 page_read_bytes만큼 kva로 read */
    if (file_read_at (s_aux->file, page->frame->kva, s_aux->page_read_bytes, s_aux->offset) != (int) s_aux->page_read_bytes) {
		file_close (s_aux->file); 
        free (s_aux);
        return false;
    }

	/* page_zero_bytes 초기화 */
	memset (page->frame->kva + s_aux->page_read_bytes, 0, s_aux->page_zero_bytes);

	file_close (s_aux->file);
    free (s_aux);
    return true;
}

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	struct segment_aux *aux;

	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		struct segment_aux *aux = malloc(sizeof(*aux));
		if (aux == NULL)
            return false;

        aux->file = file_duplicate(file);
        aux->offset = ofs;
        aux->page_read_bytes = page_read_bytes;
        aux->page_zero_bytes = page_zero_bytes;

		if (!vm_alloc_page_with_initializer (VM_ANON, upage,
					writable, lazy_load_segment, aux)) {
			free (aux);
			return false;
		}

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
		ofs += page_read_bytes;
	}
	return true;
}

/* Create a PAGE of stack at the USER_STACK. Return true on success. */
static bool
setup_stack (struct intr_frame *if_) {
	void *stack_bottom = (void *) (((uint8_t *) USER_STACK) - PGSIZE);

	/* 스택에 VM_ANON 페이지 생성, VM_MARKER_0로 마킹 */
	if (vm_alloc_page (VM_ANON | VM_MARKER_0, stack_bottom, true)) {
        /* 바로 프레임 할당 후 rsp연결 */
        if(vm_claim_page (stack_bottom)) {
            if_->rsp = USER_STACK;
			return true;
		}
	}

	return false;
}
#endif /* VM */
