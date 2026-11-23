cmake_minimum_required(VERSION 3.20)
project(IndividualRoofTextures LANGUAGES C CXX)
if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "Build with 32-bit MSVC: cmake -S . -B build -A Win32")
endif()
add_library(TS2IndividualRoofTextures SHARED TS2IndividualRoofTextures.cpp
 third_party/minhook/src/buffer.c third_party/minhook/src/hook.c
 third_party/minhook/src/trampoline.c third_party/minhook/src/hde/hde32.c)
target_include_directories(TS2IndividualRoofTextures PRIVATE third_party/minhook/include)
target_compile_features(TS2IndividualRoofTextures PRIVATE cxx_std_17)
target_compile_definitions(TS2IndividualRoofTextures PRIVATE UNICODE _UNICODE)
target_link_libraries(TS2IndividualRoofTextures PRIVATE user32)
set_target_properties(TS2IndividualRoofTextures PROPERTIES PREFIX "" SUFFIX ".asi" MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
