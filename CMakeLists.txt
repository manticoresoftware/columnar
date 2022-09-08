# Copyright (c) 2020-2022, Manticore Software LTD (https://manticoresearch.com)
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

FOREACH (policy CMP0048 CMP0090 CMP0092 CMP0117)
	IF (POLICY ${policy})
		CMAKE_POLICY ( SET ${policy} NEW )
	ENDIF ()
ENDFOREACH ()

set ( CMAKE_INTERPROCEDURAL_OPTIMIZATION $ENV{CMAKE_INTERPROCEDURAL_OPTIMIZATION} )
set ( _CMAKE_BUILD_TYPE ${CMAKE_BUILD_TYPE} )

# IMPORTANT: the below version should be in format x.y.z.
# NOTE, if "z" is even the CI will release the package. It should be made odd if the version is still under development.
project ( columnar VERSION 1.16.1 )

# sometimes CMAKE_BUILD_TYPE became set after PROJECT statement, undo it.
if (NOT _CMAKE_BUILD_TYPE AND CMAKE_BUILD_TYPE)
	unset ( CMAKE_BUILD_TYPE CACHE )
endif ()

# check what we're build. api just packs headers and build nothing
if (NOT API_ONLY)
	set ( API_ONLY OFF)
endif()

if (API_ONLY)
	message ( STATUS "Only api will be installed" )
	set ( SKIP_COLUMNAR ON )
	set ( SKIP_SECONDARY ON )
endif()

if (NOT SKIP_COLUMNAR)
	set ( INSTALL_COLUMNAR ON )
endif ()

if (NOT API_SECONDARY)
	set ( INSTALL_SECONDARY ON )
endif ()

SET ( CMAKE_MODULE_PATH "${columnar_SOURCE_DIR}/cmake" )

# Version
include ( rev )

# main interface target to export/expose headers
# Note, first we provide headers which will be available even if build of the main lib itself will not succeed
# (say, on aarch64 because of absent fastpfor). But as api target is already exposed, consumers still can use it.
add_library ( columnar_api INTERFACE )
add_library ( secondary_api INTERFACE )
target_link_libraries ( secondary_api INTERFACE columnar_api ) # actually we need only common headers, so it can be changed in future

# that will be used when build for dev/testing
add_library ( columnar::columnar_api ALIAS columnar_api )
add_library ( columnar::secondary_api ALIAS secondary_api )

# interface property, so that consumer will fire an error on configure time if API version is not match
FILE ( STRINGS columnar/columnar.h _verlist LIMIT_COUNT 1 REGEX "LIB_VERSION" )
STRING ( REGEX MATCH "[0-9]+" API_VER "${_verlist}" )
set_property ( TARGET columnar_api PROPERTY INTERFACE_COLUMNAR_API_VERSION ${API_VER} ) # must be reconfigured if version in columnar.h changes
set_property ( TARGET columnar_api APPEND PROPERTY COMPATIBLE_INTERFACE_STRING COLUMNAR_API_VERSION )

FILE ( STRINGS secondary/secondary.h _verlist LIMIT_COUNT 1 REGEX "LIB_VERSION" )
STRING ( REGEX MATCH "[0-9]+" SI_API_VER "${_verlist}" )
set_property ( TARGET secondary_api APPEND PROPERTY INTERFACE_SECONDARY_API_VERSION ${SI_API_VER} ) # must be reconfigured if version in secondary.h changes
set_property ( TARGET secondary_api APPEND PROPERTY COMPATIBLE_INTERFACE_STRING SECONDARY_API_VERSION )

# Check below is only to provide human-readable match - that is we can see that package v. X.30.Y supports API v30.
if (NOT PROJECT_VERSION_MINOR EQUAL API_VER)
	message ( WARNING "Minor (${PROJECT_VERSION_MINOR}) of project version ${PROJECT_VERSION} is not equal to LIB_VERSION (${API_VER}) in file columnar.h. That is not critical, but it would be nice if V of API is reflected someway in project version with human-readable way." )
endif ()

set (_includes include/manticore-columnar-api)
target_include_directories ( columnar_api INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>$<INSTALL_INTERFACE:${_includes}> )

