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

cmake_minimum_required ( VERSION 3.17 )

FOREACH (policy CMP0048 CMP0090 CMP0092 CMP0117 )
	IF (POLICY ${policy})
		CMAKE_POLICY ( SET ${policy} NEW )
	ENDIF ()
ENDFOREACH ()

set ( CMAKE_INTERPROCEDURAL_OPTIMIZATION $ENV{CMAKE_INTERPROCEDURAL_OPTIMIZATION})

project ( columnar VERSION 1.12.1 )
SET ( CMAKE_CXX_STANDARD 14 )
SET ( CMAKE_MODULE_PATH "${columnar_SOURCE_DIR}/cmake" )

# main interface target to export/expose headers
# Note, first we provide headers which will be available even if build of the main lib itself will not succeed
# (say, on aarch64 because of absent fastpfor). But as api target is already exposed, consumers still can use it.
add_library ( columnar_api INTERFACE )

# interface property, so that consumer will fire an error on configure time if API version is not match
FILE ( STRINGS columnar.h _verlist LIMIT_COUNT 1 REGEX "LIB_VERSION" )
STRING ( REGEX MATCH "[0-9]+" API_VER "${_verlist}" )
set_property ( TARGET columnar_api PROPERTY INTERFACE_COLUMNAR_API_VERSION ${API_VER} ) # must be reconfigured if version in columnar.h changes
set_property ( TARGET columnar_api APPEND PROPERTY COMPATIBLE_INTERFACE_STRING COLUMNAR_API_VERSION )
if (NOT PROJECT_VERSION_MINOR EQUAL API_VER)
	message ( FATAL_ERROR "Minor (${PROJECT_VERSION_MINOR}) of project version ${PROJECT_VERSION} is not equal to LIB_VERSION (${API_VER}) in file columnar.h, need to upgrade project version!" )
endif ()

include ( GNUInstallDirs )

set ( CPACK_PACKAGE_NAME "manticore-columnar-lib" )
target_include_directories ( columnar_api INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/${CPACK_PACKAGE_NAME}> )

install ( TARGETS columnar_api EXPORT ColumnarExport COMPONENT module )

# ensure ALL externally required headers are exported here
install ( FILES builder.h columnar.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/${CPACK_PACKAGE_NAME} COMPONENT dev )
install ( FILES util/util.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/${CPACK_PACKAGE_NAME}/util COMPONENT dev )

add_library ( columnar::columnar_api ALIAS columnar_api )

# helpers and parts
if (NOT SKIP_COLUMNAR)
	include ( GetFastPFOR )
endif()

