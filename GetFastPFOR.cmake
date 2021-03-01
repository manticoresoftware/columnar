# Copyright (c) 2020-2021, Manticore Software LTD (https://manticoresearch.com)
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

function ( DOWNLOAD NAME LOCATION SOURCE_LOCATION )
	include(FetchContent)
	if (EXISTS ${LOCATION})
		file(SHA1 "${LOCATION}" SHA1SUM)
		set(SHA1SUM "SHA1=${SHA1SUM}")
		FetchContent_Declare ( ${NAME} SOURCE_DIR "${SOURCE_LOCATION}" URL "${LOCATION}" URL_HASH ${SHA1SUM} )
	else()
		FetchContent_Declare ( ${NAME} SOURCE_DIR "${SOURCE_LOCATION}" URL "${LOCATION}" )
	endif()

	FetchContent_GetProperties ( ${NAME} )
	if ( NOT ${NAME}_POPULATED )
		message ( STATUS "Populating ${NAME} from ${LOCATION}" )
		FetchContent_Populate ( ${NAME} )
	endif()
endfunction()

set ( LIB_GITHUB 	"https://github.com/lemire/FastPFor/archive/master.zip")
set ( LIB_TARBALL 	"FastPFor-master.zip")
set ( LIB			"FastPFOR" )
string ( TOLOWER "${LIB}" LIB_LOWERCASE )

if ( DEFINED LIBS_BUNDLE )
	get_filename_component ( LIBS_BUNDLE "${LIBS_BUNDLE}" ABSOLUTE )
endif()

set ( LOCAL_URL "${LIBS_BUNDLE}/${LIB_TARBALL}" )

if ( EXISTS "${LOCAL_URL}" )
	set (LIB_LOCATION "${LOCAL_URL}")
else()
	set (LIB_LOCATION "${LIB_GITHUB}" )
endif()

set ( LIB_SOURCES "${CMAKE_CURRENT_BINARY_DIR}/${LIB_LOWERCASE}-src" )
set ( LIB_BUILD "${CMAKE_CURRENT_BINARY_DIR}/${LIB}")
                                    
if ( NOT EXISTS "${LIB_SOURCES}/LICENSE" )
	download ( ${LIB} ${LIB_LOCATION} ${LIB_SOURCES} )
	configure_file ( "${CMAKE_SOURCE_DIR}/libfastpfor/CMakeLists.txt" "${LIB_SOURCES}/CMakeLists.txt" COPYONLY )
endif()

add_subdirectory ( ${LIB_SOURCES} ${LIB_BUILD} EXCLUDE_FROM_ALL )

list ( APPEND EXTRA_LIBRARIES FastPFOR )