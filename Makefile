# ------------------------------------------------------------------------
#  RAVYNOS BUILD SYSTEM - TOP LEVEL
# ------------------------------------------------------------------------

PROD_VERSION = 0.7.1
PROD_FAMILY = Pre Alpha

# Kernel configuration: RELEASE, DEVELOPMENT or DEBUG
KERNEL_CONFIGS ?= RELEASE

ROOT_SOURCE_DIR = ${.CURDIR}

# ------------------------------------------------------------------------
#  Host vs. target architecture
#
#  HostArch    - the CPU the toolchain runs on (from bmake's MACHINE).
#  TARGET_ARCH - the CPU the OS is built for. x86_64 (default) is the only
#                target with a complete userland today; arm64 is the
#                Raspberry Pi 5 (BCM2712) kernel bring-up target.
#                Override per invocation:  bmake TARGET_ARCH=arm64 -C Kernel
#
#  Derived, exported spellings of the target architecture:
#    MachOArch       x86_64 | arm64    (cctools/ld64/lipo/mig "-arch")
#    CpuArch         x86_64 | aarch64  (clang triples, FreeBSD-style mk)
#    BuildArch       X86 | AArch64     (LLVM target for the target runtimes)
#    ARCH_CONFIGS    X86_64 | ARM64    (xnu)
#    MACHINE_CONFIGS NONE | BCM2712    (xnu board config)
# ------------------------------------------------------------------------
.if ${MACHINE} == "x86_64" || ${MACHINE} == "amd64"
    HostArch = x86_64
.else
    HostArch = arm64
.endif

TARGET_ARCH ?= x86_64
.if ${TARGET_ARCH} == "arm64" || ${TARGET_ARCH} == "aarch64"
    MachOArch = arm64
    CpuArch = aarch64
    BuildArch = AArch64
    ARCH_CONFIGS = ARM64
    MACHINE_CONFIGS ?= BCM2712
.elif ${TARGET_ARCH} == "x86_64" || ${TARGET_ARCH} == "amd64"
    MachOArch = x86_64
    CpuArch = x86_64
    BuildArch = X86
    ARCH_CONFIGS = X86_64
    MACHINE_CONFIGS = NONE
.else
.error Unsupported TARGET_ARCH "${TARGET_ARCH}" (expected x86_64 or arm64)
.endif

# The host toolchain is always built with every target we may build for
LLVM_HOST_TARGETS = X86;AArch64

# Building as a normal user: install files owned by that user instead of
# root:wheel so that nothing in the build needs sudo.
_BUILD_UID != id -u
.if ${_BUILD_UID} != 0
NO_ROOT = 1
BINOWN != id -un
BINGRP != id -gn
.export NO_ROOT BINOWN BINGRP
.endif

MACHINE_CPUARCH = ${CpuArch}
MK_UNIFIED_OBJDIR = no
MK_AUTO_OBJ = yes

.SYSPATH: ${ROOT_SOURCE_DIR}/BSD/share/mk
.include "./BSD/share/mk/sys.mk"
.include "./BSD/share/mk/src.tools.mk"
.include "./BSD/share/mk/bsd.linker.mk"
.include "./BSD/share/mk/bsd.compiler.mk"

_ROOT_BINARY_DIR = ${ROOT_SOURCE_DIR}/../build
ROOT_BINARY_DIR = ${_ROOT_BINARY_DIR:tA}
SRCTOP = ${ROOT_SOURCE_DIR}
OBJTOP = ${ROOT_BINARY_DIR}
SRCROOT = ${ROOT_SOURCE_DIR}
OBJROOT = ${ROOT_BINARY_DIR}
MAKEOBJDIRPREFIX = ${ROOT_BINARY_DIR}

# ------------------------------------------------------------------------
#  Top level targets
# ------------------------------------------------------------------------

world: version Developer Kernel Libraries/libfirehose_kernel Libraries BSD

# ------------------------------------------------------------------------

# Don't warn about ravynOS.sdk vs MacOSX.sdk naming
CFLAGS = -Wno-incompatible-sysroot

