cmake_minimum_required(VERSION 3.24)
project(loxe VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# 64-bit file offsets on Linux
if(UNIX AND NOT APPLE)
    add_compile_definitions(_FILE_OFFSET_BITS=64)
endif()

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
    endif()
endif()

add_subdirectory(third_party/cmarkup)
add_subdirectory(src)

enable_testing()
add_subdirectory(tests)
