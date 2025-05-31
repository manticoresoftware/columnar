# Custom DEB packaging settings

# Disable shlib dependencies feature to avoid build ID issues
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF)

# Add predefined dependencies that we know are needed
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libc6 (>= 2.17), libstdc++6 (>= 6.0)")

# Disable the need for Build IDs in the shared libraries included in the package
set(CMAKE_SKIP_RPATH ON)

# Check if we're building for ARM64/aarch64 and set additional options
string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" SYSTEM_PROCESSOR_LOWER)
if(SYSTEM_PROCESSOR_LOWER STREQUAL "aarch64" OR SYSTEM_PROCESSOR_LOWER STREQUAL "arm64")
    # For ARM64, completely disable build ID verification
    set(CPACK_DEBIAN_PACKAGE_GENERATE_SHLIBS OFF)
    set(CPACK_DEBIAN_PACKAGE_GENERATE_SHLIBS_POLICY "")
    
    # Override objdump path if fake objdump exists
    if(EXISTS "${CMAKE_BINARY_DIR}/fake_objdump.sh")
        set(ENV{OBJDUMP} "${CMAKE_BINARY_DIR}/fake_objdump.sh")
        message(STATUS "Using fake objdump for ARM64 DEB packaging: ${CMAKE_BINARY_DIR}/fake_objdump.sh")
    endif()
endif()

# Create a custom post-processing script to handle the embeddings library
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/deb_postinst.sh"
"#!/bin/sh
# Post-installation script for embeddings library
set -e

# Ensure proper permissions for the modules directory
if [ -d /usr/share/manticore/modules ]; then
    chmod 755 /usr/share/manticore/modules
    chmod 644 /usr/share/manticore/modules/libmanticore_knn_embeddings.so
fi

exit 0
")

# Set the control extras
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA "${CMAKE_CURRENT_BINARY_DIR}/deb_postinst.sh")
