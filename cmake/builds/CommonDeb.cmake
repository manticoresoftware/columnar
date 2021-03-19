# only cmake since 3.13 supports packaging of debuginfo
cmake_minimum_required ( VERSION 3.13 )

# Common debian-specific build variables
set ( CPACK_GENERATOR "DEB" )
set ( CPACK_PACKAGING_INSTALL_PREFIX "/" )
set ( BINPREFIX "usr/" )
set ( CPACK_DEBIAN_FILE_NAME DEB-DEFAULT )
set ( CPACK_DEBIAN_PACKAGE_DEBUG ON)

set ( CPACK_DEBIAN_DEBUGINFO_PACKAGE ON )

find_program ( DPKG_PROGRAM dpkg )
if ( DPKG_PROGRAM )
	# use dpkg to fix the package file name
	execute_process (
			COMMAND ${DPKG_PROGRAM} --print-architecture
			OUTPUT_VARIABLE CPACK_DEBIAN_PACKAGE_ARCHITECTURE
			OUTPUT_STRIP_TRAILING_WHITESPACE
	)
	mark_as_advanced( DPKG_PROGRAM )
endif ( DPKG_PROGRAM )

if ( NOT CPACK_DEBIAN_PACKAGE_ARCHITECTURE )
	message ( WARNING "No arch for debian build found. Provide CPACK_PACKAGE_ARCHITECTURE var with the value" )
endif ()

set_target_properties ( columnar PROPERTIES OUTPUT_NAME _manticore_columnar VERSION "${VERSION_STR}" SOVERSION 1 )
install ( TARGETS columnar LIBRARY DESTINATION ${BINPREFIX}/lib/ COMPONENT columnar ) # adds lib file and a chain of version symlinks to it

# dependencies will be auto calculated. FIXME! M.b. point them directly?
set ( CPACK_DEBIAN_PACKAGE_RECOMMENDS "manticore (>= 3.5.5)" )
set ( CPACK_DEBIAN_PACKAGE_SHLIBDEPS "ON" )
set ( CPACK_DEBIAN_PACKAGE_SECTION "misc" )
set ( CPACK_DEBIAN_PACKAGE_PRIORITY "optional" )
set ( CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION "ON" )
