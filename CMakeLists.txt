cmake_minimum_required(VERSION 3.16)
project(amlogic-multistream VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Layout math, the in-memory stream table, and MQTT command parsing have no
# FFmpeg, libmosquitto, or DRM dependency, so CI can build and run them on a
# normal Linux host. The player target below stays optional. A test that
# links libmosquitto should be added only when MOSQUITTO_FOUND is true.
add_executable(layout_engine_tests
    tests/layout_engine_test.cpp
    src/layout_engine.cpp
)
target_include_directories(layout_engine_tests PRIVATE src)
target_compile_options(layout_engine_tests PRIVATE -Wall -Wextra -Wpedantic)

add_executable(stream_manager_test
    tests/stream_manager_test.cpp
    src/stream_manager.cpp
    src/layout_engine.cpp
)
target_include_directories(stream_manager_test PRIVATE src)
target_compile_options(stream_manager_test PRIVATE -Wall -Wextra -Wpedantic)

add_executable(mqtt_smoke_test
    tests/mqtt_smoke_test.cpp
    src/mqtt_json.cpp
    src/mqtt_commands.cpp
    src/stream_manager.cpp
    src/layout_engine.cpp
)
target_include_directories(mqtt_smoke_test PRIVATE src)
target_compile_options(mqtt_smoke_test PRIVATE -Wall -Wextra -Wpedantic)

enable_testing()
add_test(NAME layout_engine COMMAND layout_engine_tests)
add_test(NAME stream_manager_test COMMAND stream_manager_test)
add_test(NAME mqtt_smoke_test COMMAND mqtt_smoke_test)

find_package(PkgConfig QUIET)
set(AMLOGIC_PLAYER_DEPS_FOUND FALSE)
if(PkgConfig_FOUND)
    pkg_check_modules(FFMPEG QUIET libavformat libavcodec libavutil libswscale)
    pkg_check_modules(MOSQUITTO QUIET libmosquitto)
    pkg_check_modules(DRM QUIET libdrm)
    if(FFMPEG_FOUND AND MOSQUITTO_FOUND AND DRM_FOUND)
        set(AMLOGIC_PLAYER_DEPS_FOUND TRUE)
    endif()
endif()

if(AMLOGIC_PLAYER_DEPS_FOUND)
    add_executable(amlogic-multistream
        src/main.cpp
        src/app.cpp
        src/stream_manager.cpp
        src/layout_engine.cpp
        src/mqtt_controller.cpp
        src/mqtt_json.cpp
        src/mqtt_commands.cpp
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
else()
    message(STATUS
        "Skipping amlogic-multistream player: need libavformat, libavcodec, libavutil, libswscale, libmosquitto, and libdrm")
endif()
