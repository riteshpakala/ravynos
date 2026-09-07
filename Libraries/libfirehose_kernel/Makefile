.PATH: ${.CURDIR}/../Libsystem/libdispatch/src/firehose

LIB  = libfirehose_kernel
SRCS = firehose_buffer.c hack.s
OBJS = firehose_buffer.o hack.o

MK_WERROR = no
SUBDIR =

DEFINES = -DKERNEL=1 -DDISPATCH_USE_DTRACE=0 \
	  -DOS_ATOMIC_CONFIG_MEMORY_ORDER_DEPENDENCY=1 \
	  -DOS_ATOMIC_CONFIG_STARVATION_FREE_ONLY=0
# Kernel headers on arm64 need the same board configuration xnu is built
# with (see xnu makedefs/MakeInc.def CFLAGS_ARM64 / MACHINE_FLAGS_ARM64_*)
.if ${MachOArch} == "arm64"
DEFINES += -Darm64 -DARM64 -D__ARM64__ -DLP64 -DPAGE_SIZE_FIXED \
	  -DARM64_BOARD_CONFIG_${MACHINE_CONFIGS}
.endif

INCLUDES = -I${ROOT_SOURCE_DIR}/Kernel/xnu/osfmk \
	  -I${ROOT_SOURCE_DIR}/Kernel/xnu/libkern \
	  -I${ROOT_SOURCE_DIR}/Kernel/xnu/libkern/firehose \
	  -I${ROOT_SOURCE_DIR}/Kernel/xnu/bsd \
	  -I${.CURDIR}/../Libsystem/private \
	  -I${SDK_SOURCE_DIR}/usr/include \
	  -I${RAVYN_SDKROOT}/usr/local/include \
	  -I${RAVYN_SDKROOT}/usr/local/include/kernel

CFLAGS = --sysroot=${RAVYN_SDKROOT} -arch ${MachOArch} ${DEFINES} ${INCLUDES} \
	  -mmacos-version-min=${MACOS_VERSION_MIN} -mkernel -Wno-packed \
	  -Wno-sign-conversion -Wno-implicit-function-declaration \
	  -Wno-cast-align

ACFLAGS = --sysroot=${RAVYN_SDKROOT} -arch ${MachOArch} ${DEFINES} ${INCLUDES} \
	  -mkernel -mmacos-version-min=${MACOS_VERSION_MIN}

# x86_64 keeps its historical location; other architectures get a
# per-arch subdirectory so an arm64 kernel build never clobbers it. The
# x86_64 archive also folds in userland libraries to satisfy stray symbols
# in the hybrid build; those libraries do not exist for arm64 yet.
.if ${MachOArch} == "x86_64"
LIBDIR = ${RAVYN_SDKROOT}/usr/local/lib/kernel
EXTRA_LIBS = -L${RAVYN_SDKROOT}/usr/local/lib/dyld -lplatform \
	  -L${RAVYN_SDKROOT}/usr/local/lib/kernel -lkernel \
	  -L${RAVYN_SDKROOT}/usr/lib -L${RAVYN_SDKROOT}/usr/lib/system \
	  -lc++ -lc++abi ${RAVYN_SDKROOT}/usr/lib/libclang_rt.osx.a
.else
LIBDIR = ${RAVYN_SDKROOT}/usr/local/lib/kernel/${MachOArch}
EXTRA_LIBS =
.endif

all: ${LIBDIR}/${LIB}.a

${LIBDIR}/${LIB}.a: ${OBJS}
	mkdir -p ${LIBDIR}
	${TOOLS}/libtool -static \
	  -o ${.TARGET} -no_warning_for_no_symbols ${OBJS} ${EXTRA_LIBS}

.include <bsd.lib.mk>

