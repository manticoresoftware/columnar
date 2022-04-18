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

cmake_minimum_required ( VERSION 3.17 )

set(PGM_TEST 0)
include ( GetPGM )

SET ( CMAKE_CXX_STANDARD 17 )

add_library ( secondary_index MODULE
    builder.cpp iterator.cpp
    sidx.h common.h builder.h iterator.h )

target_link_libraries ( secondary_index PUBLIC secondary_root pgmindexlib FastPFOR::FastPFOR util )
set_target_properties( secondary_index PROPERTIES PREFIX "" OUTPUT_NAME lib_manticore_secondary )

# runaround - POSITION_INDEPENDENT_CODE must be transitive from secondary_root, but it doesn't work (a bug in cmake?)
get_target_property ( pic secondary_root INTERFACE_POSITION_INDEPENDENT_CODE )
set_property ( TARGET secondary_index PROPERTY POSITION_INDEPENDENT_CODE ${pic} )
