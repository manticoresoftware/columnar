# ---------- focal ----------
# Above line is mandatory!
# rules to build deb package for Ubuntu 20.04 (focal)

message ( STATUS "Will create DEB for Ubuntu 20.04 (focal)" )

# we provide explicit dependencies, so shlideps is not necessary
set ( disable_shlibdeps ON )
set ( CPACK_DEBIAN_PACKAGE_DEPENDS "libc6 (>= 2.29), libgcc1 (>= 4.2), libstdc++6 (>= 9)" )

include ( builds/CommonDeb )
