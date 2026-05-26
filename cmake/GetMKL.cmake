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

# Single source of truth for the Intel MKL / OpenMP dependency used by
# embeddings builds with the `mkl` Cargo feature. Mirrors the install
# performed inside ../rust-min-libc Dockerfile so local x86_64 hosts can
# build the MKL flavor without root + apt. Consumed in two ways:
#   - included from cmake/build_embeddings.cmake for local host builds
#   - run as a script (cmake -DMKL_PLATFORM=... -DMKL_ARCH=... -DMKL_OUT_DIR=... -P cmake/GetMKL.cmake)
#     from CI workflows that need just the OpenMP runtime DLL for packaging.

cmake_minimum_required ( VERSION 3.17 )

# MKL static archive (linux only). Conda-forge mirror — pins the exact same
# files Intel ships via apt as intel-oneapi-mkl-devel. Override either var
# to bump or substitute a private mirror.
set ( EMBEDDINGS_MKL_VERSION "2024.0.0" CACHE STRING "Intel oneMKL version for embeddings builds" )
set ( EMBEDDINGS_MKL_LINUX_URL
	"https://conda.anaconda.org/conda-forge/linux-64/mkl-static-${EMBEDDINGS_MKL_VERSION}-ha770c72_49657.tar.bz2"
	CACHE STRING "Override URL for the MKL static archive (linux-x64)" )
set ( EMBEDDINGS_OPENMP_LINUX_URL
	"https://conda.anaconda.org/conda-forge/linux-64/llvm-openmp-17.0.6-h4dfa4b3_0.tar.bz2"
	CACHE STRING "Override URL for the llvm-openmp static archive (linux-x64)" )

# Intel OpenMP runtime DLL (windows-x64 only). Used to package libiomp5md.dll
# alongside the MKL .dll artifact. Same source the CI workflow used before
# this logic moved into CMake.
set ( EMBEDDINGS_OPENMP_WIN_VERSION "2025.3.3.31" CACHE STRING "intelopenmp.redist.win nuget version" )
set ( EMBEDDINGS_OPENMP_WIN_URL
	"https://api.nuget.org/v3-flatcontainer/intelopenmp.redist.win/${EMBEDDINGS_OPENMP_WIN_VERSION}/intelopenmp.redist.win.${EMBEDDINGS_OPENMP_WIN_VERSION}.nupkg"
	CACHE STRING "Override URL for the intelopenmp.redist.win nuget package" )

# Common helper: download IN_URL to IN_OUT_DIR/<basename>. Idempotent: re-uses
# an existing archive on disk. Returns the archive path in OUT_ARCHIVE_VAR.
function ( _embeddings_mkl_fetch OUT_ARCHIVE_VAR IN_URL IN_OUT_DIR IN_LABEL )
	get_filename_component ( _name "${IN_URL}" NAME )
	set ( _archive "${IN_OUT_DIR}/${_name}" )

	if ( NOT EXISTS "${_archive}" )
		message ( STATUS "Downloading ${IN_LABEL}: ${_name}" )
		file ( MAKE_DIRECTORY "${IN_OUT_DIR}" )
		file ( DOWNLOAD "${IN_URL}" "${_archive}" STATUS _status SHOW_PROGRESS )
		list ( GET _status 0 _code )
		if ( NOT _code EQUAL 0 )
			list ( GET _status 1 _err )
			file ( REMOVE "${_archive}" )
			message ( FATAL_ERROR "Failed to download ${IN_URL}: ${_err}" )
		endif()
	endif()

	set ( ${OUT_ARCHIVE_VAR} "${_archive}" PARENT_SCOPE )
endfunction()

# Detect an existing MKL installation. On hit, sets OUT_VAR to MKLROOT.
function ( _embeddings_mkl_detect_linux OUT_VAR )
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

