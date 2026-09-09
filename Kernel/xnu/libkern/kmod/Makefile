MK_WERROR = no
SUBDIR =
LIB = kmod
SRCS = c_start.c c_stop.c pure.c

CFLAGS = --sysroot=${RAVYN_SDKROOT} -mmacos-version-min=${MACOS_VERSION_MIN} \
	-I${RAVYN_SDKROOT}/usr/local/include/kernel \
	-I${ROOT_SOURCE_DIR}/Frameworks

.if ${MachOArch} == "x86_64"
LIBDIR = ${RAVYN_SDKROOT}/usr/local/lib/kernel
.else
# cross-built for the arm64 kernel; kept apart from the x86_64 library
CFLAGS += -target ${TARGET_TRIPLE}
LIBDIR = ${RAVYN_SDKROOT}/usr/local/lib/kernel/${MachOArch}
.endif

all: copyfile

copyfile: lib${LIB}.a
	mkdir -p ${LIBDIR}
	cp -f ${.OBJDIR}/lib${LIB}.a ${LIBDIR}

.include <bsd.lib.mk>
