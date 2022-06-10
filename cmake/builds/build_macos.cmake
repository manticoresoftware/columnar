# ---------- macos ----------
# Above line is mandatory!
# rules to build tgz archive for Mac OS X

message ( STATUS "Will create TGZ with build for Mac Os X" )

SET ( CPACK_PACKAGING_INSTALL_PREFIX "/" )
set ( CPACK_ARCHIVE_COMPONENT_INSTALL ON )
set ( CPACK_GENERATOR "TGZ" )
set ( CPACK_SUFFIX "-osx${CMAKE_OSX_DEPLOYMENT_TARGET}-${CMAKE_SYSTEM_PROCESSOR}" )
