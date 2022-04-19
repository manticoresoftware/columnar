# ---------- windows ----------
# Above line is mandatory!
# rules to build windows zip archive

message ( STATUS "Will create windows ZIP" )
SET ( CPACK_PACKAGING_INSTALL_PREFIX "/" )
set ( CPACK_ARCHIVE_COMPONENT_INSTALL ON )

if(TARGET columnar::columnar)
	install ( FILES $<TARGET_PDB_FILE:columnar> EXPORT ColumnarExport DESTINATION ${MODULES_DIR} COMPONENT dev OPTIONAL )
endif()
if(TARGET columnar::secondary)
	install ( FILES $<TARGET_PDB_FILE:secondary> EXPORT SecondaryExport DESTINATION ${MODULES_DIR} COMPONENT dev OPTIONAL )
endif()

set ( CPACK_GENERATOR "ZIP" )
set ( CPACK_SUFFIX "-x64" )