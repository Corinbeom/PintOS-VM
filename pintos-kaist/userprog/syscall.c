#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "threads/palloc.h"
#include "userprog/validate.h"
#include "userprog/process.h"
#include "lib/kernel/console.h"     // 커널 콘솔 입출력 함수 제공 (putbuf, printf 등)
#include "lib/user/syscall.h"       // 유저 프로그램이 사용하는 시스템 콜 번호 및 인터페이스 정의
#include "filesys/directory.h"      // 디렉터리 관련 자료구조 및 함수 (디렉터리 열기, 탐색 등)
#include "filesys/filesys.h"        // 파일 시스템 전반에 대한 함수 및 초기화/포맷 인터페이스
#include "filesys/file.h"           // 개별 파일 객체(file 구조체) 및 파일 입출력 함수 정의 (read, write 등)

void syscall_entry (void);
void syscall_handler (struct intr_frame *);
void check_address(void *addr);

static void sys_halt();
static tid_t sys_exec(const char *cmd_line);
int sys_wait(int pid);
static int sys_write(int fd, const void *buffer, unsigned size);

tid_t sys_fork(const char *thread_name, struct intr_frame *f);
static bool sys_create(const char *file, unsigned initial_size);
static bool sys_remove(const char *file);
static int sys_open(const char *file_name);
static void sys_close(int fd);
static int sys_filesize(int fd);
static int sys_read(int fd, void *buffer, unsigned size);
static void sys_seek(int fd, unsigned position);
static unsigned sys_tell(int fd);

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

void
syscall_init (void) {
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
	lock_init(&filesys_lock);
}

/* The main system call interface */
void syscall_handler(struct intr_frame *f UNUSED) {
	uint64_t syscall_num = f->R.rax;
	uint64_t arg1 = f->R.rdi;
	uint64_t arg2 = f->R.rsi;
	uint64_t arg3 = f->R.rdx;
	uint64_t arg4 = f->R.r10;
	uint64_t arg5 = f->R.r8;
	uint64_t arg6 = f->R.r9;

#ifdef VM
	// thread_current()->rsp = f->rsp;
	thread_current()->rsp_snapshot = f->rsp;
#endif

	switch (syscall_num)
	{
	case SYS_HALT:
		sys_halt();
		break;
	case SYS_EXIT:
		sys_exit((int)arg1);
		break;
	case SYS_EXEC:
		f->R.rax = sys_exec((const char *)arg1);
		break;
	case SYS_WAIT:
		f->R.rax = sys_wait((int)arg1);
		break;
	case SYS_FORK:
		f->R.rax = sys_fork((const char *)arg1, f);
		break;
	case SYS_CREATE:
		f->R.rax = sys_create((const char *)arg1, (unsigned)arg2);
		break;
	case SYS_REMOVE:
		f->R.rax = sys_remove((const char *)arg1);
		break;
	case SYS_OPEN:
		f->R.rax = sys_open((const char *)arg1);
		break;
    case SYS_CLOSE:
		sys_close((int)arg1);
		break;
	case SYS_FILESIZE:
		f->R.rax = sys_filesize((int)arg1);
		break;
	case SYS_READ:
		f->R.rax = sys_read((int)arg1, (void *)arg2, (unsigned)arg3);
		break;
	case SYS_WRITE:
		f->R.rax = sys_write((int)arg1, (const void *)arg2, (unsigned)arg3);
		break;
	case SYS_SEEK:
		sys_seek((int)arg1, (unsigned)arg2);
		break;
	case SYS_TELL:
		f->R.rax = sys_tell(arg1);
		break;

	default:
		thread_exit();
		break;
	}
}

void check_address(void *addr)
{
	// 널 포인터는 사용할 수 없으므로 바로 종료
	if (addr == NULL)
		sys_exit(-1);

	// (!((uint64_t)((addr)) >= 0x8004000000))
	// addr이 유저 영역이 아닌 커널 주소를 건드리려고 하면 보안상 위험 → 종료
	if (!is_user_vaddr(addr))
		sys_exit(-1);

	// 현재 프로세스의 페이지 테이블(pml4)에 이 주소가 실제로 매핑되어 있는지 확인
	// 즉, 유저가 요청한 주소가 현재 유효한 가상 주소인지 확인
	if (pml4_get_page(thread_current()->pml4, addr) == NULL)
		sys_exit(-1);
}

static void sys_halt() {
	// Pintos 기본 제공 종료 함수
	power_off();
}

