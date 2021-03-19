# ---------- focal ----------
# Above line is mandatory!
# rules to build deb package for Ubuntu 20.04 (focal)

message ( STATUS "Will create DEB for Ubuntu 20.04 (focal)" )

include ( builds/CommonDeb )

# some focal-specific variables and files
set ( DISTR_SUFFIX "~focal_${CPACK_DEBIAN_PACKAGE_ARCHITECTURE}" )
