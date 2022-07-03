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

set ( PGM_GITHUB "https://github.com/manticoresoftware/PGM-index/archive/refs/heads/pgm_2021.zip" )
set ( PGM_BUNDLEZIP "${LIBS_BUNDLE}/pgm_2021.zip" )

cmake_minimum_required ( VERSION 3.17 FATAL_ERROR )
include ( update_bundle )

# not mandatory, but if available, pgm will depends on it
find_package ( OpenMP )

# determine destination folder where we expect pre-built pgm
find_package ( PGM QUIET CONFIG )
return_if_target_found ( PGM::pgmindexlib "ready (no need to build)" )

# not found. Populate and prepare sources
select_nearest_url ( PGM_PLACE pgm ${PGM_BUNDLEZIP} ${PGM_GITHUB} )
fetch_sources ( pgm ${PGM_PLACE} PGM_SRC )
execute_process ( COMMAND ${CMAKE_COMMAND} -E copy_if_different "${columnar_SOURCE_DIR}/pgm/CMakeLists.txt" "${PGM_SRC}/CMakeLists.txt" )

# build external project
get_build ( PGM_BUILD pgm )
external_build ( PGM PGM_SRC PGM_BUILD )

find_package ( PGM CONFIG REQUIRED )