void sys_exit(int status)
{
	// 현재 실행 중인 스레드(프로세스)를 가져옴
	struct thread *cur = thread_current();

	// 종료 상태(status)를 현재 스레드에 저장
	// 부모 프로세스가 wait()로 이 값을 조회할 수 있도록 하기 위함
	cur->exit_status = status;

	// 종료 메시지 출력 (테스트 시 검증에 사용)
	// 예: "echo: exit(0)"
	printf("%s: exit(%d)\n", thread_name(), status);

	// 현재 스레드를 종료하고 정리 → scheduler에 의해 다른 스레드로 전환됨
	thread_exit();
}

static tid_t sys_exec(const char *cmd_line) {
	// 유효한 주소인지 검사
	validate_str(cmd_line, false);

	// 사용자로부터 받은 문자열(cmd_line)을 복사할 커널 영역의 페이지를 할당
	// PAL_ZERO는 할당된 메모리를 0으로 초기화하라는 의미
	char *cmd_line_copy = palloc_get_page(PAL_ZERO);
	
	// 만약 메모리 할당에 실패했다면, exit 처리
	if (cmd_line_copy == NULL) {
		sys_exit(-1);  // 시스템 콜 종료 코드로 -1을 반환
	}

	// 사용자 영역의 문자열을 커널 영역으로 안전하게 복사
	// PGSIZE는 한 페이지의 크기(보통 4KB)를 의미
	strlcpy(cmd_line_copy, cmd_line, PGSIZE);	

	// 실제로 새로운 프로그램을 현재 프로세스 위에 실행
	// 실패하면 -1을 반환하므로, exit 처리
	if (process_exec(cmd_line_copy) == -1) {
		sys_exit(-1);  // 실행 실패 시 종료
	}
	
	// 참고: 성공했다면 이 함수는 반환하지 않고, 새 프로그램으로 전환됨
}

int sys_wait(int pid)
{
	// 실제 로직은 process_wait() 내부에 있음
	return process_wait(pid);
}

tid_t sys_fork(const char *thread_name, struct intr_frame *f) {
	// 자식 프로세스에 넘겨줄 이름과 부모 프로세스 레지스터 상태를 인자로 전달
	return process_fork(thread_name, f);
}

static bool sys_create(const char *file, unsigned initial_size) {
	// 사용자 포인터가 유효한지 검사
	validate_ptr(file, 1, false);

	// 유저 영역에 있는 파일 이름 문자열을 커널 영역의 안전한 버퍼로 복사
	char kernel_buf[NAME_MAX + 1];  // 최대 이름 길이 + 널 문자 고려
    if (!copy_in(kernel_buf, file, sizeof kernel_buf)) {
        return false; // 문자열 복사 실패 → 파일 이름을 읽을 수 없으므로 실패
    }

	// 빈 문자열이면 파일 이름으로 부적절하므로 생성 불가
	if (strlen(kernel_buf) == 0) {
		return false;
	}

	// 루트 디렉토리 열기 → Pintos는 루트 디렉토리를 기본 작업 디렉토리로 사용
	struct dir *dir = dir_open_root();
	if (dir == NULL) {
		return false; // 루트 디렉토리 열기에 실패한 경우
	}

	struct inode *inode;

	// 동일한 이름의 파일이 이미 존재하는지 확인
    if (dir_lookup(dir, kernel_buf, &inode)) {
		dir_close(dir);  // 디렉토리 닫기
		return false;    // 이미 존재하는 파일 이름 → 생성 실패
	}
	
	// 파일 시스템 락을 획득한 후 파일 생성 시도 (동시성 보호)
	lock_acquire(&filesys_lock);
	bool success = filesys_create(kernel_buf, initial_size);
	lock_release(&filesys_lock);	// 작업 완료 이후 락 해제

	// 디렉토리 자원 정리
	dir_close(dir);

	// 파일 생성 성공 여부 반환
	return success;
}

static bool sys_remove(const char *file) {
	// 사용자 포인터가 유효한 사용자 영역 주소인지 검사
	validate_ptr(file, 1, false);

	// NULL 포인터가 넘어온 경우 삭제 실패
	if (file == NULL) {
		return false;
	}

	// 파일 시스템에서 해당 파일 삭제 시도 후 성공/실패 여부 반환
	return filesys_remove(file); 
}

static int sys_open(const char *file_name) {
	// 사용자 포인터가 유효한 사용자 영역 주소인지 검사
	validate_ptr(file_name, 1, false);

	// 파일 시스템 접근을 위한 락 획득
	lock_acquire(&filesys_lock);

	// 파일 시스템에서 파일 열기 시도
	struct file *file = filesys_open(file_name);

	// 파일이 없거나 열기에 실패한 경우 -1 반환
	if (file == NULL) {
		lock_release(&filesys_lock);
		return -1;
	}

	// 현재 프로세스의 파일 디스크립터 테이블(FDT)에 파일 등록
	int fd = process_add_file(file);

	// 파일 등록에 실패한 경우 → 열린 파일 닫기
	if (fd == -1)
		file_close(file);

	// 파일 시스템 락 해제
	lock_release(&filesys_lock);

	// 파일 디스크립터 번호 반환, 실패 시 -1 반환
	return fd;
}