# If we are building from a different OS, we want to use the host's tools
# to build our toolchain.
# Host tools build the toolchain and xnu's SETUP utilities; they always use
# the system compiler so they never depend on the toolchain being built.
.if "${.MAKE.OS}" == "Darwin"
# Wrappers in tools/host strip DEVELOPER_DIR (set for our own xcrun) so the
# Xcode shims in /usr/bin keep working while xnu's SETUP tools are built.
HOST_CC ?= ${ROOT_SOURCE_DIR}/tools/host/cc
HOST_CXX ?= ${ROOT_SOURCE_DIR}/tools/host/c++
HOST_AR ?= ${ROOT_SOURCE_DIR}/tools/host/ar
HOST_LD ?= ${ROOT_SOURCE_DIR}/tools/host/cc
HOST_FLEX ?= ${ROOT_SOURCE_DIR}/tools/host/flex
HOST_BISON ?= ${ROOT_SOURCE_DIR}/tools/host/bison
HOST_GM4 ?= ${ROOT_SOURCE_DIR}/tools/host/m4
HOST_CODESIGN ?= ${ROOT_SOURCE_DIR}/tools/host/codesign
HOST_CODESIGN_ALLOCATE ?= ${ROOT_SOURCE_DIR}/tools/host/codesign_allocate
# xnu's SETUP tools run on the host: build them against the real macOS SDK
HOST_SDKROOT != env -u DEVELOPER_DIR /usr/bin/xcrun --show-sdk-path 2>/dev/null || echo /
.else
HOST_CC ?= /usr/bin/cc
HOST_CXX ?= /usr/bin/c++
HOST_AR ?= /usr/bin/ar
HOST_LD ?= /usr/bin/ld
HOST_FLEX ?= flex
HOST_BISON ?= bison
HOST_GM4 ?= /usr/bin/m4
HOST_CODESIGN ?= /bin/true
HOST_CODESIGN_ALLOCATE ?= /bin/true
HOST_SDKROOT ?= /
.endif
.export HOST_CC HOST_CXX HOST_AR HOST_LD HOST_FLEX HOST_BISON HOST_GM4 \
	HOST_CODESIGN HOST_CODESIGN_ALLOCATE HOST_SDKROOT

# If we have a toolchain bundle, default to not building it again
# TOOLCHAIN is initially set to /Library/Developer/... by sys.mk
.if exists(${TOOLCHAIN})
MK_TOOLCHAIN ?= no
.else
MK_TOOLCHAIN ?= yes
.endif

.if ${MK_TOOLCHAIN} == "yes"
TOOLCHAIN = ${ROOT_BINARY_DIR}/Developer/Platforms/ravynOS.platform/Developer/Toolchains/Default.xctoolchain
.endif
TOOLS = ${TOOLCHAIN}/usr/bin
DEVEL = ${ROOT_SOURCE_DIR}/Developer

DARWIN_VERSION != head -1 ${ROOT_SOURCE_DIR}/Kernel/xnu/config/MasterVersion

LLVM_VERSION_MAJOR != grep 'set.LLVM_VERSION_MAJOR' Developer/Default.xctoolchain/llvm/llvm/CMakeLists.txt | sed -E 's/^.* ([0-9]+).*$$/\1/'
LLVM_VERSION_MINOR != grep 'set.LLVM_VERSION_MINOR' Developer/Default.xctoolchain/llvm/llvm/CMakeLists.txt | sed -E 's/^.* ([0-9]+).*$$/\1/'
LLVM_VERSION_PATCH != grep 'set.LLVM_VERSION_PATCH' Developer/Default.xctoolchain/llvm/llvm/CMakeLists.txt | sed -E 's/^.* ([0-9]+).*$$/\1/'

LLVM_VERSION = ${LLVM_VERSION_MAJOR}.${LLVM_VERSION_MINOR}.${LLVM_VERSION_PATCH}
LLVM_MAJOR = ${LLVM_VERSION_MAJOR}
LLVM_MINOR = ${LLVM_VERSION_MINOR}
LLVM_PATCH = ${LLVM_VERSION_PATCH}

PROD_MAJOR != echo ${PROD_VERSION} | sed 's/^([0-9]+\.\*)/\\1/'
PROD_MINOR != echo ${PROD_VERSION} | sed 's/^[0-9]+\.([0-9]+)\./\\1/'
PROD_PATCH != echo ${PROD_VERSION} | sed 's/([0-9]+)$$/\\1/'

