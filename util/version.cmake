cmake_minimum_required ( VERSION 3.17 )

include(rev)

configure_file ( ${VERSION_SRC} "${VERSION_TRG}1" )

set ( NEED_NEWFILE TRUE )
execute_process ( COMMAND ${CMAKE_COMMAND} -E compare_files "${VERSION_TRG}" "${VERSION_TRG}1" RESULT_VARIABLE _res )
if (_res EQUAL 0)
	set ( NEED_NEWFILE FALSE )
endif ()

if (NEED_NEWFILE)
	message ( STATUS "Version ${VERSION_STR} ${GIT_COMMIT_ID}@${GIT_TIMESTAMP_ID}, ${GIT_BRANCH_ID}" )
	configure_file ( "${VERSION_TRG}1" "${VERSION_TRG}" COPYONLY )
	file ( REMOVE "${VERSION_TRG}1" )
else ()
	message ( STATUS "Version not changed: ${VERSION_STR} ${GIT_COMMIT_ID}@${GIT_TIMESTAMP_ID}, ${GIT_BRANCH_ID}" )
endif ()
