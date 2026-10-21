#
# CMakeLists.txt — colang (compiler front end) build configuration
#
# Dependency: LLVM dev libs (C++ headers + static libs + CMake config)
#   Local build: LLVM 23.1.0 from source (X86-only, Release, static, /MD CRT)
#     D:\svn\bp.shanmin.com\colang\llvm-23.1.0\build\lib\cmake\llvm
#   build.bat has this path baked in; manual configure uses -DLLVM_DIR=<...>
#
# Usual commands:
#   cmake -B build -DCMAKE_BUILD_TYPE=Release -DLLVM_DIR=<...>
#   cmake --build build --config Release
#

cmake_minimum_required(VERSION 3.20)

project(colang LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# CRT mode must match the LLVM static libs:
#   OFF = /MD dynamic CRT (default, matches local LLVM 23.1.0 tree)
#   ON  = /MT static CRT (only after rebuilding LLVM with LLVM_USE_CRT=MT)
option(COLANG_STATIC_CRT "Use static CRT (/MT), must match how LLVM libs were built" OFF)

find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION} (${LLVM_DIR})")

separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND "${LLVM_DEFINITIONS}")
add_definitions(${LLVM_DEFINITIONS_LIST})

if(MSVC)
  # Sources are UTF-8 without BOM
  add_compile_options(/utf-8)
  # Warnings inside LLVM headers are not our code: SYSTEM includes (/external:I)
  # + /external:anglebrackets + /external:W0 to silence them
  add_compile_options(/W4 /external:anglebrackets /external:W0)
  if(COLANG_STATIC_CRT)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  else()
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
  endif()
endif()

# Sources live in per-layer directories, collected recursively
file(GLOB_RECURSE COLANG_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)

add_executable(colang ${COLANG_SOURCES})

target_include_directories(colang PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(colang SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})

# LLVM components:
#   Core         — IRBuilder / Module / Verifier
#   Support      — raw_fd_ostream / Triple etc.
#   BitWriter    — emit .bc
#   BitReader    — read .bc (reserved)
#   TargetParser — target triple queries (Host.h moved here in LLVM 23)
llvm_map_components_to_libnames(COLANG_LLVM_LIBS Core Support BitWriter BitReader TargetParser)
target_link_libraries(colang PRIVATE ${COLANG_LLVM_LIBS})

if(MSVC)
  # Inline code in LLVM headers (ModuleSummaryIndex/ScaledNumber) triggers C4244
  # at instantiation sites owned by our TU; exempt only that translation unit.
  set_source_files_properties(src/codegen/codegen.cpp PROPERTIES
    COMPILE_FLAGS "/wd4244 /wd4267")
endif()
