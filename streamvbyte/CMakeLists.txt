cmake_minimum_required(VERSION 3.3)

set(CMAKE_MACOSX_RPATH OFF)
project(STREAMVBYTE VERSION "1.0.0")

set(STREAMVBYTE_LIB_VERSION "1.0.0" CACHE STRING "streamvbyte library version")
set(STREAMVBYTE_LIB_SOVERSION "1" CACHE STRING "streamvbyte library soversion")

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

include(CheckCCompilerFlag)

option(STREAMVBYTE_WERROR "Treat warnings as errors." OFF)
option(STREAMVBYTE_WALL "Emit all warnings during compilation." ON)

if(STREAMVBYTE_WERROR)
    if(MSVC)
        check_c_compiler_flag("/WX" WERROR_MSVC_SUPPORTED)
        if(WERROR_MSVC_SUPPORTED)
            add_compile_options(/WX)
        endif()
        # Wall + WX may throw errors from the corecrt headers. Workaround:
        if(STREAMVBYTE_WALL)
            check_c_compiler_flag("/W3" W3_MSVC_SUPPORTED)
            if(W3_MSVC_SUPPORTED)
                add_compile_options(/W3)
            endif()
            check_c_compiler_flag("/w34714" SUPPRESS_C34714_MSVC_SUPPORTED)
            if(SUPPRESS_C34714_MSVC_SUPPORTED)
                add_compile_options(/w34714)
            endif()
            check_c_compiler_flag("/wd5045" SUPPRESS_SPECTRE_MSVC_SUPPORTED)
            if(SUPPRESS_SPECTRE_MSVC_SUPPORTED)
                add_compile_options(/wd5045)
            endif()
            check_c_compiler_flag("/sdl" SDL_MSVC_SUPPORTED)
            if(SDL_MSVC_SUPPORTED)
                add_compile_options(/sdl)
            endif()
        endif()
    else()
        check_c_compiler_flag(-Werror WERROR_GNU_SUPPORTED)
        if(WERROR_GNU_SUPPORTED)
            add_compile_options(-Werror)
        endif()
    endif()
endif()

if(STREAMVBYTE_WALL AND NOT(STREAMVBYTE_WERROR))
    if(MSVC)
        check_c_compiler_flag("/Wall" WALL_MSVC_SUPPORTED)
        if(WALL_MSVC_SUPPORTED)
            add_compile_options(/Wall)
        endif()
    else()
        check_c_compiler_flag(-Wall WALL_GNU_SUPPORTED)
        if(WALL_GNU_SUPPORTED)
            add_compile_options(-Wall)
        endif()
    endif()
endif()

option(STREAMVBYTE_SANITIZE "Sanitize addresses" OFF)

if(NOT CMAKE_BUILD_TYPE)
    message(STATUS "No build type selected")
    if(STREAMVBYTE_SANITIZE)
        message(STATUS "Default to Debug")
        set(CMAKE_BUILD_TYPE Debug
            CACHE STRING "Choose the type of build."
            FORCE
        )
    else()
        message(STATUS "Default to Release")
        set(CMAKE_BUILD_TYPE Release
            CACHE STRING "Choose the type of build."
            FORCE
        )
    endif()
endif()

if(STREAMVBYTE_SANITIZE)
    message(STATUS "Enabling memory sanitizer.")
    add_compile_options(
        -fsanitize=address
        -fno-omit-frame-pointer
        -fno-sanitize-recover=all
    )
    add_compile_definitions(ASAN_OPTIONS=detect_leaks=1)
endif()

if(MSVC)
	add_definitions("-D__restrict__=__restrict")
    add_definitions("-Drestrict=__restrict")
endif()

# test for arm
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64.*|AARCH64.*)")
    set(BASE_FLAGS ${BASE_FLAGS} "-D__ARM_NEON__")
endif()

set(STREAMVBYTE_SRCS
    ${PROJECT_SOURCE_DIR}/src/streamvbyte_encode.c
    ${PROJECT_SOURCE_DIR}/src/streamvbyte_decode.c
    ${PROJECT_SOURCE_DIR}/src/streamvbyte_zigzag.c
    ${PROJECT_SOURCE_DIR}/src/streamvbytedelta_encode.c
    ${PROJECT_SOURCE_DIR}/src/streamvbytedelta_decode.c
    ${PROJECT_SOURCE_DIR}/src/streamvbyte_0124_encode.c
    ${PROJECT_SOURCE_DIR}/src/streamvbyte_0124_decode.c
)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_library(streamvbyte "${STREAMVBYTE_SRCS}")
target_link_libraries(streamvbyte ${BASE_FLAGS})

