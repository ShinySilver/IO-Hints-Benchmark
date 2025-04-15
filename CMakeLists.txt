cmake_minimum_required(VERSION 3.20.2)
project(hpc_energy_study C)

# include MPI, OpenMP and CPM
find_package(OpenMP REQUIRED COMPONENTS C)
find_package(MPI REQUIRED)
include(CPM.cmake)

# flags
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=gnu99 -Wall ${OpenMP_C_FLAGS}")
add_definitions(-D_XOPEN_SOURCE=600 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE)

# creating targets
add_executable(fakeapp-mpi-cpu-bound src/fakeapp_cpu_bound.c)
target_link_libraries(fakeapp-mpi-cpu-bound PRIVATE ${MPI_C_LIBRARIES})
target_include_directories(fakeapp-mpi-cpu-bound PRIVATE ${MPI_C_INCLUDE_PATH})

add_executable(fakeapp-mpi-memory-bound src/fakeapp_memory_bound.c)
target_link_libraries(fakeapp-mpi-memory-bound PRIVATE ${MPI_C_LIBRARIES})
target_include_directories(fakeapp-mpi-memory-bound PRIVATE ${MPI_C_INCLUDE_PATH})

add_executable(fakeapp-mpi-barrier src/fakeapp_mpi_barrier.c)
target_link_libraries(fakeapp-mpi-barrier PRIVATE ${MPI_C_LIBRARIES})
target_include_directories(fakeapp-mpi-barrier PRIVATE ${MPI_C_INCLUDE_PATH})

add_executable(fakeapp-mpi-stdio src/fakeapp_stdio.c)
target_link_libraries(fakeapp-mpi-stdio PRIVATE ${MPI_C_LIBRARIES})
target_include_directories(fakeapp-mpi-stdio PRIVATE ${MPI_C_INCLUDE_PATH})

add_executable(fakeapp-mpi-posix src/fakeapp_posix.c)
target_link_libraries(fakeapp-mpi-posix PRIVATE ${MPI_C_LIBRARIES})
target_include_directories(fakeapp-mpi-posix PRIVATE ${MPI_C_INCLUDE_PATH})

add_executable(fakeapp-mpi-posix-odirect-osync src/fakeapp_posix_odirect_osync.c)
target_link_libraries(fakeapp-mpi-posix-odirect-osync PRIVATE ${MPI_C_LIBRARIES})
target_include_directories(fakeapp-mpi-posix-odirect-osync PRIVATE ${MPI_C_INCLUDE_PATH})

add_executable(fakeapp-mpi-mpiio-independant src/fakeapp_mpiio_independant.c)
target_link_libraries(fakeapp-mpi-mpiio-independant PRIVATE ${MPI_C_LIBRARIES})
target_include_directories(fakeapp-mpi-mpiio-independant PRIVATE ${MPI_C_INCLUDE_PATH})

add_executable(fakeapp-mpi-mpiio-shared src/fakeapp_mpiio_shared.c)
target_link_libraries(fakeapp-mpi-mpiio-shared PRIVATE ${MPI_C_LIBRARIES})
target_include_directories(fakeapp-mpi-mpiio-shared PRIVATE ${MPI_C_INCLUDE_PATH})

add_executable(fakeapp-mpi-mpiio-collective src/fakeapp_mpiio_collective.c)
target_link_libraries(fakeapp-mpi-mpiio-collective PRIVATE ${MPI_C_LIBRARIES})
target_include_directories(fakeapp-mpi-mpiio-collective PRIVATE ${MPI_C_INCLUDE_PATH})

# Downloading the STREAM benchmark, modified by the CEA to use MPI, with a weird build system.
CPMAddPackage(
        NAME mpi_stream
        GITHUB_REPOSITORY cea-hpc/mpi_stream
        GIT_TAG 925bd26
        DOWNLOAD_ONLY true
)
set(HAVE_MPI ON)
set(mpi_stream_VERSION 0.3)
configure_file(${mpi_stream_SOURCE_DIR}/c/config_cmake.h.in ${mpi_stream_SOURCE_DIR}/c/src/config.h)
add_executable(stream-benchmark ${mpi_stream_SOURCE_DIR}/c/src/stream.c)
target_link_libraries(stream-benchmark PRIVATE ${MPI_C_LIBRARIES} m)
target_include_directories(stream-benchmark PRIVATE ${mpi_stream_SOURCE_DIR}/c/src/ ${MPI_C_INCLUDE_PATH})

# Downloading the DGEMM benchmark and its dependency, OpenBLAS
#CPMAddPackage(
#        NAME dgemm-benchmark
#        URL http://phoronix-test-suite.com/benchmark-files/mtdgemm-crossroads-v1.0.0.tgz
#        MD5 b133bd78e7864cef498f92b03176cafd
#        DOWNLOAD_ONLY true
#)
#add_executable(dgemm_benchmark ${dgemm-benchmark_SOURCE_DIR}/src/mt-dgemm.c)
#target_compile_options(dgemm_benchmark PRIVATE -ffast-math -mavx2 -ftree-vectorizer-verbose=3 -O3 -fopenmp -DUSE_CBLAS)

# creating "fakeapps" target
add_custom_target(fakeapps)
add_dependencies(fakeapps
    fakeapp-mpi-cpu-bound
    fakeapp-mpi-memory-bound
    fakeapp-mpi-barrier
    fakeapp-mpi-stdio
    fakeapp-mpi-posix
    fakeapp-mpi-posix-odirect-osync
    fakeapp-mpi-mpiio-independant
    fakeapp-mpi-mpiio-shared
    fakeapp-mpi-mpiio-collective
    stream-benchmark)