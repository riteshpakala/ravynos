/*
 * Raspberry Pi 5 (Broadcom BCM2712) board definitions for ravynOS.
 *
 * Unlike the BCM2837 (Raspberry Pi 3) bring-up configuration, the Pi 5 is
 * entered through UEFI firmware (rpi5-uefi), talks over an ARM PrimeCell
 * PL011 UART and uses an ARM GIC-400 (GICv2) interrupt controller. Every
 * register base comes from the device tree the ravynOS booter builds from
 * the firmware's FDT; nothing in this header is hard-wired to a physical
 * address, so the same kernel boots under QEMU's "virt" machine.
 */

#ifndef _PEXPERT_ARM_BCM2712_H
#define _PEXPERT_ARM_BCM2712_H

#define NO_MONITOR 1
#define NO_ECORE 1

#ifndef ASSEMBLER

/* Serial console: PL011 driver in pexpert/arm/pe_serial.c */
#define PL011_UART 1

/* Interrupt controller: GICv2 or GICv3, chosen at boot (pexpert/arm/pe_gic.c) */
#define PE_GIC 1

/*
 * Cortex-A76 implements ARMv8.1 PAN (Privileged Access Never) and the kernel
 * takes exceptions with PAN set; without this the kernel never clears it in
 * copyin/copyout and every access to user memory faults forever.
 */
#define __ARM_PAN_AVAILABLE__ 1

#endif /* ! ASSEMBLER */

#endif /* ! _PEXPERT_ARM_BCM2712_H */
