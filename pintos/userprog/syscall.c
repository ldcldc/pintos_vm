#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include <string.h>

#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "threads/init.h"
#include "threads/synch.h" 
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "devices/input.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "threads/vaddr.h"
#include "userprog/process.h"

struct lock file_lock;
void syscall_entry (void);
void syscall_handler (struct intr_frame *);
void sys_exit (int status);
void check_valid_buffer (const void *buffer, size_t size, bool is_write);

#define STDIN_FILE ((struct file *) 1)
#define STDOUT_FILE ((struct file *) 2)

/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081         /* Segment selector msr */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

/* 파일 리스트 초기화 */
static void
init_file_list (struct thread *curr) {
    if (curr->file_list == NULL) {
        curr->fd_capacity = 16;
        curr->file_list = malloc (sizeof(struct file *) * curr->fd_capacity);
        if (curr->file_list != NULL) {
            curr->file_list[0] = STDIN_FILE;
            curr->file_list[1] = STDOUT_FILE;
            for (int i = 2; i < curr->fd_capacity; i++) {
                curr->file_list[i] = NULL;
            }
        }
    }
}
/* 열린 file count 함수 */
static int
count_file_ref (struct thread *curr, struct file *f) {
    int count = 0;
    if (curr->file_list == NULL || f == NULL || f == STDIN_FILE || f == STDOUT_FILE)
        return 0;
    for (int i = 0; i < curr->fd_capacity; i++) {
        if (curr->file_list[i] == f)
            count++;
    }
    return count;
}

/* 부모 file_list 복사 함수 */
bool 
sys_copy_file_list (struct thread *child, struct thread *parent) {
    if (parent->file_list == NULL) {
        child->file_list = NULL;
        return true;
    }

    child->fd_capacity = parent->fd_capacity;
    child->file_list = malloc (sizeof(struct file *) * child->fd_capacity);
    if (child->file_list == NULL) return false;

    for (int i = 0; i < parent->fd_capacity; i++) {
        struct file *f = parent->file_list[i];
        if (f == STDIN_FILE) {
            child->file_list[i] = STDIN_FILE;
        } else if (f == STDOUT_FILE) {
            child->file_list[i] = STDOUT_FILE;
        } else if (f != NULL) {
            lock_acquire (&file_lock);
            child->file_list[i] = file_duplicate (f);
            lock_release (&file_lock);
        } else {
            child->file_list[i] = NULL;
        }
    }
    return true;
}

void 
sys_file_lock_acquire (void) {
    lock_acquire (&file_lock);
}

void 
sys_file_lock_release (void) {
    lock_release (&file_lock);
}

/* 주소 == NULL or 유저영역 X or 페이지 테이블(pml4)연결 확인  */
static void 
check_address (void *addr) {    
    if (addr == NULL || !is_user_vaddr (addr)) {
        sys_exit (-1);
    }
}

static void
check_valid_page (void *addr, bool is_write) {
    check_address (addr);
        
    volatile char temp = *(volatile char *) (addr);
    
    struct page *page = spt_find_page (&thread_current ()->spt, addr);
    if (is_write && !page->writable) {
        sys_exit (-1);
    }
}

static void
check_valid_filename (char *file) {
    check_address (file);
    char *addr = file;

    while (*addr != '\0') {
        volatile char temp = *(volatile char *) (addr);        
        addr += 1;
    }
}

/* 버퍼가 속한 모든 페이지를 한 번씩 읽어서 물리 메모리에 올림(페이지 폴트 유도) */
void check_valid_buffer (const void *buffer, size_t size, bool is_write) {
    if (size == 0) return;

    char *ptr = (char *) buffer;

    for (size_t i = 0; i < size; i += PGSIZE) {
        check_valid_page (ptr + i, is_write);
    }
    
    check_valid_page (ptr + size - 1, is_write);
}


