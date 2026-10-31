cmake_minimum_required(VERSION 3.16)
project(amlogic-multistream VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED libavformat libavcodec libavutil libswscale)
pkg_check_modules(MOSQUITTO REQUIRED libmosquitto)
pkg_check_modules(DRM REQUIRED libdrm)

add_executable(amlogic-multistream
    src/main.cpp
    src/app.cpp
    src/stream_manager.cpp
    src/layout_engine.cpp
    src/mqtt_controller.cpp
    src/ffmpeg_decoder.cpp
    src/drm_renderer.cpp
)

target_include_directories(amlogic-multistream PRIVATE
    ${FFMPEG_INCLUDE_DIRS}
    ${MOSQUITTO_INCLUDE_DIRS}
    ${DRM_INCLUDE_DIRS}
    src
)

target_link_libraries(amlogic-multistream PRIVATE
    ${FFMPEG_LIBRARIES}
    ${MOSQUITTO_LIBRARIES}
    ${DRM_LIBRARIES}
)

target_compile_options(amlogic-multistream PRIVATE ${FFMPEG_CFLAGS_OTHER} ${MOSQUITTO_CFLAGS_OTHER})
