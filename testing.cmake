if (__testing_columnar_includedd)
	return ()
endif ()
set ( __testing_columnar_includedd YES )

include ( CTest )

if (NOT BUILD_TESTING)
	return()
endif()

if (NOT TARGET columnar)
	return()
endif()

# cb called by manticore ubertest - reject all non-columnar here.
function ( ubertest_filter accept_var explain_var labels_list )
	if (NOT COLUMNAR IN_LIST labels_list)
		set ( ${accept_var} 0 PARENT_SCOPE )
		set ( ${explain_var} "not specially columnar" PARENT_SCOPE )
	endif ()
endfunction ()

# cb called by manticore ubertest - modify given (filtered) test properties
function ( ubertest_properties test )
	set_property ( TEST "${test}" APPEND PROPERTY ENVIRONMENT "LIB_MANTICORE_COLUMNAR=$<TARGET_FILE:columnar>" )
endfunction ()

# set up env for configure, build and run manticore tests
set ( TEST_SPECIAL_EXTERNAL ON )

if (DEFINED ENV{MANTICORE_LOCATOR})
	set ( MANTICORE_LOCATOR $ENV{MANTICORE_LOCATOR} )
elseif (EXISTS "${columnar_SOURCE_DIR}/local_manticore_src.txt")
	file ( READ "${columnar_SOURCE_DIR}/local_manticore_src.txt" MANTICORE_LOCATOR )
else()
	file ( READ "${columnar_SOURCE_DIR}/manticore_src.txt" MANTICORE_LOCATOR )
endif ()
string ( CONFIGURE "${MANTICORE_LOCATOR}" MANTICORE_LOCATOR ) # that is to expand possible inside variables
message ( STATUS "Fetch rule is 'FetchContent_Declare ( manticore ${MANTICORE_LOCATOR} )'" )
file ( WRITE "${columnar_BINARY_DIR}/manticore-get.cmake" "FetchContent_Declare ( manticore ${MANTICORE_LOCATOR} )\n" )

include ( FetchContent )
include ( "${columnar_BINARY_DIR}/manticore-get.cmake" )
FetchContent_MakeAvailable ( manticore )
