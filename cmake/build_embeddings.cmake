# Copyright (c) 2020-2025, Manticore Software LTD (https://manticoresearch.com)
# All rights reserved
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#    http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

cmake_minimum_required ( VERSION 3.17 )

if (__build_embeddings_included)
	return ()
endif ()
set ( __build_embeddings_included YES )

function(build_embeddings_lib)
	message ( STATUS "building embeddings locally..." )

	# Set platform-specific library file names
	if(WIN32)
		set(EMBEDDINGS_LIB_FILE_SRC "${EMBEDDINGS_LIB_NAME}.dll")
		set(EMBEDDINGS_LIB_FILE_DST "lib_${EMBEDDINGS_LIB_NAME}.dll")
	elseif(APPLE)
		set(EMBEDDINGS_LIB_FILE_SRC "lib${EMBEDDINGS_LIB_NAME}.dylib")
		set(EMBEDDINGS_LIB_FILE_DST "lib_${EMBEDDINGS_LIB_NAME}.dylib")
	else()
		set(EMBEDDINGS_LIB_FILE_SRC "lib${EMBEDDINGS_LIB_NAME}.so")
		set(EMBEDDINGS_LIB_FILE_DST "lib_${EMBEDDINGS_LIB_NAME}.so")
	endif()

	if (NOT DEFINED CARGO_COMMAND)
		find_program ( CARGO_COMMAND cargo )
		if (NOT CARGO_COMMAND)
			message ( FATAL_ERROR "Cargo command not found. Please install Rust and ensure cargo is in your PATH." )
		endif ()
	endif ()

	# Pass git commit and timestamp to build.rs via environment variables
	# These variables are set by cmake/rev.cmake which is included in the main CMakeLists.txt
	# The build.rs script uses these to generate a version string in the format:
	# "VERSION commit@timestamp" (e.g., "1.1.0 38f499e@25112313")
	# This matches the format used by other Manticore libraries for consistent version display
	set(ENV{GIT_COMMIT_ID} "${GIT_COMMIT_ID}")
	set(ENV{GIT_TIMESTAMP_ID} "${GIT_TIMESTAMP_ID}")

	execute_process (
			COMMAND cargo build --manifest-path ${CMAKE_SOURCE_DIR}/embeddings/Cargo.toml --lib --release --target-dir ${CMAKE_CURRENT_BINARY_DIR}/embeddings
			RESULT_VARIABLE CMD_RESULT
	)

	if (NOT CMD_RESULT EQUAL 0)
		message ( FATAL_ERROR "Failed to build: ${CMD_RESULT}" )
	endif ()

	set(EMBEDDINGS_LIB_SRC_PATH "${CMAKE_CURRENT_BINARY_DIR}/embeddings/release/${EMBEDDINGS_LIB_FILE_SRC}")
	set(EMBEDDINGS_LIB_DST_PATH "${CMAKE_CURRENT_BINARY_DIR}/embeddings/release/${EMBEDDINGS_LIB_FILE_DST}")
	
	# Check that the library file exists before attempting to rename it.
	# This provides a clearer error message if EMBEDDINGS_LIB_NAME was not set correctly
	# or if the cargo build produced a different filename than expected.
	if (NOT EXISTS "${EMBEDDINGS_LIB_SRC_PATH}")
		message ( FATAL_ERROR "Expected library file not found: ${EMBEDDINGS_LIB_SRC_PATH}" )
	endif ()
	
	file(RENAME "${EMBEDDINGS_LIB_SRC_PATH}" "${EMBEDDINGS_LIB_DST_PATH}")
	if ( EXISTS "${CMAKE_CURRENT_BINARY_DIR}/embeddings/release/${EMBEDDINGS_LIB_NAME}.pdb" )
		file(RENAME "${CMAKE_CURRENT_BINARY_DIR}/embeddings/release/${EMBEDDINGS_LIB_NAME}.pdb" "${CMAKE_CURRENT_BINARY_DIR}/embeddings/release/lib_${EMBEDDINGS_LIB_NAME}.pdb")
	endif()
endfunction ()

