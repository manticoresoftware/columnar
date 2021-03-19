# ---------- bionic ----------
# Above line is mandatory!
# rules to build deb package for Ubuntu 18.04 (bionic)

message ( STATUS "Will create DEB for Ubuntu 18.04 (bionic)" )

include ( builds/CommonDeb )

# some focal-specific variables and files
set ( DISTR_SUFFIX "~bionic_${CPACK_DEBIAN_PACKAGE_ARCHITECTURE}" )
