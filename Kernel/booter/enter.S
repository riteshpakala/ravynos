/*
 * Hand-off from the ravynOS booter to the xnu kernel on AArch64.
 *
 * Expectations of xnu's osfmk/arm64/start.s (_start / start_first_cpu):
 *   - EL1, MMU and caches off, interrupts masked, x0 = boot_args (physical)
 *   - the kernel image, boot_args and device tree are already in RAM and
 *     visible with caches off (so they must be cleaned to the point of
 *     coherency before the MMU goes down)
 *
 * UEFI hands us the CPU at EL2 on the Raspberry Pi 5 and QEMU virt (and
 * at EL1 on some firmware). From EL2 we also have to grant EL1 access to
 * the physical counter/timer, which xnu uses (CNTP_*), and make EL1
 * AArch64 with the standard reset values.
 */

	.text
	.balign 4

	.globl machine_current_el
machine_current_el:
	mrs	x0, CurrentEL
	lsr	x0, x0, #2
	ret

	.globl machine_cntfrq
machine_cntfrq:
	mrs	x0, CNTFRQ_EL0
	ret

	.globl machine_mpidr
machine_mpidr:
	mrs	x0, MPIDR_EL1
	ret

	.globl machine_midr
machine_midr:
	mrs	x0, MIDR_EL1
	ret

/* void machine_clean_dcache(void *start, size_t len): clean+invalidate to PoC */
	.globl machine_clean_dcache
machine_clean_dcache:
	mrs	x3, CTR_EL0
	ubfx	x3, x3, #16, #4
	mov	x4, #4
	lsl	x4, x4, x3		// x4 = D-cache line size
	sub	x5, x4, #1
	bic	x2, x0, x5		// x2 = aligned start
	add	x1, x0, x1		// x1 = end
1:	dc	civac, x2
	add	x2, x2, x4
	cmp	x2, x1
	b.lo	1b
	dsb	sy
	isb
	ret

/* Windows-ARM64 targets emit stack probes for big frames; the stack is fully committed. */
	.globl __chkstk
__chkstk:
	ret

/* void machine_enter_kernel(uint64_t entry_phys, uint64_t boot_args_phys) */
	.globl machine_enter_kernel
machine_enter_kernel:
	mov	x19, x0			// entry
	mov	x20, x1			// boot_args

	msr	DAIFSet, #0xf		// mask everything
	isb

	mrs	x0, CurrentEL
	lsr	x0, x0, #2
	cmp	x0, #2
	b.eq	from_el2

	/* Already at EL1: drop MMU and caches, then jump. */
	mrs	x0, SCTLR_EL1
	bic	x0, x0, #(1 << 0)	// M
	bic	x0, x0, #(1 << 2)	// C
	bic	x0, x0, #(1 << 12)	// I
	msr	SCTLR_EL1, x0
	isb
	tlbi	vmalle1
	dsb	sy
	isb
	mov	x0, x20
	br	x19

from_el2:
	/* Turn off the EL2 MMU/caches the firmware may have enabled. */
	mrs	x0, SCTLR_EL2
	bic	x0, x0, #(1 << 0)
	bic	x0, x0, #(1 << 2)
	bic	x0, x0, #(1 << 12)
	msr	SCTLR_EL2, x0
	isb
	tlbi	alle2
	dsb	sy
	isb

	/* EL1 is AArch64; no traps of interest routed to EL2 */
	movz	x0, #0x8000, lsl #16	// HCR_EL2.RW
	msr	HCR_EL2, x0

	/* Let EL1 use the physical counter and timer; no virtual offset. */
	mov	x0, #3			// EL1PCTEN | EL1PCEN
	msr	CNTHCTL_EL2, x0
	msr	CNTVOFF_EL2, xzr

	/* No FP/SIMD trapping to EL2, no system register trapping. */
	mov	x0, #0x33ff
	msr	CPTR_EL2, x0
	msr	HSTR_EL2, xzr

	/* Coprocessor / vector defaults for EL1 */
	movz	x0, #0x0838		// SCTLR_EL1 reset value: RES1 bits, MMU/caches off
	movk	x0, #0x30C5, lsl #16
	msr	SCTLR_EL1, x0

	/* Return to EL1h with everything masked */
	mov	x0, #0x3c5		// D A I F masked, EL1h
	msr	SPSR_EL2, x0
	msr	ELR_EL2, x19
	mov	x0, x20
	isb
	eret
