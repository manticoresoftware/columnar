# ---------- jammy ----------
# Above line is mandatory!
# rules to build deb package for Ubuntu 22.04 (jammy)

message ( STATUS "Will create DEB for Ubuntu 22.04 (jammy)" )

# we provide explicit dependencies, so shlideps is not necessary
set ( disable_shlideps ON )
set ( CPACK_DEBIAN_PACKAGE_DEPENDS "libc6 (>= 2.34), libgcc1 (>= 4.2), libstdc++6 (>= 11)" )

include ( builds/CommonDeb )
