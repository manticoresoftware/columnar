# ---------- windows ----------
# Above line is mandatory!
# rules to build windows zip archive

message ( STATUS "Will create windows ZIP" )
SET ( CPACK_PACKAGING_INSTALL_PREFIX "/" )
set ( CPACK_ARCHIVE_COMPONENT_INSTALL ON )

install ( FILES $<TARGET_PDB_FILE:columnar> EXPORT ColumnarExport DESTINATION ${MODULES_DIR} COMPONENT dev OPTIONAL )

set ( CPACK_GENERATOR "ZIP" )
set ( CPACK_SUFFIX "-x64" )