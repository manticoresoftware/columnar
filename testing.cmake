if (__testing_columnar_includedd)
	return ()
endif ()
set ( __testing_columnar_includedd YES )

include ( CTest )

if (NOT BUILD_TESTING)
	return ()
endif ()

if (NOT TARGET columnar_lib)
	return ()
endif ()

# cb called by manticore ubertest adding tests - add special columnar-pass for rt tests
function ( special_ubertest_addtest testN tst_name REQUIRES )
	if (NOT NON-RT IN_LIST REQUIRES AND NOT NON-COLUMNAR IN_LIST REQUIRES)
		add_ubertest ( "${testN}" "${tst_name}" "${REQUIRES}" "col" "COLUMNAR" "--rt --ignore-weights --columnar" )
	endif ()
endfunction ()

# cb called by manticore ubertest - filter out non-columnar here.
function ( special_ubertest_filter accept_var explain_var REQUIRES )
	if (NOT COLUMNAR IN_LIST REQUIRES)
		set ( ${accept_var} 0 PARENT_SCOPE )
		set ( ${explain_var} "not specially columnar" PARENT_SCOPE )
	endif ()
endfunction ()

# cb called by manticore ubertest - append path to columnar to given test properties
function ( special_ubertest_properties test )
	set_property ( TEST "${test}" APPEND PROPERTY ENVIRONMENT "LIB_MANTICORE_COLUMNAR=$<TARGET_FILE:columnar_lib>" )
endfunction ()

# this will switch off pure manticore-specific tests: google, api, keyword consistency and benches (we don't need them here)
set ( TEST_SPECIAL_EXTERNAL ON )

if (DEFINED ENV{MANTICORE_LOCATOR})
	set ( MANTICORE_LOCATOR $ENV{MANTICORE_LOCATOR} )
elseif (EXISTS "${columnar_SOURCE_DIR}/local_manticore_src.txt")
	file ( READ "${columnar_SOURCE_DIR}/local_manticore_src.txt" MANTICORE_LOCATOR )
else ()
	file ( READ "${columnar_SOURCE_DIR}/manticore_src.txt" MANTICORE_LOCATOR )
endif ()
string ( CONFIGURE "${MANTICORE_LOCATOR}" MANTICORE_LOCATOR ) # that is to expand possible inside variables
message ( STATUS "Fetch locator is '${MANTICORE_LOCATOR}'" )
file ( WRITE "${columnar_BINARY_DIR}/manticore-get.cmake" "FetchContent_Declare ( manticore ${MANTICORE_LOCATOR} )\n" )

include ( FetchContent )
include ( "${columnar_BINARY_DIR}/manticore-get.cmake" )

# add manticore sources to the tree. All testing will be done on manticore side; necessary additional tests/properties will
# be set by cb functions defined above.
FetchContent_MakeAvailable ( manticore )
