# This code is released under the
# Apache License Version 2.0 http://www.apache.org/licenses/.
#
# Copyright (c) 2012 Louis Dionne
# Copyright (c) 2021-2022 Manticore Software LTD
#
cmake_minimum_required ( VERSION 3.17 )

FOREACH (policy CMP0091 CMP0022)
	IF (POLICY ${policy})
		CMAKE_POLICY ( SET ${policy} NEW )
	ENDIF ()
ENDFOREACH ()

set ( _CMAKE_BUILD_TYPE ${CMAKE_BUILD_TYPE} )
project ( FastPFOR LANGUAGES CXX C VERSION 0.1.9 )

# sometimes CMAKE_BUILD_TYPE became set after PROJECT statement, undo it.
if (NOT _CMAKE_BUILD_TYPE AND CMAKE_BUILD_TYPE)
	unset ( CMAKE_BUILD_TYPE CACHE )
endif ()

# Set a default build type for single-configuration CMake generators if no build type is set.
get_property ( isMultiConfig GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG )
if (NOT isMultiConfig AND NOT CMAKE_BUILD_TYPE)
	set ( CMAKE_BUILD_TYPE Release )
	message ( STATUS "Automatically set build type to Release since no other provided" )
endif ()

if (NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
	MESSAGE ( FATAL_ERROR "Please use a 64-bit system. " )
endif ()

set ( FastPFOR_SRCS
		src/bitpacking.cpp
		src/bitpackingaligned.cpp
		src/bitpackingunaligned.cpp
		#    src/horizontalbitpacking.cpp
		src/simdunalignedbitpacking.cpp
		src/simdbitpacking.cpp
		src/varintdecode.c
		src/streamvbyte.c
		)

# helpers vars to shorten generate lines
set ( GNUCXX "$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang,GNU>" )
set ( GNUC "$<COMPILE_LANG_AND_ID:C,Clang,AppleClang,GNU>" )
set ( GNUC_CXX "$<OR:${GNUCXX},${GNUC}>" )

add_library ( FastPFOR STATIC ${FastPFOR_SRCS} )
target_include_directories ( FastPFOR PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/headers>$<INSTALL_INTERFACE:include/fastpfor> )
target_compile_options ( FastPFOR PUBLIC $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:-D_CRT_SECURE_NO_WARNINGS -wd4267 -wd4244> )
set_target_properties ( FastPFOR PROPERTIES POSITION_INDEPENDENT_CODE TRUE INTERFACE_POSITION_INDEPENDENT_CODE TRUE )
target_compile_features ( FastPFOR PUBLIC cxx_std_14 )
target_compile_features ( FastPFOR PUBLIC c_std_99 )

if (CMAKE_SYSTEM_PROCESSOR STREQUAL x86_64 OR CMAKE_SYSTEM_PROCESSOR STREQUAL amd64)
	message ( STATUS "Add sse 4.1 to compiler flags for ${CMAKE_SYSTEM_PROCESSOR} arch" )
	target_compile_options ( FastPFOR PUBLIC $<${GNUC_CXX}:-msse4.1> )
endif ()

if (${CMAKE_CXX_COMPILER_ID} STREQUAL "GNU")
	# require at least gcc 4.7
	if (CMAKE_CXX_COMPILER_VERSION VERSION_LESS 4.7)
		message ( FATAL_ERROR "GCC version must be at least 4.7" )
	endif ()
	# Uncomment the following lines to see how the code compiles without AVX,SSE4.2 and/or SSE2
	#set (CMAKE_CXX_FLAGS_RELEASE "-Wall -Ofast -lm  -DNDEBUG -std=c++11 -DHAVE_CXX0X -march=x86-64")
	#set (CMAKE_CXX_FLAGS_RELEASE "-Wall -Ofast -lm  -DNDEBUG -std=c++11 -DHAVE_CXX0X -march=core2")
	#set (CMAKE_CXX_FLAGS_RELEASE "-Wall -Ofast -lm  -DNDEBUG -std=c++11 -DHAVE_CXX0X -msse4.2")

	#set (CMAKE_CXX_FLAGS_RELEASE "-Wall -Wcast-align -Ofast -lm  -DNDEBUG -std=c++11 -DHAVE_CXX0X -msse41")
	#set (CMAKE_CXX_FLAGS_DEBUG   "-Wall -Wcast-align -ggdb  -lm  -std=c++11 -DHAVE_CXX0X -msse41")
	#set (CMAKE_C_FLAGS_RELEASE "-Wall -Wcast-align -Ofast -lm  -DNDEBUG -std=c99  -msse41")
	#set (CMAKE_C_FLAGS_DEBUG   "-Wall -Wcast-align -ggdb  -lm  -std=c99 -msse41")

elseif (${CMAKE_CXX_COMPILER_ID} STREQUAL "Intel")
	if (CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14.0.1)
		message ( FATAL_ERROR "Intel version must be at least 14.0.1!" )
	endif ()
	#set (CMAKE_CXX_FLAGS_RELEASE "-Wall -Ofast  -DNDEBUG -std=c++11 -DHAVE_CXX0X  -march=native")
	#set (CMAKE_CXX_FLAGS_DEBUG   "-Wall -ggdb   -std=c++11 -DHAVE_CXX0X  -march=native")
	#set (CMAKE_C_FLAGS_RELEASE "-Wall -Ofast  -DNDEBUG -std=c99  -march=native")
	#set (CMAKE_C_FLAGS_DEBUG   "-Wall -ggdb   -std=c99  -march=native")
elseif (${CMAKE_CXX_COMPILER_ID} STREQUAL "Clang")
	if (CMAKE_CXX_COMPILER_VERSION VERSION_LESS 4.2.1)
		message ( FATAL_ERROR "Clang version must be at least 4.2.1! (provided ${CMAKE_CXX_COMPILER_VERSION}" )
	endif ()
	#set (CMAKE_CXX_FLAGS_RELEASE "-Wall -Wcast-align -O3 -DNDEBUG -std=c++11 -DHAVE_CXX0X -msse4.1 -march=native")
	#set (CMAKE_CXX_FLAGS_DEBUG   "-Wall -Wcast-align -ggdb  -std=c++11 -DHAVE_CXX0X -msse4.1 -march=native")
	#set (CMAKE_C_FLAGS_RELEASE "-Wall -Wcast-align -O3 -DNDEBUG -std=c99 -msse4.1 -march=native")
	#set (CMAKE_C_FLAGS_DEBUG   "-Wall -Wcast-align -ggdb  -std=c99 -msse4.1 -march=native")

elseif (${CMAKE_CXX_COMPILER_ID} STREQUAL "AppleClang")
	# pass
elseif (WIN32)
	# pass. TODO check MSVC versions?
else ()
	message ( FATAL_ERROR "Please, use MSVC, GCC, Clang, or the Intel compiler!" )
endif ()

message ( STATUS "Compile with ${CMAKE_CXX_COMPILER_ID} ver ${CMAKE_CXX_COMPILER_VERSION} for arch ${CMAKE_SYSTEM_PROCESSOR}" )

if (CMAKE_SYSTEM_PROCESSOR STREQUAL aarch64 OR CMAKE_SYSTEM_PROCESSOR STREQUAL arm64)
	message ( STATUS "Add simde for arm" )

	# fetch sources (original tarball)
	include ( FetchContent )
	FetchContent_Declare (
			simde
			GIT_REPOSITORY "https://github.com/simd-everywhere/simde.git"
			GIT_TAG dd0b662fd8cf4b1617dbbb4d08aa053e512b08e4
	)
	FetchContent_GetProperties ( simde )
	if (NOT simde_POPULATED)
		message ( STATUS "Populate SIMDe from git" )
		FetchContent_Populate ( simde )
	endif ()

	add_library ( SIMDe INTERFACE )
	add_library ( FastPFOR::SIMDe ALIAS SIMDe )
	target_include_directories ( SIMDe INTERFACE $<BUILD_INTERFACE:${simde_SOURCE_DIR}>$<INSTALL_INTERFACE:include/simde> )
	target_compile_options ( SIMDe INTERFACE $<${GNUC_CXX}:-DUSE_SIMDE> )

	install ( DIRECTORY "${simde_SOURCE_DIR}/simde" DESTINATION include/simde )
	install ( TARGETS SIMDe EXPORT FastPFORExport )

	target_link_libraries ( FastPFOR PUBLIC FastPFOR::SIMDe )
endif ()

# installation stuff
set ( EXPORT_CMAKE_DIR "lib/cmake/FastPFOR" )

install ( DIRECTORY "headers/" DESTINATION include/fastpfor )
install ( TARGETS FastPFOR EXPORT FastPFORExport ARCHIVE DESTINATION "lib/$<CONFIG>" )
install ( FILES "$<TARGET_FILE_DIR:FastPFOR>/FastPFOR.pdb" EXPORT FastPFORExport DESTINATION "lib/$<CONFIG>" OPTIONAL )

install ( EXPORT FastPFORExport FILE "FastPFORTargets.cmake" DESTINATION "${EXPORT_CMAKE_DIR}" NAMESPACE "FastPFOR::" )

# below is for support find_package(fastpfor)
include ( CMakePackageConfigHelpers )

set ( pkgconfin "${CMAKE_CURRENT_BINARY_DIR}/FastPFORConfig.cmake.in" )
file ( WRITE "${pkgconfin}" "@PACKAGE_INIT@

if(NOT TARGET FastPFOR::FastPFOR)
    include(\"\${CMAKE_CURRENT_LIST_DIR}/FastPFORTargets.cmake\")
    get_target_property ( configs FastPFOR::FastPFOR IMPORTED_CONFIGURATIONS )
	foreach (config RELWITHDEBINFO MINSIZEREL)
		if (NOT \${config} IN_LIST configs)
			set_property ( TARGET FastPFOR::FastPFOR PROPERTY MAP_IMPORTED_CONFIG_\${config} Release )
		endif ()
	endforeach ()
endif()" )

configure_package_config_file ( "${pkgconfin}" "${CMAKE_CURRENT_BINARY_DIR}/FastPFORConfig.cmake" INSTALL_DESTINATION "${EXPORT_CMAKE_DIR}" )
install ( FILES "${CMAKE_CURRENT_BINARY_DIR}/FastPFORConfig.cmake" DESTINATION "${EXPORT_CMAKE_DIR}" )

write_basic_package_version_file ( "FastPFORConfigVersion.cmake" COMPATIBILITY ExactVersion )
install ( FILES "${CMAKE_CURRENT_BINARY_DIR}/FastPFORConfigVersion.cmake" DESTINATION "${EXPORT_CMAKE_DIR}" )
