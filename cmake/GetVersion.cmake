cmake_minimum_required ( VERSION 2.8 FATAL_ERROR )

function(get_library_version OUTVAR HEADER)
        FILE ( STRINGS "${HEADER}" _STRINGS LIMIT_COUNT 500 REGEX "^#define[ \t]+VERSION_STR.*" )
        STRING ( REGEX REPLACE ".*\"(.*)\"(.*)$" "\\1" VERSION_STR "${_STRINGS}" )
        set("${OUTVAR}" "${VERSION_STR}" PARENT_SCOPE)
endfunction()

