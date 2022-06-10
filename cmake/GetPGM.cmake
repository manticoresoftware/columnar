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

set ( PGM_GITHUB "https://github.com/tomatolog/PGM-index/archive/refs/heads/master.zip" )

include ( helpers )

# set global cache path to cmake
get_cache ( CACHE_BUILDS )
set ( CMAKE_PREFIX_PATH "${CACHE_BUILDS}" )

macro ( return_if_pgm_found LEGEND )
	if (TARGET PGM)
		get_target_property ( TRG PGM LOCATION )
		diags ( "PGM library found ${LEGEND} at ${TRG}" )
		return ()
	endif ()
endmacro ()

find_package ( PGM QUIET CONFIG )
return_if_pgm_found ( "ready (no need to build)" )

# not found. Populate and prepare sources
select_nearest_url ( PGM_PLACE pgm "${LIBS_BUNDLE}/PGM-master.zip" ${PGM_GITHUB} )

# fetch sources (original tarball)
include ( FetchContent )
FetchContent_Declare ( pgm URL "${PGM_PLACE}" )
FetchContent_GetProperties ( pgm )
if (NOT pgm_POPULATED)
	message ( STATUS "Populate PGM from ${PGM_PLACE}" )
	FetchContent_Populate ( pgm )
endif ()

add_subdirectory ( ${pgm_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/pgm-build )
