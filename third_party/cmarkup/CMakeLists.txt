# CMarkup 11.5 — sources are Markup.h / Markup.cpp (no "C" prefix).
#
# License: firstobject CMarkup license.
# Commercial use requires written permission — see requirements.md §3.3.

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Markup.cpp")
    add_library(cmarkup STATIC Markup.cpp)
    target_include_directories(cmarkup PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_features(cmarkup PRIVATE cxx_std_17)
    if(UNIX)
        # CMarkup uses iconv for encoding conversion on POSIX platforms.
        # On Linux iconv is part of glibc — no separate link needed.
        # On macOS it is a standalone library.
        target_compile_definitions(cmarkup PUBLIC MARKUP_ICONV)
        if(APPLE)
            target_link_libraries(cmarkup PUBLIC iconv)
        endif()
    endif()
else()
    message(WARNING
        "Markup.cpp not found in third_party/cmarkup/. "
        "Place Markup.h and Markup.cpp here. Building a stub target.")
    add_library(cmarkup INTERFACE)
endif()
