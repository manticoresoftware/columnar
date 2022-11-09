# ---------- bookworm ----------
# Above line is mandatory!
# rules to build deb package for Debian Bookworm)

message ( STATUS "Will create DEB for Debian Bookworm" )

# we provide explicit dependencies, so shlideps is not necessary
set ( disable_shlideps ON )
set ( CPACK_DEBIAN_PACKAGE_DEPENDS "libc6 (>= 2.33), libgcc1 (>= 3.0), libstdc++6 (>= 11)" )

include ( builds/CommonDeb )