# install columnar and secondary API (only headers, no packaging, don't build anything at all)
if (API_ONLY)
	install ( TARGETS columnar_api EXPORT apiexport )
	install ( TARGETS secondary_api EXPORT apiexport )

	# ensure ALL externally required headers are exported here
	install ( FILES columnar/builder.h columnar/columnar.h DESTINATION ${_includes}/columnar )
	install ( FILES util/util.h DESTINATION ${_includes}/util )
	install ( FILES common/schema.h common/blockiterator.h common/filter.h DESTINATION ${_includes}/common )

	# ensure ALL externally required headers of secondary are exported here
	install ( FILES secondary/secondary.h secondary/builder.h secondary/iterator.h DESTINATION ${_includes}/secondary )

	set ( API_CMAKE_DIR "lib/cmake/columnar" )
	install ( EXPORT apiexport
			FILE "columnar-targets.cmake"
			DESTINATION "${API_CMAKE_DIR}"
			NAMESPACE "columnar::"
			)

	# below is for support find_package
	include ( CMakePackageConfigHelpers )

	set ( pkgconfin "${CMAKE_CURRENT_BINARY_DIR}/columnar-config.cmake.in" )
	file ( WRITE "${pkgconfin}" "@PACKAGE_INIT@

include(\"\${CMAKE_CURRENT_LIST_DIR}/columnar-targets.cmake\")" )

	configure_package_config_file ( "${pkgconfin}"
			"${CMAKE_CURRENT_BINARY_DIR}/columnar-config.cmake" INSTALL_DESTINATION "${API_CMAKE_DIR}" )
	install ( FILES "${CMAKE_CURRENT_BINARY_DIR}/columnar-config.cmake" DESTINATION "${API_CMAKE_DIR}" )

	write_basic_package_version_file ( "columnar-config-version.cmake" VERSION "${API_VER}.${SI_API_VER}" COMPATIBILITY ExactVersion ARCH_INDEPENDENT )
	install ( FILES "${CMAKE_CURRENT_BINARY_DIR}/columnar-config-version.cmake" DESTINATION "${API_CMAKE_DIR}" )

	# finish configuration, as it is API_ONLY
	return()
endif ()

# here we came in case of full build (i.e. columnar and/or secondary)

SET ( CMAKE_CXX_STANDARD 14 )

# Helpers
include ( helpers )
include ( CommonInfo )
include ( init_cache_settings ) # set libs_bundle, cacheb, diagnostic. etc.

# Set a default build type for single-configuration CMake generators if no build type is set.
get_property ( isMultiConfig GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG )
if (NOT isMultiConfig AND NOT CMAKE_BUILD_TYPE)
	set ( CMAKE_BUILD_TYPE RelWithDebInfo )
	message ( STATUS "Automatically set build type to RelWithDebInfo since no other provided" )
endif ()

# simpler packages: provide -DPACK=1, and DISTR_BUILD will be set from env $DISTR, easier in dockers
if (PACK)
	set ( DISTR_BUILD "$ENV{DISTR}" )
endif ()

if (DISTR_BUILD)
	set ( BUILD_TESTING 0 CACHE BOOL "No testing on packaging" )
	set ( DISTR "${DISTR_BUILD}" CACHE STRING "Choose the distr." )
	message ( STATUS "DISTR_BUILD applied. Package will be set to ${DISTR_BUILD}" )
	include ( SetBuildType )
endif ()

# if no fastpfor here, we can't continue, sorry...
include ( GetFastPFOR )

# cpack project file has minor function - it checks that configured version is the same as build version
set ( CPACK_PROJECT_CONFIG_FILE "${CMAKE_CURRENT_BINARY_DIR}/config/CPackOptions.cmake" )

# main internal interface target to collect all includes/links/dependencies
add_library ( columnar_root INTERFACE )

if (COVERAGE_TEST)
	target_compile_options ( columnar_root INTERFACE $<${GNUC_CXX}:-fprofile-arcs -ftest-coverage> )
	target_link_options ( columnar_root INTERFACE
			$<$<LINK_LANG_AND_ID:CXX,GNU>:-lgcov --coverage>
			$<$<LINK_LANG_AND_ID:C,GNU>:-lgcov --coverage>
			$<$<LINK_LANG_AND_ID:CXX,Clang,AppleClang>: --coverage>
			$<$<LINK_LANG_AND_ID:C,Clang,AppleClang>: --coverage>
			)
endif (COVERAGE_TEST)

# options for clang/gcc c and c++
target_compile_options ( columnar_root INTERFACE $<${ONLYGNUCLANGC_CXX}:-D_FILE_OFFSET_BITS=64> )
target_compile_options ( columnar_root INTERFACE $<${ONLYGNUCLANGCXX}:-fno-rtti>$<${MSCXX}:/GR-> ) # no rtti
target_compile_options ( columnar_root INTERFACE $<${MSCXX}:-D_CRT_SECURE_NO_WARNINGS -D_CRT_NONSTDC_NO_WARNINGS> )

if (TARGET FastPFOR::SIMDe)
	target_link_libraries ( columnar_root INTERFACE FastPFOR::SIMDe )
endif ()

target_link_libraries ( columnar_root INTERFACE columnar::columnar_api )
target_include_directories ( columnar_root INTERFACE columnar util common )
set_property ( TARGET columnar_root PROPERTY INTERFACE_POSITION_INDEPENDENT_CODE TRUE )

add_subdirectory ( util )
add_subdirectory ( common )

include ( GNUInstallDirs )
if (WIN32)
	SET ( MODULES_DIR "${CMAKE_INSTALL_DATADIR}/modules" )
else()
	SET ( MODULES_DIR "${CMAKE_INSTALL_DATADIR}/manticore/modules" )
endif()

if (INSTALL_COLUMNAR)
	add_subdirectory ( columnar )
	install ( TARGETS columnar_lib RUNTIME DESTINATION ${MODULES_DIR} LIBRARY DESTINATION ${MODULES_DIR} COMPONENT columnar )
	install ( FILES "$<TARGET_FILE_DIR:columnar_lib>/lib_manticore_columnar.pdb" DESTINATION ${MODULES_DIR} COMPONENT dbgsymbols OPTIONAL )
endif ()

if (INSTALL_SECONDARY)
	add_subdirectory ( secondary )
	install ( TARGETS secondary_index RUNTIME DESTINATION ${MODULES_DIR} LIBRARY DESTINATION ${MODULES_DIR} COMPONENT secondary )
	install ( FILES "$<TARGET_FILE_DIR:secondary_index>/lib_manticore_secondary.pdb" DESTINATION ${MODULES_DIR} COMPONENT dbgsymbols OPTIONAL )
endif ()

include ( CPack )
include ( testing.cmake )
