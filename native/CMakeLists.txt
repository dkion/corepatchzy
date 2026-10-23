cmake_minimum_required(VERSION 3.22)
project(corepatch_zygisk CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(corepatchzygisk SHARED
    src/main/cpp/module.cpp
    src/main/cpp/art_hooker.cpp
    src/main/cpp/jni_bridge.cpp
)

target_include_directories(corepatchzygisk PRIVATE src/main/cpp)

# Zygisk loads the .so via dlopen; keep the exported surface minimal.
target_compile_options(corepatchzygisk PRIVATE -fvisibility=hidden -Wall -Wextra)

# Only arm64 is targeted (the trampoline in art_hooker.cpp is ARM64-specific).
set_target_properties(corepatchzygisk PROPERTIES
    OUTPUT_NAME "corepatchzygisk"
)

target_link_libraries(corepatchzygisk log)
