# ---------- buster ----------
# Above line is mandatory!
# rules to build deb package for Debian Buster)

message ( STATUS "Will create DEB for Debian Buster)" )

include ( builds/CommonDeb )

# some focal-specific variables and files
set ( DISTR_SUFFIX "~buster_${CPACK_DEBIAN_PACKAGE_ARCHITECTURE}" )
