# SkyLine: startup status line for Skyrim SE 1.5.97 and AE 1.6.317+ (CommonLibSSE-NG v12, clang-cl cross-build).
cmake_minimum_required(VERSION 3.21)
project(SkyLine VERSION 1.0.1 LANGUAGES CXX)
configure_file(cmake/Version.h.in ${CMAKE_CURRENT_BINARY_DIR}/include/Version.h @ONLY)
# Static CRT (with the x64-windows-static-clangcl triplet), so Windows needs no VC++ runtime of a matching version.
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)
find_package(CommonLibSSE CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
add_library(${PROJECT_NAME} SHARED src/main.cpp src/overlay.cpp src/sampler.cpp src/frame.cpp)
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_compile_definitions(${PROJECT_NAME} PRIVATE SKYRIM_AE SKYRIM_SUPPORT_AE)
target_include_directories(${PROJECT_NAME} PRIVATE src ${CMAKE_CURRENT_BINARY_DIR}/include)
target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h)
target_link_libraries(${PROJECT_NAME} PRIVATE CommonLibSSE::CommonLibSSE spdlog::spdlog user32 gdi32 msimg32 ole32 windowscodecs)
