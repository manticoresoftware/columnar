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

# Intel MKL / OpenMP integration for embeddings builds with the `mkl` Cargo
# feature. Linux flow is detection-only: if MKL is on the host (either via
# the rust-min-libc Docker image which apt-installs it, or via a local
# install matching ../rust-min-libc/Dockerfile lines 87-105), we use it.
# Otherwise the caller silently skips the `mkl` feature — the build still
# produces a working baseline .so. Windows flow downloads only the runtime
# DLL (libiomp5md.dll), same nuget source the CI workflow used inline.

cmake_minimum_required ( VERSION 3.17 )

# Intel OpenMP runtime DLL (windows-x64 only). Same nuget package the CI
# workflow used to download inline before this logic moved into CMake.
set ( EMBEDDINGS_OPENMP_WIN_VERSION "2025.3.3.31" CACHE STRING "intelopenmp.redist.win nuget version" )
set ( EMBEDDINGS_OPENMP_WIN_URL
	"https://api.nuget.org/v3-flatcontainer/intelopenmp.redist.win/${EMBEDDINGS_OPENMP_WIN_VERSION}/intelopenmp.redist.win.${EMBEDDINGS_OPENMP_WIN_VERSION}.nupkg"
	CACHE STRING "Override URL for the intelopenmp.redist.win nuget package" )

# Linux x86_64: detect an existing MKL install. Sets OUT_VAR to the resolved
# MKLROOT, or empty string when MKL isn't present. No download, no install.
function ( embeddings_mkl_detect_linux OUT_VAR )
	if ( DEFINED ENV{MKLROOT} AND EXISTS "$ENV{MKLROOT}/lib/intel64/libmkl_intel_lp64.a" )
		set ( ${OUT_VAR} "$ENV{MKLROOT}" PARENT_SCOPE )
		return()
	endif()
	if ( EXISTS "/opt/intel/oneapi/mkl/latest/lib/intel64/libmkl_intel_lp64.a" )
		set ( ${OUT_VAR} "/opt/intel/oneapi/mkl/latest" PARENT_SCOPE )
		return()
	endif()
	if ( EXISTS "/opt/intel/oneapi/mkl/latest/lib/libmkl_intel_lp64.a" )
		# Modern oneMKL layout (no intel64/ subdir).
		set ( ${OUT_VAR} "/opt/intel/oneapi/mkl/latest" PARENT_SCOPE )
		return()
	endif()
	set ( ${OUT_VAR} "" PARENT_SCOPE )
endfunction()

# Windows x86_64: download intelopenmp.redist nuget for libiomp5md.dll. Returns
# the absolute .dll path in OUT_DLL_VAR. Same package + URL the CI workflow
# used to fetch inline.
function ( embeddings_mkl_prepare_windows OUT_DLL_VAR IN_OUT_DIR )
	set ( _root "${IN_OUT_DIR}/intelopenmp-${EMBEDDINGS_OPENMP_WIN_VERSION}" )
	set ( _dll "${_root}/runtimes/win-x64/native/libiomp5md.dll" )

	if ( NOT EXISTS "${_dll}" )
		file ( MAKE_DIRECTORY "${_root}" )
		get_filename_component ( _name "${EMBEDDINGS_OPENMP_WIN_URL}" NAME )
		set ( _archive "${IN_OUT_DIR}/${_name}" )
		if ( NOT EXISTS "${_archive}" )
			message ( STATUS "Downloading Intel OpenMP redist: ${_name}" )
			file ( DOWNLOAD "${EMBEDDINGS_OPENMP_WIN_URL}" "${_archive}" STATUS _status SHOW_PROGRESS )
			list ( GET _status 0 _code )
			if ( NOT _code EQUAL 0 )
				list ( GET _status 1 _err )
				file ( REMOVE "${_archive}" )
				message ( FATAL_ERROR "Failed to download ${EMBEDDINGS_OPENMP_WIN_URL}: ${_err}" )
			endif()
		endif()
		file ( ARCHIVE_EXTRACT INPUT "${_archive}" DESTINATION "${_root}" )
	endif()

	if ( NOT EXISTS "${_dll}" )
		message ( FATAL_ERROR "intelopenmp redist did not contain libiomp5md.dll at expected path: ${_dll}" )
	endif()

	set ( ${OUT_DLL_VAR} "${_dll}" PARENT_SCOPE )
endfunction()

# Script-mode entry: cmake -DMKL_PLATFORM=... -DMKL_ARCH=... -DMKL_OUT_DIR=... -P cmake/GetMKL.cmake
# Used by CI workflows that need the Windows OpenMP runtime DLL. Writes the
# resolved path to ${MKL_OUT_DIR}/libiomp5md_dll_path.txt.
if ( CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE )
	if ( NOT DEFINED MKL_PLATFORM OR NOT DEFINED MKL_ARCH OR NOT DEFINED MKL_OUT_DIR )
		message ( FATAL_ERROR "usage: cmake -DMKL_PLATFORM=<windows> -DMKL_ARCH=<x64> -DMKL_OUT_DIR=<dir> -P GetMKL.cmake" )
	endif()

	if ( NOT MKL_ARCH STREQUAL "x64" AND NOT MKL_ARCH STREQUAL "x86_64" )
		message ( STATUS "GetMKL: arch '${MKL_ARCH}' has no MKL build; nothing to do" )
		return()
	endif()

	file ( MAKE_DIRECTORY "${MKL_OUT_DIR}" )

	if ( MKL_PLATFORM STREQUAL "windows" )
		embeddings_mkl_prepare_windows ( _dll "${MKL_OUT_DIR}" )
		file ( WRITE "${MKL_OUT_DIR}/libiomp5md_dll_path.txt" "${_dll}" )
		message ( STATUS "libiomp5md.dll: ${_dll}" )
	else()
		message ( STATUS "GetMKL script-mode: platform '${MKL_PLATFORM}' has nothing to fetch (Linux MKL is detect-only at configure time)" )
	endif()
endif()
