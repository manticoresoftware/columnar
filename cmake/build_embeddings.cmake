# Copyright (c) 2020-2025, Manticore Software LTD (https://manticoresearch.com)
# All rights reserved
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

if (__build_embeddings_included)
	return ()
endif ()
set ( __build_embeddings_included YES )

function(build_embeddings_lib)
	message ( STATUS "building embeddings locally..." )

	if (NOT DEFINED CARGO_COMMAND)
		find_program ( CARGO_COMMAND cargo )
		if (NOT CARGO_COMMAND)
			message ( FATAL_ERROR "Cargo command not found. Please install Rust and ensure cargo is in your PATH." )
		endif ()
	endif ()

	execute_process (
			COMMAND cargo build --manifest-path ${CMAKE_SOURCE_DIR}/embeddings/Cargo.toml --lib --release --target-dir ${CMAKE_CURRENT_BINARY_DIR}/embeddings
			RESULT_VARIABLE CMD_RESULT
	)

	if (NOT CMD_RESULT EQUAL 0)
		message ( FATAL_ERROR "Failed to build: ${CMD_RESULT}" )
	endif ()
endfunction ()

