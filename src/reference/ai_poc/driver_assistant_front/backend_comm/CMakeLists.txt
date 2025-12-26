cmake_minimum_required(VERSION 3.2)
project(backend_comm C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# K230 SDK paths
set(k230_sdk ${CMAKE_CURRENT_SOURCE_DIR}/../../../../..)

# Read CONF from .last_conf (set by main SDK Makefile)
set(LAST_CONF_FILE "${k230_sdk}/.last_conf")
if(EXISTS "${LAST_CONF_FILE}")
    file(READ "${LAST_CONF_FILE}" LAST_CONF_CONTENT)
    string(REGEX MATCH "CONF=([^\n\r]+)" _ "${LAST_CONF_CONTENT}")
    set(CONF "${CMAKE_MATCH_1}")
endif()

# Buildroot paths
set(BUILDROOT_DIR "${k230_sdk}/output/${CONF}/little/buildroot-ext")
set(BUILDROOT_STAGING "${BUILDROOT_DIR}/staging")

# Source files
set(SOURCES
    src/mqtt_client.cpp
    src/stream_session.cpp
    src/gst_webrtc_peer.cpp
)

# Create static library
add_library(backend_comm STATIC ${SOURCES})

# Public include directories (available to users of this library)
target_include_directories(backend_comm
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${BUILDROOT_STAGING}/usr/include
        ${BUILDROOT_STAGING}/usr/include/gstreamer-1.0
        ${BUILDROOT_STAGING}/usr/include/glib-2.0
        ${BUILDROOT_STAGING}/usr/lib/glib-2.0/include
)

# Link directories for Paho MQTT and GStreamer
target_link_directories(backend_comm
    PUBLIC
        ${BUILDROOT_DIR}/target/usr/lib
)

# Link Paho MQTT C++ library (and its C dependency) + OpenSSL + JSON + GStreamer
target_link_libraries(backend_comm
    PUBLIC
        paho-mqttpp3
        paho-mqtt3as
        ssl
        crypto
        nlohmann_json::nlohmann_json
        gstreamer-1.0
        gstbase-1.0
        gstapp-1.0
        gstwebrtc-1.0
        gstsdp-1.0
        gstrtp-1.0
        gobject-2.0
        gmodule-2.0
        gio-2.0
        glib-2.0
        ffi
)