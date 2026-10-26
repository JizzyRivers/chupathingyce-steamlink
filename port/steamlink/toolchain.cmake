# cmake's cross build for the steam link w clang (any clang that has arm:
# the one on linux, or llvm's on windows) and valve's sdk for the rest: its
# root filesystem (glibc 2.19 and the box's libraries) and gcc 9.2's crt
# files and libgcc from its toolchain. MARVELL_SDK_PATH is the sdk, as for
# its own setenv.sh. SDL3 is built w this (.github/workflows), and
# port/steamlink/build.py gives the game the same flags
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR armv7l)

if(NOT DEFINED ENV{MARVELL_SDK_PATH})
	message(FATAL_ERROR "MARVELL_SDK_PATH: valve's steamlink-sdk")
endif()
file(TO_CMAKE_PATH "$ENV{MARVELL_SDK_PATH}" STEAMLINK_SDK)
# (a try_compile's cmake runs this file again w only these)
set(CMAKE_TRY_COMPILE_PLATFORM_VARIABLES STEAMLINK_SDK)
set(ENV{MARVELL_SDK_PATH} "${STEAMLINK_SDK}")

set(CMAKE_SYSROOT "${STEAMLINK_SDK}/rootfs")
set(CMAKE_C_COMPILER clang)
set(CMAKE_C_COMPILER_TARGET armv7a-linux-gnueabihf)
set(CMAKE_ASM_COMPILER clang)
set(CMAKE_ASM_COMPILER_TARGET armv7a-linux-gnueabihf)

# the box's cpu (a cortex a9 class core w neon), and gcc's runtime from the sdk
set(STEAMLINK_FLAGS "-march=armv7-a -mfpu=neon -mfloat-abi=hard --gcc-install-dir=${STEAMLINK_SDK}/toolchain/lib/gcc/armv7a-cros-linux-gnueabi/9.2.0")
set(CMAKE_C_FLAGS_INIT "${STEAMLINK_FLAGS}")
set(CMAKE_ASM_FLAGS_INIT "${STEAMLINK_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
