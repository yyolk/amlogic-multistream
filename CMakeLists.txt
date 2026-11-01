cmake_minimum_required(VERSION 3.16)
project(amlogic-multistream VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Layout math, the in-memory stream table, MQTT command parsing, the
# software renderer, and StreamPlayer's fake-decoder test have no FFmpeg,
# libmosquitto, or DRM dependency, so CI can build and run them on a normal
# Linux host. ffmpeg_decoder_tests and the localhost HTTP player smoke test
# are added only when the libav* packages are present. The player target
# below stays optional. A test that links libmosquitto should be added only
# when MOSQUITTO_FOUND is true.
#
# AMLOGIC_DRM_KMS defaults off. Turning it on compiles the /dev/dri hook in
# DrmRenderer. That hook requires GS-King X / CoreELEC and is not used by
# ctest. The default build does not open a DRM device.
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

add_executable(software_renderer_tests
    tests/software_renderer_test.cpp
    src/software_renderer.cpp
    src/layout_engine.cpp
)
target_include_directories(software_renderer_tests PRIVATE src)
target_compile_options(software_renderer_tests PRIVATE -Wall -Wextra -Wpedantic)

add_executable(stream_player_test
    tests/stream_player_test.cpp
    src/stream_player.cpp
    src/stream_manager.cpp
    src/layout_engine.cpp
    src/software_renderer.cpp
    src/mqtt_commands.cpp
    src/mqtt_json.cpp
)
target_include_directories(stream_player_test PRIVATE src)
target_compile_options(stream_player_test PRIVATE -Wall -Wextra -Wpedantic)

enable_testing()
add_test(NAME layout_engine COMMAND layout_engine_tests)
add_test(NAME stream_manager_test COMMAND stream_manager_test)
add_test(NAME mqtt_smoke_test COMMAND mqtt_smoke_test)
add_test(NAME software_renderer COMMAND software_renderer_tests)
add_test(NAME stream_player_test COMMAND stream_player_test)

# Stand-in for scripts/launch-coreelec.sh (that script requires GS-King X /
# CoreELEC). Checks SIGTERM against a type-2 runtime that does not forward
# the signal, SIGINT before the player starts, and a bare ELF.
add_test(NAME launch_coreelec
    COMMAND "${CMAKE_SOURCE_DIR}/scripts/test-launch-coreelec.sh")
set_tests_properties(launch_coreelec PROPERTIES TIMEOUT 120)

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

# Software-decode smoke test. Links libav* only, so it can run when the
# player target is skipped for lack of libmosquitto or libdrm. The test
# generates a local fixture with the ffmpeg CLI and exits 77 when that
# CLI is missing (ctest SKIP_RETURN_CODE).
if(FFMPEG_FOUND)
    find_program(FFMPEG_CLI ffmpeg)
    add_executable(ffmpeg_decoder_tests
        tests/ffmpeg_decoder_test.cpp
        src/ffmpeg_decoder.cpp
    )
    target_include_directories(ffmpeg_decoder_tests SYSTEM PRIVATE ${FFMPEG_INCLUDE_DIRS})
    target_include_directories(ffmpeg_decoder_tests PRIVATE src)
    if(FFMPEG_LIBRARY_DIRS)
        target_link_directories(ffmpeg_decoder_tests PRIVATE ${FFMPEG_LIBRARY_DIRS})
    endif()
    target_link_libraries(ffmpeg_decoder_tests PRIVATE ${FFMPEG_LIBRARIES})
    target_compile_options(ffmpeg_decoder_tests PRIVATE -Wall -Wextra -Wpedantic ${FFMPEG_CFLAGS_OTHER})
    if(FFMPEG_CLI)
        target_compile_definitions(ffmpeg_decoder_tests PRIVATE FFMPEG_CLI="${FFMPEG_CLI}")
    else()
        target_compile_definitions(ffmpeg_decoder_tests PRIVATE FFMPEG_CLI="")
    endif()
    add_test(NAME ffmpeg_decoder COMMAND ffmpeg_decoder_tests)
    set_tests_properties(ffmpeg_decoder PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)

    # MQTT add → software decode → SoftwareRenderer, without a broker.
    # Serves one MPEG-TS file on 127.0.0.1. Skips when the ffmpeg CLI is missing.
    add_executable(stream_player_http_test
        tests/stream_player_http_test.cpp
        src/stream_player.cpp
        src/stream_manager.cpp
        src/layout_engine.cpp
        src/software_renderer.cpp
        src/ffmpeg_decoder.cpp
    )
    target_include_directories(stream_player_http_test SYSTEM PRIVATE ${FFMPEG_INCLUDE_DIRS})
    target_include_directories(stream_player_http_test PRIVATE src)
    if(FFMPEG_LIBRARY_DIRS)
        target_link_directories(stream_player_http_test PRIVATE ${FFMPEG_LIBRARY_DIRS})
    endif()
    target_link_libraries(stream_player_http_test PRIVATE ${FFMPEG_LIBRARIES})
    target_compile_options(stream_player_http_test PRIVATE -Wall -Wextra -Wpedantic ${FFMPEG_CFLAGS_OTHER})
    if(FFMPEG_CLI)
        target_compile_definitions(stream_player_http_test PRIVATE FFMPEG_CLI="${FFMPEG_CLI}")
    else()
        target_compile_definitions(stream_player_http_test PRIVATE FFMPEG_CLI="")
    endif()
    add_test(NAME stream_player_http COMMAND stream_player_http_test)
    set_tests_properties(stream_player_http PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
else()
    message(STATUS
        "Skipping ffmpeg_decoder test: need libavformat, libavcodec, libavutil, and libswscale")
endif()

# Off unless a GS-King X / CoreELEC build explicitly asks for /dev/dri.
# CI and ctest leave it off. See src/drm_renderer.cpp.
option(AMLOGIC_DRM_KMS "Compile DRM/KMS /dev/dri hooks (requires GS-King X / CoreELEC)" OFF)

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
        src/software_renderer.cpp
        src/drm_renderer.cpp
        src/stream_player.cpp
    )
    if(AMLOGIC_DRM_KMS)
        target_compile_definitions(amlogic-multistream PRIVATE AMLOGIC_DRM_KMS=1)
    endif()

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

    # Runs the player against a local fixture. Skips when the ffmpeg CLI is
    # missing. Does not need /dev/dri; --renderer drm is expected to fail.
    add_executable(present_smoke_test tests/present_smoke_test.cpp)
    target_compile_options(present_smoke_test PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_definitions(present_smoke_test PRIVATE
        "PLAYER_BIN=\"$<TARGET_FILE:amlogic-multistream>\""
    )
    if(FFMPEG_CLI)
        target_compile_definitions(present_smoke_test PRIVATE "FFMPEG_CLI=\"${FFMPEG_CLI}\"")
    else()
        target_compile_definitions(present_smoke_test PRIVATE "FFMPEG_CLI=\"\"")
    endif()
    add_test(NAME present_smoke COMMAND present_smoke_test)
    set_tests_properties(present_smoke PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
else()
    message(STATUS
        "Skipping amlogic-multistream player: need libavformat, libavcodec, libavutil, libswscale, libmosquitto, and libdrm")
endif()
