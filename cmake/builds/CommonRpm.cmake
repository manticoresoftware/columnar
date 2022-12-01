# Common rpm-specific build variables
cmake_minimum_required ( VERSION 3.17 )

set ( CPACK_PACKAGING_INSTALL_PREFIX "/usr" )
set ( CPACK_GENERATOR "RPM" )

## RPM commons
set ( CPACK_RPM_FILE_NAME RPM-DEFAULT )
if (RELEASE_DIST)
	set ( CPACK_RPM_PACKAGE_RELEASE 1${RELEASE_DIST} )
	set ( MYVER "${CPACK_RPM_PACKAGE_VERSION}-${CPACK_RPM_PACKAGE_RELEASE}" )
else ()
	set ( CPACK_RPM_PACKAGE_RELEASE 1 )
	set ( CPACK_RPM_PACKAGE_RELEASE_DIST ON ) # that adds 'el7', 'el8', etc.
	set ( MYVER "${CPACK_RPM_PACKAGE_VERSION}-${CPACK_RPM_PACKAGE_RELEASE}%{?dist}" )
endif ()

set ( CPACK_RPM_PACKAGE_GROUP "Applications/Internet" )
set ( CPACK_RPM_PACKAGE_AUTOREQ ON )
set ( CPACK_RPM_PACKAGE_ARCHITECTURE ${CMAKE_SYSTEM_PROCESSOR} )

set ( CPACK_RPM_COMPONENT_INSTALL OFF )
set ( CPACK_RPM_INSTALL_WITH_EXEC ON )
set ( CPACK_RPM_DEBUGINFO_PACKAGE ON )
#set ( CPACK_RPM_BUILD_SOURCE_DIRS_PREFIX "/tmp" )

# uncomment this line to produce long (really long) verbose output of rpm building
#set ( CPACK_RPM_PACKAGE_DEBUG ON )