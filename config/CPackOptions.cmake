# Custom CPack options

# Ensure the library doesn't need a Build ID - specifically for embeddings
set(CPACK_DEBIAN_EMBEDDINGS_PACKAGE_SHLIBDEPS OFF)

# Skip other dependency checks
set(CPACK_DEBIAN_PACKAGE_DEBUG OFF)

# Make sure the specified version matches the actual build
if(NOT "${CPACK_PACKAGE_VERSION}" STREQUAL "${CMAKE_PROJECT_VERSION}")
    message(WARNING "CPACK_PACKAGE_VERSION ${CPACK_PACKAGE_VERSION} differs from the project version ${CMAKE_PROJECT_VERSION}, using project version.")
    set(CPACK_PACKAGE_VERSION "${CMAKE_PROJECT_VERSION}")
endif()