DARWIN_MAJOR != echo ${DARWIN_VERSION} | sed 's/^([0-9]+\.\*)/\\1/'
DARWIN_MINOR != echo ${DARWIN_VERSION} | sed 's/^[0-9]+\.([0-9]+)\./\\1/'
DARWIN_PATCH != echo ${DARWIN_VERSION} | sed 's/([0-9]+)$$/\\1/'

MACOSX_DEPLOYMENT_TARGET = 10.15
MACOS_VERSION_MIN = 10.15

RUNTIME_SPEC_PATH = ${ROOT_SOURCE_DIR}/Developer/xcbuild/Specifications

XNU_SOURCE_DIR = ${ROOT_SOURCE_DIR}/Kernel/xnu
KEXT_SOURCE_DIR = ${ROOT_SOURCE_DIR}/Kernel/Extensions
SDK_SOURCE_DIR = ${ROOT_SOURCE_DIR}/Developer/ravynOS.sdk
PLATFORM_SOURCE_DIR = ${ROOT_SOURCE_DIR}/Developer/ravynOS.platform
RAVYN_SDKROOT = ${ROOT_BINARY_DIR}/Developer/Platforms/ravynOS.platform/Developer/SDKs/ravynOS.sdk
RAVYN_SDKROOT_MACOSX = ${ROOT_BINARY_DIR}/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk
# Each target architecture gets its own staging root so an arm64 kernel
# build never overwrites the x86_64 system.
.if ${MachOArch} == "x86_64"
SYSROOT_DIR = ${ROOT_BINARY_DIR}/sysroot
.else
SYSROOT_DIR = ${ROOT_BINARY_DIR}/sysroot-${MachOArch}
.endif

TARGET_TRIPLE = ${MachOArch}-apple-darwin${DARWIN_MAJOR}

SUBDIR ?= Developer .WAIT Kernel Libraries Frameworks BSD

.export ROOT_SOURCE_DIR ROOT_BINARY_DIR ARCH_CONFIGS KERNEL_CONFIGS \
	TARGET_ARCH HostArch MachOArch MACHINE_CONFIGS LLVM_HOST_TARGETS \
	PROD_VERSION PROD_FAMILY CFLAGS DEVEL DARWIN_VERSION \
	LLVM_VERSION_MAJOR LLVM_VERSION_MINOR LLVM_VERSION_PATCH \
	LLVM_VERSION PROD_MAJOR PROD_MINOR PROD_PATCH DARWIN_MAJOR \
	DARWIN_MINOR DARWIN_PATCH MACOS_VERSION_MIN MACOSX_DEPLOYMENT_TARGET \
	RUNTIME_SPEC_PATH OPSYS MACHINE BuildArch CpuArch XNU_SOURCE_DIR \
	KEXT_SOURCE_DIR SDK_SOURCE_DIR PLATFORM_SOURCE_DIR RAVYN_SDKROOT \
	RAVYN_SDKROOT_MACOSX SYSROOT_DIR TARGET_TRIPLE MAKEOBJDIRPREFIX \
	TOOLCHAIN TOOLS SRCTOP OBJTOP SRCROOT OBJROOT MK_AUTO_OBJ MK_UNIFIED_OBJDIR

version:
	${MAKE} -C ${.CURDIR}/SystemLibrary SystemVersion.plist

# ------------------------------------------------------------------------
#  Convenience targets for the arm64 (Raspberry Pi 5) bring-up. Always run
#  from this directory with TARGET_ARCH=arm64 so every exported variable
#  (SYSROOT_DIR, MachOArch, MACHINE_CONFIGS, ...) matches, e.g.
#     bmake TARGET_ARCH=arm64 KERNEL_CONFIGS=DEVELOPMENT kernel booter
# ------------------------------------------------------------------------
kernel: .PHONY
	${MAKE} -C ${.CURDIR}/Kernel xnu_all

booter: .PHONY
	${MAKE} -C ${.CURDIR}/Kernel/booter all

.include "./BSD/share/mk/bsd.subdir.mk"
