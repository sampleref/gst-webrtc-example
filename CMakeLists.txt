cmake_minimum_required(VERSION 3.8)
project(GstWebRTCDemo)
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(GSTLIBS REQUIRED
        gobject-2.0
        glib-2.0
        gstreamer-webrtc-1.0
        gstreamer-sdp-1.0
        libsoup-2.4
        json-glib-1.0)

set(CMAKE_CXX_STANDARD 11)
include_directories(
        /usr/lib/x86_64-linux-gnu/glib-2.0/include
        /usr/include/glib-2.0
        /usr/local/include/gstreamer-1.0
        /usr/local/include/libsoup-2.4
        /usr/local/include/json-glib-1.0)

set(SOURCE_FILES_WEBRTC_1_N rtsp_webrtc_1_n.cpp)

link_directories(${GSTLIBS_LIBRARY_DIRS})

add_executable(rtsp2webrtc_1_n ${SOURCE_FILES_WEBRTC_1_N})

target_link_libraries(rtsp2webrtc_1_n ${GSTLIBS_LIBRARIES} ${CMAKE_THREAD_LIBS_INIT})