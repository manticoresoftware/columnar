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

include ( ${CMAKE_CURRENT_LIST_DIR}/GetONNXRuntime.cmake )
include ( ${CMAKE_CURRENT_LIST_DIR}/GetMKL.cmake )

function(prepare_embeddings_ort)
	if ( APPLE )
		set ( _platform "macos" )
	elseif ( WIN32 )
		set ( _platform "windows" )
	elseif ( UNIX )
		set ( _platform "linux" )
	else()
		message ( FATAL_ERROR "prepare_embeddings_ort: unsupported host platform" )
	endif()

	if ( CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$" )
		set ( _arch "aarch64" )
	else()
		set ( _arch "x64" )
	endif()

	embeddings_ort_download ( _lib_dir "${_platform}" "${_arch}" "${CMAKE_CURRENT_BINARY_DIR}/embeddings/ort" )

	set ( ENV{ORT_LIB_LOCATION} "${_lib_dir}" )
	message ( STATUS "Using ORT_LIB_LOCATION=${_lib_dir}" )
endfunction()

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
	prepare_embeddings_ort()

	# Enable platform-specific BLAS acceleration for candle when available.
	if (DEFINED EMBEDDINGS_CARGO_FEATURES)
		set(EMBEDDINGS_FEATURES_CSV "${EMBEDDINGS_CARGO_FEATURES}")
	else()
		set(EMBEDDINGS_FEATURE_LIST)
		if(APPLE)
			list(APPEND EMBEDDINGS_FEATURE_LIST accelerate)
		elseif(UNIX AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64)$")
			# Linux x86_64: ensure MKL static archives are in scope (detect a
			# system install, else download via cmake/GetMKL.cmake) and opt
			# into the mkl feature. aarch64 has no MKL equivalent — skip.
			embeddings_mkl_prepare_linux ( _mklroot "${CMAKE_CURRENT_BINARY_DIR}/embeddings/mkl" )
			set ( ENV{MKLROOT} "${_mklroot}" )
			message ( STATUS "Using MKLROOT=${_mklroot}" )
			list(APPEND EMBEDDINGS_FEATURE_LIST mkl)
		endif()
		list(JOIN EMBEDDINGS_FEATURE_LIST "," EMBEDDINGS_FEATURES_CSV)
	endif()

	# When the static lib is in scope, drop any caller-supplied download-ort
	# feature so cargo doesn't fetch a duplicate dynamic lib on top of the
	# static one. The auto-detected feature lists above never include it; this
	# only matters when the caller passes EMBEDDINGS_CARGO_FEATURES explicitly.
	if (DEFINED ENV{ORT_LIB_LOCATION} AND NOT "$ENV{ORT_LIB_LOCATION}" STREQUAL "" AND EMBEDDINGS_FEATURES_CSV)
		string(REPLACE "," ";" EMBEDDINGS_FEATURE_LIST "${EMBEDDINGS_FEATURES_CSV}")
		list(REMOVE_ITEM EMBEDDINGS_FEATURE_LIST download-ort)
		list(JOIN EMBEDDINGS_FEATURE_LIST "," EMBEDDINGS_FEATURES_CSV)
	endif()

	if (EMBEDDINGS_FEATURES_CSV)
		set(EMBEDDINGS_CARGO_FEATURE_ARGS "--features" "${EMBEDDINGS_FEATURES_CSV}")
	endif()

	execute_process (
			COMMAND cargo build --manifest-path ${CMAKE_SOURCE_DIR}/embeddings/Cargo.toml --lib --release ${EMBEDDINGS_CARGO_FEATURE_ARGS} --target-dir ${CMAKE_CURRENT_BINARY_DIR}/embeddings
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
