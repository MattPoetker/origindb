# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/Users/a12042/Development/instant_db/build_test/_deps/fmt-src"
  "/Users/a12042/Development/instant_db/build_test/_deps/fmt-build"
  "/Users/a12042/Development/instant_db/build_test/_deps/fmt-subbuild/fmt-populate-prefix"
  "/Users/a12042/Development/instant_db/build_test/_deps/fmt-subbuild/fmt-populate-prefix/tmp"
  "/Users/a12042/Development/instant_db/build_test/_deps/fmt-subbuild/fmt-populate-prefix/src/fmt-populate-stamp"
  "/Users/a12042/Development/instant_db/build_test/_deps/fmt-subbuild/fmt-populate-prefix/src"
  "/Users/a12042/Development/instant_db/build_test/_deps/fmt-subbuild/fmt-populate-prefix/src/fmt-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/Users/a12042/Development/instant_db/build_test/_deps/fmt-subbuild/fmt-populate-prefix/src/fmt-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/Users/a12042/Development/instant_db/build_test/_deps/fmt-subbuild/fmt-populate-prefix/src/fmt-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
