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

cmake_minimum_required ( VERSION 3.17 )

include ( GetPGM )

add_library ( secondary_index MODULE
    builder.cpp iterator.cpp blockreader.cpp secondary.cpp
    common.h builder.h iterator.h blockreader.h secondary.h )

target_link_libraries ( secondary_index PRIVATE PGM::pgmindexlib FastPFOR::FastPFOR columnar_root util common )
set_target_properties ( secondary_index PROPERTIES
		POSITION_INDEPENDENT_CODE ON
		INTERPROCEDURAL_OPTIMIZATION OFF
		PREFIX ""
		OUTPUT_NAME lib_manticore_secondary )
