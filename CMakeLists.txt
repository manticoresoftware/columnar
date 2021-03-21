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

cmake_minimum_required ( VERSION 2.8 )

project ( columnar )
SET ( columnar_CMAKE_DIR "${columnar_SOURCE_DIR}/cmake" )
SET ( CMAKE_MODULE_PATH "${columnar_SOURCE_DIR}/cmake" )

set ( GCC_OR_CLANG 0 )

if ( CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang" )
	set ( GCC_OR_CLANG 1 )
endif()

# override default /MD
if ( MSVC )
	set ( CMAKE_CXX_FLAGS_DEBUG_INIT "/D_DEBUG /MTd /Zi /Ob0 /Od /RTC1 /GR-" )
	set ( CMAKE_CXX_FLAGS_MINSIZEREL_INIT "/MT /O1 /Ob1 /D NDEBUG /GR-" )
	set ( CMAKE_CXX_FLAGS_RELEASE_INIT "/MT /O2 /Ob2 /D NDEBUG /GR-" )
	set ( CMAKE_CXX_FLAGS_RELWITHDEBINFO_INIT "/MT /Zi /O2 /Ob2 /D NDEBUG /GR-" )
	set ( CMAKE_CXX_STANDARD_LIBRARIES_INIT "kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib odbccp32.lib" )
elseif ( GCC_OR_CLANG )
	add_definitions ( "-D_FILE_OFFSET_BITS=64" )
endif()

if (DEFINED ENV{LIBS_BUNDLE})
	set(LIBS_BUNDLE "$ENV{LIBS_BUNDLE}")
endif()

if ( NOT LIBS_BUNDLE )
	get_filename_component(LIBS_BUNDLE "${CMAKE_CURRENT_SOURCE_DIR}/../bundle" ABSOLUTE)
endif ()

if (NOT IS_ABSOLUTE ${LIBS_BUNDLE})
	set(LIBS_BUNDLE "${CMAKE_CURRENT_BINARY_DIR}/${LIBS_BUNDLE}")
endif ()


include ( CheckFunctionExists )
string ( TOUPPER "${it}" _it )
check_function_exists ( "pread" "HAVE_PREAD" )
message ( STATUS "Generating config.h file" )
configure_file ( "${CMAKE_CURRENT_SOURCE_DIR}/config_cmake.h.in" "${CMAKE_CURRENT_BINARY_DIR}/config/config.h" ESCAPE_QUOTES )
include_directories ( "${CMAKE_CURRENT_BINARY_DIR}/config" )


set ( ACCESSOR_H accessor/attributeheader.h accessor/accessor.h accessor/accessorbool.h accessor/accessorint.h accessor/accessormva.h
	accessor/accessorstr.h accessor/accessortraits.h )

set ( ACCESSOR_SRC accessor/attributeheader.cpp accessor/accessor.cpp accessor/accessorbool.cpp accessor/accessorint.cpp accessor/accessormva.cpp
	accessor/accessorstr.cpp accessor/accessortraits.cpp )

set ( BUILDER_H builder/builderbool.h builder/builderint.h builder/builderminmax.h builder/buildermva.h builder/builderstr.h builder/buildertraits.h )
set ( BUILDER_SRC builder/builderbool.cpp builder/builderint.cpp builder/buildermva.cpp builder/builderstr.cpp builder/buildertraits.cpp )

set ( UTIL_H util/util.h util/interval.h util/reader.h )
set ( UTIL_SRC util/util.cpp util/reader.cpp util/version.cpp )

set ( COMMON_H columnar.h builder.h )
set ( COMMON_SRC columnar.cpp builder.cpp )

set ( ALL_HEADERS ${ACCESSOR_H} ${BUILDER_H} ${COMMON_H} ${UTIL_H} )
set ( ALL_SOURCES ${ACCESSOR_SRC} ${BUILDER_SRC} ${COMMON_SRC} ${UTIL_SRC} )

SET ( CMAKE_CXX_STANDARD 14 )

add_library ( columnar SHARED ${ALL_SOURCES} ${ALL_HEADERS} ${COMMON_SRC} )
set_target_properties(columnar PROPERTIES OUTPUT_NAME "_manticore_columnar")
target_include_directories ( columnar PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} )
target_include_directories ( columnar PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/util )
target_include_directories ( columnar PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/accessor )
target_include_directories ( columnar PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/builder )

if (WIN32)
	target_compile_options ( columnar PRIVATE "-wd4996" )
endif()

SET ( EXTRA_LIBRARIES )
include ( ${CMAKE_CURRENT_SOURCE_DIR}/GetFastPFOR.cmake )

target_link_libraries ( columnar ${EXTRA_LIBRARIES} )

