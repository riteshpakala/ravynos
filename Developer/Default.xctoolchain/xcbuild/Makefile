TARGETS = ${TOOLS}/xcbuild ${TOOLS}/xcodebuild ${TOOLS}/xcrun

.if "${.MAKE.OS}" == "Darwin"
RPATH = @executable_path
.else
RPATH = $$ORIGIN
.endif

all: ${TOOLS}/xcbuild

${TARGETS}:
	cmake -S ${.CURDIR} -B ${.OBJDIR} -Wno-dev \
	-DCMAKE_C_COMPILER=${HOST_CC} \
	-DCMAKE_CXX_COMPILER=${HOST_CXX} \
	-DROOT_SOURCE_DIR=${ROOT_SOURCE_DIR} \
	-DROOT_BINARY_DIR=${ROOT_BINARY_DIR} \
	'-DCMAKE_BUILD_RPATH=${RPATH}/../lib' \
	'-DCMAKE_INSTALL_RPATH=${RPATH}/../lib' \
	-DCMAKE_INSTALL_PREFIX=${TOOLCHAIN}
	CC=${HOST_CC} CXX=${HOST_CXX} make -C${.OBJDIR}
	CC=${HOST_CC} CXX=${HOST_CXX} make -C${.OBJDIR} install
.if "${.MAKE.OS}" == "Darwin"
	# xcbuild's CMake only knows $ORIGIN-style rpaths; make the tools find
	# their dylibs next to the toolchain's usr/lib on macOS.
.for tool in xcrun xcodebuild xcbuild xcode-select
	-env -u DEVELOPER_DIR /usr/bin/install_name_tool -add_rpath @executable_path/../lib ${TOOLS}/${tool}
.endfor
	-for lib in ${TOOLCHAIN}/usr/lib/lib*.dylib; do \
		env -u DEVELOPER_DIR /usr/bin/install_name_tool -add_rpath @loader_path $$lib 2>/dev/null; \
	done
.endif

