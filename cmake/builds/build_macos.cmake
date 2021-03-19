# ---------- macos ----------
# Above line is mandatory!
# rules to build tgz archive for Mac OS X

message ( STATUS "Will create TGZ with build for Mac Os X" )

# configure specific stuff
set ( CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -arch x86_64" )
set ( CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -arch x86_64" )

set_target_properties ( columnar PROPERTIES OUTPUT_NAME _manticore_columnar VERSION "${VERSION_STR}" SOVERSION 1 )
install ( TARGETS columnar LIBRARY DESTINATION "lib" COMPONENT columnar ) # adds lib file and a chain of version symlinks to it

# package specific

find_program ( SWVERSPROG sw_vers )
if ( SWVERSPROG )
	# use dpkg to fix the package file name
	execute_process (
			COMMAND ${SWVERSPROG} -productVersion
			OUTPUT_VARIABLE MACOSVER
			OUTPUT_STRIP_TRAILING_WHITESPACE
	)
	mark_as_advanced ( SWVERSPROG MACOSVER )
endif ( SWVERSPROG )

if ( NOT MACOSVER )
	set ( MACOSVER "10.12" )
endif ()

set ( CPACK_GENERATOR "TGZ" )
LIST ( APPEND PKGSUFFIXES "osx${MACOSVER}" "x86_64" )

mark_as_advanced ( CMAKE_OSX_ARCHITECTURES CMAKE_OSX_DEPLOYMENT_TARGET CMAKE_OSX_SYSROOT )
