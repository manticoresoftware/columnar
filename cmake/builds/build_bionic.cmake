# ---------- bionic ----------
# Above line is mandatory!
# rules to build deb package for Ubuntu 18.04 (bionic)

message ( STATUS "Will create DEB for Ubuntu 18.04 (bionic)" )

# we provide explicit dependencies, so shlideps is not necessary
set ( disable_shlibdeps ON )
set ( CPACK_DEBIAN_PACKAGE_DEPENDS "libc6 (>= 2.27), libgcc1 (>= 4.2), libstdc++6 (>= 5.2)" )

include ( builds/CommonDeb )
