# Copyright (c) 2021-2024, Manticore Software LTD (https://manticoresearch.com)
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

add_library ( knn_lib MODULE knn.cpp knn.h quantizer.cpp quantizer.h iterator.cpp iterator.h space.cpp space.h )

string ( TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" SYSTEM_PROCESSOR_LOWER )
if (SYSTEM_PROCESSOR_LOWER STREQUAL x86_64 OR SYSTEM_PROCESSOR_LOWER STREQUAL amd64)
	message ( STATUS "Add sse 4.2 to compiler flags for ${CMAKE_SYSTEM_PROCESSOR} arch" )
	message(STATUS "Compiler ID: ${CMAKE_CXX_COMPILER_ID}")
	message(STATUS "Compiler Version: ${CMAKE_CXX_COMPILER_VERSION}")
	message(STATUS "Compiler is MSVC-like: ${MSVC}")
	message(STATUS "GNUCLANGCXX evaluates to: $<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang,GNU>")
	target_compile_options ( knn_lib PUBLIC $<${GNUCLANGCXX}:-msse4.2 -mpopcnt> )
endif ()

target_link_libraries ( knn_lib PRIVATE hnswlib::hnswlib columnar_root util common )
set_target_properties( knn_lib PROPERTIES PREFIX "" OUTPUT_NAME lib_manticore_knn )