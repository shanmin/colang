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
# Project (compiler-private) headers live under src/include/colang, collected
# recursively. A header edit still forces a real recompile+relink, so it must
# count as a rebuild for the revision counter. The generated colang_revision.h
# lives under ${CMAKE_CURRENT_BINARY_DIR}/include, so it is never matched here.
# NOTE: the Co standard-library headers now live at the top-level include/
# (shipped to dist/include) and are deliberately NOT globbed here, so editing a
# stdlib header does not bump the compiler revision counter.
file(GLOB_RECURSE COLANG_HEADERS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/include/*.h)

# PE version resource + Revision (4th field) counter (cmake/revision.txt).
# Parse the three hand-authored components from version.h (single source of truth).
if(MSVC)
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/include/colang/version.h" _colang_ver_h)
  foreach(_comp MAJOR MINOR PATCH)
    string(REGEX MATCH "COLANG_VERSION_${_comp} +([0-9]+)" _m "${_colang_ver_h}")
    set(COLANG_VER_${_comp} "${CMAKE_MATCH_1}")
  endforeach()
  string(REGEX MATCH "COLANG_VERSION_STRING +\"([^\"]+)\"" _m "${_colang_ver_h}")
  set(COLANG_VERSION_STRING "${CMAKE_MATCH_1}")
  # read persisted "<version> <revision>" for the non-auto path seed. The
  # recorded counter is taken as-is unless the record CONFIRMS a front-version
  # change (parseable version that differs from the current three segments ->
  # seed 0, mirrors the script). A legacy/unreadable record never resets: the
  # number is kept so the seed is never rewound by accident.
  set(COLANG_VER_REVISION "0")
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/revision.txt")
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/cmake/revision.txt" _rvtxt)
    if(_rvtxt MATCHES "([0-9]+)\\.([0-9]+)\\.([0-9]+)[ \r\n\t]+([0-9]+)")
      set(_seed_ver "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}")
      if(_seed_ver STREQUAL COLANG_VERSION_STRING)
        set(COLANG_VER_REVISION "${CMAKE_MATCH_4}")
      endif()
    else()
      # legacy / bare-number form: keep the recorded counter (version unknown,
      # so no reset is ever inferred from it).
      string(REGEX REPLACE "[^0-9]" "" _seed_n "${_rvtxt}")
      if(NOT _seed_n STREQUAL "")
        set(COLANG_VER_REVISION "${_seed_n}")
      endif()
    endif()
  endif()
  set(COLANG_VERSION_FULL "${COLANG_VERSION_STRING}.${COLANG_VER_REVISION}")
endif()

set(_rev_hdr "${CMAKE_CURRENT_BINARY_DIR}/include/colang/colang_revision.h")
set(_rc_out  "${CMAKE_CURRENT_BINARY_DIR}/version.rc")

# Revision counter semantics: bump +1 ONLY when a real source changes, and embed
# the new value the same pass. Achieved with a source-level custom command whose
# OUTPUTs are the generated revision header + PE resource and whose DEPENDS are
# the colang sources: it reruns iff a source is newer than those OUTPUTs, which
# also makes colang relink; a no-op build leaves the OUTPUTs up-to-date so nothing
# reruns and the number holds. (A TARGET PRE_BUILD step would fire on every build
# on the VS generator, so it is NOT used.) VS-only: Ninja/Make map PRE_BUILD to
# PRE_LINK and would loop; other generators seed version.rc and keep fallback 0.
if(MSVC AND CMAKE_GENERATOR MATCHES "Visual Studio")
  add_custom_command(
    OUTPUT "${_rev_hdr}" "${_rc_out}"
    COMMAND ${CMAKE_COMMAND}
      -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
      -DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
      -DVER_MAJOR=${COLANG_VER_MAJOR}
      -DVER_MINOR=${COLANG_VER_MINOR}
      -DVER_PATCH=${COLANG_VER_PATCH}
      -DVER_STRING=${COLANG_VERSION_STRING}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/increment_revision.cmake
    DEPENDS ${COLANG_SOURCES} ${COLANG_HEADERS}
            ${CMAKE_CURRENT_SOURCE_DIR}/cmake/increment_revision.cmake
            ${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.rc.in
    COMMENT "bump colang revision (only on source change)")
  add_executable(colang ${COLANG_SOURCES} "${_rc_out}" "${_rev_hdr}")
  set_source_files_properties("${_rev_hdr}" PROPERTIES HEADER_FILE_ONLY TRUE)
elseif(MSVC)
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.rc.in" "${_rc_out}" @ONLY)
  add_executable(colang ${COLANG_SOURCES} "${_rc_out}")
else()
  add_executable(colang ${COLANG_SOURCES})
endif()

# Generated revision header lives in the binary include dir
target_include_directories(colang PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/include)

target_include_directories(colang PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/include)
target_include_directories(colang SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})

# LLVM components. The front-end + X86 lowering + Object/MC closure is shared
# by both COFF (Windows) and ELF (Linux) paths; only the debug-info/driver
# extras are format-specific. Names must be valid component names so
# llvm_map_components_to_libnames computes the ordered dependency closure
# ("all" does NOT expand here).
set(COLANG_LLVM_COMPONENTS
  Core Support BitWriter BitReader TargetParser Target X86
  Analysis ScalarOpts IPO TransformUtils ObjCARCOpts InstCombine
  CodeGen SelectionDAG AsmParser IRReader
  Object BinaryFormat MC MCParser Option Passes Coroutines
  Demangle DebugInfoDWARF)
if(MSVC)
  # Windows/COFF path: PE debug info + Windows driver + lld-CFF deps.
  list(APPEND COLANG_LLVM_COMPONENTS
    DebugInfoPDB DebugInfoCodeView DebugInfoMSF
    WindowsDriver WindowsManifest LibDriver LTO DTLTO)
else()
  # Linux/ELF path: strip the Windows-only components above; lld::elf still
  # pulls LibDriver/LTO for the linker closure.
  list(APPEND COLANG_LLVM_COMPONENTS LibDriver LTO DTLTO)
endif()
llvm_map_components_to_libnames(COLANG_LLVM_LIBS ${COLANG_LLVM_COMPONENTS})

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
# The linker backend is platform-specific: a Windows build embeds lldCOFF, a
# Linux build embeds lldELF; the other driver is not compiled in (guarded by
# LLD_HAS_DRIVER in lld_link.cpp), so we must not request its lib either.
if(MSVC)
  set(COLANG_LLD_LIBS
    "${LLVM_LIBRARY_DIR}/lldCOFF.lib"
    "${LLVM_LIBRARY_DIR}/lldCommon.lib")
else()
  set(COLANG_LLD_LIBS
    "${LLVM_LIBRARY_DIR}/liblldELF.a"
    "${LLVM_LIBRARY_DIR}/liblldCommon.a")
endif()
target_link_libraries(colang PRIVATE ${COLANG_LLD_LIBS} ${COLANG_LLVM_LIBS})

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
