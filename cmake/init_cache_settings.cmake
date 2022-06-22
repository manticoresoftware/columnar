if (__init_columnar_cache_settings_included)
	return ()
endif ()
set ( __init_columnar_cache_settings_included YES )

# bundle - contains sources (tarballs) of 3-rd party libs. If not provided, try path 'bundle' aside sources.
# if it is provided anyway (via cmake var, ir via env var) and NOT absolute - point it into binary (build) dir.
if (DEFINED ENV{LIBS_BUNDLE})
	set ( LIBS_BUNDLE "$ENV{LIBS_BUNDLE}" )
endif ()

if (NOT LIBS_BUNDLE)
	get_filename_component ( LIBS_BUNDLE "${columnar_SOURCE_DIR}/../bundle" ABSOLUTE )
endif ()

if (NOT IS_ABSOLUTE ${LIBS_BUNDLE})
	set ( LIBS_BUNDLE "${columnar_BINARY_DIR}/${LIBS_BUNDLE}" )
endif ()

SET ( LIBS_BUNDLE "${LIBS_BUNDLE}" CACHE PATH "Choose the path to the dir which contains downloaded sources for libs like re2, icu, stemmer, etc." FORCE )

# cacheb (means 'cache binary') - contains unpacked sources and builds of 3-rd party libs, alive between rebuilds.
# if not provided, set to folder 'cache' aside bundle. If not absolute, point it into binary (build) dir.
if (DEFINED ENV{CACHEB})
	set ( CACHEB "$ENV{CACHEB}" )
endif ()

if (NOT DEFINED CACHEB)
	get_filename_component ( CACHEB "${LIBS_BUNDLE}/../cache" ABSOLUTE )
endif ()

if (NOT IS_ABSOLUTE ${CACHEB})
	set ( CACHEB "${columnar_BINARY_DIR}/${CACHEB}" )
endif ()

if (DEFINED CACHEB)
	SET ( CACHEB "${CACHEB}" CACHE PATH "Cache dir where unpacked sources and builds found." )
endif ()
