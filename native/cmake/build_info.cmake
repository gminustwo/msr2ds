# The build's commit and its date, for the log's first line (runtime/log.c): written before each build into
# build_info.h, and rewritten only when they change, so a build with nothing new recompiles nothing.
# cmake -DSRC=<the repository> -DOUT=<build_info.h> -P build_info.cmake
set(git git -c safe.directory=* -C "${SRC}")   # (WSL builds from /mnt/d: the checkout belongs to Windows' user)
execute_process(COMMAND ${git} rev-parse --short=9 HEAD OUTPUT_VARIABLE commit OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
execute_process(COMMAND ${git} log -1 --format=%cs OUTPUT_VARIABLE date OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
execute_process(COMMAND ${git} status --porcelain --untracked-files=no OUTPUT_VARIABLE changed OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if (NOT commit)
    set(commit "unknown")
elseif (changed)
    set(commit "${commit}+changes")   # built with changes not yet committed
endif()
if (NOT date)
    set(date "unknown")
endif()
set(text "#define MSR_BUILD_COMMIT \"${commit}\"\n#define MSR_BUILD_DATE \"${date}\"\n")
set(old "")
if (EXISTS "${OUT}")
    file(READ "${OUT}" old)
endif()
if (NOT old STREQUAL text)
    file(WRITE "${OUT}" "${text}")
endif()
