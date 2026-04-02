# gen_git_sha.cmake — invoked as a custom command at build time.
# Writes git_sha.h only when the SHA has changed, so downstream
# recompilation is skipped when nothing moved.

execute_process(
  COMMAND git describe --match ForceNone --abbrev=8 --always
  WORKING_DIRECTORY ${SRC_DIR}
  OUTPUT_VARIABLE GIT_SHA
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET
  RESULT_VARIABLE GIT_RESULT
)
if(NOT GIT_RESULT EQUAL 0 OR GIT_SHA STREQUAL "")
  set(GIT_SHA "00000000")
endif()

set(NEW_CONTENT "#pragma once\n#define BM_SBC_GIT_SHA 0x${GIT_SHA}\n")

# Only overwrite if content changed — avoids unnecessary recompilation.
if(EXISTS ${OUT})
  file(READ ${OUT} OLD_CONTENT)
  if(OLD_CONTENT STREQUAL NEW_CONTENT)
    return()
  endif()
endif()

file(WRITE ${OUT} ${NEW_CONTENT})
