if (__update_bundle_columnar_included)
	return ()
endif ()
set ( __update_bundle_columnar_included YES )

IF (POLICY CMP0135)
	CMAKE_POLICY ( SET CMP0135 NEW )
ENDIF ()

# env WRITEB (as bool) means that we can store downloaded stuff to our bundle (that's to refresh the bundle)
# env CACHEB may provide path to persistent folder where we will build heavy stuff (unpacked sources, builds)
include ( helpers )
diag ( DIAGNOSTIC )

if ( USE_AVX2 )
	set ( SUFF "${CMAKE_SYSTEM_NAME}-x86_64-v3" )
else()
	set ( SUFF "${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}" )
endif()
string ( TOLOWER "${SUFF}" SUFF )
diag (SUFF)

# SUFF is line like 'darwin-x86_64' (system-arch)

# special cache folder where artefacts keep. Make it absolute also
if (DEFINED CACHEB)
	if (NOT EXISTS ${CACHEB})
		get_filename_component ( REL_BBUILD "${CACHEB}" REALPATH BASE_DIR "${columnar_BINARY_DIR}" )
		file ( MAKE_DIRECTORY ${REL_BBUILD} )
	endif ()
	#	get_filename_component(CACHEB "${CACHEB}" ABSOLUTE)
	diag ( CACHEB )
	set ( HAVE_BBUILD TRUE )
endif ()

# HAVE_BBUILD means we will build in aside folder (inside CACHEB) and then store the result for future.

# make libs_bundle absolute, if any
if (DEFINED LIBS_BUNDLE)
	get_filename_component ( LIBS_BUNDLE "${LIBS_BUNDLE}" ABSOLUTE )
endif ()

unset ( WRITEB )
set ( WRITEB "$ENV{WRITEB}" )
if (WRITEB)
	message ( STATUS "========================================================" )
	message ( STATUS "WRITEB is set, will modify bundle, will collect stuff..." )
	message ( STATUS "${LIBS_BUNDLE}" )
	message ( STATUS "========================================================" )
	file ( MAKE_DIRECTORY ${LIBS_BUNDLE} )
else ()
	message ( STATUS "WRITEB is not set, bundle will NOT be modified..." )
endif ()

diag ( WRITEB )
diag ( LIBS_BUNDLE )
diag ( CACHEB )
diag ( HAVE_BBUILD )

if (HAVE_BBUILD)
	set ( CACHE_BUILDS "${CACHEB}/${SUFF}" )
else ()
	set ( CACHE_BUILDS "${columnar_BINARY_DIR}/cache" )
endif ()

# that is once populate cache to cmake prefix path
append_prefix ( "${CACHE_BUILDS}" )

# get path for build folder. In case with HAVE_BBUILD it will be suffixed with /arch/name flag.
function ( GET_BUILD RESULT NAME )
	if (NOT HAVE_BBUILD)
		set ( detail "local " )
	endif ()
	diags ( "${NAME} build will be set to ${detail}${CACHE_BUILDS}/${NAME}" )
	set ( ${RESULT} "${CACHE_BUILDS}/${NAME}" PARENT_SCOPE )
endfunction ()

# set PLACE to external url or to path in bundle.
# if WRITEB is active, download external url into bundle
function ( select_nearest_url PLACE NAME BUNDLE_URL REMOTE_URL )
	if (NOT EXISTS "${BUNDLE_URL}" AND WRITEB)
		diags ( "fetch ${REMOTE_URL} into ${BUNDLE_URL}..." )
		file ( DOWNLOAD ${REMOTE_URL} ${BUNDLE_URL} SHOW_PROGRESS )
		message ( STATUS "Absent ${NAME} put into ${BUNDLE_URL}" )
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

function ( fetch_sources NAME URL OUTDIR )
	include ( FetchContent )
	FetchContent_Declare ( ${NAME} URL "${URL}" )
	FetchContent_GetProperties ( ${NAME} )
	if (NOT ${NAME}_POPULATED)
		message ( STATUS "Populate ${NAME} from ${URL}" )
		FetchContent_Populate ( ${NAME} )
	endif ()

	string ( TOUPPER "${NAME}" UNAME )
	mark_as_advanced ( FETCHCONTENT_SOURCE_DIR_${UNAME} FETCHCONTENT_UPDATES_DISCONNECTED_${UNAME} )
	set ( ${OUTDIR} "${${NAME}_SOURCE_DIR}" PARENT_SCOPE )
endfunction ()

function ( is_amd64 RESULT )
	string ( TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" SYSTEM_PROCESSOR_LOWER )
	if (SYSTEM_PROCESSOR_LOWER STREQUAL x86_64 OR SYSTEM_PROCESSOR_LOWER STREQUAL amd64)
		set (${RESULT} TRUE PARENT_SCOPE)
	endif ()
endfunction ()

function ( get_avx_flags RESULT )
	is_amd64 (AMD)
	if (NOT AMD)
		return()
	endif()

	if (USE_AVX2)
		message ( STATUS "Add AVX2 flags to compiler flags for ${CMAKE_SYSTEM_PROCESSOR} arch" )
		if (MSVC OR CLANG_CL)
			set ( ${RESULT} "/arch:AVX2" PARENT_SCOPE )
		else()
			set ( ${RESULT} "-march=x86-64-v3" PARENT_SCOPE )
		endif()
	else ()
		message ( STATUS "Add SSE flags to compiler flags for ${CMAKE_SYSTEM_PROCESSOR} arch" )
		if (MSVC OR CLANG_CL)
			set ( ${RESULT} "/arch:AVX" PARENT_SCOPE )
		else ()
			set ( ${RESULT} "-march=x86-64-v2" PARENT_SCOPE )
		endif ()
	endif ()
endfunction ( )

function ( external_build module MODULE_SRC_NAME MODULE_BUILD_NAME )
	get_avx_flags ( FLAGS )
	if (FLAGS)
		set ( ENV{CXXFLAGS} ${FLAGS} )
		set ( ENV{CFLAGS} ${FLAGS} )
	endif ()

	set ( CMAKE_ARGS "" )
	set ( MODULE_SRC "${${MODULE_SRC_NAME}}" )
	set ( MODULE_BUILD "${${MODULE_BUILD_NAME}}" )
	configure_file ( ${columnar_SOURCE_DIR}/cmake/external-build.cmake.in ${module}-build/CMakeLists.txt @ONLY )
	execute_process ( COMMAND ${CMAKE_COMMAND} -G "${CMAKE_GENERATOR}" . WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/${module}-build )
	execute_process ( COMMAND ${CMAKE_COMMAND} --build . WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/${module}-build )
endfunction ()