if (DEFINED COLUMNAR_DEST)
	add_custom_command ( TARGET columnar POST_BUILD	COMMAND ${CMAKE_COMMAND} -E rm -f "${COLUMNAR_DEST}/$<TARGET_FILE_NAME:columnar>" )
	add_custom_command ( TARGET columnar POST_BUILD	COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:columnar>" "${COLUMNAR_DEST}/."	)
	if ( MSVC )
		add_custom_command ( TARGET columnar POST_BUILD	COMMAND ${CMAKE_COMMAND} -E rm -f "${COLUMNAR_DEST}/$<TARGET_PDB_FILE_NAME:columnar>" )
		add_custom_command ( TARGET columnar POST_BUILD	COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_PDB_FILE:columnar>" "${COLUMNAR_DEST}/." )
	endif ()
endif ()

# --------------- related with packaging -------------------

set ( PACKAGE_NAME "manticore-columnar-lib" )
set ( COMPANY_NAME "Manticore Software LTD" )
set ( BUILD_TAG "" CACHE STRING "The tag which will be added to build" )
set ( PKGSUFFIXES ) # list of suffixes which will be append to package name, will be '-'-separated and added to package file name along with DISTR_SUFFIX

# getting library version from version.h.in
include( GetVersion )
get_library_version( VERSION_STR "${CMAKE_CURRENT_SOURCE_DIR}/util/version.h.in" )

# simpler packages: provide -DPACK=1, and DISTR_BUILD will be set from env $DISTR, easier in dockers
if (PACK)
        set(DISTR_BUILD "$ENV{DISTR}")
endif ()


# Make release build for the pointed distr
# That will override defaults and give possibility to build
# the distribution with minimal command line
if ( DISTR_BUILD )
        # set default options to be included into build
        set ( DISTR "${DISTR_BUILD}" CACHE STRING "Choose the distr." )
        if ( NOT "${DISTR_BUILD}" STREQUAL "macosbrew" )
                SET ( CMAKE_INSTALL_PREFIX "/usr" CACHE FILEPATH "Forced install to /usr" FORCE )
        endif ()
        message ( STATUS "DISTR_BUILD applied. Package will be set to ${DISTR_BUILD}" )
endif ()

include ( SetBuildType )

configure_file ( "LICENSE" "${CMAKE_CURRENT_BINARY_DIR}/LICENSE" COPYONLY )

set ( CPACK_SOURCE_IGNORE_FILES "/\\\\.idea/" "/\\\\.git/"
                "/\\\\.svn/" "/autom4te\\\\.cache/" "/bin/" "/junk/" "/win/" "/gmock/" "/gtest/" "/libtest"   "/pkgconfig/"
                ".swp$" ".orig$" ".log$" ".deps/" )
set ( CPACK_SOURCE_GENERATOR TGZ )
set ( CPACK_PACKAGE_NAME "${PACKAGE_NAME}" )
set ( CPACK_PACKAGE_CONTACT "Manticore Team <build@manticoresearch.com>" )
set ( CMAKE_PROJECT_HOMEPAGE_URL "https://github.com/manticoresoftware/columnar/" )
set ( CPACK_PACKAGE_VENDOR "${COMPANY_NAME}" )
set ( CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_BINARY_DIR}/LICENSE" )
set ( CPACK_PACKAGE_DESCRIPTION_SUMMARY "Manticore Columnar Library is a column-oriented storage library, aiming to provide decent performance with low memory footprint at big data volume")
set ( CPACK_PACKAGE_DESCRIPTION "Manticore Columnar Library is a column-oriented storage library, aiming to provide decent performance with low memory footprint at big data volume. When used in combination with Manticore Search can be beneficial for faster / lower resource consumption log/metrics analytics and running log / metric analytics in docker / kubernetes")
set ( CPACK_RPM_PACKAGE_DESCRIPTION "${CPACK_PACKAGE_DESCRIPTION}" ) # the description will default to the default one (This is an installer created using CPack..." otherwise, i.e. it doesn't take CPACK_PACKAGE_DESCRIPTION as a default 
set ( CPACK_PROJECT_CONFIG_FILE "${CMAKE_CURRENT_BINARY_DIR}/config/CPackOptions.cmake" )

include ( CPack )
cpack_add_component( columnar )

if ( WIN32 )
        IF ( CMAKE_EXE_LINKER_FLAGS MATCHES "x64" )
                LIST ( APPEND PKGSUFFIXES "x64" )
        else ()
                LIST ( APPEND PKGSUFFIXES "win32" )
        endif ()
endif ()

set ( CPACK_SUFFIX "" )
FOREACH ( SUFFIX ${PKGSUFFIXES} )
        set ( CPACK_SUFFIX "${CPACK_SUFFIX}-${SUFFIX}" )
endforeach ()

add_custom_target (
                columnarver
                COMMAND ${CMAKE_COMMAND}
                        -D SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
                        -D BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
                        -D BUILD_TAG=${BUILD_TAG}
                        -D DISTR_SUFFIX=${DISTR_SUFFIX}
                        -D PACKAGE_NAME=${PACKAGE_NAME}
                        -D CPACK_SUFFIX=${CPACK_SUFFIX}
                        -P ${CMAKE_CURRENT_SOURCE_DIR}/rev.cmake
                COMMENT "Generate version from sources"
                VERBATIM
                SOURCES
                        ${CMAKE_CURRENT_SOURCE_DIR}/rev.cmake
                        ${CMAKE_CURRENT_SOURCE_DIR}/util/version.h.in
                        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CPackOptions.cmake.in
)

add_dependencies ( columnar columnarver )
