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

set(src_root "${PROJECT_SOURCE_DIR}/src")

set(CMAKE_INSTALL_PREFIX "../")
file(GLOB APP_SOURCES ${src_root}/*.cpp)

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

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DASIO_STANDALONE -Dunix")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DASIO_STANDALONE -Dunix")

add_executable(${BIN} ${APP_SOURCES})
target_link_libraries(${BIN} -Wl,--start-group
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
    -Wl,--end-group)

install(TARGETS ${BIN} DESTINATION out)