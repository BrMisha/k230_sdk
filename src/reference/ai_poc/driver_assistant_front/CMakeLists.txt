cmake_minimum_required(VERSION 3.2)
include(./cmake/Riscv64.cmake)

project(driver_assistant_front C CXX)

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

include_directories(${PROJECT_SOURCE_DIR})
include_directories(${k230_sdk}/src/big/mpp/userapps/api/)
include_directories(${k230_sdk}/src/big/mpp/include)
include_directories(${k230_sdk}/src/big/mpp/include/comm)
include_directories(${k230_sdk}/src/common/cdk/user/component/datafifo/include)

include_directories(${IPCMSG_ROOT}/include)
include_directories(${THIRDPARTY_PATH}/asio)

link_directories(${k230_sdk}/src/common/cdk/user/component/datafifo/slave/lib)
link_directories(${k230_sdk}/src/big/mpp/userapps/lib)

link_directories(${IPCMSG_ROOT}/host/lib)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DASIO_STANDALONE -Dunix")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DASIO_STANDALONE -Dunix")

add_executable(${BIN} ${APP_SOURCES})
target_link_libraries(${BIN} -Wl,--start-group datafifo pthread sys ipcmsg -Wl,--end-group)

install(TARGETS ${BIN} DESTINATION out)