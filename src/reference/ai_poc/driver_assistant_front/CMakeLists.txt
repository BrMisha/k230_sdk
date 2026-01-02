cmake_minimum_required(VERSION 3.30)
include(./cmake/Riscv64.cmake)

project(driver_assistant_front C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set up SDK paths first (needed for FetchContent dependencies to find OpenSSL, etc.)
set(nncase_sdk_root "${PROJECT_SOURCE_DIR}/../../../big/nncase")
set(k230_sdk ${nncase_sdk_root}/../../../)
set(CDK_SRC_DIR ${k230_sdk}/src/common/cdk)
set(RTSMART_SRC_DIR ${k230_sdk}/src/big/rt-smart)
set(IPCMSG_ROOT ${CDK_SRC_DIR}/user/component/ipcmsg)
set(THIRDPARTY_PATH ${CDK_SRC_DIR}/user/thirdparty)

# Read CONF from .last_conf (set by main SDK Makefile)
set(LAST_CONF_FILE "${k230_sdk}/.last_conf")
if(EXISTS "${LAST_CONF_FILE}")
    file(READ "${LAST_CONF_FILE}" LAST_CONF_CONTENT)
    string(REGEX MATCH "CONF=([^\n\r]+)" _ "${LAST_CONF_CONTENT}")
    set(CONF "${CMAKE_MATCH_1}")
else()
    message(FATAL_ERROR "Cannot find .last_conf - please build SDK first with 'make CONF=...'")
endif()

# Buildroot paths (using staging for proper cross-compilation sysroot)
set(BUILDROOT_DIR "${k230_sdk}/output/${CONF}/little/buildroot-ext")
set(BUILDROOT_STAGING "${BUILDROOT_DIR}/staging")

# Configure OpenSSL for cross-compilation (required by libdatachannel)
set(OPENSSL_ROOT_DIR "${BUILDROOT_STAGING}/usr")
set(OPENSSL_INCLUDE_DIR "${BUILDROOT_STAGING}/usr/include")
set(OPENSSL_CRYPTO_LIBRARY "${BUILDROOT_DIR}/target/usr/lib/libcrypto.so")
set(OPENSSL_SSL_LIBRARY "${BUILDROOT_DIR}/target/usr/lib/libssl.so")

# Configure pkg-config for cross-compilation (for libnice)
set(ENV{PKG_CONFIG_PATH} "${BUILDROOT_STAGING}/usr/lib/pkgconfig:${BUILDROOT_STAGING}/usr/share/pkgconfig")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${BUILDROOT_STAGING}")

# Fetch dependencies from GitHub (after project() so toolchain is set up)
include(FetchContent)

# Allow FetchContent_Populate for patching libdatachannel before building
cmake_policy(SET CMP0169 OLD)

FetchContent_Declare(
    argparse
    URL https://github.com/p-ranav/argparse/archive/refs/tags/v3.2.tar.gz
)

FetchContent_Declare(
    fusion
    GIT_REPOSITORY https://github.com/xioTechnologies/Fusion.git
    GIT_TAG        v1.2.11
    SOURCE_SUBDIR  Fusion  # Only include the Fusion library, not examples
)

FetchContent_Declare(
    json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
)

# libdatachannel for WebRTC data channels (replaces GStreamer webrtcbin)
FetchContent_Declare(
    libdatachannel
    GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
    GIT_TAG v0.24.0
)
# Use libjuice (built-in, lighter) instead of libnice for ICE
set(USE_NICE OFF CACHE BOOL "Use libnice for ICE")
set(NO_MEDIA ON CACHE BOOL "Disable media transport")
set(NO_WEBSOCKET ON CACHE BOOL "Disable WebSocket support")
set(NO_EXAMPLES ON CACHE BOOL "Disable examples")
set(NO_TESTS ON CACHE BOOL "Disable tests")

# Ensure pthread is found for cross-compilation
set(THREADS_PREFER_PTHREAD_FLAG ON)
set(CMAKE_THREAD_LIBS_INIT "-lpthread")
set(CMAKE_HAVE_THREADS_LIBRARY 1)
set(CMAKE_USE_WIN32_THREADS_INIT 0)
set(CMAKE_USE_PTHREADS_INIT 1)
set(THREADS_FOUND TRUE)

# Make non-libdatachannel dependencies available first
FetchContent_MakeAvailable(argparse fusion json)

# libdatachannel needs patching for RISC-V (usrsctp uses BSD types not in glibc)
FetchContent_GetProperties(libdatachannel)
if(NOT libdatachannel_POPULATED)
    FetchContent_Populate(libdatachannel)

    # Patch usrsctp's sctp_os_userspace.h to include netinet/ip.h and define BSD types
    # The BSD types (u_int, u_long, u_short, u_char, caddr_t) are not in glibc headers
    set(SCTP_OS_FILE "${libdatachannel_SOURCE_DIR}/deps/usrsctp/usrsctplib/netinet/sctp_os_userspace.h")
    file(READ "${SCTP_OS_FILE}" SCTP_OS_CONTENT)

    # Add includes and typedefs at the very beginning after the include guard
    # Note: The actual include guard is __sctp_os_userspace_h__ not _NETINET_SCTP_OS_USERSPACE_H_
    # _GNU_SOURCE is needed for pthread_rwlock_t and other POSIX extensions in glibc
    string(REPLACE
        "#define __sctp_os_userspace_h__"
        "#define __sctp_os_userspace_h__

/* BSD types for RISC-V/Linux glibc - patched by CMake */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sys/types.h>
#include <pthread.h>
#include <netinet/ip.h>
#include <net/if.h>
#ifndef u_long
typedef unsigned long u_long;
#endif
#ifndef u_int
typedef unsigned int u_int;
#endif
#ifndef u_short
typedef unsigned short u_short;
#endif
#ifndef u_char
typedef unsigned char u_char;
#endif
#ifndef caddr_t
typedef char* caddr_t;
#endif
/* End of BSD types patch */"
        SCTP_OS_CONTENT "${SCTP_OS_CONTENT}")
    file(WRITE "${SCTP_OS_FILE}" "${SCTP_OS_CONTENT}")

    # Also patch user_malloc.h - it's included by user_mbuf.h before sctp_os_userspace.h
    # so it needs its own BSD type definitions
    set(USER_MALLOC_FILE "${libdatachannel_SOURCE_DIR}/deps/usrsctp/usrsctplib/user_malloc.h")
    file(READ "${USER_MALLOC_FILE}" USER_MALLOC_CONTENT)
    string(REPLACE
        "#include <sys/types.h>"
        "#include <sys/types.h>
/* BSD types for RISC-V/Linux glibc - patched by CMake */
#ifndef u_long
typedef unsigned long u_long;
#endif
#ifndef u_int
typedef unsigned int u_int;
#endif
#ifndef u_short
typedef unsigned short u_short;
#endif
#ifndef u_char
typedef unsigned char u_char;
#endif
/* End of BSD types patch */"
        USER_MALLOC_CONTENT "${USER_MALLOC_CONTENT}")
    file(WRITE "${USER_MALLOC_FILE}" "${USER_MALLOC_CONTENT}")

    add_subdirectory(${libdatachannel_SOURCE_DIR} ${libdatachannel_BINARY_DIR})

    # usrsctp's CMakeLists.txt only adds _GNU_SOURCE when CMAKE_SYSTEM_NAME is "Linux"
    # but our toolchain sets it to "Generic". Add it manually for pthread_rwlock_t support.
    target_compile_definitions(usrsctp PRIVATE _GNU_SOURCE)
endif()

# libwebsockets from buildroot build
set(LIBWEBSOCKETS_ROOT "${BUILDROOT_DIR}/build/libwebsockets-4.0.21")

set(src_root "${PROJECT_SOURCE_DIR}/src")
set(ui_root "${PROJECT_SOURCE_DIR}/ui/export")

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "${CMAKE_CURRENT_BINARY_DIR}" CACHE PATH "Install directory" FORCE)
endif()

