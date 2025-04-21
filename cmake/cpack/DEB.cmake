# Custom DEB packaging settings

# Disable shlib dependencies feature to avoid build ID issues
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF)

# Add predefined dependencies that we know are needed
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libc6 (>= 2.17), libstdc++6 (>= 6.0)")

# Disable the need for Build IDs in the shared libraries included in the package
set(CMAKE_SKIP_RPATH ON)

# Create a custom post-processing script to handle the embeddings library
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/deb_postinst.sh"
"#!/bin/sh
# Post-installation script for embeddings library
set -e

# Ensure proper permissions for the modules directory
if [ -d /usr/share/manticore/modules ]; then
    chmod 755 /usr/share/manticore/modules
    chmod 644 /usr/share/manticore/modules/libmanticoresearch_text_embeddings.so
fi

exit 0
")

# Set the control extras
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA "${CMAKE_CURRENT_BINARY_DIR}/deb_postinst.sh")