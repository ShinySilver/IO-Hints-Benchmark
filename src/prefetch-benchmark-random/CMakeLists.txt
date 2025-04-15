cmake_minimum_required(VERSION 3.20)
project(prefetch-benchmark-random)

# Sources
set (SOURCES prefetch-benchmark.c)

# prefetch-benchmark
add_executable(prefetch-benchmark-random ${SOURCES})
target_link_libraries(prefetch-benchmark-random rt)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(prefetch-benchmark-random PRIVATE -fsanitize=address)
    target_link_options(prefetch-benchmark-random PRIVATE -fsanitize=address)
endif()

# prefetch-benchmark-lustre
add_executable(prefetch-benchmark-random-lustre ${SOURCES})
target_include_directories(prefetch-benchmark-random-lustre PRIVATE /usr/include/lustre/)
target_link_libraries(prefetch-benchmark-random-lustre liblustreapi.so rt)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(prefetch-benchmark-random-lustre PRIVATE -fsanitize=address)
    target_link_options(prefetch-benchmark-random-lustre PRIVATE -fsanitize=address)
endif()
target_compile_definitions(prefetch-benchmark-random-lustre PUBLIC WITH_LUSTRE)

