/*
 * Cross building xnu from Linux using my ravynOS.sdk and toolchain works
 * up to the final link, which has a few unresolved symbols that seem to be
 * artifacts of the hybrid build environment. For now, we try to resolve them
 * with aliases and stubs, and hope it works.
 * -- zoe 1/24/26
 */

#if defined(__x86_64__)
#include <architecture/i386/asm_help.h>

.text

.globl ___ulock_wait
___ulock_wait = _ulock_wait

.globl ___ulock_wake
___ulock_wake = _ulock_wake

LEAF(___cxa_atexit, 0)
        xorq %rax, %rax
        ret

X_LEAF(_OSSpinLockLock, _OSSpinLockTry)

#elif defined(__arm64__)

.text

.globl ___ulock_wait
___ulock_wait = _ulock_wait

.globl ___ulock_wake
___ulock_wake = _ulock_wake

.globl ___cxa_atexit
.p2align 2
___cxa_atexit:
        mov     x0, #0
        ret

/*
 * The x86_64 archive folds in libplatform for OSSpinLock*; arm64 has no
 * userland libraries yet, so provide the three spin-lock primitives here
 * (acquire/release semantics, LDAXR/STXR loop).
 */
.globl _OSSpinLockLock
.p2align 2
_OSSpinLockLock:
1:      ldaxr   w1, [x0]
        cbnz    w1, 1b
        mov     w2, #1
        stxr    w3, w2, [x0]
        cbnz    w3, 1b
        ret

.globl _OSSpinLockTry
.p2align 2
_OSSpinLockTry:
1:      ldaxr   w1, [x0]
        cbnz    w1, 2f
        mov     w2, #1
        stxr    w3, w2, [x0]
        cbnz    w3, 1b
        mov     w0, #1
        ret
2:      clrex
        mov     w0, #0
        ret

.globl _OSSpinLockUnlock
.p2align 2
_OSSpinLockUnlock:
        stlr    wzr, [x0]
        ret

/*
 * bsd/kern/kern_pgo.c (CONFIG_PGO in the DEVELOPMENT kernel) references the
 * compiler-rt profile runtime, which is not built for the arm64 kernel.
 * These stubs report "no profile data" until it is.
 */
.globl ___llvm_profile_get_size_for_buffer_internal
.p2align 2
___llvm_profile_get_size_for_buffer_internal:
        mov     x0, #0
        ret

.globl ___llvm_profile_write_buffer_internal
.p2align 2
___llvm_profile_write_buffer_internal:
        mov     w0, #0
        ret

#else
#error Unsupported platform
#endif
