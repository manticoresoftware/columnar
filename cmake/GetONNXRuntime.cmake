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

# Single source of truth for the ONNX Runtime static library used by embeddings
# builds. Consumed in two ways:
#   - included from cmake/build_embeddings.cmake for local host builds
#   - run as a script (cmake -DORT_PLATFORM=... -DORT_ARCH=... -DORT_OUT_DIR=... -P cmake/GetONNXRuntime.cmake)
#     from CI workflows, so neither place hardcodes the version, URL, or asset name.
# Bump versions here; both code paths pick up the new values.

cmake_minimum_required ( VERSION 3.17 )

set ( EMBEDDINGS_ORT_VERSION "1.24.2" CACHE STRING "ONNX Runtime version for embeddings builds" )
set ( EMBEDDINGS_ORT_GLIBC "2_17" CACHE STRING "ONNX Runtime glibc baseline (Linux only)" )

# Map (platform, arch) -> (asset stem, archive extension) per csukuangfj/onnxruntime-libs releases.
function ( _embeddings_ort_asset OUT_ASSET OUT_EXT IN_PLATFORM IN_ARCH )
	set ( _ver "${EMBEDDINGS_ORT_VERSION}" )
	set ( _glibc "${EMBEDDINGS_ORT_GLIBC}" )

	if ( IN_PLATFORM STREQUAL "linux" )
		if ( IN_ARCH MATCHES "^(aarch64|arm64)$" )
			set ( _arch "aarch64" )
		else()
			set ( _arch "x64" )
		endif()
		set ( _asset "onnxruntime-linux-${_arch}-static_lib-${_ver}-glibc${_glibc}" )
		set ( _ext "zip" )
	elseif ( IN_PLATFORM STREQUAL "macos" )
		if ( IN_ARCH MATCHES "^(aarch64|arm64)$" )
			set ( _arch "arm64" )
		else()
			set ( _arch "x86_64" )
		endif()
		set ( _asset "onnxruntime-osx-${_arch}-static_lib-${_ver}" )
		set ( _ext "zip" )
	elseif ( IN_PLATFORM STREQUAL "windows" )
		if ( IN_ARCH MATCHES "^(aarch64|arm64)$" )
			set ( _arch "arm64" )
		else()
			set ( _arch "x64" )
		endif()
		set ( _asset "onnxruntime-win-${_arch}-static_lib-MD-Release-${_ver}" )
		set ( _ext "tar.bz2" )
	else()
		message ( FATAL_ERROR "embeddings_ort: unsupported platform '${IN_PLATFORM}' (expected linux|macos|windows)" )
	endif()

	set ( ${OUT_ASSET} "${_asset}" PARENT_SCOPE )
	set ( ${OUT_EXT} "${_ext}" PARENT_SCOPE )
endfunction()

# Download + extract the ORT static lib for the given platform/arch into IN_OUT_DIR.
# Idempotent: skips network when the lib dir already exists. On success, sets
# OUT_LIB_DIR_VAR (in caller scope) to the absolute lib/ path.
function ( embeddings_ort_download OUT_LIB_DIR_VAR IN_PLATFORM IN_ARCH IN_OUT_DIR )
	_embeddings_ort_asset ( _asset _ext "${IN_PLATFORM}" "${IN_ARCH}" )

	set ( _url "https://github.com/csukuangfj/onnxruntime-libs/releases/download/v${EMBEDDINGS_ORT_VERSION}/${_asset}.${_ext}" )
	set ( _archive "${IN_OUT_DIR}/${_asset}.${_ext}" )
	set ( _root "${IN_OUT_DIR}/${_asset}" )

	if ( NOT EXISTS "${_root}/lib" )
		message ( STATUS "Downloading ${_asset}.${_ext}" )
		file ( MAKE_DIRECTORY "${IN_OUT_DIR}" )
		file ( DOWNLOAD "${_url}" "${_archive}" STATUS _status SHOW_PROGRESS )
		list ( GET _status 0 _code )
		if ( NOT _code EQUAL 0 )
			list ( GET _status 1 _err )
			message ( FATAL_ERROR "Failed to download ${_url}: ${_err}" )
		endif()
		file ( ARCHIVE_EXTRACT INPUT "${_archive}" DESTINATION "${IN_OUT_DIR}" )
	endif()

	if ( NOT EXISTS "${_root}/lib" )
		message ( FATAL_ERROR "ORT lib dir not found after extract: ${_root}/lib" )
	endif()

	set ( ${OUT_LIB_DIR_VAR} "${_root}/lib" PARENT_SCOPE )
endfunction()

# Script-mode entry: cmake -DORT_PLATFORM=... -DORT_ARCH=... -DORT_OUT_DIR=... -P cmake/GetONNXRuntime.cmake
# Writes the resolved lib dir to ${ORT_OUT_DIR}/lib_path.txt so shell callers
# don't have to recompute the asset name to find it.
if ( CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE )
	if ( NOT DEFINED ORT_PLATFORM OR NOT DEFINED ORT_ARCH OR NOT DEFINED ORT_OUT_DIR )
		message ( FATAL_ERROR "usage: cmake -DORT_PLATFORM=<linux|macos|windows> -DORT_ARCH=<x64|aarch64> -DORT_OUT_DIR=<dir> -P GetONNXRuntime.cmake" )
	endif()

	embeddings_ort_download ( _lib_dir "${ORT_PLATFORM}" "${ORT_ARCH}" "${ORT_OUT_DIR}" )

	file ( WRITE "${ORT_OUT_DIR}/lib_path.txt" "${_lib_dir}" )
	message ( STATUS "ORT lib dir: ${_lib_dir}" )
endif()