static void sys_close(int fd) {
	// 파일 디스크립터를 통해 파일 객체 가져오기
	struct file *file = process_get_file(fd);

	// 유효하지 않거나 이미 닫힌 fd인 경우 return
	if (file == NULL) {
		return;
	}

	// 파일을 닫고 관련 자원 해제
	file_close(file);

	// 현재 스레드의 파일 디스크립터 테이블에서 해당 엔트리 비우기
	thread_current()->FDT[fd] = NULL;
}

static int sys_filesize(int fd) {
	// 파일 디스크립터 번호를 이용해 파일 객체 가져오기
	struct file *file = process_get_file(fd);

	// 유효하지 않은 fd이거나 파일이 열려 있지 않은 경우 -1 반환
	if (file == NULL) {
		return -1;
	}

	// 해당 파일의 크기(바이트 단위)를 반환
	return file_length(file);
}

static int sys_read(int fd, void *buffer, unsigned size) {
	// 사용자 버퍼 포인터가 유효한지 확인
	validate_ptr(buffer, size, true);

	// 버퍼를 문자 단위로 접근하기 위해 char 포인터로 변환
	char *ptr = (char *)buffer;
	int bytes_read = 0;

	// 파일 시스템 동시 접근 방지를 위한 락 획득
	lock_acquire(&filesys_lock);

	if (fd == STDIN_FILENO)  // 표준 입력일 경우
	{
		// 키보드 입력을 한 글자씩 읽어서 버퍼에 저장
		for (int i = 0; i < size; i++) {
			*ptr++ = input_getc();
			bytes_read++;
		}
		lock_release(&filesys_lock);
	}
	else
	{
		// stdout(1), stderr(2), 음수 등 읽을 수 없는 fd는 실패 처리
		if (fd < 3) {
			lock_release(&filesys_lock);
			return -1;
		}

		// 파일 디스크립터 테이블에서 파일 객체 가져오기
		struct file *file = process_get_file(fd);
		if (file == NULL) {
			lock_release(&filesys_lock);
			return -1;
		}

		// 파일에서 size만큼 읽어 버퍼에 저장
		bytes_read = file_read(file, buffer, size);

		lock_release(&filesys_lock);
	}

	// 읽은 바이트 수 반환 (0 이상)
	return bytes_read;
}

static int sys_write(int fd, const void *buffer, unsigned size) {
	// 사용자 버퍼 포인터가 유효한지 확인
	validate_ptr(buffer, size, false);

	// stdin(0), stderr(2)은 출력 대상이 아니므로 에러 처리
	if (fd == 0 || fd == 2) {
		return -1;
	}

	// stdout(1)인 경우 → 콘솔에 출력
	if (fd == 1)
	{
		putbuf(buffer, size);  // 버퍼 내용을 콘솔에 출력
		return size;           // 출력한 바이트 수 반환
	}
	
	// 일반 파일인 경우 → 해당 fd로 열린 파일 객체 조회
	struct file *file = process_get_file(fd);
	if (file == NULL)
		return -1;

	// 파일 시스템 접근을 위한 락 획득
	lock_acquire(&filesys_lock);

	// 파일에 버퍼 내용 쓰기
	int bytes_write = file_write(file, buffer, size);

	// 락 해제
	lock_release(&filesys_lock);

	// 쓰기 실패 시 -1 반환
	if (bytes_write < 0)
		return -1;

	// 성공한 경우 실제로 쓴 바이트 수 반환
	return bytes_write;
}

static void sys_seek(int fd, unsigned position) {
	// 파일 디스크립터를 통해 파일 객체 가져오기
	struct file *file = process_get_file(fd);

	// 유효하지 않은 fd이면 아무 작업도 하지 않고 종료
	if (file == NULL) {
		return;
	}

	// 파일의 읽기/쓰기 위치를 지정된 위치로 변경
	file_seek(file, position);
}

static unsigned sys_tell(int fd)
{
	// 파일 디스크립터를 통해 파일 객체 가져오기
	struct file *file = process_get_file(fd);

	// 유효하지 않은 fd이면 0 반환 (unsigned 타입이므로 -1 대신 0 사용)
	if (file == NULL)
		return 0;

	// 현재 파일의 읽기/쓰기 위치(offset)를 반환
	return file_tell(file);
}