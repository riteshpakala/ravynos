# ravynOS AArch64 UEFI booter (`bootaa64.efi`)

A freestanding UEFI application that loads the ravynOS arm64 kernel on
Raspberry Pi 5 (rpi5-uefi firmware) and on QEMU's `virt` machine (EDK2
ArmVirt). It plays the role iBoot plays on Apple hardware: it reads the
kernel from the boot volume, builds the flattened device tree and the
`boot_args` structure xnu expects, leaves UEFI and jumps to the kernel at
EL1 with the MMU off.

Built by `bmake TARGET_ARCH=arm64 -C Kernel/booter` with the ravynOS
toolchain's clang (`--target=aarch64-unknown-windows`) and `lld-link`, so no
EFI SDK is needed. Output: `../build/booter/bootaa64.efi`, also installed
to `../build/sysroot-arm64/System/Library/CoreServices/`.

## Boot volume layout

The card image written by [MaryPi](https://github.com/rao-studios/MaryPi)
has an MBR with a FAT32 first partition carrying:

```
EFI/BOOT/BOOTAA64.EFI            this booter
ravynos/com.ravynos.boot.plist   boot configuration (below)
ravynos/kernel                   arm64 XNU Mach-O (payload level 1)
ravynos/kernelcache              prelinked kernel + kexts (payload level 2)
bcm2712-rpi-5-b.dtb              fallback FDT if the firmware publishes none
config.txt, RPI_EFI.fd           Raspberry Pi firmware + UEFI (not used by the booter)
```

## `com.ravynos.boot.plist`

The schema mirrors Apple's `com.apple.Boot.plist`. Both keys are optional.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `Kernel` | string | `\ravynos\kernel` | Backslash path of the kernel on the boot volume. Forward slashes are accepted. If the file is missing, `\ravynos\kernelcache` is tried. |
| `Kernel Flags` | string | `-v serial=3 debug=0x8 cpus=1` | Copied verbatim into `boot_args.CommandLine` (max 607 bytes). See `Docs/BootArgs.md`. |

```xml
<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0"><dict>
    <key>Kernel</key>       <string>\ravynos\kernel</string>
    <key>Kernel Flags</key> <string>-v serial=3 debug=0x8 cpus=1</string>
</dict></plist>
```

`serial=3` is what makes the kernel talk on the PL011; without it there is
no console at all until a framebuffer driver exists. Keep `debug` to `0x8`
(kprintf): `0x40` (DB_ARP) makes the serial KDP stub wait for a remote
debugger before IOKit starts.

## What the booter does

1. Reads the plist and the kernel (`macho.c`): the kernel is a static,
   position-independent Mach-O linked at `0xfffffff007004000`. It is loaded
   unslid: each segment goes to `physBase + (vmaddr - 0xfffffff000000000)`.
2. Finds the hardware (`fdt.c`, `main.c: discover_machine`): the firmware's
   FDT from the EFI configuration table (`EFI_DT_TABLE_GUID`), or the
   `bcm2712-rpi-5-b.dtb` file on the card. It needs a PL011 (`arm,pl011`,
   preferring `/chosen/stdout-path`), a GICv2 (`arm,gic-400` or
   `arm,cortex-a15-gic`) and the `/cpus` nodes. Bus addresses are translated
   through parent `ranges`, which matters on the Pi 5 (`/soc` maps
   `0x7c000000` to `0x107c000000`).
3. Chooses RAM: the largest contiguous run of usable UEFI memory (everything
   except Reserved, Unusable, MMIO and PalCode), 2 MiB aligned. That run is
   `[physBase, physBase + memSize)`; the kernel will own all of it.
4. Lays out memory and claims it from UEFI with `AllocatePages(AllocateAddress)`:

   ```
   physBase + 0x7004000   kernel segments (__TEXT first)
   16 KiB aligned         Apple device tree
   16 KiB aligned         boot_args
   16 KiB aligned         topOfKernelData: 8 pages of bootstrap page tables built by xnu
   ```

5. Builds the Apple flattened device tree (`appledt.c`), fills `boot_args`
   (revision 2), takes the GOP framebuffer into `Video` if there is one.
6. `ExitBootServices`, switches its own console to the raw PL011, cleans the
   data cache over everything it wrote, and enters the kernel (`enter.S`):
   from EL2 it sets `HCR_EL2.RW`, grants EL1 the physical timer
   (`CNTHCTL_EL2.EL1PCTEN|EL1PCEN`, `CNTVOFF_EL2 = 0`), disables EL2 traps,
   resets `SCTLR_EL1`, and `eret`s to EL1h with all exceptions masked and
   `x0 = boot_args` (physical). From EL1 it just turns the MMU off and branches.

## Device tree the kernel sees

xnu's pexpert looks nodes up by their `name` property (`DTFindEntry`) or by
path (`DTLookupEntry`), so every node carries `name`. Only what the kernel
reads is emitted:

```
device-tree               model, target-type ("Pi5" | "QEMUvirt"), compatible, platform-name
  chosen                  debug-enabled=1, boot-args
    memory-map            DeviceTree={paddr,size}, BootArgs={paddr,size}   (uint64 pairs)
  cpus
    cpu0                  cpu-id=0, reg=MPIDR&0xffffff, state="running", cluster-id=0,
                          timebase-frequency=CNTFRQ_EL0, clock-frequency, bus-frequency, ...
  arm-io                  device_type="bcm2712-io" | "qemu-virt-io", ranges={0, soc_base, size},
                          chip-revision=0
    pl011                 reg={offset, size}          (offsets from soc_base)
    interrupt-controller  interrupt-controller="master", reg={gicd_off, gicd_size, gicc_off, gicc_size}
  defaults
```

Consumers in the kernel:

- `pexpert/arm/pe_identify_machine.c`: `/arm-io` `device_type` and `ranges`
  (`pe_arm_get_soc_base_phys`), `/cpus/*` frequencies, the
  `interrupt-controller` node (`pe_arm_map_interrupt_controller`), and for
  `bcm2712-io`/`qemu-virt-io` the GICv2 bring-up in `pe_gicv2.c`.
- `pexpert/arm/pe_serial.c`: the `pl011` node.
- `osfmk/arm64/machine_routines.c`: `/cpus/cpuN` (`cpu-id`, `reg`, `state`).
- `pexpert/arm/pe_init.c`: root `model`/`target-type`, `/chosen/debug-enabled`.
- `iokit/Kernel/IODeviceTreeSupport.cpp`: `/chosen/memory-map/DeviceTree`
  (16-byte entries are informational; the 8-byte form would make IOKit free
  the tree).

Only the boot CPU is described until PSCI `CPU_ON` support exists; boot
with `cpus=1`.

## Debugging

The booter prints to the UEFI console and, after `ExitBootServices`, to the
PL011 directly. A panic prints `booter panic: ...` and parks the CPU in
`wfe`. Useful checks when the kernel does not come up:

- `running at EL2` is expected on rpi5-uefi and QEMU.
- `kernel region ... does not fit` or `cannot claim ...`: the largest RAM run
  is too small or the firmware sits inside it; the memory map is printed.
- No output after `entering kernel`: the kernel started but `serial=3` is
  missing from the flags, or the PL011 offset is wrong for this board.