set_target_properties(
    streamvbyte
    PROPERTIES
        VERSION "${STREAMVBYTE_LIB_VERSION}"
        SOVERSION "${STREAMVBYTE_LIB_SOVERSION}"
        WINDOWS_EXPORT_ALL_SYMBOLS YES
)

# helpers vars to shorten generate lines
set ( GNUCXX "$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang,GNU>" )
set ( GNUC "$<COMPILE_LANG_AND_ID:C,Clang,AppleClang,GNU>" )
set ( GNUC_CXX "$<OR:${GNUCXX},${GNUC}>" )

string ( TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" SYSTEM_PROCESSOR_LOWER )
if (SYSTEM_PROCESSOR_LOWER STREQUAL x86_64 OR SYSTEM_PROCESSOR_LOWER STREQUAL amd64)
	message ( STATUS "Add sse 4.2 to compiler flags for ${CMAKE_SYSTEM_PROCESSOR} arch" )
	if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND MSVC)
		target_compile_options ( streamvbyte PUBLIC -msse4.2 -mpopcnt )
	else()
		target_compile_options ( streamvbyte PUBLIC $<${GNUC_CXX}:-msse4.2> )
	endif()
endif ()

target_include_directories ( streamvbyte PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>$<INSTALL_INTERFACE:include> )

option(STREAMVBYTE_SANITIZE_UNDEFINED "Sanitize undefined behavior" OFF)
if(STREAMVBYTE_SANITIZE_UNDEFINED)
    add_compile_options(-fsanitize=undefined -fno-sanitize-recover=all)
    add_link_options(-fsanitize=undefined -fno-sanitize-recover=all)
endif()

message(STATUS "CMAKE_SYSTEM_PROCESSOR: " ${CMAKE_SYSTEM_PROCESSOR})
message(STATUS "CMAKE_BUILD_TYPE: " ${CMAKE_BUILD_TYPE}) # this tends to be "sticky" so you can remain unknowingly in debug mode
message(STATUS "CMAKE_C_COMPILER: " ${CMAKE_C_COMPILER}) # important to know which compiler is used
message(STATUS "CMAKE_C_FLAGS: " ${CMAKE_C_FLAGS}) # important to know the flags
message(STATUS "CMAKE_C_FLAGS_DEBUG: " ${CMAKE_C_FLAGS_DEBUG})
message(STATUS "CMAKE_C_FLAGS_RELEASE: " ${CMAKE_C_FLAGS_RELEASE})

###

set ( EXPORT_CMAKE_DIR "lib/cmake/streamvbyte" )

install(
    FILES
        ${PROJECT_SOURCE_DIR}/include/streamvbyte.h
        ${PROJECT_SOURCE_DIR}/include/streamvbytedelta.h
        ${PROJECT_SOURCE_DIR}/include/streamvbyte_zigzag.h
    DESTINATION include
)

install ( TARGETS streamvbyte EXPORT streamvbyteexport ARCHIVE DESTINATION "lib/$<CONFIG>" )
install ( FILES "$<TARGET_FILE_DIR:streamvbyte>/streamvbyte.pdb" EXPORT streamvbyteexport DESTINATION "lib/$<CONFIG>" OPTIONAL )
install ( EXPORT streamvbyteexport FILE "streamvbyteTargets.cmake" DESTINATION "${EXPORT_CMAKE_DIR}" NAMESPACE "streamvbyte::" )

include ( CMakePackageConfigHelpers )

set ( pkgconfin "${CMAKE_CURRENT_BINARY_DIR}/streamvbyte.cmake.in" )
file ( WRITE "${pkgconfin}" "@PACKAGE_INIT@

if(NOT TARGET streamvbyte::streamvbyte)
    include(\"\${CMAKE_CURRENT_LIST_DIR}/streamvbyteTargets.cmake\")
endif()" )

configure_package_config_file ( "${pkgconfin}" "${CMAKE_CURRENT_BINARY_DIR}/streamvbyteConfig.cmake" INSTALL_DESTINATION "${EXPORT_CMAKE_DIR}" )
install ( FILES "${CMAKE_CURRENT_BINARY_DIR}/streamvbyteConfig.cmake" DESTINATION "${EXPORT_CMAKE_DIR}" )

write_basic_package_version_file ( "streamvbyteConfigVersion.cmake" COMPATIBILITY ExactVersion )
install ( FILES "${CMAKE_CURRENT_BINARY_DIR}/streamvbyteConfigVersion.cmake" DESTINATION "${EXPORT_CMAKE_DIR}" )