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

add_library ( util OBJECT
		util_private.cpp
		version.cpp
		reader.cpp
		codec.cpp
		util.h
		util_private.h
		delta.h
		delta_impl.h
		reader.h
		codec.h
		bitvec.h
		)

include ( CheckFunctionExists )
check_function_exists ( pread HAVE_PREAD )
set_source_files_properties ( reader.cpp PROPERTIES COMPILE_DEFINITIONS HAVE_PREAD=${HAVE_PREAD} )

target_link_libraries ( util PRIVATE FastPFOR::FastPFOR streamvbyte::streamvbyte columnar_root )
set_property ( TARGET util PROPERTY POSITION_INDEPENDENT_CODE ON )

message ( STATUS "Version ${CMAKE_PROJECT_VERSION} ${GIT_COMMIT_ID}@${GIT_TIMESTAMP_ID}, ${GIT_BRANCH_ID}" )
configure_file ( ${CMAKE_CURRENT_SOURCE_DIR}/version.h.in ${columnar_BINARY_DIR}/config/gen_version.h )

target_include_directories ( util PUBLIC ${columnar_BINARY_DIR}/config )
target_include_directories ( util INTERFACE ${CMAKE_CURRENT_SOURCE_DIR} )
