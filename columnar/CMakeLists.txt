# Copyright (c) 2021-2025, Manticore Software LTD (https://manticoresearch.com)
# All rights reserved
#
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#    http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

cmake_minimum_required ( VERSION 3.17 )

target_include_directories ( columnar_root INTERFACE accessor builder )
add_subdirectory ( builder )
add_subdirectory ( accessor )

# main library
add_library ( columnar_lib MODULE columnar.cpp builder.cpp columnar.h builder.h )
target_compile_options ( columnar_lib PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:-wd4996> )
target_link_libraries ( columnar_lib PRIVATE columnar_root util common builder accessor )
set_target_properties( columnar_lib PROPERTIES PREFIX "" OUTPUT_NAME lib_manticore_columnar${lib_arch_suffix} )
