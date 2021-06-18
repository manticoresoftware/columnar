if (__helpers_fastpfor_included)
	return ()
endif ()
set ( __helpers_fastpfor_included YES )

#diagnostic helpers
if (DEFINED ENV{DIAGNOSTIC})
	set ( DIAGNOSTIC "$ENV{DIAGNOSTIC}" )
endif ()

include ( CMakePrintHelpers )
function ( DIAG )
	if (DIAGNOSTIC)
		cmake_print_variables ( ${ARGN} )
	endif ()
endfunction ()
diag ( DIAGNOSTIC )

function ( DIAGS MSG )
	if (DIAGNOSTIC)
		message ( STATUS "${MSG}" )
	endif ()
endfunction ()

function ( infomsg MSG )
	if (NOT CMAKE_REQUIRED_QUIET)
		message ( STATUS "${MSG}" )
	endif ()
endfunction ()

# bundle - contains sources (tarballs) of 3-rd party libs. If not provided, try path 'bundle' aside sources.
# if it is provided anyway (via cmake var, ir via env var) and NOT absolute - point it into binary (build) dir.
if (DEFINED ENV{LIBS_BUNDLE})
	set ( LIBS_BUNDLE "$ENV{LIBS_BUNDLE}" )
endif ()

if (NOT LIBS_BUNDLE)
	get_filename_component ( LIBS_BUNDLE "${CMAKE_SOURCE_DIR}/../bundle" ABSOLUTE )
endif ()

if (NOT IS_ABSOLUTE ${LIBS_BUNDLE})
	get_filename_component ( LIBS_BUNDLE "${CMAKE_BINARY_DIR}/${LIBS_BUNDLE}" ABSOLUTE )
	set ( ENV{LIBS_BUNDLE} "${LIBS_BUNDLE}" )
endif ()

SET ( LIBS_BUNDLE "${LIBS_BUNDLE}" CACHE PATH "Choose the path to the dir which contains all helper libs like expat, mysql, etc." )

# cacheb (means 'cache binary') - contains unpacked sources and builds of 3-rd party libs, alive between rebuilds.
# if not provided, set to folder 'cache' aside bundle. If not absolute, point it into binary (build) dir.
if (DEFINED ENV{CACHEB})
	set ( CACHEB "$ENV{CACHEB}" )
endif ()

if (NOT DEFINED CACHEB)
	get_filename_component ( CACHEB "${LIBS_BUNDLE}/../cache" ABSOLUTE )
endif ()

if (NOT IS_ABSOLUTE ${CACHEB})
	set ( CACHEB "${CMAKE_BINARY_DIR}/${CACHEB}" )
endif ()

# HAVE_BBUILD means we will build in aside folder (inside CACHEB) and then store the result for future.
if (DEFINED CACHEB)
	SET ( CACHEB "${CACHEB}" CACHE PATH "Cache dir where unpacked sources and builds found." )
	if (NOT EXISTS ${CACHEB})
		get_filename_component ( REL_BBUILD "${CACHEB}" REALPATH BASE_DIR "${CMAKE_BINARY_DIR}" )
		file ( MAKE_DIRECTORY ${REL_BBUILD} )
	endif ()
	diag ( CACHEB )
	set ( ENV{CACHEB} "${CACHEB}" )
	set ( HAVE_BBUILD TRUE )
endif ()

# SUFF is line like 'darwin-x86_64' (system-arch)
SET ( SUFF "${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}" )
string ( TOLOWER "${SUFF}" SUFF )

diag ( CMAKE_SOURCE_DIR CMAKE_CURRENT_SOURCE_DIR )
diag ( CMAKE_BINARY_DIR CMAKE_CURRENT_BINARY_DIR )

# env WRITEB (as bool) means that we can store downloaded stuff to our bundle (that's to refresh the bundle)
if (DEFINED ENV{WRITEB})
	set (WRITEB $ENV{WRITEB})
endif()

if (WRITEB)
	infomsg ( "========================================================" )
	infomsg ( "env WRITEB is set, will modify bundle, will collect stuff..." )
	infomsg ( "${LIBS_BUNDLE}" )
	infomsg ( "========================================================" )
	file ( MAKE_DIRECTORY ${LIBS_BUNDLE} )
else ()
	infomsg ( "WRITEB is not set, bundle will NOT be modified..." )
endif ()

diag ( LIBS_BUNDLE )
diag ( CACHEB )
diag ( HAVE_BBUILD )

# get path for build folder. In case with HAVE_BBUILD it will be suffixed with arch/name flag.
function ( GET_BUILD RESULT NAME )
	if (HAVE_BBUILD)
		diags ( "${NAME} build will be set to ${CACHEB}/${SUFF}/${NAME}" )
		set ( ${RESULT} "${CACHEB}/${SUFF}/${NAME}" PARENT_SCOPE )
	else ()
		diags ( "${NAME} build will be set to local ${columnar_BINARY_DIR}/${NAME}" )
		set ( ${RESULT} "${columnar_BINARY_DIR}/${NAME}" PARENT_SCOPE )
	endif ()
endfunction ()

# get path for platform-specific cache
function ( GET_CACHE RESULT )
	if (HAVE_BBUILD)
		set ( ${RESULT} "${CACHEB}/${SUFF}" PARENT_SCOPE )
	else ()
		set ( ${RESULT} "${columnar_BINARY_DIR}/cache" PARENT_SCOPE )
	endif ()
endfunction ()

# set PLACE to external url or to path in bundle.
# if WRITEB is active, download external url into bundle
function ( SELECT_NEAREST_URL PLACE NAME BUNDLE_URL REMOTE_URL )
	diag ( BUNDLE_URL )
	diag ( REMOTE_URL )
	if (NOT EXISTS "${BUNDLE_URL}" AND WRITEB)
		diags ( "fetch ${REMOTE_URL} into ${BUNDLE_URL}..." )
		file ( DOWNLOAD ${REMOTE_URL} ${BUNDLE_URL} SHOW_PROGRESS )
		infomsg ( "Absent ${NAME} put into ${BUNDLE_URL}" )
	endif ()

	if (EXISTS "${BUNDLE_URL}")
		set ( ${PLACE} "${BUNDLE_URL}" PARENT_SCOPE )
	else ()
		set ( ${PLACE} "${REMOTE_URL}" PARENT_SCOPE )
	endif ()

	diag ( NAME )
	diag ( BUNDLE_URL )
	diag ( REMOTE_URL )
endfunction ()
