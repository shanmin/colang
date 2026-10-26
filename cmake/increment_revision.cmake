# ============================================================
# increment_revision.cmake - colang build-time Revision counter (4th version field)
#
# Invoked by CMakeLists.txt as a PRE_BUILD custom command on the `colang`
# target (VS generator only):
#   cmake -DSOURCE_DIR=.. -DBINARY_DIR=.. -DVER_MAJOR=.. -DVER_MINOR=..
#         -DVER_PATCH=.. -DVER_STRING=.. -P cmake/increment_revision.cmake
#
# Semantics (P-design, user approved; revised 2026-10-24):
#   - Bump by 1 ONLY when colang actually rebuilds/relinks (a no-op incremental
#     build leaves the target up-to-date and this script never runs). Monotonic
#     WITHIN one three-segment version (a plain rebuild / deleting build/ does NOT
#     reset it).
#   - When the front version (VER_STRING, e.g. 0.3.1) CHANGES, Revision auto-
#     resets to 0 (the version.h edit itself triggers this pass, and the version
#     recorded in revision.txt no longer matches).
#   - revision.txt persists "<version> <revision>" (checked into VCS).
#   - Writes the counter back to cmake/revision.txt (checked into VCS), and
#     regenerates the build-time header + PE resource with the NEW value so this
#     pass embeds it.
#   - Idempotent writes: files are rewritten only when content changes, to keep
#     mtimes stable and avoid a self-forced rebuild loop.
# Kept ASCII-only (rc/codepage policy, same as build.bat / version.rc.in).
# ============================================================

if(NOT DEFINED SOURCE_DIR OR NOT DEFINED BINARY_DIR)
  message(FATAL_ERROR "increment_revision.cmake: SOURCE_DIR/BINARY_DIR must be passed via -D")
endif()
if(NOT DEFINED VER_STRING)
  set(VER_STRING "0.0.0")
endif()

# ---- read persisted "<version> <revision>" state ----
set(_rev_file "${SOURCE_DIR}/cmake/revision.txt")
set(_stored_ver "")
set(_n "0")
if(EXISTS "${_rev_file}")
  file(READ "${_rev_file}" _rv)
  if(_rv MATCHES "([0-9]+)\\.([0-9]+)\\.([0-9]+)[ \r\n\t]+([0-9]+)")
    set(_stored_ver "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}")
    set(_n "${CMAKE_MATCH_4}")
  else()
    # legacy / bare-number form: recover the number but treat the version as a
    # mismatch so the counter resets on this first new-format pass.
    string(REGEX REPLACE "[^0-9]" "" _try "${_rv}")
    if(NOT _try STREQUAL "")
      set(_n "${_try}")
    endif()
  endif()
endif()

# ---- version-change auto-reset; otherwise +1 on this real rebuild ----
# Front three-segment version changed (or unrecorded) -> Revision resets to 0.
# Same version -> monotonic +1 (rebuild / clearing build/ does NOT reset).
if(NOT _stored_ver STREQUAL "${VER_STRING}")
  set(_rev "0")
else()
  math(EXPR _rev "${_n} + 1")
endif()

set(_full "${VER_STRING}.${_rev}")

# ---- write-only-if-changed helper ----
function(_colang_write_if_changed path content)
  if(EXISTS "${path}")
    file(READ "${path}" _cur)
    if(_cur STREQUAL "${content}")
      return()
    endif()
  endif()
  get_filename_component(_dir "${path}" DIRECTORY)
  file(MAKE_DIRECTORY "${_dir}")
  file(WRITE "${path}" "${content}")
endfunction()

# 1) persist "<version> <revision>" (0 on version change, else previous + 1)
_colang_write_if_changed("${_rev_file}" "${VER_STRING} ${_rev}\n")

# 2) regenerate the build-time revision header (included by version.h)
set(_hdr "// auto-generated at build time by cmake/increment_revision.cmake - do not edit\n#pragma once\n#define COLANG_VERSION_REVISION ${_rev}\n")
_colang_write_if_changed("${BINARY_DIR}/include/colang/colang_revision.h" "${_hdr}")

# 3) regenerate the PE version resource from the template (new revision)
set(COLANG_VER_MAJOR "${VER_MAJOR}")
set(COLANG_VER_MINOR "${VER_MINOR}")
set(COLANG_VER_PATCH "${VER_PATCH}")
set(COLANG_VER_REVISION "${_rev}")
set(COLANG_VERSION_STRING "${VER_STRING}")
set(COLANG_VERSION_FULL "${_full}")

set(_rc_tmp "${BINARY_DIR}/version.rc.gen")
configure_file("${SOURCE_DIR}/cmake/version.rc.in" "${_rc_tmp}" @ONLY)
file(READ "${_rc_tmp}" _rc_new)
_colang_write_if_changed("${BINARY_DIR}/version.rc" "${_rc_new}")
file(REMOVE "${_rc_tmp}")

message(STATUS "colang revision: stored_ver='${_stored_ver}' cur='${VER_STRING}' ${_n} -> ${_rev} (full ${_full})")
