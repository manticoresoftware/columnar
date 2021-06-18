#!/bin/bash
#Run the very same test suite as run on CI, but locally

#export CTEST_BUILD_CONFIGURATION=Debug
#export VERBOSE=1
#export DIAGNOSTIC=1
#ctest -VV -S cmake/citest.cmake
ctest -V -S cmake/citest.cmake --progress
