# Copyright (c) 2023-2025, Manticore Software LTD (https://manticoresearch.com)
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
# Temporary PR dependency: replace with the canonical upstream merge SHA after
# https://github.com/manticoresoftware/hnswlib/pull/4 is merged.
set ( HNSW_GITHUB "https://github.com/manticoresoftware/hnswlib/archive/a1166683c062593925ea85b7538e60d13e1ef040.zip" )
set ( HNSW_BUNDLEZIP "${LIBS_BUNDLE}/hnswlib-a116668.tar.gz" )

cmake_minimum_required ( VERSION 3.17 FATAL_ERROR )
include ( update_bundle )

# determine destination folder where we expect pre-built hnswlib
find_package ( hnswlib 1.2.0 EXACT QUIET CONFIG )
return_if_target_found ( hnswlib::hnswlib "found ready" )

# not found. Populate and prepare sources
select_nearest_url ( HNSW_PLACE hnswlib ${HNSW_BUNDLEZIP} ${HNSW_GITHUB} )
fetch_sources ( hnswlib ${HNSW_PLACE} HNSW_SRC )

get_build ( HNSW_BUILD hnswlib )
external_build ( hnswlib HNSW_SRC HNSW_BUILD )

find_package ( hnswlib 1.2.0 EXACT REQUIRED CONFIG )
