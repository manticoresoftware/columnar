# Copyright (c) 2020-2025, Manticore Software LTD (https://manticoresearch.com)
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

foreach (policy CMP0048 CMP0090 CMP0092 CMP0117)
	if (POLICY ${policy})
		cmake_policy ( SET ${policy} NEW )
	endif ()
endforeach ()

set ( CMAKE_INTERPROCEDURAL_OPTIMIZATION $ENV{CMAKE_INTERPROCEDURAL_OPTIMIZATION} )
set ( _CMAKE_BUILD_TYPE ${CMAKE_BUILD_TYPE} )

# The below version is used for versioning of the package and is replaced by the version from the pack workflow
project ( columnar VERSION 0.0.0 )

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
	set ( SKIP_KNN ON )
endif()

if (NOT SKIP_COLUMNAR)
	set ( INSTALL_COLUMNAR ON )
endif ()

if (NOT SKIP_SECONDARY)
	set ( INSTALL_SECONDARY ON )
endif ()

if (NOT SKIP_KNN)
	set ( INSTALL_KNN ON )
endif ()

set ( CMAKE_MODULE_PATH "${columnar_SOURCE_DIR}/cmake" )

# Version
include ( rev )

# main interface target to export/expose headers
# Note, first we provide headers which will be available even if build of the main lib itself will not succeed
# (say, on aarch64 because of absent fastpfor). But as api target is already exposed, consumers still can use it.
add_library ( columnar_api INTERFACE )
add_library ( secondary_api INTERFACE )
add_library ( knn_api INTERFACE )
target_link_libraries ( secondary_api INTERFACE columnar_api knn_api ) # actually we need only common headers, so it can be changed in future

# that will be used when build for dev/testing
add_library ( columnar::columnar_api ALIAS columnar_api )
add_library ( columnar::secondary_api ALIAS secondary_api )
add_library ( columnar::knn_api ALIAS knn_api )

# interface property, so that consumer will fire an error on configure time if API version is not match
file ( STRINGS columnar/columnar.h _verlist LIMIT_COUNT 2 REGEX "LIB_VERSION" )
string ( REGEX MATCH "[0-9]+" API_VER "${_verlist}" )
set_property ( TARGET columnar_api PROPERTY INTERFACE_COLUMNAR_API_VERSION ${API_VER} ) # must be reconfigured if version in columnar.h changes
set_property ( TARGET columnar_api APPEND PROPERTY COMPATIBLE_INTERFACE_STRING COLUMNAR_API_VERSION )

file ( STRINGS secondary/secondary.h _verlist LIMIT_COUNT 2 REGEX "LIB_VERSION" )
string ( REGEX MATCH "[0-9]+" SI_API_VER "${_verlist}" )
set_property ( TARGET secondary_api APPEND PROPERTY INTERFACE_SECONDARY_API_VERSION ${SI_API_VER} ) # must be reconfigured if version in secondary.h changes
set_property ( TARGET secondary_api APPEND PROPERTY COMPATIBLE_INTERFACE_STRING SECONDARY_API_VERSION )

file ( STRINGS knn/knn.h _verlist LIMIT_COUNT 2 REGEX "LIB_VERSION" )
string ( REGEX MATCH "[0-9]+" KNN_API_VER "${_verlist}" )
set_property ( TARGET knn_api APPEND PROPERTY INTERFACE_KNN_API_VERSION ${KNN_API_VER} ) # must be reconfigured if version in knn.h changes
set_property ( TARGET knn_api APPEND PROPERTY COMPATIBLE_INTERFACE_STRING KNN_API_VERSION )

set (_includes include/manticore-columnar-api)
target_include_directories ( columnar_api INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>$<INSTALL_INTERFACE:${_includes}> )

# install columnar and secondary API (only headers, no packaging, don't build anything at all)
if (API_ONLY)
	install ( TARGETS columnar_api EXPORT apiexport )
	install ( TARGETS secondary_api EXPORT apiexport )
	install ( TARGETS knn_api EXPORT apiexport )

	# ensure ALL externally required headers are exported here
	install ( FILES columnar/builder.h columnar/columnar.h DESTINATION ${_includes}/columnar )
	install ( FILES util/util.h DESTINATION ${_includes}/util )
	install ( FILES common/schema.h common/blockiterator.h common/filter.h DESTINATION ${_includes}/common )

	# ensure ALL externally required headers of secondary are exported here
	install ( FILES secondary/secondary.h secondary/builder.h secondary/iterator.h DESTINATION ${_includes}/secondary )

	# ensure ALL externally required headers of knn are exported here
	install ( FILES knn/knn.h DESTINATION ${_includes}/knn )

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

	write_basic_package_version_file ( "columnar-config-version.cmake" VERSION "${API_VER}.${SI_API_VER}.${KNN_API_VER}" COMPATIBILITY ExactVersion ARCH_INDEPENDENT )
	install ( FILES "${CMAKE_CURRENT_BINARY_DIR}/columnar-config-version.cmake" DESTINATION "${API_CMAKE_DIR}" )

	# finish configuration, as it is API_ONLY
	return()
endif ()

# here we came in case of full build (i.e. columnar and/or secondary and/or knn)

set ( CMAKE_CXX_STANDARD 17 )

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

include ( GetStreamvbyte )

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
	set ( MODULES_DIR "${CMAKE_INSTALL_DATADIR}/modules" )
else()
	set ( MODULES_DIR "${CMAKE_INSTALL_DATADIR}/manticore/modules" )
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

