# zhpe-support core and libfabric built; no MPI

cmake_minimum_required(VERSION 3.5)

project (libzhpeq2)

include("../common.cmake")
include("../libzhpe_offloaded_stats.cmake")
include("../libfabric.cmake")

include_directories(BEFORE ../include asic/include)

add_subdirectory(libzhpeq_backend)
add_subdirectory(libzhpeq_util_fab)
add_subdirectory(lf_tests)
add_subdirectory(tests)
