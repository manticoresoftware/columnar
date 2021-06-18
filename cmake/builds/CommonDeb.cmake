# only cmake since 3.13 supports packaging of debuginfo
cmake_minimum_required ( VERSION 3.17 )

SET ( CPACK_PACKAGING_INSTALL_PREFIX "/usr" )

# Common debian-specific build variables
set ( CPACK_GENERATOR DEB )
set ( CPACK_DEBIAN_FILE_NAME DEB-DEFAULT )
set ( CPACK_DEB_COMPONENT_INSTALL ON )
set ( CPACK_DEBIAN_MODULE_DEBUGINFO_PACKAGE ON )

set ( CPACK_DEBIAN_PACKAGE_PRIORITY optional )

set ( CPACK_DEBIAN_DEV_PACKAGE_ARCHITECTURE all)
set ( CPACK_DEBIAN_DEV_PACKAGE_RECOMMENDS manticore-columnar-lib )
set ( CPACK_DEBIAN_DEV_PACKAGE_SECTION devel )
set ( CPACK_DEBIAN_DEV_DESCRIPTION "This package includes headers and cmake project for manticore columnar library" )

# dependencies will be auto calculated. FIXME! M.b. point them directly?
set ( CPACK_DEBIAN_MODULE_PACKAGE_NAME ${CPACK_PACKAGE_NAME} )
set ( CPACK_DEBIAN_MODULE_PACKAGE_RECOMMENDS "manticore (>= 3.6.1)" )
set ( CPACK_DEBIAN_MODULE_PACKAGE_SHLIBDEPS ON )
set ( CPACK_DEBIAN_MODULE_PACKAGE_SECTION misc )
set ( CPACK_DEBIAN_MODULE_PACKAGE_CONTROL_STRICT_PERMISSION ON )

# just output tons of diagnostic info at the end of generation
set ( CPACK_DEBIAN_PACKAGE_DEBUG ON )