#include "userprog/validate.h"
#include "userprog/syscall.h"     /* syscall_exit() */
#include "threads/thread.h"       /* thread_current(), pml4 */
#include "threads/vaddr.h"        /* PHYS_BASE, pg_ofs */
#include "threads/mmu.h"          /* PGSIZE */
#include "threads/pte.h"          /* pml4_get_page() */
#include <string.h>               /* memcpy */
#include "userprog/syscall.h"
#include "vm/vm.h"

/* 내부 헬퍼: 단일 가상 주소 uaddr이 
   - NULL이 아니고
   - 사용자 영역에 속하며
   - 현재 프로세스의 페이지 테이블에 매핑되어 있는지 확인 */
// static bool
// check_page (const void *uaddr) {
//     if (uaddr == NULL || !is_user_vaddr(uaddr))
// 		return false;

// 	void *va = pg_round_down(uaddr);
// 	struct supplemental_page_table *spt = &thread_current()->spt;
// 	return spt_find_page(spt, va) != NULL;
// }
static bool
check_page(const void *uaddr, bool write) {
    if (uaddr == NULL || !is_user_vaddr(uaddr))
        return false;

    void *va = pg_round_down(uaddr);
    struct supplemental_page_table *spt = &thread_current()->spt;
    struct page *page = spt_find_page(spt, va);

    if (page == NULL) {
        // 스택 성장 조건 만족 여부는 vm_try_handle_fault 내부에서 판단
        struct intr_frame fake_if;
        fake_if.rsp = thread_current()->rsp;  // 따로 저장해둔 rsp 값 필요
        bool ok = vm_try_handle_fault(&fake_if, va, true, write, true);
        if (!ok) return false;
        page = spt_find_page(spt, va);
    }

    if (page == NULL) return false;
    if (write && !page->writable) return false;

    return true;
}


/* 사용자 포인터 uaddr로부터 size 바이트까지의 주소 범위를
   페이지 단위로 하나씩 순회하며 모두 접근 가능한지 확인
   → 접근 불가능한 주소가 포함되어 있으면 프로세스를 종료함 (sys_exit(-1))
   → 사용 예: read(), write(), exec() 등에서 전달받은 사용자 버퍼 검증 */
void
validate_ptr (const void *uaddr, size_t size, bool write) {
    if (size == 0) return;  // 검사할 바이트 수가 0이면 return

    const uint8_t *usr = uaddr; // 현재 검사할 위치 (byte 단위 포인터)
    size_t left = size;         // 검사해야 할 남은 바이트 수

    // 검사할 전체 영역을 페이지 단위로 나누어 한 페이지씩 접근 가능 여부를 확인
    while (left > 0) {
        // 현재 usr 포인터가 가리키는 주소가 사용자 영역에 있고
        // 실제로 물리 메모리에 매핑되어 있는지 확인
        // void *page_va = pg_round_down(usr);

		// struct supplemental_page_table *spt = &thread_current()->spt;
		// struct page *page = spt_find_page(spt, page_va);

		// if (page == NULL) {
		// 	// 스택 확장 또는 lazy allocation을 유도하기 위한 claim 시도
		// 	if (!vm_claim_page(page_va))
		// 		sys_exit(-1);
		// }

        if (!check_page(usr, write))
            sys_exit(-1);
        // 현재 페이지에서 끝까지 남은 바이트 수 계산
        size_t page_left = PGSIZE - pg_ofs (usr);

        // 남은 전체 바이트와 현재 페이지에서 가능한 바이트 중 더 작은 만큼만 이동
        size_t chunk = left < page_left ? left : page_left;

        usr  += chunk;  // 검사할 포인터를 다음 영역으로 이동
        left -= chunk;  // 검사해야 할 남은 바이트 수 갱신
    }
}

/* 사용자 문자열 str이 \0을 만날 때까지 매 바이트 접근 가능한지 확인
   → 문자열 전체가 사용자 영역 내에 안전하게 존재해야 함
   → 접근 불가능한 주소를 만나면 프로세스를 종료함 (sys_exit(-1))
   → 사용 예: exec("..."), open("...") 등에서 문자열 인자 검증 */
void validate_str(const char *str, bool write) {
	for (const char *p = str;; ++p) {
		validate_ptr(p, 1, write);  // 현재 문자가 존재하는 1바이트 주소가 유효한지 확인
		if (*p == '\0') break;  // 문자열 끝(널 문자)에 도달하면 검사 종료
	}
}

/* 사용자 영역 주소 uaddr에서 단일 바이트를 안전하게 읽기
   → 예외(page fault) 발생 시 -1 반환 */
int64_t get_user (const uint8_t *uaddr) {
    int64_t result;
    __asm __volatile (
        "movabsq $done_get, %0\n"   // 예외 발생 시 점프할 주소를 %0에 저장
        "movzbq %1, %0\n"           // uaddr이 가리키는 1바이트 값을 읽어 zero-extend 후 %0에 저장
        "done_get:\n"               // 예외 발생 시 이곳으로 점프하여 결과 처리
        : "=&a" (result)            // 출력: result에 저장됨 (%rax 사용)
        : "m" (*uaddr));            // 입력: uaddr이 가리키는 메모리 바이트
    return result;
}

/* 사용자 영역 주소 udst에 단일 바이트 byte를 안전하게 쓰기
   → 예외(page fault) 발생 시 false 반환 */
bool put_user (uint8_t *udst, uint8_t byte) {
    int64_t error_code;  // 예외 발생 여부 확인을 위한 변수

    printf("[put_user] trying to write to %p\n", udst);  // 디버깅용 출력

    __asm __volatile (
        "movabsq $done_put, %0\n"  // 예외 발생 시 복구할 위치 주소를 %0에 저장
        "movb %b2, %1\n"           // byte 값을 udst가 가리키는 주소에 저장 시도 (1바이트)
        "done_put:\n"              // 예외 발생 시 여기로 복귀
        : "=&a" (error_code),      // 출력: %rax에 저장될 복구 주소 → 예외가 없으면 그대로 통과
          "=m" (*udst)             // 출력 메모리 위치: 실제 쓰기 대상
        : "q" (byte));             // 입력: 저장할 바이트 값

    return error_code != -1;       // 예외 발생 시 false, 아니면 true 반환
}

/* 사용자 영역에서 커널 영역으로 size 바이트만큼 메모리를 복사
   → 복사 전 대상 범위를 validate_ptr()로 검증 */
size_t
copy_in (void *kernel_dst, const void *user_src, size_t size) {
    validate_ptr (user_src, size, false);           // 복사 전에 사용자 포인터 범위 검증
    memcpy (kernel_dst, user_src, size);     // 검증 완료 → 커널 영역으로 복사 수행
    return size;                             // 실제 복사한 바이트 수 반환
}

/* 커널 영역에서 사용자 영역으로 size 바이트만큼 메모리를 복사
   → 복사 전 대상 범위를 validate_ptr()로 검증 */
size_t
copy_out (void *user_dst, const void *kernel_src, size_t size) {
    validate_ptr (user_dst, size, true);           // 복사 전에 사용자 목적지 포인터 범위 검증
    memcpy (user_dst, kernel_src, size);     // 검증 완료 → 사용자 영역으로 복사 수행
    return size;                             // 실제 복사한 바이트 수 반환
}