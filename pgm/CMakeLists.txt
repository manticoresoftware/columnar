cmake_minimum_required ( VERSION 3.17 )
project ( PiecewiseGeometricModelIndex
		VERSION 1.0
		HOMEPAGE_URL https://github.com/gvinciguerra/PGM-index )

set ( CMAKE_CXX_STANDARD 17 )

# PGM-index library
add_library ( pgmindexlib INTERFACE )
target_include_directories ( pgmindexlib INTERFACE $<BUILD_INTERFACE:${PiecewiseGeometricModelIndex_SOURCE_DIR}/include>$<INSTALL_INTERFACE:include> )

find_package ( OpenMP )
if (OpenMP_CXX_FOUND)
	message ( STATUS "OpenMP found" )
	target_link_libraries ( pgmindexlib INTERFACE OpenMP::OpenMP_CXX )
endif ()
target_compile_features ( pgmindexlib INTERFACE cxx_std_17 )

# installation stuff
set ( EXPORT_CMAKE_DIR "lib/cmake/PGM" )

install ( DIRECTORY "include/" DESTINATION include )
install ( TARGETS pgmindexlib EXPORT pgmexport )
install ( EXPORT pgmexport FILE "PGMTargets.cmake" DESTINATION "${EXPORT_CMAKE_DIR}" NAMESPACE "PGM::" )

# below is for support find_package(fastpfor)
include ( CMakePackageConfigHelpers )

set ( pkgconfin "${CMAKE_CURRENT_BINARY_DIR}/PGMConfig.cmake.in" )
file ( WRITE "${pkgconfin}" "@PACKAGE_INIT@

if(NOT TARGET PGM::pgmindexlib)
    include(\"\${CMAKE_CURRENT_LIST_DIR}/PGMTargets.cmake\")
endif()" )

configure_package_config_file ( "${pkgconfin}" "${CMAKE_CURRENT_BINARY_DIR}/PGMConfig.cmake" INSTALL_DESTINATION "${EXPORT_CMAKE_DIR}" )
install ( FILES "${CMAKE_CURRENT_BINARY_DIR}/PGMConfig.cmake" DESTINATION "${EXPORT_CMAKE_DIR}" )