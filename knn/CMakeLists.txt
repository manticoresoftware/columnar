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

cmake_minimum_required ( VERSION 3.21 ) # because of IMPORTED_RUNTIME_ARTIFACTS

include ( GetHNSW )

add_library ( knn_lib MODULE knn.cpp knn.h iterator.cpp iterator.h embeddings.cpp embeddings.h quantizer.cpp quantizer.h space.cpp space.h ${CMAKE_SOURCE_DIR}/embeddings/manticoresearch_text_embeddings.h )
target_include_directories(knn_lib PRIVATE ${CMAKE_SOURCE_DIR}/embeddings)

target_link_libraries ( knn_lib PRIVATE hnswlib::hnswlib columnar_root util common )
set_target_properties( knn_lib PROPERTIES PREFIX "" OUTPUT_NAME lib_manticore_knn${lib_arch_suffix} )

# Try to find manticoresearch text embeddings library
message(STATUS "Looking for manticoresearch text embeddings library...")

if ( BUILD_EMBEDDINGS_LOCALLY )
	include (build_embeddings)
	build_embeddings_lib()
endif()

# find embeddings library
set(EMBEDDINGS_LIB_NAME _manticore_knn_embeddings)
set ( CMAKE_FIND_LIBRARY_SUFFIXES .so .dylib .dll )
FIND_LIBRARY ( EMBEDDINGS_LIB NAMES lib_${EMBEDDINGS_LIB_NAME} HINTS
		"${CMAKE_SOURCE_DIR}/embeddings/target/debug"
		"${CMAKE_SOURCE_DIR}/embeddings/target/release"
		"${CMAKE_CURRENT_BINARY_DIR}/embeddings/release"
		NO_CMAKE_FIND_ROOT_PATH
		NO_DEFAULT_PATH
)

if (PACK AND NOT EMBEDDINGS_LIB)
	message(FATAL_ERROR "Could not find manticoresearch text embeddings library at: ${EMBEDDINGS_LIB}")
endif()

# Add embeddings library as a component to be included in packages
add_library ( embeddings_lib MODULE IMPORTED )
set_target_properties ( embeddings_lib PROPERTIES IMPORTED_LOCATION "${EMBEDDINGS_LIB}" INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/embeddings/")

if ( NOT EXTERNAL_LIB ) # install only once, not from external_project build
	install ( IMPORTED_RUNTIME_ARTIFACTS embeddings_lib RUNTIME DESTINATION ${MODULES_DIR} LIBRARY DESTINATION ${MODULES_DIR} COMPONENT embeddings )
endif()

message ( STATUS "Found manticoresearch text embeddings library: ${EMBEDDINGS_LIB}" )
