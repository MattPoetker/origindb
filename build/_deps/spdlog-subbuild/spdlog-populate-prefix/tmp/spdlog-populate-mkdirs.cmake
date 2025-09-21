# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/Users/a12042/Development/instant_db/build/_deps/spdlog-src"
  "/Users/a12042/Development/instant_db/build/_deps/spdlog-build"
  "/Users/a12042/Development/instant_db/build/_deps/spdlog-subbuild/spdlog-populate-prefix"
  "/Users/a12042/Development/instant_db/build/_deps/spdlog-subbuild/spdlog-populate-prefix/tmp"
  "/Users/a12042/Development/instant_db/build/_deps/spdlog-subbuild/spdlog-populate-prefix/src/spdlog-populate-stamp"
  "/Users/a12042/Development/instant_db/build/_deps/spdlog-subbuild/spdlog-populate-prefix/src"
  "/Users/a12042/Development/instant_db/build/_deps/spdlog-subbuild/spdlog-populate-prefix/src/spdlog-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/Users/a12042/Development/instant_db/build/_deps/spdlog-subbuild/spdlog-populate-prefix/src/spdlog-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/Users/a12042/Development/instant_db/build/_deps/spdlog-subbuild/spdlog-populate-prefix/src/spdlog-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
