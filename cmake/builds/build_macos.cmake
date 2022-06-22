# ---------- macos ----------
# Above line is mandatory!
# rules to build tgz archive for Mac OS X

message ( STATUS "Will create TGZ with build for Mac Os X" )

set ( CPACK_PACKAGING_INSTALL_PREFIX /usr/local )
set ( CPACK_ARCHIVE_COMPONENT_INSTALL OFF )
set ( CPACK_GENERATOR "TGZ" )
set ( CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_FILE_NAME}-osx${CMAKE_OSX_DEPLOYMENT_TARGET}-${CMAKE_SYSTEM_PROCESSOR}" )