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

set ( EMBEDDINGS_ORT_VERSION "" CACHE STRING "ONNX Runtime version used for local Linux embeddings builds; defaults to embeddings/Cargo.toml metadata" )
set ( EMBEDDINGS_ORT_GLIBC "" CACHE STRING "ONNX Runtime glibc baseline used for local Linux embeddings builds; defaults to embeddings/Cargo.toml metadata" )

function(read_embeddings_ort_metadata OUT_ORT_VERSION OUT_ORT_GLIBC)
	set ( CARGO_TOML "${CMAKE_SOURCE_DIR}/embeddings/Cargo.toml" )
	if (NOT EXISTS "${CARGO_TOML}")
		message ( FATAL_ERROR "embeddings Cargo.toml was not found: ${CARGO_TOML}" )
	endif()

	file ( READ "${CARGO_TOML}" CARGO_TOML_CONTENT )
	string ( REGEX MATCH "\\[package\\.metadata\\.manticore\\.ort\\][^\[]*" ORT_METADATA "${CARGO_TOML_CONTENT}" )
	if (NOT ORT_METADATA)
		message ( FATAL_ERROR "Missing [package.metadata.manticore.ort] version/linux-glibc in ${CARGO_TOML}" )
	endif()

	if (NOT ORT_METADATA MATCHES "version[ \t]*=[ \t]*\"([^\"]+)\"")
		message ( FATAL_ERROR "Missing [package.metadata.manticore.ort] version in ${CARGO_TOML}" )
	endif()
	set ( ORT_VERSION "${CMAKE_MATCH_1}" )

	if (NOT ORT_METADATA MATCHES "linux-glibc[ \t]*=[ \t]*\"([^\"]+)\"")
		message ( FATAL_ERROR "Missing [package.metadata.manticore.ort] linux-glibc in ${CARGO_TOML}" )
	endif()
	set ( ORT_GLIBC "${CMAKE_MATCH_1}" )

	if (EMBEDDINGS_ORT_VERSION)
		set ( ORT_VERSION "${EMBEDDINGS_ORT_VERSION}" )
	endif()
	if (EMBEDDINGS_ORT_GLIBC)
		set ( ORT_GLIBC "${EMBEDDINGS_ORT_GLIBC}" )
	endif()

	set ( ${OUT_ORT_VERSION} "${ORT_VERSION}" PARENT_SCOPE )
	set ( ${OUT_ORT_GLIBC} "${ORT_GLIBC}" PARENT_SCOPE )
endfunction()

function(prepare_embeddings_ort)
	if (NOT UNIX OR APPLE)
		return()
	endif()

	read_embeddings_ort_metadata ( ORT_VERSION ORT_GLIBC )

	if (CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
		set ( ORT_ARCH "aarch64" )
	else()
		set ( ORT_ARCH "x64" )
	endif()

	set ( ORT_ASSET "onnxruntime-linux-${ORT_ARCH}-static_lib-${ORT_VERSION}-glibc${ORT_GLIBC}" )
	set ( ORT_URL "https://github.com/csukuangfj/onnxruntime-libs/releases/download/v${ORT_VERSION}/${ORT_ASSET}.zip" )
	set ( ORT_ROOT "${CMAKE_CURRENT_BINARY_DIR}/embeddings/ort/${ORT_ASSET}" )
	set ( ORT_ZIP "${CMAKE_CURRENT_BINARY_DIR}/embeddings/ort/${ORT_ASSET}.zip" )

	if (NOT EXISTS "${ORT_ROOT}/lib")
		message ( STATUS "Downloading ONNX Runtime static library: ${ORT_ASSET}" )
		file ( MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/embeddings/ort" )
		file ( DOWNLOAD "${ORT_URL}" "${ORT_ZIP}" STATUS ORT_DOWNLOAD_STATUS SHOW_PROGRESS )
		list ( GET ORT_DOWNLOAD_STATUS 0 ORT_DOWNLOAD_CODE )
		if (NOT ORT_DOWNLOAD_CODE EQUAL 0)
			list ( GET ORT_DOWNLOAD_STATUS 1 ORT_DOWNLOAD_ERROR )
			message ( FATAL_ERROR "Failed to download ${ORT_URL}: ${ORT_DOWNLOAD_ERROR}" )
		endif()
		file ( ARCHIVE_EXTRACT INPUT "${ORT_ZIP}" DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/embeddings/ort" )
	endif()

	if (NOT EXISTS "${ORT_ROOT}/lib")
		message ( FATAL_ERROR "ONNX Runtime lib directory was not found: ${ORT_ROOT}/lib" )
	endif()

	set ( ENV{ORT_LIB_PATH} "${ORT_ROOT}/lib" )
	message ( STATUS "Using ONNX Runtime from ORT_LIB_PATH=$ENV{ORT_LIB_PATH}" )
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
		elseif(UNIX)
			# MKL provides multi-threaded BLAS on Linux; skip if not available
			execute_process(COMMAND pkg-config --exists mkl-dynamic-lp64-seq RESULT_VARIABLE MKL_FOUND OUTPUT_QUIET ERROR_QUIET)
			if(MKL_FOUND EQUAL 0)
				list(APPEND EMBEDDINGS_FEATURE_LIST mkl)
			endif()
		endif()
		list(JOIN EMBEDDINGS_FEATURE_LIST "," EMBEDDINGS_FEATURES_CSV)
	endif()

	if (UNIX AND NOT APPLE AND DEFINED ENV{ORT_LIB_PATH} AND NOT "$ENV{ORT_LIB_PATH}" STREQUAL "" AND EMBEDDINGS_FEATURES_CSV)
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
