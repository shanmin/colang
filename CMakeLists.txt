#
# CMakeLists.txt — colang (compiler front end) build configuration
#
# Dependency: LLVM dev libs (C++ headers + static libs + CMake config)
#   Local build: LLVM 23.1.0 from source (X86-only, Release, static, /MT CRT
#     via -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded; 2026-10-23 MT flip)
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
#   OFF = /MD dynamic CRT
#   ON  = /MT static CRT (requires the local LLVM tree rebuilt with
#         -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded; the legacy
#         LLVM_USE_CRT_<CFG> knob is no longer consumed in LLVM 23)
# Default ON: the local LLVM 23.1.0 build (X86 backend + lld closure) was
# rebuilt /MT (2026-10-23 flip), and colang's in-process lld embeds those libs,
# so colang must link /MT too or LNK2038 fires. Keeps the "no VC redist" story.
option(COLANG_STATIC_CRT "Use static CRT (/MT), must match how LLVM libs were built" ON)

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

# PE version resource: parse version.h (single source of truth) -> configure_file version.rc
if(MSVC)
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/include/colang/version.h" _colang_ver_h)
  foreach(_comp MAJOR MINOR PATCH)
    string(REGEX MATCH "COLANG_VERSION_${_comp} +([0-9]+)" _m "${_colang_ver_h}")
    set(COLANG_VER_${_comp} "${CMAKE_MATCH_1}")
  endforeach()
  string(REGEX MATCH "COLANG_VERSION_STRING +\"([^\"]+)\"" _m "${_colang_ver_h}")
  set(COLANG_VERSION_STRING "${CMAKE_MATCH_1}")
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.rc.in"
                 "${CMAKE_CURRENT_BINARY_DIR}/version.rc" @ONLY)
endif()

if(MSVC)
  add_executable(colang ${COLANG_SOURCES} "${CMAKE_CURRENT_BINARY_DIR}/version.rc")
else()
  add_executable(colang ${COLANG_SOURCES})
endif()

target_include_directories(colang PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(colang SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})

# LLVM components (Phase 0 spike-verified closure, do not trim):
#   front end      — Core / Support / BitWriter / BitReader / TargetParser /
#                    IRReader / Option / Passes / Coroutines
#   X86 lowering   — Target / X86 / CodeGen / SelectionDAG / AsmParser /
#                    Analysis / ScalarOpts / IPO / TransformUtils /
#                    ObjCARCOpts / InstCombine
#   lld COFF deps  — Object / BinaryFormat / MC / MCParser / Demangle /
#                    DebugInfo* / Windows* / LibDriver / LTO / DTLTO
#   Names must be valid component names so llvm_map_components_to_libnames
#   computes the ordered dependency closure ("all" does NOT expand here).
llvm_map_components_to_libnames(COLANG_LLVM_LIBS
  Core Support BitWriter BitReader TargetParser Target X86
  Analysis ScalarOpts IPO TransformUtils ObjCARCOpts InstCombine
  CodeGen SelectionDAG AsmParser IRReader
  Object BinaryFormat MC MCParser Option Passes Coroutines
  DebugInfoPDB DebugInfoCodeView DebugInfoDWARF DebugInfoMSF
  WindowsDriver WindowsManifest LibDriver LTO DTLTO Demangle)

# lld is shipped as static libs but has no CMake package here. Its public
# header lld/Common/Driver.h lives in the lld source tree, a sibling of the
# llvm source tree referenced by LLVM_MAIN_INCLUDE_DIR. Derive that path
# (overridable via a cache var); the lld libs sit in ${LLVM_LIBRARY_DIR},
# which LLVM exports as build/$(Configuration)/lib (MSBuild resolves the
# config at build time, so this is correct for the multi-config VS generator).
set(LLD_INCLUDE_DIR "${LLVM_MAIN_INCLUDE_DIR}/../../lld/include" CACHE PATH
    "Path to the lld public headers (lld/Common/Driver.h)")
get_filename_component(LLD_INCLUDE_DIR "${LLD_INCLUDE_DIR}" ABSOLUTE)
target_include_directories(colang SYSTEM PRIVATE "${LLD_INCLUDE_DIR}")

# Link lld libs first (they depend on LLVM, not vice versa), then LLVM closure.
target_link_libraries(colang PRIVATE
  "${LLVM_LIBRARY_DIR}/lldCOFF.lib"
  "${LLVM_LIBRARY_DIR}/lldCommon.lib"
  ${COLANG_LLVM_LIBS})

if(MSVC)
  # Inline code in LLVM/lld backend headers (ModuleSummaryIndex/ScaledNumber and
  # COFF/X86 lowering) triggers C4244/C4267 at instantiation sites owned by our
  # TU; and the X86 lowering templates inlined into codegen.cpp trip C4702
  # (unreachable code inside llvm/ADT DenseMap.h/bit.h) during the codegen phase.
  # All originate from LLVM headers, not our code; exempt the two TUs that pull
  # the codegen + lld headers.
  set_source_files_properties(src/codegen/codegen.cpp PROPERTIES
    COMPILE_FLAGS "/wd4244 /wd4267 /wd4702")
  set_source_files_properties(src/link/lld_link.cpp PROPERTIES
    COMPILE_FLAGS "/wd4244 /wd4267")
endif()