# Set a default build type for single-configuration (for multi-config there are NO default settings anyway),
# it should be selected with --config SOMETHING on build time
if (NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
	message ( STATUS "No config specified, forced to RelWithDebInfo" )
	set ( CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Choose the type of build." FORCE )
endif ()

# if no faspfor, we're still useful as columnar_api is already created and points to correct headers
if (TARGET FastPFOR::FastPFOR)

	# helpers vars to shorten generate lines
	set ( CLANGCXX "$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang>" )
	set ( GNUCXX "$<COMPILE_LANG_AND_ID:CXX,GNU>" )
	set ( GNUCLANGCXX "$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang,GNU>" )
	set ( CLANGC "$<COMPILE_LANG_AND_ID:C,Clang,AppleClang>" )
	set ( GNUC "$<COMPILE_LANG_AND_ID:C,GNU>" )
	set ( GNUCLANGC "$<COMPILE_LANG_AND_ID:C,Clang,AppleClang,GNU>" )
	set ( GNUC_CXX "$<OR:${GNUCXX},${GNUC}>" )
	set ( CLANGC_CXX "$<OR:${CLANGCXX},${CLANGC}>" )
	set ( GNUCLANGC_CXX "$<OR:${GNUCLANGCXX},${GNUCLANGC}>" )
	if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
		set ( CLANG_CL 1 )
		set ( ONLYGNUCLANGCXX 0 )
		set ( ONLYGNUCLANGC_CXX 0 )
		set ( MSCXX 1 )
	else ()
		set ( CLANG_CL 0 )
		set ( ONLYGNUCLANGCXX "${GNUCLANGCXX}" )
		set ( ONLYGNUCLANGC_CXX "${GNUCLANGC_CXX}" )
		set ( MSCXX "$<COMPILE_LANG_AND_ID:CXX,MSVC>" )
	endif ()

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

	target_link_libraries ( columnar_root INTERFACE columnar_api )
	target_include_directories ( columnar_root INTERFACE accessor builder util )
	set_property ( TARGET columnar_root PROPERTY INTERFACE_POSITION_INDEPENDENT_CODE TRUE )

	add_subdirectory ( util )
	add_subdirectory ( builder )
	add_subdirectory ( accessor )

	# main library
	add_library ( columnar MODULE columnar.cpp builder.cpp columnar.h builder.h )
	target_compile_options ( columnar PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:-wd4996> )
	target_link_libraries ( columnar PRIVATE columnar_root util builder accessor )
	set_target_properties( columnar PROPERTIES PREFIX "" OUTPUT_NAME lib_manticore_columnar )

	SET ( MODULES_DIR "${CMAKE_INSTALL_DATADIR}/manticore/modules" )

	# simpler packages: provide -DPACK=1, and DISTR_BUILD will be set from env $DISTR, easier in dockers
	if (PACK)
		set ( DISTR_BUILD "$ENV{DISTR}" )
	endif ()

	if (DISTR_BUILD)
		# --------------- related with packaging -------------------
		set ( BUILD_TESTING 0 CACHE BOOL "No testing on packaging" )

		set ( CPACK_PACKAGE_VENDOR "Manticore Software LTD" )
		set ( BUILD_TAG "$ENV{BUILD_TAG}" CACHE STRING "The tag which will be added to build" )

		# Make release build for the pointed distr
		# That will override defaults and give possibility to build
		# the distribution with minimal command line
		# set default options to be included into build
		set ( DISTR "${DISTR_BUILD}" CACHE STRING "Choose the distr." )
		message ( STATUS "DISTR_BUILD applied. Package will be set to ${DISTR_BUILD}" )

		set ( CPACK_SOURCE_IGNORE_FILES "/\\\\.idea/" "/\\\\.git/"
				"/\\\\.svn/" "/autom4te\\\\.cache/" "/bin/" "/junk/" "/win/" "/gmock/" "/gtest/" "/libtest"   "/pkgconfig/"
				".swp$" ".orig$" ".log$" ".deps/" )
		set ( CPACK_SOURCE_GENERATOR TGZ )
		set ( CPACK_PACKAGE_RELOCATABLE ON )
		set ( CPACK_PACKAGE_CONTACT "Manticore Team <build@manticoresearch.com>" )
		set ( CMAKE_PROJECT_HOMEPAGE_URL "https://github.com/manticoresoftware/columnar/" )
		set ( CPACK_PACKAGE_URL "https://github.com/manticoresoftware/columnar/" )
		set ( CPACK_PACKAGE_DESCRIPTION_SUMMARY "Manticore Columnar Library is a column-oriented storage library, aiming to provide decent performance with low memory footprint at big data volume")
		set ( CPACK_PACKAGE_DESCRIPTION "Manticore Columnar Library is a column-oriented storage library, aiming to provide decent performance with low memory footprint at big data volume. When used in combination with Manticore Search can be beneficial for faster / lower resource consumption log/metrics analytics and running log / metric analytics in docker / kubernetes")
		set ( CPACK_RPM_PACKAGE_DESCRIPTION "${CPACK_PACKAGE_DESCRIPTION}" ) # the description will default to the default one (This is an installer created using CPack..." otherwise, i.e. it doesn't take CPACK_PACKAGE_DESCRIPTION as a default
		set ( CPACK_PROJECT_CONFIG_FILE "${CMAKE_CURRENT_BINARY_DIR}/config/CPackOptions.cmake" )
		SET ( CPACK_PACKAGE_LICENSE "Apache-2.0" )

		set ( CPACK_COMPONENTS_ALL module dev )
		set ( CPACK_COMPONENT_DEV_DEPENDS module )

		include ( SetBuildType )

		add_custom_target (
				columnarpackver
				COMMAND ${CMAKE_COMMAND}
				-D SOURCE_DIR=${columnar_SOURCE_DIR}
				-D BINARY_DIR=${columnar_BINARY_DIR}
				-D BUILD_TAG=${BUILD_TAG}
				-D DISTR=${DISTR}
				-D CPACK_PACKAGE_NAME=${CPACK_PACKAGE_NAME}
				-D CPACK_SUFFIX=${CPACK_SUFFIX}
				-D VERSION_STR=${CMAKE_PROJECT_VERSION}
				-D CMAKE_MODULE_PATH=${CMAKE_MODULE_PATH}
				-D SRC=${CMAKE_MODULE_PATH}/CPackOptions.cmake.in
				-D DST=${columnar_BINARY_DIR}/config/CPackOptions.cmake
				-P ${columnar_SOURCE_DIR}/packversion.cmake
				VERBATIM
				COMMENT "Assign package version data"
				BYPRODUCTS ${columnar_BINARY_DIR}/config/CPackOptions.cmake
				SOURCES ${CMAKE_MODULE_PATH}/CPackOptions.cmake.in
				DEPENDS ${CMAKE_MODULE_PATH}/CPackOptions.cmake.in
		)

		add_dependencies ( columnar columnarpackver )
	endif()

	install ( TARGETS columnar EXPORT ColumnarExport RUNTIME DESTINATION ${MODULES_DIR} LIBRARY DESTINATION ${MODULES_DIR} COMPONENT module )

	# aliases are in game when we plug columnar sources via add_subdirectory().
	# We essentially need columnar_api (it provides correct path to includes), but module itself still need to be located.
	# Suggest to build columnar as external project when you want 'just use' it, then install to the known prefix and
	# plug via find_package (columnar CONFIG).
	# In case of development, when it is added via add_subdirectory to manticore build tree, it may be manually provided via
	# env LIB_MANTICORE_COLUMNAR in particular.
	add_library ( columnar::columnar ALIAS columnar )

else()
	message (STATUS "FastPFOR is disabled or not available, only columnar::columnar_api target created")
endif()

set (CMAKE_CMAKE_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/columnar")
install ( EXPORT ColumnarExport
		FILE "columnar-targets.cmake"
		DESTINATION "${CMAKE_CMAKE_DIR}"
		NAMESPACE "columnar::"
		COMPONENT dev
		)

# below is for support find_package
include ( CMakePackageConfigHelpers )

configure_package_config_file ( "${CMAKE_MODULE_PATH}/columnar-config.cmake.in"
		"${CMAKE_CURRENT_BINARY_DIR}/columnar-config.cmake" INSTALL_DESTINATION "${CMAKE_CMAKE_DIR}" )
install ( FILES "${CMAKE_CURRENT_BINARY_DIR}/columnar-config.cmake" DESTINATION "${CMAKE_CMAKE_DIR}" COMPONENT dev)

write_basic_package_version_file ( "columnar-config-version.cmake" COMPATIBILITY SameMinorVersion )
install ( FILES "${CMAKE_CURRENT_BINARY_DIR}/columnar-config-version.cmake" DESTINATION "${CMAKE_CMAKE_DIR}" COMPONENT dev)

include ( CPack )
include (testing.cmake)
