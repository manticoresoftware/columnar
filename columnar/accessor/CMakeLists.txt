# Copyright (c) 2021-2025, Manticore Software LTD (https://manticoresearch.com)
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

add_library ( accessor OBJECT
		attributeheader.cpp
		accessor.cpp
		accessorbool.cpp
		accessorint.cpp
		accessormva.cpp
		accessorstr.cpp
		accessortraits.cpp
		check.cpp
		attributeheader.h
		accessor.h
		accessorbool.h
		accessorint.h
		accessormva.h
		accessorstr.h
		accessortraits.h
		check.h
		)

target_link_libraries ( accessor PRIVATE columnar_root )
set_property ( TARGET accessor PROPERTY POSITION_INDEPENDENT_CODE TRUE )
target_include_directories ( accessor INTERFACE ${CMAKE_CURRENT_SOURCE_DIR} )

