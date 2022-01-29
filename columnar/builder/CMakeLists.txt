# Copyright (c) 2021, Manticore Software LTD (https://manticoresearch.com)
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

CMAKE_MINIMUM_REQUIRED ( VERSION 3.17 )

ADD_LIBRARY ( builder OBJECT
		builderbool.cpp
		builderint.cpp
		buildermva.cpp
		builderstr.cpp
		buildertraits.cpp
		builderbool.h
		builderint.h
		builderminmax.h
		buildermva.h
		builderstr.h
		buildertraits.h
		)

target_link_libraries ( builder PRIVATE FastPFOR::FastPFOR columnar_root )

# runaround - POSITION_INDEPENDENT_CODE must be transitive from columnar_root, but it doesn't work (a bug in cmake?)
get_target_property ( pic columnar_root INTERFACE_POSITION_INDEPENDENT_CODE )
set_property ( TARGET builder PROPERTY POSITION_INDEPENDENT_CODE ${pic} )

target_include_directories ( builder INTERFACE ${CMAKE_CURRENT_SOURCE_DIR} )

