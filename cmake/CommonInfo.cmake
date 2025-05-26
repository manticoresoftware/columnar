# Common informational variables for CPack

set ( CPACK_PACKAGE_VENDOR "Manticore Software LTD" )

set ( CMAKE_PROJECT_HOMEPAGE_URL "https://github.com/manticoresoftware/columnar/" )

set ( CPACK_PACKAGE_RELOCATABLE ON )

set ( CPACK_PACKAGE_CONTACT "Manticore Team <build@manticoresearch.com>" )

set ( CPACK_PACKAGE_URL "https://github.com/manticoresoftware/columnar/" )

set ( CPACK_PACKAGE_DESCRIPTION_SUMMARY "Manticore Columnar Library is a column-oriented storage library, aiming to provide decent performance with low memory footprint at big data volume" )

set ( CPACK_PACKAGE_DESCRIPTION "Manticore Columnar Library is a column-oriented storage library, aiming to provide decent performance with low memory footprint at big data volume. When used in combination with Manticore Search can be beneficial for faster / lower resource consumption log/metrics analytics and running log / metric analytics in docker / kubernetes" )

set ( CPACK_RPM_PACKAGE_DESCRIPTION "${CPACK_PACKAGE_DESCRIPTION}" ) # the description will default to the default one (This is an installer created using CPack..." otherwise, i.e. it doesn't take CPACK_PACKAGE_DESCRIPTION as a default

## Don't be confused; there is NO generic *_LICENSE variables in cmake/cpack, only for rpm and resource file
#set ( CPACK_PACKAGE_LICENSE "Apache-2.0" )
set ( CPACK_RPM_PACKAGE_LICENSE "Apache-2.0" )
#set ( CPACK_DEBIAN_PACKAGE_LICENSE "Apache-2.0" )
set ( CPACK_RESOURCE_FILE_LICENSE "${columnar_SOURCE_DIR}/LICENSE" )

if (DEFINED ENV{PACKAGE_VERSION} AND NOT "$ENV{PACKAGE_VERSION}" STREQUAL "")
    set ( CPACK_PACKAGE_VERSION "$ENV{PACKAGE_VERSION}" )
else()
    set ( CPACK_PACKAGE_VERSION "${PROJECT_VERSION}-${GIT_TIMESTAMP_ID}-${GIT_COMMIT_ID}" )
endif()
if (DEFINED ENV{RPM_PACKAGE_VERSION} AND NOT "$ENV{RPM_PACKAGE_VERSION}" STREQUAL "")
    set ( CPACK_RPM_PACKAGE_VERSION "$ENV{RPM_PACKAGE_VERSION}" )
else()
    set ( CPACK_RPM_PACKAGE_VERSION "${PROJECT_VERSION}_${GIT_TIMESTAMP_ID}.${GIT_COMMIT_ID}" )
endif()

set ( CPACK_PACKAGE_NAME "manticore-columnar-lib" )
set ( CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}" )
