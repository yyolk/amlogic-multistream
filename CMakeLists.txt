cmake_minimum_required(VERSION 3.16)
project(amlogic-multistream VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(amlogic-multistream src/main.cpp)

# Future dependencies:
# find_package(PkgConfig REQUIRED)
# pkg_check_modules(FFMPEG REQUIRED libavformat libavcodec libavutil)
# pkg_check_modules(PAHO REQUIRED libpaho-mqttpp3)
# target_link_libraries(amlogic-multistream ${FFMPEG_LIBRARIES} ${PAHO_LIBRARIES})
