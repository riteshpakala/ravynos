# ------------------------------------------------------------------------
#  RAVYNOS BUILD SYSTEM - ravyninit (freestanding process 1 for arm64 bring-up)
#
#  Installed as ${SYSROOT_DIR}/sbin/ravyninit. MaryPi copies it to
#  /sbin/launchd on the root partition while no real launchd exists.
# ------------------------------------------------------------------------

PROG = ravyninit
OBJDIR = ${.OBJDIR}
CC_INIT = ${TOOLS}/clang

INIT_CFLAGS = -target ${TARGET_TRIPLE} -mmacos-version-min=${MACOS_VERSION_MIN} -ffreestanding -fno-builtin -nostdlib \
	-fno-stack-protector -fno-asynchronous-unwind-tables -O2 -g -Wall \
	-Wno-unused-function

all: ${OBJDIR}/${PROG}

${OBJDIR}/ravyninit.o: ${.CURDIR}/ravyninit.c
	${CC_INIT} ${INIT_CFLAGS} -c ${.CURDIR}/ravyninit.c -o ${.TARGET}

${OBJDIR}/start.o: ${.CURDIR}/start.S
	${CC_INIT} ${INIT_CFLAGS} -c ${.CURDIR}/start.S -o ${.TARGET}

${OBJDIR}/${PROG}: ${OBJDIR}/ravyninit.o ${OBJDIR}/start.o
	${CC_INIT} -target ${TARGET_TRIPLE} -mmacos-version-min=${MACOS_VERSION_MIN} -nostdlib -static -Wl,-e,_start \
		-fuse-ld=${LD} -o ${.TARGET} ${OBJDIR}/start.o ${OBJDIR}/ravyninit.o
	mkdir -p ${SYSROOT_DIR}/sbin
	cp -f ${.TARGET} ${SYSROOT_DIR}/sbin/${PROG}
	@echo "ravyninit: ${SYSROOT_DIR}/sbin/${PROG}"

clean:
	rm -f ${OBJDIR}/*.o ${OBJDIR}/${PROG}

.include <bsd.obj.mk>
