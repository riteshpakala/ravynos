# ------------------------------------------------------------------------
#  RAVYNOS BUILD SYSTEM - Kernel and Extensions
# ------------------------------------------------------------------------

AVAILABILITY_PL = ${RAVYN_SDKROOT}/usr/local/libexec/availability.pl
AVAILABILITY_PL_PATH = ${RAVYN_SDKROOT}/usr/local/libexec/availability.pl
SEED_DEFINES = -DRC_ENABLE_XNU_PRODUCT_INFO_FILTER
PATH := ${TOOLCHAIN}/usr/bin:${PATH}
# xnu compiles in parallel under gmake; bmake's own -j does not reach it
GMAKE_JOBS ?= ${.MAKE.JOBS:U8}
# XNU_VERBOSE=YES echoes every xnu compile/link command
XNU_VERBOSE ?= yes

# xnu names its output after the kernel configuration and, for board-specific
# builds, the machine configuration:
#   RELEASE/NONE      -> kernel
#   DEVELOPMENT/NONE  -> kernel.development
#   <cfg>/BCM2712     -> kernel.<cfg>.bcm2712
.if "${KERNEL_CONFIGS}" == "RELEASE"
DEBUG =
.else
DEBUG = .${KERNEL_CONFIGS:tl}
.endif
.if "${MACHINE_CONFIGS}" == "NONE"
KERNEL_NAME = kernel${DEBUG}
.else
KERNEL_NAME = kernel.${KERNEL_CONFIGS:tl}.${MACHINE_CONFIGS:tl}
.endif

# Per-target-architecture pieces. x86_64 keeps the historical layout; the
# arm64 bring-up target gets its own symbol root and kernel runtime
# libraries so it never disturbs the x86_64 SDK or sysroot.
XNU_ARCH_ARGS = ARCH_CONFIGS=${ARCH_CONFIGS} MACHINE_CONFIGS=${MACHINE_CONFIGS} \
	ARCH_STRING_FOR_CURRENT_MACHINE_CONFIG=${MachOArch}
# Everything xnu would otherwise ask xcrun for, so the build never depends on
# xcrun resolving SDKs or tools on the host.
XNU_HOST_ARGS = SDKROOT_RESOLVED=${RAVYN_SDKROOT} HOST_SDKROOT_RESOLVED=${HOST_SDKROOT} \
	HOST_GM4=${HOST_GM4} HOST_CODESIGN=${HOST_CODESIGN} \
	HOST_CODESIGN_ALLOCATE=${HOST_CODESIGN_ALLOCATE} \
	STRIP=${STRIPBIN} LIPO=${LIPO} LIBTOOL=${TOOLCHAIN}/usr/bin/libtool NM=${NM} \
	NMEDIT=${NMEDIT} DSYMUTIL=${DSYMUTIL} AR=${AR} AS=${AS} \
	CTFCONVERT=${CTFCONVERT} CTFMERGE=${CTFMERGE} CTFINSERT=${CTFINSERT} \
	MIG=${MIG} MIGCOM=${MIGCOM} IIG=${IIG} UNIFDEF=${UNIFDEF} \
	CC=${CC} CXX=${CXX} MIGCC=${CC}
.if ${MachOArch} == "x86_64"
XNU_SYMROOT = ${.OBJDIR}/xnu/sym
KERNEL_RUNTIME_DEP = compiler_rt
KERNEL_LIB_ARGS =
KERNEL_INSTALL_DIRS = ${RAVYN_SDKROOT}/System/Library/Kernels \
	${SYSROOT_DIR}/System/Library/Kernels
.else
XNU_SYMROOT = ${.OBJDIR}/xnu/sym-${MachOArch}
KERNEL_RUNTIME_DEP = kernel_runtime
KERNEL_LIBDIR = ${RAVYN_SDKROOT}/usr/local/lib/kernel/${MachOArch}
KERNEL_LIB_ARGS = LD_KERNEL_LIBS="-L${KERNEL_LIBDIR} -lcc_kext" \
	LD_KERNEL_ARCHIVES="-L${KERNEL_LIBDIR} -lfirehose_kernel" \
	LD_KERNEL_PROFILE_LIBS=
