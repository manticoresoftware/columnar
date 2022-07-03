# Copyright (c) 2020-2022, Manticore Software LTD (https://manticoresearch.com)
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

set ( FP_GITHUB "https://github.com/manticoresoftware/FastPFor/archive/refs/heads/simde.zip" )
set ( FP_BUNDLEZIP "${LIBS_BUNDLE}/FastPFor-simde.zip" )

cmake_minimum_required ( VERSION 3.17 FATAL_ERROR )
include ( update_bundle )

# determine destination folder where we expect pre-built fastpfor
find_package ( FastPFOR QUIET CONFIG )
return_if_target_found ( FastPFOR::FastPFOR "ready (no need to build)" )

# not found. Populate and prepare sources
select_nearest_url ( FP_PLACE fastpfor ${FP_BUNDLEZIP} ${FP_GITHUB} )
fetch_sources ( fastpfor ${FP_PLACE} FASTPFOR_SRC )
execute_process ( COMMAND ${CMAKE_COMMAND} -E copy_if_different "${columnar_SOURCE_DIR}/libfastpfor/CMakeLists.txt" "${FASTPFOR_SRC}/CMakeLists.txt" )

# build external project
get_build ( FASTPFOR_BUILD fastpfor )
external_build ( FastPFOR FASTPFOR_SRC FASTPFOR_BUILD )

find_package ( FastPFOR CONFIG REQUIRED )