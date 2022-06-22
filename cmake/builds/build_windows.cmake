# ---------- windows ----------
# Above line is mandatory!
# rules to build windows zip archive

message ( STATUS "Will create windows ZIP" )

set ( CPACK_NSIS_INSTALL_ROOT "c:" )
set ( CPACK_NSIS_DISPLAY_NAME "Manticore columnar" )
set ( CPACK_NSIS_PACKAGE_NAME "Manticore libraries" )
set ( CPACK_PACKAGE_INSTALL_DIRECTORY manticore )
set ( CPACK_ARCHIVE_COMPONENT_INSTALL ON )

set ( CPACK_GENERATOR "ZIP;NSIS" )
set ( CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_FILE_NAME}-x64" )

set ( CPACK_COMPONENT_COLUMNAR_GROUP libs )
set ( CPACK_COMPONENT_SECONDARY_GROUP libs )

set ( CPACK_COMPONENT_GROUP_LIBS_DISPLAY_NAME "Manticore modules" )
set ( CPACK_COMPONENT_COLUMNAR_DISPLAY_NAME "Columnar storage library")
set ( CPACK_COMPONENT_COLUMNAR_DESCRIPTION "${CPACK_PACKAGE_DESCRIPTION_SUMMARY}" )
set ( CPACK_COMPONENT_SECONDARY_DISPLAY_NAME "Secondary index library" )
set ( CPACK_COMPONENT_SECONDARY_DESCRIPTION "Secondary index" )

set ( CPACK_COMPONENT_DBGSYMBOLS_DISPLAY_NAME "Debug symbols" )
set ( CPACK_COMPONENT_DBGSYMBOLS_DISABLED ON )
set ( CPACK_COMPONENT_DBGSYMBOLS_DOWNLOADED ON )

# base where installer will download the archives
set ( DISTR_URL "https://repo.manticoresearch.com/repository/manticoresearch_windows" )

if (DEV_BUILD)
	set ( CPACK_DOWNLOAD_SITE "${DISTR_URL}/dev/x64/" )
else ()
	set ( CPACK_DOWNLOAD_SITE "${DISTR_URL}/release/x64/" )
endif ()

set ( CPACK_NSIS_MODIFY_PATH OFF )
set ( CPACK_NSIS_UNINSTALL_NAME Uninstall-columnar )

# HKLM/SOFTWARE/Wow6432Node/Manticore Software LTD/manticore