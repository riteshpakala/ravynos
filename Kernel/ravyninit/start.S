/*
 * Process entry for a static Darwin arm64 executable: the kernel starts us
 * with sp pointing at argc; there is no dyld and no libSystem.
 */
	.text
	.globl _start
	.p2align 2
_start:
	mov	x29, #0
	mov	x30, #0
	mov	x9, sp
	and	x9, x9, #~15
	mov	sp, x9
	bl	_ravyninit_main
1:	mov	x16, #1			// exit(0) if the shell ever returns
	mov	x0, #0
	svc	#0x80
	b	1b
