# CMake toolchain file for cross-compiling to Windows x86_64
# using MinGW-w64 on Linux.
#
# Usage:
#   cmake -B build-win64 -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-toolchain.cmake
#
# All Windows libraries should be placed in ~/win64-sysroot/

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# Compilers
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)

# Where to look for Windows libraries
set(WIN64_SYSROOT "$ENV{HOME}/win64-sysroot")

set(CMAKE_FIND_ROOT_PATH
    "${WIN64_SYSROOT}/qt6/6.9.3/mingw_64"
    "${WIN64_SYSROOT}/hamlib/hamlib-w64-4.6.5"
    "${WIN64_SYSROOT}/fftw3"
    "${WIN64_SYSROOT}/boost"
)

# Search for programs on the host (e.g., moc, rcc, uic)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
# Search for libraries and headers only in the sysroot
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Qt6 host tools path (moc, rcc, uic run on Linux)
set(QT_HOST_PATH "${WIN64_SYSROOT}/qt6-host/6.9.3/gcc_64")

# Stub windeployqt for cross-compilation (DLLs are bundled manually)
set(WINDEPLOYQT "${WIN64_SYSROOT}/bin/windeployqt" CACHE FILEPATH "" FORCE)

# Disable pkg-config: it finds Linux libraries, not Windows ones
set(PKG_CONFIG_EXECUTABLE "NOTFOUND" CACHE FILEPATH "" FORCE)
set(PKG_CONFIG_FOUND FALSE CACHE BOOL "" FORCE)

# MinGW-w64 posix thread model needs explicit pthread linkage.
# Use full path because pkg-config leaks -L/usr/lib/x86_64-linux-gnu which
# would cause -lpthread to find the Linux (ELF) version and skip it.
execute_process(
    COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=libpthread.a
    OUTPUT_VARIABLE MINGW_PTHREAD_LIB OUTPUT_STRIP_TRAILING_WHITESPACE)
set(CMAKE_CXX_STANDARD_LIBRARIES "${CMAKE_CXX_STANDARD_LIBRARIES} ${MINGW_PTHREAD_LIB}")
set(CMAKE_C_STANDARD_LIBRARIES "${CMAKE_C_STANDARD_LIBRARIES} ${MINGW_PTHREAD_LIB}")