/* 현재 스레드에 파일 추가 */
static int 
process_add_file (struct file *f) {
    struct thread *curr = thread_current ();

    /* 첫 할당 시 */
    init_file_list(curr);

    if (curr->file_list == NULL) {
        return -1;
    }

    /* 빈자리 탐색 */
    for (int i = 2; i < curr->fd_capacity; i++) {
        if (curr->file_list[i] == NULL) {
            curr->file_list[i] = f;
            return i;
        }
    }
    /* 꽉차면 (배열 크기 * 2) */
    int old_cap = curr->fd_capacity;
    int new_cap = old_cap * 2;

    struct file **new_list = realloc (curr->file_list, sizeof(struct file *) * new_cap);
    if (new_list == NULL) {
        return -1; 
    }

    /* 초기화 */
    for (int i = old_cap; i < new_cap; i++) {
        new_list[i] = NULL;
    }

    curr->file_list = new_list;
    curr->fd_capacity = new_cap;

    /* 새로 받은자리에 할당 */
    curr->file_list[old_cap] = f;
    return old_cap;
} 

void
syscall_init (void) {
	lock_init (&file_lock);
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}

/* 종료 */ 
static void
sys_halt (void) {
	power_off();
}

/* exit_status에 종료 상태 저장 후 exit (static 아님)*/
void
sys_exit (int status) {
	thread_current ()->exit_status = status; 
	thread_exit ();
}

/* process_fork후 자식 tid 리턴 */
static tid_t
sys_fork (const char *thread_name, struct intr_frame *f) {
    check_address ((void *) thread_name);
    return process_fork (thread_name, f);
}

/* exec후 return 안함 실패시 return -1 */
static int
sys_exec (const char *file) {    
    check_valid_filename (file);
    /* 인자로 받은 파일명+인자(최대128byte)을 사용하기 전에 
     * process_exec에서 process_cleanup하는데 128바이트여도
     * 커널에 계속 쌓인다고 생각하면 배열로 저장하기에는 위험하니 malloc */
    char *temp = palloc_get_page (PAL_ZERO);
    if (temp == NULL) {
        return -1;
    }

    strlcpy (temp, file, PGSIZE);

    process_exec (temp);
     
    /* 여기까지 왔으면 return -1 */
    return -1;
}

/* pid종료 대기 */
static int
sys_wait (int pid) {
	return process_wait(pid);
}

/* file create(lock) */
static bool
sys_create (const char *file, unsigned initial_size) {	
    check_valid_filename (file);
    lock_acquire (&file_lock); 
    bool success = filesys_create (file, initial_size);
    lock_release (&file_lock);

    return success;
}


/* file remove */
static bool
sys_remove (const char *file) {	
    check_valid_filename (file);
    lock_acquire (&file_lock); 
    bool success = filesys_remove (file);
    lock_release (&file_lock);

    return success;
}

/* file open */
static int
sys_open (const char *file){	
    check_valid_filename (file);
    lock_acquire(&file_lock); 
    struct file *curr_file = filesys_open (file);
    lock_release(&file_lock);

    if (curr_file == NULL) {
        return -1;
    }

    int fd = process_add_file (curr_file);

    if (fd == -1) {
        lock_acquire(&file_lock);
        file_close (curr_file);
        lock_release(&file_lock);
    }
	
    return fd;
}

/* file 바이트 반환(file_length) */
static int
sys_filesize (int fd) {
    struct thread *curr = thread_current ();
    if (fd < 0 || curr->file_list == NULL || fd >= curr->fd_capacity) { 
        return -1;
    }
    
    struct file *f = thread_current ()->file_list[fd]; 
    if (f == NULL || f == STDIN_FILE || f == STDOUT_FILE) {
        return -1;
    }

    lock_acquire(&file_lock);
    int length = file_length (f);
    lock_release(&file_lock);
    
    return length;
}

