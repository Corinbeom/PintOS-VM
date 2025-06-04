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
#include "vm/vm.h"
#include "intrinsic.h"
#include "userprog/syscall.h"
#ifdef VM
#include "vm/vm.h"
#include "vm/file.h"
#endif

#define MAX_ARGS 128

static void process_cleanup (void);
static bool load (const char *file_name, struct intr_frame *if_);
static void initd (void *f_name);
static void __do_fork (void *);

static int parse_args(char *, char *[]);
static void argument_stack(char *argv[], int argc, struct intr_frame *_if);

/* General process initializer for initd and other process. */
static void
process_init (void) {
	struct thread *current = thread_current ();
	current->FDT = palloc_get_multiple(PAL_ZERO, FDT_PAGES);
	current->running_file = NULL;
	current->next_FD = 3;
}

/*
 * process_create_initd()
 * PintOS에서 사용자 프로그램 실행을 처음 시작할 때 호출되는 함수입니다.
 * 실행할 사용자 프로그램 이름(예: "initd arg1 arg2")을 받아서,
 * 커널 스레드를 생성하고, 그 안에서 사용자 프로세스를 실행하게 만듭니다.
 */
tid_t process_create_initd(const char *file_name)
{
	char *fn_copy, *fn_parse;     // file_name의 복사본들
	char *prog_name;              // 프로그램 이름만 따로 저장
	char *save_ptr;               // strtok_r에서 내부 상태 추적용
	tid_t tid;                    // 생성된 스레드의 ID (thread identifier)

	/* file_name의 복사본 두 개를 만들기 위한 페이지 할당 */
	fn_copy = palloc_get_page(0);   // 자식에게 전달할 전체 인자 문자열 보관용
	fn_parse = palloc_get_page(0);  // strtok_r로 파일 이름만 파싱하기 위한 임시 용도

	/* 메모리 할당 실패 시 오류 반환 (누수 방지용 해제 포함) */
	if (fn_copy == NULL || fn_parse == NULL) {
		palloc_free_page(fn_copy);   // NULL이어도 안전하게 호출 가능
		palloc_free_page(fn_parse);
		return TID_ERROR;
	}
	
	/* file_name 문자열을 두 버퍼에 각각 복사 */
	strlcpy(fn_copy, file_name, PGSIZE);   // 자식 프로세스에 넘길 원본 인자 전체
	strlcpy(fn_parse, file_name, PGSIZE);  // strtok_r로 파싱해서 스레드 이름 추출용

	/* fn_parse를 사용해서 첫 번째 단어(=실행 파일 이름)만 분리 */
	// 예: "initd arg1 arg2" → prog_name = "initd"
	prog_name = strtok_r(fn_parse, " ", &save_ptr);

	/* 새 스레드를 생성
	 * - prog_name: 스레드 이름 (디버깅용으로 사용됨)
	 * - initd: 새 스레드에서 실행할 함수 (사용자 프로그램을 시작하는 함수)
	 * - fn_copy: 자식에게 전달할 전체 인자 문자열
	 */
	tid = thread_create(prog_name, PRI_DEFAULT, initd, fn_copy);

	/* 스레드 생성 실패 시 fn_copy 메모리 회수 */
	if (tid == TID_ERROR)
		palloc_free_page(fn_copy);

	/* 파싱용 메모리는 부모만 쓰기 때문에 항상 해제 */
	palloc_free_page(fn_parse);

	/* 생성된 스레드의 tid를 반환 */
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
 * TID_ERROR if the thread cannot be created. */
tid_t process_fork(const char *name, struct intr_frame *if_) {
	// 시스템 콜 진입 시점에 저장된 인터럽트 프레임의 내용을 부모의 intr_frame에 복사
	// → 자식 프로세스 생성 시, 이를 참조해 동일한 실행 상태를 구성하게 함
	memcpy(&thread_current()->intr_frame, if_, sizeof(struct intr_frame));

	// 자식 스레드 생성: 이름, 우선순위, 시작 함수(__do_fork), 인자(부모 스레드 포인터)를 전달
	// __do_fork는 자식 스레드가 시작할 때 호출되며, 부모의 상태를 복제하는 작업 수행
	tid_t fork_tid = thread_create(name, PRI_DEFAULT, __do_fork, thread_current());
	if (fork_tid == TID_ERROR)
		return TID_ERROR;  // 자식 생성 실패 시 오류 반환

	// 자식의 tid를 이용해 자식 스레드 포인터를 가져옴
	struct thread *child = get_child_by_tid(fork_tid);

	// 자식 스레드가 복제 작업을 완료할 때까지 부모는 대기
	// → 자식이 intr_frame 등의 초기화 작업을 마칠 때까지 동기화
	if (child != NULL) {
		sema_down(&child->fork_sema);
	}

	// 깨어난 뒤 자식 스레드의 tid를 반환
	return fork_tid;
}

#ifndef VM
/* Duplicate the parent's address space by passing this function to the
 * pml4_for_each. This is only for the project 2. */
static bool
duplicate_pte (uint64_t *pte, void *va, void *aux) {
	// 현재 실행 중인 스레드(=자식 프로세스) 가져오기
	struct thread *current = thread_current ();

	// aux는 부모 스레드로 전달된 인자
	struct thread *parent = (struct thread *) aux;

	void *parent_page;  // 부모 프로세스의 물리 주소를 저장할 변수
	void *newpage;      // 자식 프로세스용 새 물리 페이지
	bool writable;      // 페이지가 쓰기 가능한지 여부

	// 커널 주소 공간은 복사하지 않음 → 사용자 영역만 처리
	if (is_kernel_vaddr(va))
		return true;

	// 부모 프로세스의 페이지 테이블에서 해당 가상 주소에 대응하는 물리 주소를 가져옴
	parent_page = pml4_get_page(parent->pml4, va);
	if (parent_page == NULL)
		return false; // 매핑된 페이지가 없다면 실패

	// 자식 프로세스용으로 새로운 사용자 페이지를 할당 (0으로 초기화된 페이지)
	newpage = palloc_get_page(PAL_USER | PAL_ZERO);
	if (newpage == NULL)
		return false; // 메모리 부족 등으로 할당 실패

	// 부모 페이지 내용을 자식의 새 페이지로 복사
	memcpy(newpage, parent_page, PGSIZE);

	// 복사한 페이지가 쓰기 가능한 페이지인지 확인
	writable = is_writable(pte);

	// 자식의 페이지 테이블에 해당 가상 주소를 새 페이지에 매핑
	if (!pml4_set_page(current->pml4, va, newpage, writable)) {
		// 매핑 실패 시 false 반환 (예: 중복 매핑 등)
		return false;
	}

	// 성공적으로 복제 완료
	return true;
}
#endif

/* A thread function that copies parent's execution context.
 * Hint) parent->tf does not hold the userland context of the process.
 *       That is, you are required to pass second argument of process_fork to
 *       this function. */
static void
__do_fork(void *aux) {
	struct intr_frame if_;                      	// 자식이 사용할 인터럽트 프레임
	struct thread *parent = (struct thread *)aux; 	// 부모 스레드
	struct thread *current = thread_current();     	// 현재 실행 중인 자식 스레드
	struct intr_frame *parent_if = &parent->intr_frame;
	bool succ = true;

	// 자식 프로세스용 필드 초기화 (children, FDT 등)
	process_init();

	// 부모의 인터럽트 프레임(CPU 상태)을 자식에 복사
	memcpy(&if_, parent_if, sizeof(struct intr_frame));

	// 자식 프로세스를 위한 새로운 페이지 테이블 생성
	current->pml4 = pml4_create();
	if (current->pml4 == NULL)
		goto error;  // 생성 실패 시 에러 처리

	// 페이지 테이블 활성화 (CR3에 로드)
	process_activate(current);

#ifdef VM
	// 보조 페이지 테이블 초기화 및 복사 (VM 기능이 켜져 있는 경우)
	supplemental_page_table_init(&current->spt);
	supplemental_page_table_copy(&current->spt, &parent->spt);
		
#else
	// 단순 페이지 테이블 복사 (VM 기능이 꺼져 있는 경우)
	if (!pml4_for_each(parent->pml4, duplicate_pte, parent))
		goto error;
#endif
	// 파일 디스크립터 테이블(FDT) 복제
	int fd_end = parent->next_FD;
	for (int fd = 0; fd < fd_end; fd++) {
		if (fd <= 2)
			// stdin, stdout, stderr은 그대로 공유
			current->FDT[fd] = parent->FDT[fd];
		else {
			// 일반 파일은 다시 열어서 자식이 독립적으로 사용하게 함
			if (parent->FDT[fd] != NULL) 
				current->FDT[fd] = file_duplicate(parent->FDT[fd]);
		}
	}
	current->next_FD = fd_end;

	// 자식 프로세스는 fork()의 반환값으로 0을 받아야 하므로 레지스터 설정
	if_.R.rax = 0;

	// 세그먼트 레지스터와 EFLAGS 설정 (유저 모드 전환 준비)
	if_.ds = if_.es = if_.ss = SEL_UDSEG;
	if_.cs = SEL_UCSEG;
	if_.eflags = FLAG_IF;

	// 자식이 준비 완료되었음을 부모에게 알림 (부모의 sema_down을 깨움)
	sema_up(&current->fork_sema);

	// 자식 프로세스를 유저 모드로 전환 (ret-from-fork)
	if (succ)
		do_iret(&if_);

error:
	// 실패 시 자식 종료 처리
	current->exit_status = -1;
	sema_up(&current->fork_sema);  // 부모가 기다리는 경우를 위해 신호 보냄
	thread_exit();                 // 자식 프로세스 종료
}

/* Switch the current execution context to the f_name.
 * Returns -1 on fail. */
int process_exec(void *f_name)
{
	// 최대 MAX_ARGS 개수만큼의 인자들을 저장할 배열 선언
	char *argv[MAX_ARGS];

	// f_name은 "실행파일명 인자1 인자2 ..." 형태의 문자열임
	// 이를 공백 기준으로 파싱하여 argv에 저장하고 argc에 개수를 저장
	int argc = parse_args(f_name, argv);

	bool success;

	/* intr_frame 구조체는 유저 프로세스의 레지스터 정보를 저장
	 * 현재 스레드의 멤버를 사용할 수 없는 이유는,
	 * process_exec가 현재 실행 중인 스레드의 실행 컨텍스트를 완전히 새로 바꾸기 때문임.
	 * → _if는 임시로 스택에 선언된 intr_frame */
	struct intr_frame _if;

	_if.ds = _if.es = _if.ss = SEL_UDSEG;
	_if.cs = SEL_UCSEG;
	_if.eflags = FLAG_IF | FLAG_MBS;

	/* 현재 프로세스에서 실행 중이던 프로그램과 자원들을 모두 정리
	 * - 열린 파일 닫기
	 * - 페이지 테이블 해제
	 * - 유저 스택 정리 등 */
	process_cleanup();

	/* 파일 이름 파싱 결과의 첫 번째 토큰은 실제 실행할 파일 이름임 */
	ASSERT(argv[0] != NULL);

	// 실행할 유저 프로그램을 메모리에 로드 (ELF 파일 분석 및 페이지 할당 포함)
	lock_acquire(&filesys_lock);
	success = load(argv[0], &_if);
	lock_release(&filesys_lock);
	/* 실행 파일 로드에 실패했으면 f_name 해제, -1 리턴 후 종료 */
	if (!success) {
    	palloc_free_page(f_name);
		return -1;
	}
    
    // load 성공 시, 유저 스택에 인자 전달
	argument_stack(argv, argc, &_if);
    // load 성공 시에도 f_name 해제
    palloc_free_page(f_name);

	/* 커널에서 유저 프로세스로 전환
	 * do_iret는 레지스터 값을 복원하고 유저 모드로 진입시키는 어셈블리 함수
	 * _if에 저장된 값들을 이용하여 유저 프로그램을 실행 */
	do_iret(&_if);

	/* do_iret는 유저 모드로 완전히 전환되기 때문에 이 아래 코드는 실행되지 않음 */
	NOT_REACHED();
}

// 문자열 target을 공백(" ") 기준으로 잘라서 각 토큰(인자)을 argv 배열에 저장하고, 인자의 개수를 반환하는 함수
// 예: target = "echo hello world" → argv = ["echo", "hello", "world", NULL]
static int parse_args(char *target, char *argv[])
{
	int argc = 0; // 인자의 개수를 세기 위한 변수
	char *token;
	char *save_ptr; // strtok_r에서 파싱 상태를 유지하기 위한 포인터 (reentrant-safe)

	// 첫 번째 토큰 추출. strtok_r는 문자열을 공백을 기준으로 분리
	for (token = strtok_r(target, " ", &save_ptr);
		 token != NULL;
		 token = strtok_r(NULL, " ", &save_ptr)) // 이후 토큰부터는 첫 인자에 NULL 전달
	{
		argv[argc++] = token; // 잘라낸 인자를 argv 배열에 저장하고 argc 증가
	}

	// argv는 마지막에 NULL 포인터로 끝나야 exec 계열 함수에서 제대로 처리됨 (C 언어 컨벤션)
	argv[argc] = NULL;

	// 최종적으로 인자의 개수를 반환
	return argc;
}

// 사용자 프로그램의 스택을 구성하여 인자들을 전달하는 함수
static void argument_stack(char *argv[], int argc, struct intr_frame *_if) {
    uint64_t rsp_arr[argc]; // 각 인자 문자열의 시작 주소를 저장할 배열

    // 문자열을 스택에 역순으로 복사
    for (int i = argc - 1; i >= 0; i--) {
        size_t len = strlen(argv[i]) + 1;     // 문자열 길이 + 널 문자 포함
        _if->rsp -= len;                      // 스택 아래로 공간 확보
        rsp_arr[i] = _if->rsp;                // 해당 문자열이 위치한 주소 저장
        memcpy((void *)_if->rsp, argv[i], len); // 스택에 문자열 복사
    }

    // 16바이트 정렬 맞추기 (rsp를 16의 배수로 내림 정렬)
    _if->rsp = _if->rsp & ~0xF;  // 하위 4비트 0으로 마스킹 → 16의 배수

    // NULL sentinel push (argv[argc] = NULL)
    _if->rsp -= 8;                      // 포인터 크기만큼 스택 아래로
    memset(_if->rsp, 0, sizeof(char **)); // 0으로 채움 (NULL)

    // argv[i] 포인터들을 역순으로 push
    for (int i = argc - 1; i >= 0; i--) {
        _if->rsp -= 8;                         // 8바이트 공간 확보
        memcpy(_if->rsp, &rsp_arr[i], sizeof(char **)); // 각 문자열의 주소를 복사
    }

    // fake return address
    _if->rsp -= 8;
    memset(_if->rsp, 0, sizeof(void *)); // 가짜 리턴 주소 = 0

    // 사용자 프로그램 시작 시 인자 전달을 위한 레지스터 설정
    _if->R.rdi = argc;             // 첫 번째 인자: argc
    _if->R.rsi = _if->rsp + 8;     // 두 번째 인자: argv (가짜 리턴 주소 다음부터가 argv[0] 배열)
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
int process_wait(tid_t child_tid) {
	// 인터럽트를 비활성화하여 동기화 문제를 방지하고 현재 스레드를 얻음
	enum intr_level old_level = intr_disable();
	struct thread *cur = thread_current();

	// 현재 스레드(부모)의 자식 리스트에서 주어진 TID를 가진 자식을 탐색
	struct thread *search_cur = get_child_by_tid(child_tid);
	intr_set_level(old_level); // 인터럽트 다시 활성화

	// 만약 해당 자식이 존재하지 않는다면 잘못된 접근이므로 -1 반환
	if (search_cur == NULL)
		return -1;

	// 자식이 종료될 때까지 부모 프로세스를 대기 상태로 전환 (세마포어 다운)
	sema_down(&search_cur->wait_sema);

	// 이후 자식 종료 시 process_exit으로부터 대기를 마치고 깨어남 (세마포어 업)
	// 자식의 종료 상태(exit_status)를 받아옴
	int stat = search_cur->exit_status;

	// 자식 리스트에서 해당 자식 정보를 제거
	list_remove(&search_cur->child_elem);

	// 자식이 완전히 종료될 수 있도록 process_exit의 자식을 깨워줌 (세마포어 업)
	sema_up(&search_cur->exit_sema);

	// 자식의 종료 상태를 부모에게 반환
	return stat;
}

struct thread *get_child_by_tid(tid_t child_tid) {
    struct thread *cur = thread_current();  // 현재 실행 중인 스레드(=부모 스레드)를 가져옴
    struct thread *v = NULL;                // 결과를 저장할 포인터

    // 현재 스레드의 자식 리스트를 순회함
    for (struct list_elem *i = list_begin(&cur->children); 
         i != list_end(&cur->children); 
         i = i->next) {

        // 리스트 요소 i를 thread 구조체로 변환
        struct thread *t = list_entry(i, struct thread, child_elem);

        // 자식 스레드의 tid가 찾고자 하는 child_tid와 같다면
        if (t->tid == child_tid) {
            v = t;       // 찾은 자식 스레드를 v에 저장
            break;       // 더 이상 탐색할 필요 없으므로 반복문 종료
        }
    }

    return v;  // 찾았으면 해당 스레드 포인터 반환, 못 찾았으면 NULL 반환
}

/* Exit the process. This function is called by thread_exit (). */
void process_exit(void) {
	// 현재 종료 중인 프로세스(스레드)를 가져옴
	struct thread *cur = thread_current();

	// 파일 디스크립터 테이블(FDT)에 열려 있는 모든 파일을 닫기
    // 일반적으로 stdin(0), stdout(1), stderr(2)는 닫지 않고 3번부터 닫음
    for(int i = 3; i < cur->next_FD; i++){
        // 만약 해당 FD 슬롯에 열린 파일이 있다면
        if (cur->FDT[i] != NULL)
            file_close(cur->FDT[i]); 	// 해당 파일 닫기
        cur->FDT[i] = NULL; 			// 슬롯을 NULL로 초기화
    }

    // 파일 디스크립터 테이블에 할당했던 메모리 해제
    palloc_free_multiple(cur->FDT, FDT_PAGES);
	
	/* TODO : page 삭제 로직 추가 */

    // 현재 실행 파일 닫기(deny_write 해제는 해당 함수 안에서 자동으로 적용)
    file_close(cur->running_file);

	// 부모 프로세스가 존재하는 경우 동기화 처리 진행
	if (cur->parent != NULL) {
		// process_wait에서 부모가 기다리고 있다면 이를 깨워줌 (세마포어 업)
		sema_up(&cur->wait_sema);

		// 부모가 자식의 상태를 회수할 때까지 대기 (세마포어 다운)
		sema_down(&cur->exit_sema);
	}

	// 부모의 자식 상태 회수 후 process_wait으로부터 대기를 마치고 깨어남 (세마포어 업)
	// 프로세스 리소스 정리
	process_cleanup();
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
	file = filesys_open (file_name);
	if (file == NULL) {
		printf ("load: %s: open failed\n", file_name);
		goto done;
	}

	/* Read and verify executable header. */
	if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
			|| memcmp (ehdr.e_ident, "\177ELF\2\1\1", 7)
			|| ehdr.e_type != 2
			|| ehdr.e_machine != 0x3E // amd64
			|| ehdr.e_version != 1
			|| ehdr.e_phentsize != sizeof (struct Phdr)
			|| ehdr.e_phnum > 1024) {
		printf ("load: %s: error loading executable\n", file_name);
		goto done;
	}

	/* Read program headers. */
	file_ofs = ehdr.e_phoff;
	for (i = 0; i < ehdr.e_phnum; i++) {
		struct Phdr phdr;

		if (file_ofs < 0 || file_ofs > file_length (file))
			goto done;
		file_seek (file, file_ofs);

		if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
			goto done;
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
								read_bytes, zero_bytes, writable))
						goto done;
				}
				else
					goto done;
				break;
		}
	}
	t->running_file = file;		// 스레드의 running_file을 현재 파일로 설정
	
	file_deny_write(file);		// 현재 실행 중인 파일 쓰기 금지
	/* Set up stack. */
	if (!setup_stack (if_))
		goto done;

	/* Start address. */
	if_->rip = ehdr.e_entry;

	success = true;
	goto done;
