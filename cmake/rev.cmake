cmake_minimum_required ( VERSION 3.17 )

# guess version strings from current git repo
function ( guess_from_git )
	if (NOT EXISTS "${columnar_SOURCE_DIR}/.git")
		return ()
	endif ()

	find_package ( Git QUIET )
	if (NOT GIT_FOUND)
		return ()
	endif ()

	# extract short hash as GIT_COMMIT_ID
	execute_process ( COMMAND "${GIT_EXECUTABLE}" log -1 --format=%h
			WORKING_DIRECTORY "${columnar_SOURCE_DIR}"
			RESULT_VARIABLE res
			OUTPUT_VARIABLE GIT_COMMIT_ID
			ERROR_QUIET
			OUTPUT_STRIP_TRAILING_WHITESPACE )
	set ( GIT_COMMIT_ID "${GIT_COMMIT_ID}" PARENT_SCOPE )

	# extract timestamp and make number YYMMDD from it
	# it would be --date=format:%y%m%d, but old git on centos doesn't understand it
	execute_process ( COMMAND "${GIT_EXECUTABLE}" log -1 --date=short --format=%cd
			WORKING_DIRECTORY "${columnar_SOURCE_DIR}"
			RESULT_VARIABLE res
			OUTPUT_VARIABLE GIT_TIMESTAMP_ID
			ERROR_QUIET
			OUTPUT_STRIP_TRAILING_WHITESPACE )
	string ( REPLACE "-" "" GIT_TIMESTAMP_ID "${GIT_TIMESTAMP_ID}" )
	string ( SUBSTRING "${GIT_TIMESTAMP_ID}" 2 -1 GIT_TIMESTAMP_ID )
	set ( GIT_TIMESTAMP_ID "${GIT_TIMESTAMP_ID}" PARENT_SCOPE )

	# timestamp for reproducable packages
	execute_process ( COMMAND "${GIT_EXECUTABLE}" log -1 --pretty=%ct
			WORKING_DIRECTORY "${columnar_SOURCE_DIR}"
			RESULT_VARIABLE res
			OUTPUT_VARIABLE GIT_EPOCH_ID
			ERROR_QUIET
			OUTPUT_STRIP_TRAILING_WHITESPACE )
	set ( ENV{SOURCE_DATE_EPOCH} ${GIT_EPOCH_ID} )

	# extract branch name (top of 'git status -s -b'), throw out leading '## '
	execute_process ( COMMAND "${GIT_EXECUTABLE}" status -s -b
			WORKING_DIRECTORY "${columnar_SOURCE_DIR}"
			RESULT_VARIABLE res
			OUTPUT_VARIABLE GIT_BRANCH_ID
			ERROR_QUIET
			OUTPUT_STRIP_TRAILING_WHITESPACE )
	string ( REGEX REPLACE "\n.*$" "" GIT_BRANCH_ID "${GIT_BRANCH_ID}" )
	string ( REPLACE "## " "" GIT_BRANCH_ID "${GIT_BRANCH_ID}" )
	set ( GIT_BRANCH_ID "git branch ${GIT_BRANCH_ID}" PARENT_SCOPE )
endfunction ()

# guess version strings from template header file (git archive mark it there)
function ( extract_from_git_slug HEADER )
	if (EXISTS "${HEADER}")
		FILE ( STRINGS "${HEADER}" _CONTENT )
		foreach (LINE ${_CONTENT})
			# match definitions like - // GIT_*_ID VALUE
			if ("${LINE}" MATCHES "^//[ \t]+(GIT_.*_ID)[ \t]\"(.*)\"")
				set ( ${CMAKE_MATCH_1} "${CMAKE_MATCH_2}" )
			endif ()
		endforeach ()
		if (GIT_COMMIT_ID STREQUAL "$Format:%h$")
			return () # no slug
		endif ()
		# commit id
		set ( GIT_COMMIT_ID "${GIT_COMMIT_ID}" PARENT_SCOPE )
		# timestamp
		string ( REPLACE "-" "" GIT_TIMESTAMP_ID "${GIT_TIMESTAMP_ID}" )
		string ( SUBSTRING "${GIT_TIMESTAMP_ID}" 2 6 GIT_TIMESTAMP_ID )
		set ( GIT_TIMESTAMP_ID "${GIT_TIMESTAMP_ID}" PARENT_SCOPE )
		# epoch for packaging
		set ( ENV{SOURCE_DATE_EPOCH} ${GIT_EPOCH_ID} )
		# branch id
		set ( GIT_BRANCH_ID "from tarball" PARENT_SCOPE )
	endif ()
endfunction ()

# function definitions finished, execution starts from here
##################################

# first try to use binary git
guess_from_git ()

# 2-nd try - if we build from git archive. Correct hash and date provided then, but no branch
if (NOT GIT_COMMIT_ID)
	extract_from_git_slug ( "${columnar_SOURCE_DIR}/util/version.h.in" )
endif ()

# determine build as even/odd value of patch version
math ( EXPR oddvalue "${PROJECT_VERSION_PATCH} % 2" OUTPUT_FORMAT DECIMAL )

if (oddvalue)
	set ( DEV_BUILD ON )
endif ()

# nothing found
if (NOT GIT_COMMIT_ID)
	message ( STATUS "Dev mode, no guess, using predefined version" )
	set ( GIT_TIMESTAMP_ID "000000" )
	set ( GIT_COMMIT_ID "deadbeef" )
	set ( GIT_BRANCH_ID "developer version" )
	set ( ENV{SOURCE_DATE_EPOCH} "1607089638" )
	set ( DEV_BUILD ON )
endif ()

# configure packaging
SET ( ENV{SOURCE_DATE_EPOCH} "${SOURCE_DATE_EPOCH}" ) # that makes builds reproducable
configure_file ( "${columnar_SOURCE_DIR}/cmake/CPackOptions.cmake.in" "${columnar_BINARY_DIR}/config/CPackOptions.cmake" @ONLY )
