# Writes netplay_build_id.h, rewriting it only when it changed. The id is the last commit that touched code, plus
# "-dirty" if code differs from it, so documentation-only commits do not split players into incompatible builds.
set(PATHSPEC . ":(exclude)docs" ":(exclude)tools" ":(exclude)README.md" ":(exclude)*.md")
execute_process(COMMAND git update-index -q --refresh WORKING_DIRECTORY ${SRC} ERROR_QUIET)
execute_process(COMMAND git log -1 --format=%h --abbrev=12 -- ${PATHSPEC} WORKING_DIRECTORY ${SRC}
                OUTPUT_VARIABLE ID OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
execute_process(COMMAND git diff --quiet HEAD -- ${PATHSPEC} WORKING_DIRECTORY ${SRC} RESULT_VARIABLE DIRTY ERROR_QUIET)
if(NOT ID)
  set(ID "unknown")
endif()
if(DIRTY EQUAL 1)
  set(ID "${ID}-dirty")
endif()
file(WRITE ${OUT}.tmp "#define NETPLAY_BUILD_ID \"${ID}-strictfp\"\n")
execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different ${OUT}.tmp ${OUT})
