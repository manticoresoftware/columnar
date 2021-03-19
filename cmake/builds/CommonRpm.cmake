# Common rpm-specific build variables
cmake_minimum_required ( VERSION 3.12 )

set ( BINPREFIX "usr/" )

set ( CPACK_GENERATOR "RPM" )
SET ( CPACK_RPM_FILE_NAME "RPM-DEFAULT" )
  
set ( CPACK_PACKAGING_INSTALL_PREFIX "/" )

set ( CPACK_RPM_PACKAGE_RELEASE 1 )
set ( CPACK_RPM_PACKAGE_RELEASE_DIST ON )
set ( CPACK_RPM_PACKAGE_URL "https://github.com/manticoresoftware/columnar/" )
set ( CPACK_RPM_PACKAGE_GROUP "Applications/Internet" )

set ( CPACK_RPM_PACKAGE_SUGGESTS "manticore >= 3.5.5" )

set (CPACK_RPM_BUILD_SOURCE_DIRS_PREFIX  OFF)

string ( LENGTH "${CMAKE_SOURCE_DIR}" source_dir_len_ )
if ( source_dir_len_ LESS 75 )
	message ( STATUS "set src prefix to /tmp/m due to too long path")
	set ( CPACK_RPM_BUILD_SOURCE_DIRS_PREFIX "/tmp")
endif ()

SET ( CPACK_RPM_PACKAGE_LICENSE "Apache-2.0" )

set_target_properties ( columnar PROPERTIES OUTPUT_NAME _manticore_columnar VERSION "${VERSION_STR}" SOVERSION 1 )
install ( TARGETS columnar LIBRARY DESTINATION ${BINPREFIX}/lib/ COMPONENT columnar ) # adds lib file and a chain of version symlinks to it
