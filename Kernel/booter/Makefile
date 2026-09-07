# ------------------------------------------------------------------------
#  RAVYNOS BUILD SYSTEM - AArch64 UEFI booter (bootaa64.efi)
#
#  A freestanding PE/COFF EFI application built with the ravynOS toolchain's
#  clang and lld-link, targeting aarch64-unknown-windows so no external EFI
#  SDK is needed. Output: ${ROOT_BINARY_DIR}/booter/bootaa64.efi, also
#  installed into ${SYSROOT_DIR}/System/Library/CoreServices/.
# ------------------------------------------------------------------------

BOOTER = bootaa64.efi
OUTDIR = ${ROOT_BINARY_DIR}/booter
OBJDIR = ${.OBJDIR}

SRCS = main.c string.c console.c plist.c fdt.c appledt.c macho.c
ASRCS = enter.S
OBJS = ${SRCS:.c=.o} ${ASRCS:.S=.o}

BOOTER_CC = ${TOOLS}/clang
BOOTER_LD = ${TOOLS}/lld-link

TARGET_FLAGS = --target=aarch64-unknown-windows -ffreestanding -fno-builtin \
	-fno-stack-protector -fshort-wchar -mgeneral-regs-only -fno-common \
	-fno-asynchronous-unwind-tables -fno-exceptions -nostdlibinc \
	-I${.CURDIR} \
	-O2 -g -Wall -Wextra -Wno-unused-parameter \
	-DBOOTER_DARWIN_VERSION=\"${DARWIN_VERSION}\" \
	-DBOOTER_PROD_VERSION=\"${PROD_VERSION}\"

all: ${OUTDIR}/${BOOTER}

.for src in ${SRCS}
${src:.c=.o}: ${.CURDIR}/${src} ${.CURDIR}/booter.h ${.CURDIR}/efi.h
	${BOOTER_CC} ${TARGET_FLAGS} -c ${.CURDIR}/${src} -o ${.TARGET}
.endfor

.for src in ${ASRCS}
${src:.S=.o}: ${.CURDIR}/${src}
	${BOOTER_CC} ${TARGET_FLAGS} -c ${.CURDIR}/${src} -o ${.TARGET}
.endfor

${OUTDIR}/${BOOTER}: ${OBJS}
	mkdir -p ${OUTDIR}
	${BOOTER_LD} /machine:arm64 /subsystem:efi_application /entry:efi_main \
		/nodefaultlib /out:${.TARGET} ${OBJS}
	mkdir -p ${SYSROOT_DIR}/System/Library/CoreServices
	cp -f ${.TARGET} ${SYSROOT_DIR}/System/Library/CoreServices/${BOOTER}
	@echo "booter: ${.TARGET}"

clean:
	rm -f ${OBJS} ${OUTDIR}/${BOOTER}

.include <bsd.obj.mk>
