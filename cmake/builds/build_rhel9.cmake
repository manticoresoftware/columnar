# ---------- rhel9 ----------
# Above line is mandatory!
# rules to build rpm package for Red Hat linux 9 / Centos Stream 9

message ( STATUS "Will create RPM for RedHat/Centos Stream 9" )
set ( RELEASE_DIST ".el9" )
include ( builds/CommonRpm )