# Add media streaming submodule
add_subdirectory(media_streaming)

# Add backend communication submodule (MQTT, WebRTC signaling)
add_subdirectory(backend_comm)

file(GLOB APP_SOURCES ${src_root}/*.cpp ${src_root}/*.c)

# LVGL and UI sources
file(GLOB UI_SOURCES
    ${ui_root}/*.c
    ${ui_root}/screens/*.c
    ${ui_root}/components/*.c
    ${ui_root}/images/*.c
)

set(BIN driver_assistant_front)

# RTSP server and live555 paths
set(RTSP_SERVER_DIR ${CDK_SRC_DIR}/user/middleware/rtsp_server)
set(LIVE555_ROOT ${THIRDPARTY_PATH}/live)

include_directories(${PROJECT_SOURCE_DIR})
include_directories(${k230_sdk}/src/big/mpp/userapps/api/)
include_directories(${k230_sdk}/src/big/mpp/include)
include_directories(${k230_sdk}/src/big/mpp/include/comm)
include_directories(${k230_sdk}/src/common/cdk/user/component/datafifo/include)

include_directories(${IPCMSG_ROOT}/include)
include_directories(${THIRDPARTY_PATH}/asio)
include_directories(${CDK_SRC_DIR}/user/middleware/mp4_format/include)

# RTSP server includes
include_directories(${RTSP_SERVER_DIR}/include)
include_directories(${LIVE555_ROOT}/BasicUsageEnvironment/include)
include_directories(${LIVE555_ROOT}/groupsock/include)
include_directories(${LIVE555_ROOT}/liveMedia/include)
include_directories(${LIVE555_ROOT}/UsageEnvironment/include)

# libwebsockets includes
include_directories(${LIBWEBSOCKETS_ROOT}/include)

# Buildroot staging includes (OpenSSL, GLib, etc.)
include_directories(${BUILDROOT_STAGING}/usr/include)
include_directories(${BUILDROOT_STAGING}/usr/include/glib-2.0)
include_directories(${BUILDROOT_STAGING}/usr/lib/glib-2.0/include)

# LVGL includes
include_directories(${k230_sdk}/src/big/mpp/userapps/src/lvgl)
include_directories(${k230_sdk}/src/big/mpp/userapps/src/lvgl/src)
include_directories(${ui_root})

# Fusion AHRS library includes
include_directories(${fusion_SOURCE_DIR}/Fusion)

link_directories(${k230_sdk}/src/common/cdk/user/component/datafifo/slave/lib)
link_directories(${k230_sdk}/src/big/mpp/userapps/lib)
link_directories(${CDK_SRC_DIR}/user/middleware/mp4_format/lib)

link_directories(${IPCMSG_ROOT}/host/lib)

# RTSP server and live555 library directories
link_directories(${RTSP_SERVER_DIR}/lib)
link_directories(${LIVE555_ROOT}/BasicUsageEnvironment)
link_directories(${LIVE555_ROOT}/groupsock)
link_directories(${LIVE555_ROOT}/liveMedia)
link_directories(${LIVE555_ROOT}/UsageEnvironment)

# libwebsockets library
link_directories(${LIBWEBSOCKETS_ROOT}/lib)

# Buildroot target libraries (OpenSSL, GLib, zlib, pcre, etc.)
# Note: staging has headers/pkg-config, target has the actual .so files
link_directories(${BUILDROOT_DIR}/target/usr/lib)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DASIO_STANDALONE -Dunix -fno-lto")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DASIO_STANDALONE -Dunix -fno-lto")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fno-lto")

# Build LVGL 9.3.0 from source (compatible with SquareLine Studio 9.2.2 export)
# Tell LVGL to use our lv_conf.h (with LV_OS_PTHREAD enabled) via LV_CONF_INCLUDE_SIMPLE
add_compile_definitions(LV_CONF_INCLUDE_SIMPLE)
add_subdirectory(${k230_sdk}/src/big/mpp/userapps/src/lvgl ${CMAKE_CURRENT_BINARY_DIR}/lvgl)

# Exclude ARM-specific assembly files (NEON/Helium) from LVGL build for RISC-V
get_target_property(LVGL_SOURCES lvgl SOURCES)
list(FILTER LVGL_SOURCES EXCLUDE REGEX ".*lv_blend_(neon|helium)\\.S$")
set_target_properties(lvgl PROPERTIES SOURCES "${LVGL_SOURCES}")

# Fusion AHRS library sources (multiple .c files)
file(GLOB FUSION_SOURCES ${fusion_SOURCE_DIR}/Fusion/*.c)

add_executable(${BIN} ${APP_SOURCES} ${UI_SOURCES} ${FUSION_SOURCES})
target_link_libraries(${BIN} nlohmann_json::nlohmann_json)
target_link_libraries(${BIN} -Wl,--start-group
    media_streaming
    backend_comm
    LibDataChannel::LibDataChannelStatic
    datafifo
    pthread
    sys
    ipcmsg
    mp4
    rtsp_server
    liveMedia
    UsageEnvironment
    BasicUsageEnvironment
    groupsock
    websockets
    ssl
    crypto
    z
    pcre
    glib-2.0
    lvgl
    argparse
    atomic
    m
    -Wl,--end-group)

install(TARGETS ${BIN} DESTINATION out)