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

	embeddings_ort_download ( _lib_dir "${_platform}" "${_arch}" "${MANTICORE_EMBEDDINGS_DEPS_DIR}/ort" )

	set ( ENV{ORT_LIB_LOCATION} "${_lib_dir}" )
	message ( STATUS "Using ORT_LIB_LOCATION=${_lib_dir}" )
endfunction()

function(build_embeddings_lib)
	message ( STATUS "Configuring local embeddings build target..." )

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

	if (NOT DEFINED MANTICORE_EMBEDDINGS_TARGET_DIR)
		if (DEFINED ENV{CARGO_TARGET_DIR} AND NOT "$ENV{CARGO_TARGET_DIR}" STREQUAL "")
			set(_embeddings_target_dir_default "$ENV{CARGO_TARGET_DIR}")
		else()
			set(_embeddings_target_dir_default "${columnar_SOURCE_DIR}/embeddings/target")
		endif()
		set(MANTICORE_EMBEDDINGS_TARGET_DIR "${_embeddings_target_dir_default}" CACHE PATH "Shared Cargo target directory for MCL embeddings")
	endif()
	if (NOT DEFINED MANTICORE_EMBEDDINGS_DEPS_DIR)
		set(MANTICORE_EMBEDDINGS_DEPS_DIR "${MANTICORE_EMBEDDINGS_TARGET_DIR}/manticore-deps" CACHE PATH "Shared dependency cache directory for MCL embeddings")
	endif()
	file(MAKE_DIRECTORY "${MANTICORE_EMBEDDINGS_TARGET_DIR}" "${MANTICORE_EMBEDDINGS_DEPS_DIR}")
	message ( STATUS "Using MANTICORE_EMBEDDINGS_TARGET_DIR=${MANTICORE_EMBEDDINGS_TARGET_DIR}" )
	message ( STATUS "Using MANTICORE_EMBEDDINGS_DEPS_DIR=${MANTICORE_EMBEDDINGS_DEPS_DIR}" )

	prepare_embeddings_ort()

	# Enable platform-specific BLAS acceleration for candle when available.
	if (DEFINED EMBEDDINGS_CARGO_FEATURES)
		set(EMBEDDINGS_FEATURES_CSV "${EMBEDDINGS_CARGO_FEATURES}")
	else()
		set(EMBEDDINGS_FEATURE_LIST)
		if(APPLE)
			list(APPEND EMBEDDINGS_FEATURE_LIST accelerate)
		elseif(UNIX AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64)$")
			embeddings_mkl_detect_linux ( _mklroot )
			if ( _mklroot )
				set ( ENV{MKLROOT} "${_mklroot}" )
				list(APPEND EMBEDDINGS_FEATURE_LIST mkl)
			endif()
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

	set(EMBEDDINGS_LIB_SRC_PATH "${MANTICORE_EMBEDDINGS_TARGET_DIR}/release/${EMBEDDINGS_LIB_FILE_SRC}")
	set(EMBEDDINGS_LIB_DST_PATH "${CMAKE_CURRENT_BINARY_DIR}/embeddings/release/${EMBEDDINGS_LIB_FILE_DST}")

	file(GLOB_RECURSE EMBEDDINGS_RUST_SOURCES CONFIGURE_DEPENDS
			"${columnar_SOURCE_DIR}/embeddings/*.toml"
			"${columnar_SOURCE_DIR}/embeddings/Cargo.lock"
			"${columnar_SOURCE_DIR}/embeddings/build.rs"
			"${columnar_SOURCE_DIR}/embeddings/src/*.rs"
	)

	set(EMBEDDINGS_VERSION_STAMP "${CMAKE_CURRENT_BINARY_DIR}/embeddings/version.stamp")
	set(EMBEDDINGS_VERSION_STAMP_CONTENT "${GIT_COMMIT_ID}\n${GIT_TIMESTAMP_ID}\n$ENV{ORT_LIB_LOCATION}\n$ENV{MKLROOT}\n${EMBEDDINGS_FEATURES_CSV}\n${MANTICORE_EMBEDDINGS_TARGET_DIR}\n${MANTICORE_EMBEDDINGS_DEPS_DIR}\n")
	if (EXISTS "${EMBEDDINGS_VERSION_STAMP}")
		file(READ "${EMBEDDINGS_VERSION_STAMP}" EMBEDDINGS_CURRENT_VERSION_STAMP)
	endif()
	if (NOT EMBEDDINGS_CURRENT_VERSION_STAMP STREQUAL EMBEDDINGS_VERSION_STAMP_CONTENT)
		file(WRITE "${EMBEDDINGS_VERSION_STAMP}" "${EMBEDDINGS_VERSION_STAMP_CONTENT}")
	endif()

	set(EMBEDDINGS_CARGO_ENV
			"GIT_COMMIT_ID=${GIT_COMMIT_ID}"
			"GIT_TIMESTAMP_ID=${GIT_TIMESTAMP_ID}"
	)
	set(_embeddings_cc "")
	if (DEFINED ENV{CC})
		set(_embeddings_cc "$ENV{CC}")
	endif()
	if (NOT _embeddings_cc)
		set(_embeddings_cc "${CMAKE_C_COMPILER}")
	endif()
	if (NOT _embeddings_cc AND UNIX)
		find_program(_embeddings_cc NAMES clang cc gcc)
	endif()
	if (_embeddings_cc)
		list(APPEND EMBEDDINGS_CARGO_ENV "CC=${_embeddings_cc}")
	endif()
	set(_embeddings_cxx "")
	if (DEFINED ENV{CXX})
		set(_embeddings_cxx "$ENV{CXX}")
	endif()
	if (NOT _embeddings_cxx)
		set(_embeddings_cxx "${CMAKE_CXX_COMPILER}")
	endif()
	if (NOT _embeddings_cxx AND UNIX)
		find_program(_embeddings_cxx NAMES clang++ c++ g++)
	endif()
	if (_embeddings_cxx)
		list(APPEND EMBEDDINGS_CARGO_ENV "CXX=${_embeddings_cxx}")
	endif()
	if (DEFINED ENV{ORT_LIB_LOCATION} AND NOT "$ENV{ORT_LIB_LOCATION}" STREQUAL "")
		list(APPEND EMBEDDINGS_CARGO_ENV "ORT_LIB_LOCATION=$ENV{ORT_LIB_LOCATION}")
	endif()
	if (DEFINED ENV{MKLROOT} AND NOT "$ENV{MKLROOT}" STREQUAL "")
		list(APPEND EMBEDDINGS_CARGO_ENV "MKLROOT=$ENV{MKLROOT}")
	endif()

	add_custom_command(
			OUTPUT "${EMBEDDINGS_LIB_DST_PATH}"
			COMMAND ${CMAKE_COMMAND} -E env ${EMBEDDINGS_CARGO_ENV}
				"${CARGO_COMMAND}" build --manifest-path "${columnar_SOURCE_DIR}/embeddings/Cargo.toml" --lib --release ${EMBEDDINGS_CARGO_FEATURE_ARGS} --target-dir "${MANTICORE_EMBEDDINGS_TARGET_DIR}"
			COMMAND ${CMAKE_COMMAND}
				-DEMBEDDINGS_LIB_SRC_PATH=${EMBEDDINGS_LIB_SRC_PATH}
				-DEMBEDDINGS_LIB_DST_PATH=${EMBEDDINGS_LIB_DST_PATH}
				-DEMBEDDINGS_PDB_SRC_PATH=${MANTICORE_EMBEDDINGS_TARGET_DIR}/release/${EMBEDDINGS_LIB_NAME}.pdb
				-DEMBEDDINGS_PDB_DST_PATH=${CMAKE_CURRENT_BINARY_DIR}/embeddings/release/lib_${EMBEDDINGS_LIB_NAME}.pdb
				-P "${columnar_SOURCE_DIR}/cmake/copy_embeddings_artifacts.cmake"
			DEPENDS ${EMBEDDINGS_RUST_SOURCES} "${EMBEDDINGS_VERSION_STAMP}"
			WORKING_DIRECTORY "${columnar_SOURCE_DIR}/embeddings"
			COMMENT "Building manticoresearch text embeddings library"
			VERBATIM
	)

	if (NOT TARGET manticore_knn_embeddings)
		add_custom_target(manticore_knn_embeddings ALL DEPENDS "${EMBEDDINGS_LIB_DST_PATH}")
	endif()

	set(EMBEDDINGS_LIB "${EMBEDDINGS_LIB_DST_PATH}" PARENT_SCOPE)
	set(MANTICORE_KNN_EMBEDDINGS_LIB "${EMBEDDINGS_LIB_DST_PATH}" CACHE INTERNAL "Path to manticoresearch text embeddings library" FORCE)
endfunction ()
