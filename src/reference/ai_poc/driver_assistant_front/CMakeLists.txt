cmake_minimum_required(VERSION 3.2)
include(./cmake/Riscv64.cmake)

project(driver_assistant_front C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(nncase_sdk_root "${PROJECT_SOURCE_DIR}/../../../big/nncase")
set(k230_sdk ${nncase_sdk_root}/../../../)
set(CDK_SRC_DIR ${k230_sdk}/src/common/cdk)
set(RTSMART_SRC_DIR ${k230_sdk}/src/big/rt-smart)
set(IPCMSG_ROOT ${CDK_SRC_DIR}/user/component/ipcmsg)
set(THIRDPARTY_PATH ${CDK_SRC_DIR}/user/thirdparty)

# Buildroot sysroot for libwebsockets
set(BUILDROOT_SYSROOT "${k230_sdk}/output/k230_canmv_01studio_defconfig/little/buildroot-ext/host/riscv64-buildroot-linux-gnu/sysroot")

set(src_root "${PROJECT_SOURCE_DIR}/src")
set(ui_root "${PROJECT_SOURCE_DIR}/ui/export")

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "${CMAKE_CURRENT_BINARY_DIR}" CACHE PATH "Install directory" FORCE)
endif()

# Add media streaming submodule
add_subdirectory(media_streaming)

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

# libwebsockets from buildroot
include_directories(${BUILDROOT_SYSROOT}/usr/include)

# LVGL includes
include_directories(${k230_sdk}/src/big/mpp/userapps/src/lvgl)
include_directories(${k230_sdk}/src/big/mpp/userapps/src/lvgl/src)
include_directories(${ui_root})

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

# libwebsockets library from buildroot
link_directories(${BUILDROOT_SYSROOT}/usr/lib)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DASIO_STANDALONE -Dunix -fno-lto --sysroot=${BUILDROOT_SYSROOT}")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DASIO_STANDALONE -Dunix -fno-lto --sysroot=${BUILDROOT_SYSROOT}")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fno-lto --sysroot=${BUILDROOT_SYSROOT}")

# Build LVGL 9.3.0 from source (compatible with SquareLine Studio 9.2.2 export)
# Tell LVGL to use our lv_conf.h (with LV_OS_PTHREAD enabled) via LV_CONF_INCLUDE_SIMPLE
add_compile_definitions(LV_CONF_INCLUDE_SIMPLE)
add_subdirectory(${k230_sdk}/src/big/mpp/userapps/src/lvgl ${CMAKE_CURRENT_BINARY_DIR}/lvgl)

# Exclude ARM-specific assembly files (NEON/Helium) from LVGL build for RISC-V
get_target_property(LVGL_SOURCES lvgl SOURCES)
list(FILTER LVGL_SOURCES EXCLUDE REGEX ".*lv_blend_(neon|helium)\\.S$")
set_target_properties(lvgl PROPERTIES SOURCES "${LVGL_SOURCES}")

add_executable(${BIN} ${APP_SOURCES} ${UI_SOURCES})
target_link_libraries(${BIN} -Wl,--start-group
    media_streaming
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
    lvgl
    -Wl,--end-group)

install(TARGETS ${BIN} DESTINATION out)