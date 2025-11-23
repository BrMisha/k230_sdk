cmake_minimum_required(VERSION 3.2)
project(media_streaming C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# K230 SDK paths
set(k230_sdk ${CMAKE_CURRENT_SOURCE_DIR}/../../../../..)
set(CDK_SRC_DIR ${k230_sdk}/src/common/cdk)
set(THIRDPARTY_PATH ${CDK_SRC_DIR}/user/thirdparty)

# Source files
set(SOURCES
    src/media_streamer_file.cpp
    src/media_streamer_rtsp.cpp
)

# Create static library
add_library(media_streaming STATIC ${SOURCES})

# Public include directories (available to users of this library)
target_include_directories(media_streaming
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${k230_sdk}/src/big/mpp/include
        ${k230_sdk}/src/big/mpp/include/comm
        ${CDK_SRC_DIR}/user/middleware/mp4_format/include
        ${CDK_SRC_DIR}/user/middleware/rtsp_server/include
        ${THIRDPARTY_PATH}/live/BasicUsageEnvironment/include
        ${THIRDPARTY_PATH}/live/groupsock/include
        ${THIRDPARTY_PATH}/live/liveMedia/include
        ${THIRDPARTY_PATH}/live/UsageEnvironment/include
)