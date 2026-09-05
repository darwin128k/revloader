cmake_minimum_required(VERSION 3.16)
project(revloader LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

enable_language(RC)

option(REVLOADER_STANDALONE "Run GoldSrc in this process (compile hl launcher sources) instead of spawning hl.exe" OFF)
set(HL_DIR "" CACHE PATH "Path to the hl launcher repository (required when REVLOADER_STANDALONE=ON)")

add_executable(cstrike WIN32 src/main.cpp)

set(CSTRIKE_RES "${CMAKE_CURRENT_BINARY_DIR}/cstrike.res")
add_custom_command(
    OUTPUT "${CSTRIKE_RES}"
    COMMAND "${CMAKE_RC_COMPILER}" /nologo /i "${CMAKE_CURRENT_SOURCE_DIR}/res" /fo "${CSTRIKE_RES}" "${CMAKE_CURRENT_SOURCE_DIR}/res/cstrike.rc"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/res/cstrike.rc" "${CMAKE_CURRENT_SOURCE_DIR}/res/cstrike.ico"
    COMMENT "Embedding cstrike.ico"
    VERBATIM
)
target_sources(cstrike PRIVATE "${CSTRIKE_RES}")
set_source_files_properties("${CSTRIKE_RES}" PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
target_link_libraries(cstrike PRIVATE advapi32 shell32 user32)
target_compile_definitions(cstrike PRIVATE _CRT_SECURE_NO_WARNINGS)

if(REVLOADER_STANDALONE)
    if(HL_DIR STREQUAL "")
        message(FATAL_ERROR "REVLOADER_STANDALONE=ON requires -DHL_DIR=/path/to/hl")
    endif()
    get_filename_component(HL_DIR "${HL_DIR}" ABSOLUTE)
    if(NOT EXISTS "${HL_DIR}/src/launcher.cpp")
        message(FATAL_ERROR "HL_DIR=${HL_DIR} does not contain src/launcher.cpp")
    endif()
    target_sources(cstrike PRIVATE "${HL_DIR}/src/launcher.cpp")
    target_include_directories(cstrike PRIVATE "${HL_DIR}/src")
    target_compile_definitions(cstrike PRIVATE REVLOADER_STANDALONE)
    message(STATUS "revloader standalone: using hl sources from ${HL_DIR}")
endif()

if (MSVC)
    target_compile_options(cstrike PRIVATE /utf-8)
endif()
set_target_properties(cstrike PROPERTIES OUTPUT_NAME cstrike)