/* file 데이터 read */
static int
sys_read (int fd, void *buffer, unsigned size) {
    check_valid_buffer (buffer, size, true); 
    struct thread *curr = thread_current ();

    init_file_list (curr);

    /* fd 유효성 check */
    if (fd < 0 || curr->file_list == NULL || fd >= curr->fd_capacity || curr->file_list[fd] == NULL) {
        return -1;
    }
    if (curr->file_list[fd] == STDOUT_FILE) {
        return -1;
    }

    /* fd == 0이고 file_list[0]에 파일이 없으면 */
    if (curr->file_list[fd] == STDIN_FILE) {
        unsigned int i;
        unsigned char *buf = (unsigned char *) buffer;
        for (i = 0; i < size; i++) {
            char c = input_getc ();
            if (c == '\0' || c == '\r' || c == '\n') {
                buf[i] = c;
                i++;
                break;
            }
            buf[i] = c;
        }
        return i;
    }
    
    /* 멀쩡하면 파일 읽기 */
    if (curr->file_list == NULL || fd >= curr->fd_capacity || curr->file_list[fd] == NULL) {
        return -1;
    }

    lock_acquire(&file_lock);
    int size_read = file_read (curr->file_list[fd], buffer, size);
    lock_release(&file_lock);

    return size_read;
}

/* file에 데이터 write */
static int
sys_write (int fd, const void *buffer, unsigned size) {
    check_valid_buffer (buffer, size, false); 
    struct thread *curr = thread_current ();
    
    init_file_list (curr);

    /* fd 유효성 check */
    if (fd < 0 || curr->file_list == NULL || fd >= curr->fd_capacity || curr->file_list[fd] == NULL) {
        return 0;
    }
    if (curr->file_list[fd] == STDIN_FILE) {
        return 0;
    }

    /* fd == 1이고 file_list[1]에 파일이 없으면 */
    if (curr->file_list[fd] == STDOUT_FILE) {
        if (size > 0) {
            putbuf (buffer, size);
            return size;
        }
        return 0;
    }

    /* 멀쩡하면 파일에 쓰기 */
    if (curr->file_list == NULL || fd >= curr->fd_capacity || curr->file_list[fd] == NULL) {
        return 0;
    }

    lock_acquire(&file_lock);
    int size_write = file_write (curr->file_list[fd], buffer, size);
    lock_release(&file_lock);

    return size_write;
}

/* file_position 변경 */
static void
sys_seek (int fd, unsigned position) {
    struct thread *curr = thread_current ();
    if (fd < 0 || curr->file_list == NULL || fd >= curr->fd_capacity || curr->file_list[fd] == NULL) {
        return;
    }

    struct file *f = curr->file_list[fd]; 
    if (f == NULL || f == STDIN_FILE || f == STDOUT_FILE) {
        return;
    }

    lock_acquire(&file_lock);
    file_seek (curr->file_list[fd], position);
    lock_release(&file_lock);
}

/* file_position 반환 */
static unsigned
sys_tell (int fd) {
    struct thread *curr = thread_current ();
    if (fd < 0 || curr->file_list == NULL || fd >= curr->fd_capacity || curr->file_list[fd] == NULL) {
        return 0;
    }

    struct file *f = curr->file_list[fd]; 
    if (f == NULL || f == STDIN_FILE || f == STDOUT_FILE) {
        return -1;
    }

    lock_acquire(&file_lock);
    unsigned pos = file_tell (curr->file_list[fd]);
    lock_release(&file_lock);
    return pos;
}

/* file close */
static void
sys_close (int fd) {
    struct thread *curr = thread_current ();

    if (fd < 0 || curr->file_list == NULL || fd >= curr->fd_capacity || curr->file_list[fd] == NULL) {
        return; 
    }

    struct file *curr_file = curr->file_list[fd];
    
    if (curr_file != STDIN_FILE && curr_file != STDOUT_FILE) {
        lock_acquire(&file_lock);
        if (count_file_ref (curr, curr_file) == 1) {
            file_close (curr_file);
        }
        lock_release(&file_lock);
    }
    curr->file_list[fd] = NULL;
}

