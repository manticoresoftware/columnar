# Copyright (c) 2023-2024, Manticore Software LTD (https://manticoresearch.com)
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

set ( ANNOY_GITHUB "https://github.com/manticoresoftware/annoy/archive/refs/heads/main.zip" )
set ( ANNOY_BUNDLEZIP "${LIBS_BUNDLE}/annoy.zip" )

cmake_minimum_required ( VERSION 3.17 FATAL_ERROR )
include ( update_bundle )

# determine destination folder where we expect pre-built annoy
find_package ( Annoy QUIET CONFIG )
return_if_target_found ( Annoy::Annoy "found ready" )

# not found. Populate and prepare sources
select_nearest_url ( ANNOY_PLACE annoy ${ANNOY_BUNDLEZIP} ${ANNOY_GITHUB} )
fetch_sources ( annoy ${ANNOY_PLACE} ANNOY_SRC )

get_build ( ANNOY_BUILD annoy )
external_build ( Annoy ANNOY_SRC ANNOY_BUILD )

find_package ( Annoy REQUIRED CONFIG )