done:
	if (!success && file != NULL)
		file_close(file); // 성공하지 못한 경우 파일 닫기
	return success;		  // load 성공 여부 반환	
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

	file_seek (file, ofs);
	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* TODO : Delete allocating and mapping physical page part */

		/* Get a page of memory. */
		uint8_t *kpage = palloc_get_page (PAL_USER);
		if (kpage == NULL)
			return false;

		/* Load this page. */
		if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes) {
			palloc_free_page (kpage);
			return false;
		}
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
	/* Create page */
	/* Set up page members */
	/* Using insert_vme(), add vm_enty to hash table */
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

bool lazy_load_segment(struct page *page, void *aux)
{
	/* TODO: Load the segment from the file */
	/* TODO: This called when the first page fault occurs on address VA. */
	/* TODO: VA is available when calling this function. */

	struct load_info *load_info = (struct load_info *)aux;

	// 1) 파일의 position을 ofs으로 지정한다.
	file_seek(load_info->file, load_info->ofs);
	// 2) 파일을 read_bytes만큼 물리 프레임에 읽어 들인다.
	if (file_read(load_info->file, page->frame->kva, load_info->read_bytes) != (int)(load_info->read_bytes))
	{
		palloc_free_page(page->frame->kva);
		return false;
	}
	// 3) 다 읽은 지점부터 zero_bytes만큼 0으로 채운다.
	memset(page->frame->kva + load_info->read_bytes, 0, load_info->zero_bytes);
	// free(load_info); // 🚨 Todo : 어디서 반환하지?

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
	ASSERT((read_bytes + zero_bytes) % PGSIZE == 0); // read_bytes + zero_bytes가 페이지 크기(PGSIZE)의 배수인지 확인
	ASSERT(pg_ofs(upage) == 0);						 // upage가 페이지 정렬되어 있는지 확인
	ASSERT(ofs % PGSIZE == 0)						 // ofs가 페이지 정렬되어 있는지 확인;

	while (read_bytes > 0 || zero_bytes > 0) // read_bytes와 zero_bytes가 0보다 큰 동안 루프를 실행
	{
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		/* 이 페이지를 채우는 방법을 계산합니다.
		파일에서 PAGE_READ_BYTES 바이트를 읽고
		최종 PAGE_ZERO_BYTES 바이트를 0으로 채웁니다. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* TODO: Set up aux to pass information to the load_infosegment. */
		// vm_alloc_page_with_initializer에 제공할 aux 인수로 필요한 보조 값들을 설정해야 합니다.
		// 바이너리 로딩(loading of binary)을 위해 필요한 정보를 포함하는 구조체를 만들어야 할 수도 있습니다.
		// void *aux = NULL;
		struct load_info *load_info = (struct load_info *)malloc(sizeof(struct load_info));
		load_info->file = file;
		load_info->ofs = ofs;
		load_info->read_bytes = page_read_bytes;
		load_info->zero_bytes = page_zero_bytes;
		// vm_alloc_page_with_initializer를 호출하여 대기 중인 객체를 생성합니다.
		if (!vm_alloc_page_with_initializer(VM_FILE, upage,
											writable, lazy_load_segment, load_info))
			return false;

		/* Advance. */
		// 읽은 바이트와 0으로 채운 바이트를 추적하고 가상 주소를 증가시킵니다.
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
	bool success = false;
	void *stack_bottom = (void *)(((uint8_t *)USER_STACK) - PGSIZE);

	/* TODO: Map the stack on stack_bottom and claim the page immediately.
	 * TODO: If success, set the rsp accordingly.
	 * TODO: You should mark the page is stack. */
	/* TODO: stack_bottom에 스택을 매핑하고 페이지를 즉시 요청하세요.
	 * TODO: 성공하면, rsp를 그에 맞게 설정하세요.
	 * TODO: 페이지가 스택임을 표시해야 합니다. */
	/* TODO: Your code goes here */
	if (vm_alloc_page_with_initializer(VM_ANON | VM_MARKER_0, stack_bottom, 1, NULL, NULL))
	// VM_MARKER_0: 스택이 저장된 메모리 페이지를 식별
	// writable: 값을 넣어야 하니 True
	// lazy_load를 하지 않을 거니까 init과 aux는 NULL
	{
		success = vm_claim_page(stack_bottom); // 페이지 요청
		if (success)
			if_->rsp = USER_STACK;
	}
	return success;
}
#endif /* VM */

int process_add_file(struct file *file) {
    // 현재 실행 중인 스레드(=프로세스) 가져오기
    struct thread *curr = thread_current();

    // 파일 디스크립터(fd)는 0~2는 이미 예약된 상태(stdin, stdout, stderr)
    // 따라서 일반 파일은 3번부터 사용
    for (int fd = 3; fd < MAX_FD; fd++) {
        // 현재 FDT(File Descriptor Table)에서 비어있는 슬롯 찾기
        if (curr->FDT[fd] == NULL) {
            // 비어 있는 슬롯을 찾으면 해당 위치에 파일 포인터 저장
            curr->FDT[fd] = file;

            // 다음 검색할 fd 번호를 갱신
            curr->next_FD = fd + 1;

            // 성공적으로 등록한 fd 번호 반환
            return fd;
        }
    }

    // 모든 슬롯이 차서 더 이상 파일을 열 수 없다면 -1 반환
    return -1;
}

struct file *process_get_file(int fd) {
    // 현재 실행 중인 스레드(=프로세스) 가져오기
    struct thread *curr = thread_current();

    // fd가 0~2(stdin, stdout, stderr)인 경우 시스템 콜에서 따로 처리
    // 또한, 허용되지 않는 범위의 fd인 경우도 NULL 반환
    if (fd < 3 || fd >= MAX_FD) {
        return NULL;  // 유효하지 않은 fd → 실패
    }

    // 유효한 fd이면, 해당 위치의 파일 포인터를 반환
    return curr->FDT[fd];
}

