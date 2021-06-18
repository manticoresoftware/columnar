# Common rpm-specific build variables
cmake_minimum_required ( VERSION 3.17 )

SET ( CPACK_PACKAGING_INSTALL_PREFIX "/usr" )

set ( CPACK_GENERATOR "RPM" )
SET ( CPACK_RPM_FILE_NAME "RPM-DEFAULT" )

#set ( CPACK_RPM_PACKAGE_RELEASE 1 ) # that is 1 by default
set ( CPACK_RPM_PACKAGE_RELEASE_DIST ON ) # that adds 'el7', 'el8', etc.
set ( CPACK_RPM_PACKAGE_GROUP "Applications/Internet" )

SET ( CPACK_RPM_COMPONENT_INSTALL ON )
SET ( CPACK_RPM_MAIN_COMPONENT module )
set ( CPACK_RPM_MODULE_DEBUGINFO_PACKAGE ON )
SET ( CPACK_RPM_MODULE_PACKAGE_AUTOREQ ON )

#set ( CPACK_RPM_PACKAGE_SUGGESTS "manticore >= 3.5.5" ) # not supported in centos 7

set (CPACK_RPM_BUILD_SOURCE_DIRS_PREFIX OFF)

string ( LENGTH "${CMAKE_SOURCE_DIR}" source_dir_len_ )
if ( source_dir_len_ LESS 75 )
	message ( STATUS "set src prefix to /tmp/m due to too long path")
	set ( CPACK_RPM_BUILD_SOURCE_DIRS_PREFIX "/tmp")
endif ()

SET ( CPACK_RPM_PACKAGE_LICENSE "Apache-2.0" )
