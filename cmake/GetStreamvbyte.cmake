# Copyright (c) 2020-2025, Manticore Software LTD (https://manticoresearch.com)
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

set ( SVB_GITHUB "https://github.com/manticoresoftware/streamvbyte/archive/refs/heads/master.zip" )
set ( SVB_BUNDLEZIP "${LIBS_BUNDLE}/streamvbyte.zip" )

cmake_minimum_required ( VERSION 3.17 FATAL_ERROR )
include ( update_bundle )

# determine destination folder where we expect pre-built lib
find_package ( streamvbyte QUIET CONFIG )
return_if_target_found ( streamvbyte::streamvbyte "ready (no need to build)" )

# not found. Populate and prepare sources
select_nearest_url ( SVB_PLACE streamvbyte ${SVB_BUNDLEZIP} ${SVB_GITHUB} )
fetch_sources ( streamvbyte ${SVB_PLACE} STREAMVBYTE_SRC )
execute_process ( COMMAND ${CMAKE_COMMAND} -E copy_if_different "${columnar_SOURCE_DIR}/streamvbyte/CMakeLists.txt" "${STREAMVBYTE_SRC}/CMakeLists.txt" )

# build external project
get_build ( STREAMVBYTE_BUILD streamvbyte )
external_build ( streamvbyte STREAMVBYTE_SRC STREAMVBYTE_BUILD )

find_package ( streamvbyte REQUIRED CONFIG )