KERNEL_INSTALL_DIRS = ${SYSROOT_DIR}/System/Library/Kernels
.endif

.if "${.MAKE.OS}" ==  "Linux"
HOST_CODESIGN = /bin/true
IIG = /bin/true
HOST_LD ?= /usr/bin/ld
DKPATH = ${.OBJDIR}/xnu/${KERNEL_CONFIGS}_${ARCH_CONFIGS}/iokit/DriverKit

${DKPATH}: .NOTMAIN
	mkdir -p ${DKPATH}
	cp -Rf ${.CURDIR}/xnu/iokit/DriverKit/*.h ${DKPATH}/
TARGETS = ${DKPATH}
.endif
TARGETS += xnu_all

MAKEFLAGS=
.MAKEFLAGS=

PLATFORM_DIR=${ROOT_BINARY_DIR}/Developer/Platforms/ravynOS.platform

all: ${TARGETS}
installhdrs: .PHONY
	mkdir -p ${ROOT_BINARY_DIR}${.CURDIR}/xnu
	touch ${ROOT_BINARY_DIR}${.CURDIR}/xnu/.mach_kernel.timestamp
	touch ${ROOT_BINARY_DIR}${.CURDIR}/xnu/.symbolset.timestamp
	PATH="${PATH}" ${GMAKE} -j${GMAKE_JOBS} -C ${.CURDIR}/xnu LOGCOLORS=y PLATFORM=MacOSX BUILD_LTO=0 \
		BUILD_PROFILE=0 BUILD_WERROR=0 DO_CTFMERGE=0 \
		SDKVERSION=${MACOS_VERSION_MIN} SRCROOT=${.CURDIR}/xnu \
		OBJROOT=${.OBJDIR}/xnu DSTROOT=${RAVYN_SDKROOT} \
		MIGCOM=${MIGCOM} MIG=${MIG} IIG=${IIG} MIGCC=${CC} \
		CC=${CC} CXX=${CXX} HOST_OS_VERSION=${MACOS_VERSION_MIN} \
		UNIFDEF=${UNIFDEF} CTFCONVERT=${CTFCONVERT} \
		CTFMERGE=${CTFMERGE} CTFINSERT=${CTFINSERT} \
		HOST_SDKROOT=${RAVYN_SDKROOT} VERBOSE=${XNU_VERBOSE} \
		AVAILABILITY_PL=${AVAILABILITY_PL} SDKROOT=macosx \
		AVAILABILITY_PL_PATH=${AVAILABILITY_PL_PATH} XCRUN=${XCRUN} \
		KERNEL_CONFIGS=${KERNEL_CONFIGS} ${XNU_ARCH_ARGS} ${XNU_HOST_ARGS} \
		DEVELOPER_DIR=${ROOT_BINARY_DIR}/Developer HOST_FLEX=${HOST_FLEX} \
		SYMROOT=${ROOT_BINARY_DIR} HOST_CC=${HOST_CC} HOST_BISON=${HOST_BISON} \
		HOST_CXX=${HOST_CXX} HOST_LD=${HOST_LD} \
		installhdrs

libfirehose_kernel: .PHONY
	${MAKE} -C ${ROOT_SOURCE_DIR}/Libraries/libfirehose_kernel all

compiler_rt: .PHONY
	${MAKE} -C ${ROOT_SOURCE_DIR}/Libraries all

# Kernel-only compiler-rt builtins for the arm64 bring-up target
kernel_runtime: .PHONY
	${MAKE} -C ${ROOT_SOURCE_DIR}/Libraries kernel_runtime

# Arguments for every xnu gmake invocation of the kernel build
XNU_BUILD_ARGS = \
	PLATFORM=MacOSX BUILD_LTO=0 BUILD_WERROR=0 DO_CTFMERGE=0 \
	SDKVERSION=${MACOS_VERSION_MIN} SRCROOT=${.CURDIR}/xnu \
	OBJROOT=${.OBJDIR}/xnu DSTROOT=${RAVYN_SDKROOT} \
	SYMROOT=${XNU_SYMROOT} BUILD_STATIC_LINK=1 ${KERNEL_LIB_ARGS} \
	MIGCOM=${MIGCOM} MIG=${MIG} IIG=${IIG} CC=${CC} CXX=${CXX} \
	UNIFDEF=${UNIFDEF} CTFCONVERT=${CTFCONVERT} LD="${CC} --ld-path=${LD}" \
	CTFMERGE=${CTFMERGE} CTFINSERT=${CTFINSERT} AR=${AR} \
	NM=${NM} NMEDIT=${NMEDIT} DSYMUTIL=${DSYMUTIL} LIPO=${LIPO} \
	STRIP=${STRIPBIN} AS=${AS} AVAILABILITY_PL=${AVAILABILITY_PL} \
	MACHINE=${MACHINE} XCRUN=${XCRUN} SDKROOT=macosx \
	HOST_OS_VERSION=${MACOS_VERSION_MIN} VERBOSE=${XNU_VERBOSE} \
	AVAILABILITY_PL_PATH=${AVAILABILITY_PL_PATH} \
	DEVELOPER_DIR=${ROOT_BINARY_DIR}/Developer \
	LIBTOOL=${TOOLCHAIN}/usr/bin/libtool VERBOSE=${XNU_VERBOSE} \
	KERNEL_CONFIGS=${KERNEL_CONFIGS} ${XNU_ARCH_ARGS} ${XNU_HOST_ARGS} \
	HOST_CC=${HOST_CC} HOST_CXX=${HOST_CXX} HOST_LD=${HOST_LD}

xnu_all: installhdrs libfirehose_kernel ${KERNEL_RUNTIME_DEP}
	# "all" and "install" must not run in one gmake: with -j GNU make runs
	# both goals concurrently and xnu's setup timestamps race.
	PATH="${PATH}" ${GMAKE} -j${GMAKE_JOBS} -C ${.CURDIR}/xnu LOGCOLORS=y ${XNU_BUILD_ARGS} all
	PATH="${PATH}" ${GMAKE} -j${GMAKE_JOBS} -C ${.CURDIR}/xnu LOGCOLORS=y ${XNU_BUILD_ARGS} install
.for target_dir in ${KERNEL_INSTALL_DIRS}
	mkdir -p ${target_dir} ${target_dir:H}/Extensions
	rm -rf ${target_dir}/${KERNEL_NAME}.dSYM ${target_dir}/kernel.dSYM
	cp -Rf ${XNU_SYMROOT}/${KERNEL_NAME}.dSYM ${target_dir}/
	cp -f ${XNU_SYMROOT}/${KERNEL_NAME} ${target_dir}/
.if ${KERNEL_NAME} != "kernel"
	# Also install under the canonical name the booters look for
	cp -Rf ${XNU_SYMROOT}/${KERNEL_NAME}.dSYM ${target_dir}/kernel.dSYM
	cp -f ${XNU_SYMROOT}/${KERNEL_NAME} ${target_dir}/kernel
.endif
	cp -Rf ${XNU_SYMROOT}/System.kext ${target_dir:H}/Extensions/
.endfor

.if ${MachOArch} == "x86_64"
# NOTE! This kxld build is for the host toolchain. The target kxld is built
# later in IOKitUser
SUBDIR = xnu/libkern/kmod xnu/libkern/kxld
SUBDIR += IOKitUser
SUBDIR += Extensions
.else
# arm64: libkmod for the target, the AArch64 UEFI booter, then the kexts the
# booter hands to the kernel (linked at boot by kxld; no kernelcache tool).
SUBDIR = xnu/libkern/kmod .WAIT booter Extensions
.endif

.include <bsd.subdir.mk>
