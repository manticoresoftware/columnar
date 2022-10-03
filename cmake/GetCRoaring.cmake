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

set ( RB_GITHUB "https://github.com/RoaringBitmap/CRoaring/archive/refs/heads/master.zip" )
set ( RB_BUNDLEZIP "${LIBS_BUNDLE}/roaring.zip" )

cmake_minimum_required ( VERSION 3.17 FATAL_ERROR )
include ( update_bundle )

# determine destination folder where we expect pre-built lib
find_package ( roaring QUIET CONFIG )
return_if_target_found ( roaring::roaring "ready (no need to build)" )

# not found. Populate and prepare sources
select_nearest_url ( RB_PLACE roaring ${RB_BUNDLEZIP} ${RB_GITHUB} )
fetch_sources ( roaring ${RB_PLACE} ROARING_SRC )

# build external project
get_build ( ROARING_BUILD roaring )
external_build ( roaring ROARING_SRC ROARING_BUILD )

find_package ( roaring REQUIRED CONFIG )