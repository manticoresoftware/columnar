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

add_library ( knn_lib MODULE knn.cpp knn.h iterator.cpp iterator.h embeddings.cpp embeddings.h manticoresearch_text_embeddings.h )

if (CMAKE_SYSTEM_PROCESSOR STREQUAL x86_64 OR CMAKE_SYSTEM_PROCESSOR STREQUAL amd64)
	message ( STATUS "Add sse 4.1 to compiler flags for ${CMAKE_SYSTEM_PROCESSOR} arch" )
	target_compile_options ( knn_lib PUBLIC $<${GNUC_CXX}:-msse4.1> )
endif ()

target_link_libraries ( knn_lib PRIVATE hnswlib::hnswlib columnar_root util common )
set_target_properties( knn_lib PROPERTIES PREFIX "" OUTPUT_NAME lib_manticore_knn )

# Try to find manticoresearch text embeddings library
message(STATUS "Looking for manticoresearch text embeddings library...")

# Set the library name
set(EMBEDDINGS_LIB_NAME manticoresearch_text_embeddings)

# Set platform-specific library file names
if(WIN32)
	set(EMBEDDINGS_LIB_FILE "${EMBEDDINGS_LIB_NAME}.lib")
elseif(APPLE)
	set(EMBEDDINGS_LIB_FILE "lib${EMBEDDINGS_LIB_NAME}.a")
else()
	set(EMBEDDINGS_LIB_FILE "lib${EMBEDDINGS_LIB_NAME}.a")
endif()

if(NOT DEFINED ENV{SYSROOT_URL})
	message(STATUS "SYSROOT_URL not set, building embeddings locally...")

	if(NOT DEFINED CARGO_COMMAND)
		find_program(CARGO_COMMAND cargo)
		if(NOT CARGO_COMMAND)
			message(FATAL_ERROR "Cargo command not found. Please install Rust and ensure cargo is in your PATH.")
		endif()
	endif()

	execute_process(
		COMMAND cargo build --manifest-path ${CMAKE_SOURCE_DIR}/embeddings/Cargo.toml --lib --release
		RESULT_VARIABLE CMD_RESULT
	)
	if(NOT CMD_RESULT EQUAL 0)
		message(FATAL_ERROR "Failed to build: ${CMD_RESULT}")
	endif()
endif()

# Set the path to look for the library
set(EMBEDDINGS_LIB_PATH "${CMAKE_SOURCE_DIR}/embeddings/target/release")

# Construct the full library path
set(EMBEDDINGS_LIB "${EMBEDDINGS_LIB_PATH}/${EMBEDDINGS_LIB_FILE}")

# Check if library exists
if(EXISTS ${EMBEDDINGS_LIB})
	message(STATUS "Found manticoresearch text embeddings library: ${EMBEDDINGS_LIB}")
else()
	message(FATAL_ERROR "Could not find manticoresearch text embeddings library at: ${EMBEDDINGS_LIB}")
endif()

target_link_libraries(knn_lib PRIVATE "-Wl,--whole-archive" ${EMBEDDINGS_LIB} "-Wl,--no-whole-archive")
set_property(TARGET knn_lib PROPERTY LINK_FLAGS "-Wl,--no-as-needed")