# Linux x86_64: ensure MKL static archives + libiomp5.a are in scope. Returns
# the directory to use as MKLROOT (its lib/intel64/ has the archives).
function ( embeddings_mkl_prepare_linux OUT_MKLROOT_VAR IN_OUT_DIR )
	_embeddings_mkl_detect_linux ( _detected )
	if ( _detected )
		message ( STATUS "Detected MKL at ${_detected}" )
		set ( ${OUT_MKLROOT_VAR} "${_detected}" PARENT_SCOPE )
		return()
	endif()

	# Conda-forge fallback. Same files Intel ships via oneAPI apt; we just
	# replay the layout dance the rust-min-libc Dockerfile does on top.
	set ( _root "${IN_OUT_DIR}/mkl-${EMBEDDINGS_MKL_VERSION}" )

	if ( NOT EXISTS "${_root}/lib/libmkl_intel_lp64.a" )
		file ( MAKE_DIRECTORY "${_root}" )
		_embeddings_mkl_fetch ( _mkl_archive "${EMBEDDINGS_MKL_LINUX_URL}" "${IN_OUT_DIR}" "MKL static" )
		_embeddings_mkl_fetch ( _omp_archive "${EMBEDDINGS_OPENMP_LINUX_URL}" "${IN_OUT_DIR}" "llvm-openmp" )

		file ( ARCHIVE_EXTRACT INPUT "${_mkl_archive}" DESTINATION "${_root}" )
		file ( ARCHIVE_EXTRACT INPUT "${_omp_archive}" DESTINATION "${_root}" )
	endif()

	# Mirror the rust-min-libc symlink dance: intel-mkl-src 0.8.1 expects
	# lib/intel64/, while modern oneMKL ships flat in lib/. Symlink the old
	# path so the build script finds archives there. libiomp5.a lives under
	# the openmp package's lib/ — symlink into MKL's lib too so `-liomp5`
	# resolves during static link.
	if ( NOT EXISTS "${_root}/lib/intel64" )
		file ( CREATE_LINK "." "${_root}/lib/intel64" SYMBOLIC )
	endif()

	if ( NOT EXISTS "${_root}/lib/libiomp5.a" AND EXISTS "${_root}/lib/libomp.a" )
		# llvm-openmp ships libomp.a; intel-mkl-src expects libiomp5.a.
		file ( CREATE_LINK "${_root}/lib/libomp.a" "${_root}/lib/libiomp5.a" SYMBOLIC )
	endif()

	foreach ( _required libmkl_intel_lp64.a libmkl_intel_thread.a libmkl_core.a libiomp5.a )
		if ( NOT EXISTS "${_root}/lib/${_required}" )
			message ( FATAL_ERROR "MKL prepare: missing ${_required} under ${_root}/lib — conda-forge archive layout may have changed; override -DEMBEDDINGS_MKL_LINUX_URL / -DEMBEDDINGS_OPENMP_LINUX_URL" )
		endif()
	endforeach()

	set ( ${OUT_MKLROOT_VAR} "${_root}" PARENT_SCOPE )
endfunction()

# Windows x86_64: download intelopenmp.redist nuget for libiomp5md.dll.
# Returns absolute path to the .dll in OUT_DLL_VAR. Mirrors the block the
# CI workflow used to do inline.
function ( embeddings_mkl_prepare_windows OUT_DLL_VAR IN_OUT_DIR )
	set ( _root "${IN_OUT_DIR}/intelopenmp-${EMBEDDINGS_OPENMP_WIN_VERSION}" )
	set ( _dll "${_root}/runtimes/win-x64/native/libiomp5md.dll" )

	if ( NOT EXISTS "${_dll}" )
		file ( MAKE_DIRECTORY "${_root}" )
		_embeddings_mkl_fetch ( _archive "${EMBEDDINGS_OPENMP_WIN_URL}" "${IN_OUT_DIR}" "Intel OpenMP redist" )
		# .nupkg is a zip; ARCHIVE_EXTRACT handles it.
		file ( ARCHIVE_EXTRACT INPUT "${_archive}" DESTINATION "${_root}" )
	endif()

	if ( NOT EXISTS "${_dll}" )
		message ( FATAL_ERROR "intelopenmp redist did not contain libiomp5md.dll at expected path: ${_dll}" )
	endif()

	set ( ${OUT_DLL_VAR} "${_dll}" PARENT_SCOPE )
endfunction()

# Script-mode entry: cmake -DMKL_PLATFORM=... -DMKL_ARCH=... -DMKL_OUT_DIR=... -P cmake/GetMKL.cmake
# Writes the resolved path to ${MKL_OUT_DIR}/<key>_path.txt so shell callers
# can read it back without recomputing layout. Keys: mklroot, libiomp5md_dll.
if ( CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE )
	if ( NOT DEFINED MKL_PLATFORM OR NOT DEFINED MKL_ARCH OR NOT DEFINED MKL_OUT_DIR )
		message ( FATAL_ERROR "usage: cmake -DMKL_PLATFORM=<linux|windows> -DMKL_ARCH=<x64> -DMKL_OUT_DIR=<dir> -P GetMKL.cmake" )
	endif()

	if ( NOT MKL_ARCH STREQUAL "x64" AND NOT MKL_ARCH STREQUAL "x86_64" )
		message ( STATUS "GetMKL: arch '${MKL_ARCH}' has no MKL build; nothing to do" )
		return()
	endif()

	if ( MKL_PLATFORM STREQUAL "linux" )
		embeddings_mkl_prepare_linux ( _mklroot "${MKL_OUT_DIR}" )
		file ( WRITE "${MKL_OUT_DIR}/mklroot_path.txt" "${_mklroot}" )
		message ( STATUS "MKLROOT: ${_mklroot}" )
	elseif ( MKL_PLATFORM STREQUAL "windows" )
		embeddings_mkl_prepare_windows ( _dll "${MKL_OUT_DIR}" )
		file ( WRITE "${MKL_OUT_DIR}/libiomp5md_dll_path.txt" "${_dll}" )
		message ( STATUS "libiomp5md.dll: ${_dll}" )
	else()
		message ( STATUS "GetMKL: platform '${MKL_PLATFORM}' has no MKL build; nothing to do" )
	endif()
endif()
