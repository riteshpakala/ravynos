.include <bsd.suffixes.mk>

.iig.cpp .iig.h .iig.o:
	@echo '[WARNING] NOT processing IIG file ${.IMPSRC} -> ${.TARGET}'

_KEXT_FOLDER = ${.OBJDIR}/${KEXT}.kext
_KEXT_LIB = ${_KEXT_FOLDER}/Contents/MacOS/${KEXT}

.if defined(RAVYN_SDKROOT)
SDKROOT = ${RAVYN_SDKROOT}
.endif

.PATH: ${.OBJDIR}
SRCS += kmod_info.c
OBJS = ${SRCS:C/\..*$/.o/}

CFLAGS += -DKERNEL --sysroot=${SDKROOT} -I${SDKROOT}/usr/include \
	-I${SDKROOT}/usr/local/include -I${SDKROOT}/usr/local/include/kernel
CXXFLAGS += -fapple-kext ${CFLAGS}
LDFLAGS += -nostdlib -Wl,-bundle -Wl,-undefined,dynamic_lookup \
	-Wl,-kext -Wl,-segalign,${KEXT_SEGALIGN}
# the arm64 bring-up kernel uses 16 KiB pages; kexts linked at boot must keep
# every segment page aligned so their protections can be set per segment
.if ${MachOArch} == "x86_64"
KEXT_SEGALIGN = 0x1000
.else
KEXT_SEGALIGN = 0x4000
.endif

.if defined(RPATHS)
.for rpath in ${RPATHS}
LDFLAGS += -Wl,-rpath,${rpath}
.endfor
.endif

.if "${INSTALL_NAME_DIR}" != ""
LDFLAGS += -Wl,-install_name,${INSTALL_NAME_DIR}/${KEXT}
.elsif "${INSTALL_NAME}" != ""
LDFLAGS += -Wl,-install_name,${INSTALL_NAME}
.endif

.if ${MachOArch} != "x86_64"
# Cross-compiled kext (arm64 bring-up): explicit target, per-arch libkmod.
CFLAGS += -target ${TARGET_TRIPLE}
LDFLAGS += -target ${TARGET_TRIPLE}
# osfmk/arm/machine_routines.h needs IOInterruptHandler, which the kernel's
# pexpert.h pulls in; the userland copy under System.framework/PrivateHeaders
# (which some kexts put on their include path) does not. Search ours first.
CFLAGS := -I${ROOT_SOURCE_DIR}/Kernel/xnu/pexpert -I${ROOT_SOURCE_DIR}/Kernel/xnu/iokit ${CFLAGS}
# kernel-private headers (proc_reg.h) select the board through this define
CFLAGS += -DARM64_BOARD_CONFIG_${MACHINE_CONFIGS}
# the bring-up kernel is CONFIG_EMBEDDED: no reserved vtable slots in libkern
# classes, so kext class layouts must be built the same way
CFLAGS += -DAPPLE_KEXT_VTABLE_PADDING=0
_KMOD_LIBDIR = ${SDKROOT}/usr/local/lib/kernel/${MachOArch}
.else
_KMOD_LIBDIR = ${SDKROOT}/usr/local/lib/kernel
.endif

.if defined(KERNEL_PRIVATE)
CFLAGS += -DKERNEL_PRIVATE
LDFLAGS += -L${_KMOD_LIBDIR} -lkmod
.endif

.if defined(MACOS_VERSION_MIN)
CFLAGS += -mmacos-version-min=${MACOS_VERSION_MIN}
LDFLAGS += -mmacos-version-min=${MACOS_VERSION_MIN}
.endif

.if defined(MAIN_FUNCTION)
MAIN_FUNCTION_DECL = extern kern_return_t ${MAIN_FUNCTION}(kmod_info_t *ki, void *data);
.else
MAIN_FUNCTION = 0
.endif

.if defined(ANTIMAIN_FUNCTION)
ANTIMAIN_FUNCTION_DECL = extern kern_return_t ${ANTIMAIN_FUNCTION}(kmod_info_t *ki, void *data);
.else
ANTIMAIN_FUNCTION = 0
.endif

all: ${_KEXT_LIB}

${_KEXT_FOLDER}:
	mkdir -p ${.TARGET}/Contents/MacOS

${_KEXT_FOLDER}/Contents/Info.plist: ${.CURDIR}/${INFO_PLIST}
	sed -e 's/@BUNDLE_IDENTIFIER@/${BUNDLE_IDENTIFIER}/' \
	    -e 's/@BUNDLE_VERSION@/${BUNDLE_VERSION}/' \
	    ${.CURDIR}/${INFO_PLIST} >${.TARGET}

${_KEXT_LIB}: ${_KEXT_FOLDER} ${_KEXT_FOLDER}/Contents/Info.plist ${OBJS} \
		${.OBJDIR}/kmod_info.c 
	${CXX} -o ${.TARGET} ${OBJS} ${LDFLAGS}

.if defined(KERNEL_PRIVATE)
${_KEXT_LIB}: ${_KMOD_LIBDIR}/libkmod.a
.endif

kmod_info.c: ${.OBJDIR}/kmod_info.c
${.OBJDIR}/kmod_info.c:
	(echo '#include <mach/kmod.h>'; \
	 echo 'extern kern_return_t _start(kmod_info_t *ki, void *data);'; \
	 echo 'extern kern_return_t _stop(kmod_info_t *ki, void *data);'; \
	 echo '${MAIN_FUNCTION_DECL}'; \
	 echo '${ANTIMAIN_FUNCTION_DECL}'; \
	 echo '__attribute__((visibility("default"))) KMOD_EXPLICIT_DECL(${BUNDLE_IDENTIFIER}, "${BUNDLE_VERSION}", _start, _stop)'; \
	 echo '__private_extern__ kmod_start_func_t *_realmain = ${MAIN_FUNCTION};'; \
	 echo '__private_extern__ kmod_stop_func_t *_antimain = ${ANTIMAIN_FUNCTION};'; \
	 echo '__private_extern__ int _kext_apple_cc = __APPLE_CC__;') >${.TARGET}

.include <bsd.lib.mk>
