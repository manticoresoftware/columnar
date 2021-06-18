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

ADD_LIBRARY ( util OBJECT
		util.cpp
		version.cpp
		delta.cpp
		reader.cpp
		util.h
		interval.h
		delta.h
		reader.h
		)

include ( CheckFunctionExists )
set ( HAVE_PREAD 0 )
check_function_exists ( pread HAVE_PREAD )
set_source_files_properties ( reader.cpp PROPERTIES COMPILE_DEFINITIONS HAVE_PREAD=${HAVE_PREAD} )

target_link_libraries ( util PRIVATE FastPFOR::FastPFOR columnar_root)

# runaround - POSITION_INDEPENDENT_CODE must be transitive from columnar_root, but it doesn't work (a bug in cmake?)
get_target_property ( pic columnar_root INTERFACE_POSITION_INDEPENDENT_CODE )
set_property ( TARGET util PROPERTY POSITION_INDEPENDENT_CODE ${pic} )

set (VERSION_DIR "${columnar_BINARY_DIR}/config")
target_include_directories ( util PRIVATE ${VERSION_DIR} )

add_custom_target (
		columnarver
		COMMAND ${CMAKE_COMMAND}
		-D VERSION_SRC=${CMAKE_CURRENT_SOURCE_DIR}/version.h.in
		-D VERSION_TRG=${VERSION_DIR}/gen_version.h
		-D CMAKE_MODULE_PATH=${CMAKE_MODULE_PATH}
		-D VERSION_STR=${CMAKE_PROJECT_VERSION}
		-D SOURCE_DIR=${columnar_SOURCE_DIR}
		-P ${CMAKE_CURRENT_SOURCE_DIR}/version.cmake
		VERBATIM
		COMMENT "Assign build version data"
		BYPRODUCTS ${VERSION_DIR}/gen_version.h
		SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/version.h.in
		DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/version.h.in
)

add_dependencies ( util columnarver )

target_include_directories ( util INTERFACE ${CMAKE_CURRENT_SOURCE_DIR} )
