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

/* Interrupt controller: GICv2 driver in pexpert/arm/pe_gicv2.c */
#define GICV2 1

#endif /* ! ASSEMBLER */

#endif /* ! _PEXPERT_ARM_BCM2712_H */
