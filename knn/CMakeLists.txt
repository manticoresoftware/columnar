# Copyright (c) 2021-2023, Manticore Software LTD (https://manticoresearch.com)
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

include ( GetHNSW )

add_library ( knn_lib MODULE knn.cpp knn.h iterator.cpp iterator.h )

if (CMAKE_SYSTEM_PROCESSOR STREQUAL x86_64 OR CMAKE_SYSTEM_PROCESSOR STREQUAL amd64)
	message ( STATUS "Add sse 4.1 to compiler flags for ${CMAKE_SYSTEM_PROCESSOR} arch" )
	target_compile_options ( knn_lib PUBLIC $<${GNUC_CXX}:-msse4.1> )
endif ()

target_link_libraries ( knn_lib PRIVATE hnswlib::hnswlib columnar_root util common )
set_target_properties( knn_lib PROPERTIES PREFIX "" OUTPUT_NAME lib_manticore_knn )