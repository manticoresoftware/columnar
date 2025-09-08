if (__testing_columnar_includedd)
	return ()
endif ()
set ( __testing_columnar_includedd YES )

include ( CTest )

if (NOT BUILD_TESTING)
	return ()
endif ()

if ((NOT TARGET columnar_lib) AND (NOT TARGET secondary_index))
	return ()
endif ()

# cb called by manticore ubertest adding tests - add special columnar\secondary-pass for rt tests
function ( special_ubertest_addtest testN tst_name REQUIRES )
	if (NOT NON-RT IN_LIST REQUIRES AND NOT NON-COLUMNAR IN_LIST REQUIRES)
		add_ubertest ( "${testN}" "${tst_name}" "${REQUIRES}" "col" "COLUMNAR" "--rt --ignore-weights --columnar" )
	elseif (NOT NON-SECONDARY IN_LIST REQUIRES)
		add_ubertest ( "${testN}" "${tst_name}" "${REQUIRES}" "secondary" "SECONDARY" "" )
	endif ()
endfunction ()

# cb called by manticore ubertest - filter out non-columnar or non-secondary here.
function ( special_ubertest_filter accept_var explain_var REQUIRES )
	if ((NOT COLUMNAR IN_LIST REQUIRES) AND (NOT SECONDARY IN_LIST REQUIRES))
		set ( ${accept_var} 0 PARENT_SCOPE )
		set ( ${explain_var} "not specially columnar" PARENT_SCOPE )
	endif ()
endfunction ()

# cb called by manticore ubertest - append path to columnar to given test properties
function ( special_ubertest_properties test )
	set_property ( TEST "${test}" APPEND PROPERTY ENVIRONMENT "LIB_MANTICORE_COLUMNAR=$<TARGET_FILE:columnar_lib>" )
	set_property ( TEST "${test}" APPEND PROPERTY ENVIRONMENT "LIB_MANTICORE_SECONDARY=$<TARGET_FILE:secondary_index>" )
endfunction ()

# this will switch off pure manticore-specific tests: google, api, keyword consistency and benches (we don't need them here)
set ( TEST_SPECIAL_EXTERNAL ON )

message ( STATUS "Checking MANTICORE_LOCATOR sources..." )
if (DEFINED ENV{MANTICORE_LOCATOR} AND NOT "$ENV{MANTICORE_LOCATOR}" STREQUAL "")
	set ( MANTICORE_LOCATOR $ENV{MANTICORE_LOCATOR} )
	message ( STATUS "Using MANTICORE_LOCATOR from environment: '${MANTICORE_LOCATOR}'" )
elseif (EXISTS "${columnar_SOURCE_DIR}/local_manticore_src.txt")
	file ( READ "${columnar_SOURCE_DIR}/local_manticore_src.txt" MANTICORE_LOCATOR )
	message ( STATUS "Using MANTICORE_LOCATOR from local_manticore_src.txt: '${MANTICORE_LOCATOR}'" )
else ()
	file ( READ "${columnar_SOURCE_DIR}/manticore_src.txt" MANTICORE_LOCATOR )
	message ( STATUS "Using MANTICORE_LOCATOR from manticore_src.txt: '${MANTICORE_LOCATOR}'" )
endif ()

message ( STATUS "MANTICORE_LOCATOR before configure: '${MANTICORE_LOCATOR}'" )
string ( CONFIGURE "${MANTICORE_LOCATOR}" MANTICORE_LOCATOR ) # that is to expand possible inside variables
message ( STATUS "MANTICORE_LOCATOR after configure: '${MANTICORE_LOCATOR}'" )

file ( WRITE "${columnar_BINARY_DIR}/manticore-get.cmake" "FetchContent_Declare ( manticore ${MANTICORE_LOCATOR} GIT_SUBMODULES \"\" )\n" )
message ( STATUS "Written to ${columnar_BINARY_DIR}/manticore-get.cmake: 'FetchContent_Declare ( manticore ${MANTICORE_LOCATOR} GIT_SUBMODULES \"\" )'" )

include ( FetchContent )
include ( "${columnar_BINARY_DIR}/manticore-get.cmake" )

# add manticore sources to the tree. All testing will be done on manticore side; necessary additional tests/properties will
# be set by cb functions defined above.
FetchContent_MakeAvailable ( manticore )