cmake_minimum_required(VERSION 3.16)
project(revloader LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Temporary stand-in for this install's RevLoader cstrike.exe. Not part of
# vellum; meant to be deleted once the overlay launcher takes over.
add_executable(cstrike WIN32 src/main.cpp)
target_link_libraries(cstrike PRIVATE advapi32 shell32 user32)
target_compile_definitions(cstrike PRIVATE _CRT_SECURE_NO_WARNINGS)
if (MSVC)
    target_compile_options(cstrike PRIVATE /utf-8)
endif()
set_target_properties(cstrike PROPERTIES OUTPUT_NAME cstrike)