/* oldfd의 파일을 newfd에 연결 */
static int
sys_dup2 (int oldfd, int newfd) {
    struct thread *curr = thread_current ();
    init_file_list (curr);

    /* oldfd, newfd 유효성 검사 */
    if (oldfd < 0 || curr->file_list == NULL || oldfd >= curr->fd_capacity || curr->file_list[oldfd] == NULL) {
        return -1; 
    }
    if (newfd < 0) return -1;
    if (oldfd == newfd) return newfd;

    /* newfd >= capacity이면 realloc */
    if (newfd >= curr->fd_capacity) {
        int old_cap = curr->fd_capacity;
        
        int new_cap = (old_cap * 2 > newfd + 1) ? (old_cap * 2) : (newfd + 1);

        struct file **new_list = realloc (curr->file_list, sizeof(struct file *) * new_cap);
        if (new_list == NULL) return -1;

        for (int i = old_cap; i < new_cap; i++) {
            new_list[i] = NULL;
        }

        curr->file_list = new_list;
        curr->fd_capacity = new_cap;
    }

    struct file *old_f = curr->file_list[oldfd];
    struct file *new_f = curr->file_list[newfd];

    /* newfd에 파일이 있고, 더 이상 참조하는 fd가 없으면 close */
    if (new_f != NULL && new_f != STDIN_FILE && new_f != STDOUT_FILE) {
        if (count_file_ref (curr, new_f) == 1) {
            lock_acquire (&file_lock);
            file_close (new_f);
            lock_release (&file_lock);
        }
    }

    /* 포인터 연결 (오프셋 공유) */
    curr->file_list[newfd] = old_f;

    return newfd;
}

/* The main system call interface */
void
syscall_handler (struct intr_frame *f) {
    /* 시스템 콜 번호: rax
	 * 인자 순서 rdi, rsi, rdx, r10, r8, r9 
     */
    thread_current ()->ursp = f->rsp;
    
    switch (f->R.rax) {
        case SYS_HALT:
            sys_halt ();
            break;

        case SYS_EXIT: 
			/* f->R.rdi == status */
			sys_exit((int) f->R.rdi);
            break;

        case SYS_FORK:
            /* f->R.rdi == thread_name, 레지스터 상태 f */
            f->R.rax = sys_fork ((const char *) f->R.rdi, f);
            break;
            
        case SYS_EXEC:
            /* f->R.rdi == char *file */
            f->R.rax = sys_exec ((const char *) f->R.rdi);
            break;
			
		case SYS_WAIT:
            /* f->R.rdi == pid */
            f->R.rax = sys_wait ((int) f->R.rdi);
            break;

		case SYS_CREATE:
			/* f->R.rdi == file, f->R.rsi == initial_size */
			f->R.rax = sys_create ((const char *)f->R.rdi, (unsigned int) f->R.rsi);
			break;

		case SYS_REMOVE:
			/* f->R.rdi == file */
			f->R.rax = sys_remove ((const char *)f->R.rdi);
			break;

		case SYS_OPEN:
			/* f->R.rdi == file */
			f->R.rax = sys_open ((const char *)f->R.rdi);
			break;

		case SYS_FILESIZE:
			/* f->R.rdi == fd */
			f->R.rax = sys_filesize ((int) f->R.rdi);
			break;

		case SYS_READ:
			/* f->R.rdi == fd,	f->R.rsi == buffer,  f->R.rdx == size */
			f->R.rax = sys_read ((int) f->R.rdi, (void *) f->R.rsi, (unsigned int) f->R.rdx);
			break;

        case SYS_WRITE: 
			/* f->R.rdi == fd,	f->R.rsi == buffer,  f->R.rdx == size */
			f->R.rax = sys_write((int) f->R.rdi, (void *) f->R.rsi, (unsigned int) f->R.rdx);
            break;

        case SYS_SEEK:
            /* f->R.rdi == fd, f->R.rsi == position */
            sys_seek ((int) f->R.rdi, (unsigned) f->R.rsi);
            break;

        case SYS_TELL:
            /* f->R.rdi == fd */
            f->R.rax = sys_tell ((int) f->R.rdi);
            break;

		case SYS_CLOSE:
			/* f->R.rdi == fd */
			sys_close ((int) f->R.rdi);
			break;

        case SYS_DUP2:
            /* f->R.rdi == oldfd, f->R.rsi == newfd */
            f->R.rax = sys_dup2 ((int) f->R.rdi, (int) f->R.rsi);
            break;
        default:
            sys_exit(-1);
            break;
    }
}
