# ---------- wheezy ----------
# Above line is mandatory!
# rules to build deb package for Debian Wheezy

message ( STATUS "Will create DEB for Debian Wheezy)" )

include ( builds/CommonDeb )

# some focal-specific variables and files
set ( DISTR_SUFFIX "~wheezy_${CPACK_DEBIAN_PACKAGE_ARCHITECTURE}" )
