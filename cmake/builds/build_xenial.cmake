# ---------- xenial ----------
# Above line is mandatory!
# rules to build deb package for Ubuntu 16.04 (xenial)

message ( STATUS "Will create DEB for Ubuntu 16.04 (xenial)" )

include ( builds/CommonDeb )

# some focal-specific variables and files
set ( DISTR_SUFFIX "~xenial_${CPACK_DEBIAN_PACKAGE_ARCHITECTURE}" )
