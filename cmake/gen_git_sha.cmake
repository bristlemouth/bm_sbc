# gen_git_sha.cmake — invoked as a custom command at build time.
# Writes git_sha.h only when content has changed, so downstream
# recompilation is skipped when nothing moved.
#
# Expects: -DSRC_DIR=<repo root>  -DOUT=<output header path>

# --- Git SHA (8-char abbreviated) -----------------------------------------
execute_process(
  COMMAND git describe --match ForceNone --abbrev=8 --always
  WORKING_DIRECTORY ${SRC_DIR}
  OUTPUT_VARIABLE GIT_SHA
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET
  RESULT_VARIABLE GIT_SHA_RESULT
)
if(NOT GIT_SHA_RESULT EQUAL 0 OR GIT_SHA STREQUAL "")
  set(GIT_SHA "00000000")
endif()

# --- Version tag (e.g. v0.1.0, v0.1.0-3-g472aefb3-dirty) ----------------
execute_process(
  COMMAND git describe --always --dirty --abbrev=8
  WORKING_DIRECTORY ${SRC_DIR}
  OUTPUT_VARIABLE VERSION_TAG
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET
  RESULT_VARIABLE VERSION_TAG_RESULT
)
if(NOT VERSION_TAG_RESULT EQUAL 0 OR VERSION_TAG STREQUAL "")
  set(VERSION_TAG "${GIT_SHA}")
endif()

# --- Parse major.minor.patch from the tag ---------------------------------
if(VERSION_TAG MATCHES "^v([0-9]+)\\.([0-9]+)\\.([0-9]+)")
  set(VER_MAJOR ${CMAKE_MATCH_1})
  set(VER_MINOR ${CMAKE_MATCH_2})
  set(VER_PATCH ${CMAKE_MATCH_3})
else()
  set(VER_MAJOR 0)
  set(VER_MINOR 0)
  set(VER_PATCH 0)
endif()

# --- Compose header -------------------------------------------------------
set(NEW_CONTENT "\
#pragma once
#define BM_SBC_GIT_SHA         0x${GIT_SHA}
#define BM_SBC_VERSION_TAG     \"${VERSION_TAG}\"
#define BM_SBC_VERSION_MAJOR   ${VER_MAJOR}
#define BM_SBC_VERSION_MINOR   ${VER_MINOR}
#define BM_SBC_VERSION_PATCH   ${VER_PATCH}
")

# Only overwrite if content changed — avoids unnecessary recompilation.
if(EXISTS ${OUT})
  file(READ ${OUT} OLD_CONTENT)
  if(OLD_CONTENT STREQUAL NEW_CONTENT)
    return()
  endif()
endif()

file(WRITE ${OUT} ${NEW_CONTENT})
