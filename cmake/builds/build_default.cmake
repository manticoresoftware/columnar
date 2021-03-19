# ---------- default ----------
# Above line is mandatory!
# rules to build default zip archive
message ( STATUS "Will create default ZIP" )

set ( CPACK_GENERATOR "ZIP" )

set_target_properties ( columnar PROPERTIES OUTPUT_NAME _manticore_columnar VERSION "${VERSION_STR}" SOVERSION 1 )
install ( TARGETS columnar LIBRARY DESTINATION "lib" COMPONENT columnar ) # adds lib file and a chain of version symlinks to it
