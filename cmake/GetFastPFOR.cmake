# Copyright (c) 2020-2021, Manticore Software LTD (https://manticoresearch.com)
# All rights reserved
#
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

set ( FP_GITHUB "https://github.com/lemire/FastPFor/archive/refs/heads/master.zip" )

include (helpers)

# set global cache path to cmake
get_cache ( CACHE_BUILDS )
set ( CMAKE_PREFIX_PATH "${CACHE_BUILDS}" )

macro ( return_if_fastpfor_found LEGEND )
	if (TARGET FastPFOR::FastPFOR)
		get_target_property ( TRG FastPFOR::FastPFOR LOCATION )
		diags ( "FastPFOR library found ${LEGEND} at ${TRG}" )
		return ()
	endif ()
endmacro ()

find_package ( FastPFOR QUIET CONFIG)
return_if_fastpfor_found ( "ready (no need to build)" )

# not found. Populate and prepare sources
select_nearest_url ( FP_PLACE fastpfor "${LIBS_BUNDLE}/FastPFor-master.zip" ${FP_GITHUB} )

# fetch sources (original tarball)
include ( FetchContent )
FetchContent_Declare ( fastpfor URL "${FP_PLACE}" )
FetchContent_GetProperties ( fastpfor )
if (NOT fastpfor_POPULATED)
	message ( STATUS "Populate fastpfor from ${FP_PLACE}" )
	FetchContent_Populate ( fastpfor )
endif ()

# configure needs columnar_SOURCE_DIR, fastpfor_SOURCE_DIR, FP_BUILD
get_build ( FP_BUILD fastpfor )
configure_file ( ${CMAKE_MODULE_PATH}/fastpfor.cmake.in fastpfor-build/CMakeLists.txt )

# build and export FastPFOR
execute_process ( COMMAND ${CMAKE_COMMAND} -G "${CMAKE_GENERATOR}" . WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/fastpfor-build )
execute_process ( COMMAND ${CMAKE_COMMAND} --build . WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/fastpfor-build )

find_package ( FastPFOR CONFIG)
return_if_fastpfor_found ( "was built and saved" )