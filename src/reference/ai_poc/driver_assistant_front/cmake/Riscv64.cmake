include(CMakeForceCompiler)

# target system
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR riscv64)

# cross compiler (glibc - Xuantie toolchain)
set(CMAKE_C_COMPILER riscv64-unknown-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER riscv64-unknown-linux-gnu-g++)

# Sysroot from the Xuantie toolchain (has glibc headers)
set(CMAKE_SYSROOT "/opt/toolchain/Xuantie-900-gcc-linux-5.10.4-glibc-x86_64-V2.6.0/sysroot")
set(CMAKE_C_FLAGS "--sysroot=${CMAKE_SYSROOT}" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS "--sysroot=${CMAKE_SYSROOT}" CACHE STRING "" FORCE)

# Skip linking during compiler tests
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_C_FLAGS_DEBUG "-O0 -g3")
set(CMAKE_C_FLAGS_RELEASE "-O0 -s")
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g3")
set(CMAKE_CXX_FLAGS_RELEASE "-O0 -s")
#SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fopenmp")

# set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -march=rv64imafdcv -mabi=lp64d -mcmodel=medany")
# set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=rv64imafdcv -mabi=lp64d -mcmodel=medany")
