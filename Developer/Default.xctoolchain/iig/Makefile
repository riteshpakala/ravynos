CXX := ${HOST_CXX}
SRCS = iig.cpp
OBJS = iig.o
.if "${.MAKE.OS}" == "Darwin"
# Pure C++ tool: build it against the host SDK only. The toolchain include
# directory redefines mbstate_t and breaks libc++'s codecvt instantiation.
CXXFLAGS = -std=gnu++17
.else
CXXFLAGS += -std=gnu++17
.endif

all: ${TOOLS}/iig

${TOOLS}/iig: ${OBJS}
	${CXX} -o ${.TARGET} ${OBJS}

