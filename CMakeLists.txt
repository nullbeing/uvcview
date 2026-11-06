cmake_minimum_required(VERSION 3.13)
project(uvcview LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)

find_package(PkgConfig REQUIRED)
pkg_check_modules(UVC REQUIRED libuvc)
pkg_check_modules(FREETYPE REQUIRED freetype2)
find_package(JPEG REQUIRED)

add_executable(uvcview
    uvcview.cpp
    font8x16.cpp
)

target_include_directories(uvcview PRIVATE
    ${UVC_INCLUDE_DIRS}
    ${FREETYPE_INCLUDE_DIRS}
    ${JPEG_INCLUDE_DIRS}
)

target_link_libraries(uvcview PRIVATE
    ${UVC_LIBRARIES}
    ${FREETYPE_LIBRARIES}
    ${JPEG_LIBRARIES}
    pthread
)
