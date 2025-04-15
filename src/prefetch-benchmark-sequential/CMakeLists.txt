cmake_minimum_required(VERSION 3.20)
project(prefetch-benchmark)

# Sources
set (SOURCES prefetch-benchmark.c)

# prefetch-benchmark
add_executable(prefetch-benchmark ${SOURCES})
target_link_libraries(prefetch-benchmark rt)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(prefetch-benchmark PRIVATE -fsanitize=address)
    target_link_options(prefetch-benchmark PRIVATE -fsanitize=address)
endif()

# prefetch-benchmark-lustre
add_executable(prefetch-benchmark-lustre ${SOURCES})
target_include_directories(prefetch-benchmark-lustre PRIVATE /usr/include/lustre/)
target_link_libraries(prefetch-benchmark-lustre liblustreapi.so rt)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(prefetch-benchmark-lustre PRIVATE -fsanitize=address)
    target_link_options(prefetch-benchmark-lustre PRIVATE -fsanitize=address)
endif()
target_compile_definitions(prefetch-benchmark-lustre PUBLIC WITH_LUSTRE)