if (INSTALL_KNN)
	add_subdirectory ( knn )
	install ( TARGETS knn_lib RUNTIME DESTINATION ${MODULES_DIR} LIBRARY DESTINATION ${MODULES_DIR} COMPONENT knn )
	install ( FILES "$<TARGET_FILE_DIR:knn_lib>/lib_manticore_knn.pdb" DESTINATION ${MODULES_DIR} COMPONENT dbgsymbols OPTIONAL )
	
	# Add embeddings library as a component to be included in packages

	# Define platform-specific embeddings library names
	if(WIN32)
		set(EMBEDDINGS_LIB_NAME "manticore_knn_embeddings.dll")
	elseif(APPLE)
		set(EMBEDDINGS_LIB_NAME "libmanticore_knn_embeddings.dylib")
	else()
		set(EMBEDDINGS_LIB_NAME "libmanticore_knn_embeddings.so")
	endif()

	# Look for embeddings library in various places
	set(EMBEDDINGS_SEARCH_PATHS
		"${CMAKE_BINARY_DIR}/${EMBEDDINGS_LIB_NAME}"
		"${CMAKE_BINARY_DIR}/embeddings/${EMBEDDINGS_LIB_NAME}"
		"${CMAKE_SOURCE_DIR}/embeddings/target/release/${EMBEDDINGS_LIB_NAME}"
		"${CMAKE_SOURCE_DIR}/build/${EMBEDDINGS_LIB_NAME}"
		"${CMAKE_CURRENT_BINARY_DIR}/${EMBEDDINGS_LIB_NAME}"
		"${CMAKE_CURRENT_SOURCE_DIR}/${EMBEDDINGS_LIB_NAME}"
	)

	foreach(SEARCH_PATH ${EMBEDDINGS_SEARCH_PATHS})
		if(EXISTS ${SEARCH_PATH})
			message(STATUS "Found embeddings library at: ${SEARCH_PATH}")
			
			# Special handling for ARM64 Linux builds to avoid Build ID issues
			if(UNIX AND NOT APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
				message(STATUS "ARM64 detected - using alternative installation method")
				
				# Copy to build directory with a different name to avoid CPack scanning
				set(PROCESSED_LIB_PATH "${CMAKE_BINARY_DIR}/embeddings_lib.so")
				file(COPY ${SEARCH_PATH} DESTINATION ${CMAKE_BINARY_DIR})
				file(RENAME "${CMAKE_BINARY_DIR}/${EMBEDDINGS_LIB_NAME}" "${PROCESSED_LIB_PATH}")
				
				# Install as a regular file, not a library component
				install(FILES ${PROCESSED_LIB_PATH}
					DESTINATION ${MODULES_DIR}
					RENAME ${EMBEDDINGS_LIB_NAME}
					COMPONENT embeddings
					PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
					
				message(STATUS "ARM64 embeddings library will be installed via file copy")
			else()
				# Regular installation for x86_64 and other platforms
				# For Linux systems, try to strip the library to add proper Build ID
				if(UNIX AND NOT APPLE)
					message(STATUS "Preparing embeddings library for Linux packaging")
					
					# Copy to a known location in build directory
					set(PROCESSED_LIB_PATH "${CMAKE_BINARY_DIR}/${EMBEDDINGS_LIB_NAME}")
					file(COPY ${SEARCH_PATH} DESTINATION ${CMAKE_BINARY_DIR})
					
					# Try to add Build ID using different methods
					execute_process(
						COMMAND file ${PROCESSED_LIB_PATH}
						OUTPUT_VARIABLE FILE_OUTPUT
						ERROR_QUIET
					)
					
					message(STATUS "Library file info: ${FILE_OUTPUT}")
					
					# Method 1: Try strip with build-id preservation
					execute_process(
						COMMAND strip --strip-unneeded --preserve-dates ${PROCESSED_LIB_PATH}
						RESULT_VARIABLE STRIP_RESULT
						OUTPUT_QUIET
						ERROR_QUIET
					)
					
					if(STRIP_RESULT EQUAL 0)
						message(STATUS "Successfully processed with strip")
					else()
						# Method 2: Try objcopy to add build-id
						execute_process(
							COMMAND objcopy --add-gnu-debuglink=/dev/null ${PROCESSED_LIB_PATH}
							RESULT_VARIABLE OBJCOPY_RESULT
							OUTPUT_QUIET
							ERROR_QUIET
						)
						
						if(OBJCOPY_RESULT EQUAL 0)
							message(STATUS "Successfully processed with objcopy")
						else()
							# Method 3: Create a symbolic link approach
							message(STATUS "Using original library without modification")
						endif()
					endif()
					
					# Ensure proper permissions
					execute_process(
						COMMAND chmod 644 ${PROCESSED_LIB_PATH}
						RESULT_VARIABLE CHMOD_RESULT
					)
					
					if(EXISTS ${PROCESSED_LIB_PATH})
						set(SEARCH_PATH ${PROCESSED_LIB_PATH})
						message(STATUS "Using processed embeddings library for installation")
					endif()
				endif()

				install(FILES ${SEARCH_PATH} DESTINATION ${MODULES_DIR} COMPONENT embeddings)
			endif()
			
			break()
		endif()
	endforeach()
endif ()

# Add component dependency - KNN depends on embeddings
set(CPACK_COMPONENT_KNN_DEPENDS embeddings)

# Include custom DEB packaging settings before CPack
if(UNIX AND NOT APPLE)
    include(cmake/cpack/DEB.cmake)
endif()

include ( CPack )
include ( testing.cmake )
