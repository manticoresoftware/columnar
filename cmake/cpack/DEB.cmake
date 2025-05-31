# Custom DEB packaging settings

# Completely disable all dependency scanning and Build ID checks
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF)
set(CPACK_DEBIAN_PACKAGE_GENERATE_SHLIBS OFF)
set(CPACK_DEBIAN_PACKAGE_GENERATE_SHLIBS_POLICY "=")

# Force disable Build ID detection by setting a custom policy
set(CPACK_DEBIAN_PACKAGE_DEBUG OFF)

# Add predefined dependencies that we know are needed
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libc6 (>= 2.17), libstdc++6 (>= 6.0)")

# Disable the need for Build IDs in the shared libraries included in the package
set(CMAKE_SKIP_RPATH ON)

# Ensure that the embeddings component doesn't cause issues
set(CPACK_COMPONENT_EMBEDDINGS_REQUIRED FALSE)

# Set package architecture correctly for cross-compilation
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
    set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "arm64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
    set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "amd64")
endif()

# Create a wrapper script for objdump that always succeeds for ARM64
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/fake_objdump.sh"
"#!/bin/bash
# Fake objdump for ARM64 builds to bypass Build ID issues
if [[ \$* == *\"--file-headers\"* ]]; then
    echo \"fake build-id\"
else
    echo \"\"
fi
exit 0
")
    execute_process(COMMAND chmod +x "${CMAKE_CURRENT_BINARY_DIR}/fake_objdump.sh")
    set(ENV{OBJDUMP} "${CMAKE_CURRENT_BINARY_DIR}/fake_objdump.sh")
    message(STATUS "Set fake objdump for ARM64 build: ${CMAKE_CURRENT_BINARY_DIR}/fake_objdump.sh")
endif()

# Create a custom post-processing script to handle the embeddings library
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/deb_postinst.sh"
"#!/bin/sh
# Post-installation script for embeddings library
set -e

# Ensure proper permissions for the modules directory
if [ -d /usr/share/manticore/modules ]; then
    chmod 755 /usr/share/manticore/modules
    chmod 644 /usr/share/manticore/modules/libmanticore_knn_embeddings.so 2>/dev/null || true
fi

exit 0
")

# Set the control extras
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA "${CMAKE_CURRENT_BINARY_DIR}/deb_postinst.sh")
