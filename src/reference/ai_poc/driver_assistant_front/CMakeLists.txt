cmake_minimum_required(VERSION 3.30)
include(./cmake/Riscv64.cmake)

project(driver_assistant_front C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Fetch dependencies from GitHub (after project() so toolchain is set up)
include(FetchContent)

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

FetchContent_MakeAvailable(argparse fusion json)

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
    glib-2.0
    z
    pcre
    lvgl
    argparse
    m
    -Wl,--end-group)

install(TARGETS ${BIN} DESTINATION out)