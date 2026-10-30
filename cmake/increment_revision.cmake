# ============================================================
# increment_revision.cmake - colang build-time Revision counter (4th version field)
#
# Invoked by CMakeLists.txt as a PRE_BUILD custom command on the `colang`
# target (VS generator only):
#   cmake -DSOURCE_DIR=.. -DBINARY_DIR=.. -DVER_MAJOR=.. -DVER_MINOR=..
#         -DVER_PATCH=.. -DVER_STRING=.. -P cmake/increment_revision.cmake
#
# Semantics (P-design, user approved; revised 2026-10-24, reset narrowed 2026-10-28):
#   - Bump by 1 ONLY when colang actually rebuilds/relinks (a no-op incremental
#     build leaves the target up-to-date and this script never runs). Monotonic
#     WITHIN one three-segment version (a plain rebuild / deleting build/ does NOT
#     reset it).
#   - Revision is written as 0 ONLY on a CONFIRMED three-segment version change:
#     revision.txt carries a parseable "<ver> <rev>" and <ver> differs from
#     VER_STRING (e.g. 0.4.0 vs 0.4.1). While the front three fields are
#     unchanged the counter is never rewound, and a record that cannot confirm
#     the version (legacy bare number, damaged file) NEVER resets either - it
#     keeps counting, so a reset can never happen by accident (user rule
#     2026-10-28: no reset while MAJOR.MINOR.PATCH is unchanged).
#     A missing/unusable record has no history to preserve and starts at 0.
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
set(_stored_ver "")   # "" = version unknown (record missing / legacy / damaged)
set(_n "0")
set(_have_n FALSE)      # TRUE once a counter value was recovered from the record
if(EXISTS "${_rev_file}")
  file(READ "${_rev_file}" _rv)
  if(_rv MATCHES "([0-9]+)\\.([0-9]+)\\.([0-9]+)[ \r\n\t]+([0-9]+)")
    set(_stored_ver "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}")
    set(_n "${CMAKE_MATCH_4}")
    set(_have_n TRUE)
  else()
    # legacy / bare-number form: the counter is known but the version is not, so
    # keep counting instead of resetting (a reset needs a CONFIRMED version
    # change; never rewind on an unreadable record).
    string(REGEX REPLACE "[^0-9]" "" _try "${_rv}")
    if(NOT _try STREQUAL "")
      set(_n "${_try}")
      set(_have_n TRUE)
    endif()
  endif()
endif()

# ---- advance the counter (reset ONLY on a confirmed version change) ----
# Same three-segment version -> monotonic +1 (rebuild / clearing build/ never
# resets). Confirmed version change -> 0. Unconfirmed record -> keep counting.
if(NOT _have_n)
  set(_rev "0")                          # no history at all: first recorded pass
elseif(_stored_ver STREQUAL "")
  math(EXPR _rev "${_n} + 1")            # version unknown: never rewind
elseif(NOT _stored_ver STREQUAL "${VER_STRING}")
  set(_rev "0")                          # confirmed MAJOR.MINOR.PATCH change
else()
  math(EXPR _rev "${_n} + 1")            # front three fields unchanged
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

# 1) persist "<version> <revision>" (0 only on a confirmed version change,
#    otherwise previous + 1 - see the decision block above)
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
