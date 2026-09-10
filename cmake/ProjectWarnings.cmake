# Keep diagnostics local to each first-party target. Directory-wide flags
# also affect FetchContent dependencies (including Catch2 added after src).
function(obd_target_warnings target)
    target_compile_options(${target} PRIVATE -Wall -Wextra)
    if(OBD_WARNINGS_AS_ERRORS)
        target_compile_options(${target} PRIVATE -Werror)
    endif()
endfunction()
