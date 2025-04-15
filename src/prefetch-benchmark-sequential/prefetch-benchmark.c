#define _POSIX_C_SOURCE 200112
#define _XOPEN_SOURCE 500
#define _GNU_SOURCE 
//#define __USE_FILE_OFFSET64

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <math.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/time.h>
#include <aio.h>
#include <stdint.h>
#include <time.h>

#ifdef WITH_LUSTRE
#include "lustre/lustreapi.h"
#endif

#define nsleep(nanoseconds) nanosleep((const struct timespec[]){{0, nanoseconds}}, NULL);

// How many time we do the same measure in a row to increase precision
#define DURATION_PER_EXPERIMENT_US (15*(uint64_t)1e6)

// Dropping the cache might be asynchronous (or not, we don't know). As such, we sleep for the duration below after a cache drop, just to be sure.
#define CACHE_DROP_DELAY_SECONDS 0

// The target file. Could also be /gpfs/aquamini2/nicolasl/random_file.bin, on kiwi.
#ifdef WITH_LUSTRE
#define TARGET_FILE "/fs1/nicolasl/random_file.bin"
#else
//#define TARGET_FILE "/gpfs/aquamini2/nicolasl/random_file.bin"
#define TARGET_FILE "/mnt/disk/nicolasl/random_file.bin"

// On GPFS, we have an hard time cleaning the cache. As such, we read a few GB of data to clean the cache between experiments
#define SECONDARY_IO_SIZE 4194304 // 4 MB
#define SECONDARY_IO_COUNT 1024 // For a total of 4 GB, which is the size of GPFS page cache
#endif

// Whether or not a "desc" field should be included in the output csv
#define OUTPUT_EXPERIMENT_DESCRIPTION false

// Individual file sizes to be tested
static const uint64_t file_sizes[] = {64*1024*1024, 1024*1024*1024, 16ul*1024*1024*1024};
static const int file_size_count = 3;

// Individual I/O sizes to be tested
static const uint64_t io_sizes[] = {4*1024, 16*1024, 64*1024, 1024*1024, 16*1024*1024, 256*1024*1024};
static const int io_size_count = 6;

// Individual delays for just in time prefetch (in us)
static const uint64_t jit_prefetch_delays[] = {0, 1000, 10000, 100000, 1000000};
static const int jit_prefetch_delay_count = 5;

// Individual inter arrival times between I/Os (in ns)
static const uint64_t io_interarrival_times[] = {0, 100, 10000, 1000000};
static const int io_interarrival_time_count = 4;

// Used for throughput instrumentation 
static inline uint64_t get_timestamp_us();

// Use /proc/sys/vm/drop_caches to drop the client page cache 
static inline void client_cache_drop(int fd);

// Use fadvise to evict some data from the client page cache 
static inline void client_cache_evict(int fd, uint64_t offset, uint64_t length);

// Use lla_ladvise to evict some data from the server page cache
#ifdef WITH_LUSTRE
static inline void server_cache_evict(int fd, uint64_t offset, uint64_t length);
#endif

// Use fadvise to prefetch some data to the client page cache 
static inline void client_cache_prefetch(int fd, uint64_t offset, uint64_t length);

// Use lla_ladvise to prefetch some data to the server page cache 
#ifdef WITH_LUSTRE
static inline void server_cache_prefetch(int fd, uint64_t offset, uint64_t length);
#endif

// Use asynchronous I/O to forcefully prefetch some data 
static inline void aio_prefetch(int fd, uint64_t offset, uint64_t length);

void perform_baseline_benchmark(char *target_file, FILE *output_file){

    for(int h = 0; h<io_interarrival_time_count; h++){
        uint64_t io_interarrival_time_ns = io_interarrival_times[h];

        // O_DIRECT
        int fd = open(TARGET_FILE, O_RDONLY, O_DIRECT | O_SYNC); 
        for(int i = 0; i<file_size_count; i++){
            uint64_t file_size = file_sizes[i];

            for(int j = 0; j<io_size_count; j++){
                uint64_t io_size = io_sizes[j];
                if(io_size>file_size) continue;

                // Allocating the read buffer
                char *buffer = malloc(sizeof(char)*io_size);

                // Starting the campaign
                int experiment_count;
                uint64_t t0 = get_timestamp_us(), read_duration = 0;
                for(experiment_count=0; get_timestamp_us() - t0 < DURATION_PER_EXPERIMENT_US; experiment_count++){

                    // Cleaning the cache at the beginning of each experiment
                    #ifdef WITH_LUSTRE
                    server_cache_evict(fd, 0, file_size);
                    #endif
                    client_cache_drop(fd);
                    // sleep(CACHE_DROP_DELAY_SECONDS); // cache drops might be async, so we do a small sleep

                    // Running the experimentation once
                    lseek(fd, 0, SEEK_SET);
                    uint64_t t1 = get_timestamp_us();
                    for(size_t volume = 0; volume<file_size; volume+=io_size){
                        int ret = read(fd, buffer, io_size);
                        if(__glibc_unlikely(ret < 0)){
                            printf("Error reading file \"%s\": %s\n", TARGET_FILE, strerror(errno));
                            exit(0);
                        }
                        if(io_interarrival_time_ns!=0){
                            read_duration += get_timestamp_us()-t1;
                            nsleep(io_interarrival_time_ns);
                            t1 = get_timestamp_us();
                        }
                    }
                    read_duration += get_timestamp_us()-t1;
                }
                free(buffer);
                fprintf(output_file, "target='%s', category='Baseline', label='O_DIRECT', "
                    #if OUTPUT_EXPERIMENT_DESCRIPTION
                    "desc='File not cached, no readahead, using O_DIRECT', "
                    #endif
                    "file_size=%llu, interarrival_time_us=%llu, io_size=%llu, throughput_gb_per_second=%.3f\n", target_file, file_size, io_interarrival_time_ns, io_size,
                    experiment_count*file_size/(read_duration*1e-6)/(1ul << 30));
                fflush(output_file);
            }
        }
        close(fd);

        // Not cached
        FILE *fp = fopen(TARGET_FILE, "r");
        for(int i = 0; i<file_size_count; i++){
            uint64_t file_size = file_sizes[i];

            for(int j = 0; j<io_size_count; j++){
                uint64_t io_size = io_sizes[j];
                if(io_size>file_size) continue;

                // Allocating the read buffer
                char *buffer = malloc(sizeof(char)*io_size);

                // Starting the campaign
                int experiment_count;
                uint64_t t0 = get_timestamp_us(), read_duration = 0;
                for(experiment_count=0; get_timestamp_us() - t0 < DURATION_PER_EXPERIMENT_US; experiment_count++){

                    // Cleaning the cache at the beginning of each experiment
                    #ifdef WITH_LUSTRE
                    server_cache_evict(fileno(fp), 0, file_size);
                    #endif
                    client_cache_drop(fileno(fp));
                    // sleep(CACHE_DROP_DELAY_SECONDS); // cache drops might be async, so we do a small sleep

                    // Running the experimentation once
                    fseek(fp, 0, SEEK_SET);
                    uint64_t t1 = get_timestamp_us();
                    for(size_t volume = 0; volume<file_size; volume+=io_size){
                        int ret = fread(buffer, sizeof(char), io_size, fp);
                        if(__glibc_unlikely(ret < 0)){
                            printf("Error reading file \"%s\": %s\n", TARGET_FILE, strerror(errno));
                            exit(0);
                        }
                        if(io_interarrival_time_ns!=0){
                            read_duration += get_timestamp_us()-t1;
                            nsleep(io_interarrival_time_ns);
                            t1 = get_timestamp_us();
                        }
                    }
                    read_duration += get_timestamp_us()-t1;
                }
                fprintf(output_file, "target='%s', category='Baseline', label='Not cached', "
                    #if OUTPUT_EXPERIMENT_DESCRIPTION
                    "desc='File not cached', "
                    #endif
                    "file_size=%llu, interarrival_time_us=%llu, io_size=%llu, throughput_gb_per_second=%.3f\n", target_file, file_size, io_interarrival_time_ns, io_size,
                    experiment_count*file_size/(read_duration*1e-6)/(1ul << 30));
                free(buffer);
                fflush(output_file);
            }
        }

        // Not cached but marked as sequential
        for(int i = 0; i<file_size_count; i++){
            uint64_t file_size = file_sizes[i];

            for(int j = 0; j<io_size_count; j++){
                uint64_t io_size = io_sizes[j];
                if(io_size>file_size) continue;

                // Allocating the read buffer
                char *buffer = malloc(sizeof(char)*io_size);

                // Starting the campaign
                int experiment_count;
                uint64_t t0 = get_timestamp_us(), read_duration = 0;
                for(experiment_count=0; get_timestamp_us() - t0 < DURATION_PER_EXPERIMENT_US; experiment_count++){

                    // Cleaning the cache at the beginning of each experiment
                    #ifdef WITH_LUSTRE
                    server_cache_evict(fileno(fp), 0, file_size);
                    #endif
                    client_cache_drop(fileno(fp));
                    // sleep(CACHE_DROP_DELAY_SECONDS); // cache drops might be async, so we do a small sleep

                    // Running the experimentation once
                    fseek(fp, 0, SEEK_SET);
                    posix_fadvise(fileno(fp), 0, file_size, POSIX_FADV_SEQUENTIAL);
                    uint64_t t1 = get_timestamp_us();
                    for(size_t volume = 0; volume<file_size; volume+=io_size){
                        int ret = fread(buffer, sizeof(char), io_size, fp);
                        if(__glibc_unlikely(ret < 0)){
                            printf("Error reading file \"%s\": %s\n", TARGET_FILE, strerror(errno));
                            exit(0);
                        }
                        if(io_interarrival_time_ns!=0){
                            read_duration += get_timestamp_us()-t1;
                            nsleep(io_interarrival_time_ns);
                            t1 = get_timestamp_us();
                        }
                    }
                    read_duration += get_timestamp_us()-t1;
                }
                fprintf(output_file, "target='%s', category='Extended baseline', label='Not cached but marked as sequential', "
                    #if OUTPUT_EXPERIMENT_DESCRIPTION
                    "desc='File not cached, but fadvise was used to mark it as sequential', "
                    #endif
                    "file_size=%llu, interarrival_time_us=%llu, io_size=%llu, throughput_gb_per_second=%.3f\n", target_file, file_size, io_interarrival_time_ns, io_size,
                    experiment_count*file_size/(read_duration*1e-6)/(1ul << 30));
                free(buffer);
                fflush(output_file);
            }
        }

        // Not cached but marked as random
        for(int i = 0; i<file_size_count; i++){
            uint64_t file_size = file_sizes[i];

            for(int j = 0; j<io_size_count; j++){
                uint64_t io_size = io_sizes[j];
                if(io_size>file_size) continue;

                // Allocating the read buffer
                char *buffer = malloc(sizeof(char)*io_size);

                // Starting the campaign
                int experiment_count;
                uint64_t t0 = get_timestamp_us(), read_duration = 0;
                for(experiment_count=0; get_timestamp_us() - t0 < DURATION_PER_EXPERIMENT_US; experiment_count++){

                    // Cleaning the cache at the beginning of each experiment
                    #ifdef WITH_LUSTRE
                    server_cache_evict(fileno(fp), 0, file_size);
                    #endif
                    client_cache_drop(fileno(fp));
                    // sleep(CACHE_DROP_DELAY_SECONDS); // cache drops might be async, so we do a small sleep

                    // Running the experimentation once
                    fseek(fp, 0, SEEK_SET);
                    posix_fadvise(fileno(fp), 0, file_size, POSIX_FADV_RANDOM);
                    uint64_t t1 = get_timestamp_us();
                    for(size_t volume = 0; volume<file_size; volume+=io_size){
                        int ret = fread(buffer, sizeof(char), io_size, fp);
                        if(__glibc_unlikely(ret < 0)){
                            printf("Error reading file \"%s\": %s\n", TARGET_FILE, strerror(errno));
                            exit(0);
                        }
                        if(io_interarrival_time_ns!=0){
                            read_duration += get_timestamp_us()-t1;
                            nsleep(io_interarrival_time_ns);
                            t1 = get_timestamp_us();
                        }
                    }
                    read_duration += get_timestamp_us()-t1;
                }
                fprintf(output_file, "target='%s', category='Extended baseline', label='Not cached but marked as random', "
                    #if OUTPUT_EXPERIMENT_DESCRIPTION
                    "desc='File not cached, but fadvise was used to mark it as random', "
                    #endif
                    "file_size=%llu, interarrival_time_us=%llu, io_size=%llu, throughput_gb_per_second=%.3f\n", target_file, file_size, io_interarrival_time_ns, io_size,
                    experiment_count*file_size/(read_duration*1e-6)/(1ul << 30));
                free(buffer);
                fflush(output_file);
            }
        }
        fclose(fp);
    }
}

void perform_offline_prefetch_benchmark(char *target_file, FILE *output_file){

    for(int h = 0; h<io_interarrival_time_count; h++){
        uint64_t io_interarrival_time_ns = io_interarrival_times[h];
        FILE *fp = fopen(TARGET_FILE, "r");

        // Cached
        for(int i = 0; i<file_size_count; i++){
            uint64_t file_size = file_sizes[i];

            for(int j = 0; j<io_size_count; j++){
                uint64_t io_size = io_sizes[j];
                if(io_size>file_size) continue;

                // Allocating the read buffer
                char *buffer = malloc(sizeof(char)*io_size);

                // Starting the campaign
                int experiment_count;
                uint64_t t0 = get_timestamp_us(), read_duration = 0;
                for(experiment_count=0; get_timestamp_us() - t0 < DURATION_PER_EXPERIMENT_US; experiment_count++){

                    // Cleaning the cache at the beginning of each experiment
                    #ifdef WITH_LUSTRE
                    server_cache_evict(fileno(fp), 0, file_size);
                    #endif
                    client_cache_drop(fileno(fp));
                    // sleep(CACHE_DROP_DELAY_SECONDS); // cache drops might be async, so we do a small sleep

                    // Running the experimentation once: first a dummy read to have the file in cache, then the instrumented read
                    fseek(fp, 0, SEEK_SET);
                    for(size_t volume = 0; volume<file_size; volume+=io_size){
                        int ret = fread(buffer, sizeof(char), io_size, fp);
                        if(__glibc_unlikely(ret < 0)){
                            printf("Error reading file \"%s\": %s\n", TARGET_FILE, strerror(errno));
                            exit(0);
                        }
                    }
                    fseek(fp, 0, SEEK_SET);
                    uint64_t t1 = get_timestamp_us();
                    for(size_t volume = 0; volume<file_size; volume+=io_size){
                        int ret = fread(buffer, sizeof(char), io_size, fp);
                        if(__glibc_unlikely(ret < 0)){
                            printf("Error reading file \"%s\": %s\n", TARGET_FILE, strerror(errno));
                            exit(0);
                        }
                        if(io_interarrival_time_ns!=0){
                            read_duration += get_timestamp_us()-t1;
                            nsleep(io_interarrival_time_ns);
                            t1 = get_timestamp_us();
                        }
                    }
                    read_duration += get_timestamp_us()-t1;
                }
                fprintf(output_file, "target='%s', category='Offline prefetch', label='Offline prefetch\\n(sync read)', "
                    #if OUTPUT_EXPERIMENT_DESCRIPTION
                    "desc='File was read once before the experiment', "
                    #endif
                    "file_size=%llu, interarrival_time_us=%llu, io_size=%llu, throughput_gb_per_second=%.3f\n", target_file, file_size, io_interarrival_time_ns, io_size,
                    experiment_count*file_size/(read_duration*1e-6)/(1ul << 30));
                free(buffer);
                fflush(output_file);
            }
        }

        // Cached client-side only
        #ifdef WITH_LUSTRE
        for(int i = 0; i<file_size_count; i++){
            uint64_t file_size = file_sizes[i];

            for(int j = 0; j<io_size_count; j++){
                uint64_t io_size = io_sizes[j];
                if(io_size>file_size) continue;

                // Allocating the read buffer
                char *buffer = malloc(sizeof(char)*io_size);

                // Starting the campaign
                int experiment_count;
                uint64_t t0 = get_timestamp_us(), read_duration = 0;
                for(experiment_count=0; get_timestamp_us() - t0 < DURATION_PER_EXPERIMENT_US; experiment_count++){

                    // Cleaning the cache at the beginning of each experiment
                    #ifdef WITH_LUSTRE
                    server_cache_evict(fileno(fp), 0, file_size);
                    #endif
                    client_cache_drop(fileno(fp));
                    // sleep(CACHE_DROP_DELAY_SECONDS); // cache drops might be async, so we do a small sleep

                    // Running the experimentation once: first a dummy read to have the file in cache, then the instrumented read
                    fseek(fp, 0, SEEK_SET);
                    for(size_t volume = 0; volume<file_size; volume+=io_size){
                        int ret = fread(buffer, sizeof(char), io_size, fp);
                        if(__glibc_unlikely(ret < 0)){
                            printf("Error reading file \"%s\": %s\n", TARGET_FILE, strerror(errno));
                            exit(0);
                        }
                    }
                    server_cache_evict(fileno(fp), 0, file_size);
                    
                    fseek(fp, 0, SEEK_SET);
                    uint64_t t1 = get_timestamp_us();
                    for(size_t volume = 0; volume<file_size; volume+=io_size){
                        int ret = fread(buffer, sizeof(char), io_size, fp);
                        if(__glibc_unlikely(ret < 0)){
                            printf("Error reading file \"%s\": %s\n", TARGET_FILE, strerror(errno));
                            exit(0);
                        }
                        if(io_interarrival_time_ns!=0){
                            read_duration += get_timestamp_us()-t1;
                            nsleep(io_interarrival_time_ns);
                            t1 = get_timestamp_us();
                        }
                    }
                    read_duration += get_timestamp_us()-t1;
                }
                fprintf(output_file, "target='%s', category='Offline prefetch', label='Offline client-side prefetch\\n(sync read + ladvise evict)', "
                    #if OUTPUT_EXPERIMENT_DESCRIPTION
                    "desc='File was read once before the experiment, but lla_ladvise was used to evict it from the server cache', "
                    #endif
                    "file_size=%llu, interarrival_time_us=%llu, io_size=%llu, throughput_gb_per_second=%.3f\n", target_file, file_size, io_interarrival_time_ns, io_size,
                    experiment_count*file_size/(read_duration*1e-6)/(1ul << 30));
                free(buffer);
                fflush(output_file);
            }
        }
        #endif

        // Cached server-side only (1)
        for(int i = 0; i<file_size_count; i++){
            uint64_t file_size = file_sizes[i];

            for(int j = 0; j<io_size_count; j++){
                uint64_t io_size = io_sizes[j];
                if(io_size>file_size) continue;

                // Allocating the read buffer
                char *buffer = malloc(sizeof(char)*io_size);

                // Starting the campaign
                int experiment_count;
                uint64_t t0 = get_timestamp_us(), read_duration = 0;
                for(experiment_count=0; get_timestamp_us() - t0 < DURATION_PER_EXPERIMENT_US; experiment_count++){

                    // Cleaning the cache at the beginning of each experiment
                    #ifdef WITH_LUSTRE
                    server_cache_evict(fileno(fp), 0, file_size);
                    #endif
                    client_cache_drop(fileno(fp));
                    // sleep(CACHE_DROP_DELAY_SECONDS); // cache drops might be async, so we do a small sleep

                    // Running the experimentation once: first a dummy read to have the file in cache, then the instrumented read
                    fseek(fp, 0, SEEK_SET);
                    for(size_t volume = 0; volume<file_size; volume+=io_size){
                        int ret = fread(buffer, sizeof(char), io_size, fp);
                        if(__glibc_unlikely(ret < 0)){
                            printf("Error reading file \"%s\": %s\n", TARGET_FILE, strerror(errno));
                            exit(0);
                        }
                    }
                    client_cache_drop(fileno(fp));
                    
                    fseek(fp, 0, SEEK_SET);
                    uint64_t t1 = get_timestamp_us();
                    for(size_t volume = 0; volume<file_size; volume+=io_size){
                        int ret = fread(buffer, sizeof(char), io_size, fp);
                        if(__glibc_unlikely(ret < 0)){
                            printf("Error reading file \"%s\": %s\n", TARGET_FILE, strerror(errno));
                            exit(0);
                        }
                        if(io_interarrival_time_ns!=0){
                            read_duration += get_timestamp_us()-t1;
                            nsleep(io_interarrival_time_ns);
                            t1 = get_timestamp_us();
                        }
                    }
                    read_duration += get_timestamp_us()-t1;
                }
                fprintf(output_file, "target='%s', category='Offline prefetch', label='Offline server-side prefetch\\n(sync read + drop_cache evict)', "
                    #if OUTPUT_EXPERIMENT_DESCRIPTION
                    "desc='File was read once before the experiment, but /proc/sys/vm/drop_caches was used to evict it from the client cache', "
                    #endif
                    "file_size=%llu, interarrival_time_us=%llu, io_size=%llu, throughput_gb_per_second=%.3f\n", target_file, file_size, io_interarrival_time_ns, io_size,
                    experiment_count*file_size/(read_duration*1e-6)/(1ul << 30));
                free(buffer);
                fflush(output_file);
            }
        }

        // Cached server-side only (2)
        for(int i = 0; i<file_size_count; i++){
            uint64_t file_size = file_sizes[i];

            for(int j = 0; j<io_size_count; j++){
                uint64_t io_size = io_sizes[j];
                if(io_size>file_size) continue;

                // Allocating the read buffer
                char *buffer = malloc(sizeof(char)*io_size);

                // Starting the campaign
                int experiment_count;
                uint64_t t0 = get_timestamp_us(), read_duration = 0;
                for(experiment_count=0; get_timestamp_us() - t0 < DURATION_PER_EXPERIMENT_US; experiment_count++){

                    // Cleaning the cache at the beginning of each experiment
                    #ifdef WITH_LUSTRE
                    server_cache_evict(fileno(fp), 0, file_size);
                    #endif
                    client_cache_drop(fileno(fp));
                    // sleep(CACHE_DROP_DELAY_SECONDS); // cache drops might be async, so we do a small sleep

                    // Running the experimentation once: first a dummy read to have the file in cache, then the instrumented read
                    fseek(fp, 0, SEEK_SET);
                    for(size_t volume = 0; volume<file_size; volume+=io_size){
                        int ret = fread(buffer, sizeof(char), io_size, fp);
                        if(__glibc_unlikely(ret < 0)){
                            printf("Error reading file \"%s\": %s\n", TARGET_FILE, strerror(errno));
                            exit(0);
                        }
                    }
                    client_cache_evict(fileno(fp), 0, file_size);
                    
                    fseek(fp, 0, SEEK_SET);
                    uint64_t t1 = get_timestamp_us();
                    for(size_t volume = 0; volume<file_size; volume+=io_size){
                        int ret = fread(buffer, sizeof(char), io_size, fp);
                        if(__glibc_unlikely(ret < 0)){
                            printf("Error reading file \"%s\": %s\n", TARGET_FILE, strerror(errno));
                            exit(0);
                        }
                        if(io_interarrival_time_ns!=0){
                            read_duration += get_timestamp_us()-t1;
                            nsleep(io_interarrival_time_ns);
                            t1 = get_timestamp_us();
                        }
                    }
                    read_duration += get_timestamp_us()-t1;
                }
                fprintf(output_file, "target='%s', category='Offline prefetch', label='Offline server-side prefetch\\n(sync read + fadvise evict)', "
                    #if OUTPUT_EXPERIMENT_DESCRIPTION
                    "desc='File was read once before the experiment, but fadvise was used to evict it from the client cache', "
                    #endif
                    "file_size=%llu, interarrival_time_us=%llu, io_size=%llu, throughput_gb_per_second=%.3f\n", target_file, file_size, io_interarrival_time_ns, io_size,
                    experiment_count*file_size/(read_duration*1e-6)/(1ul << 30));
                free(buffer);
                fflush(output_file);
            }
        }
        fclose(fp);
    }
}

void perform_jit_prefetch_benchmark(char *target_file, FILE *output_file){

    for(int h = 0; h<io_interarrival_time_count; h++){
        uint64_t io_interarrival_time_ns = io_interarrival_times[h];
        FILE *fp = fopen(TARGET_FILE, "r");

        // JIT prefetched both sides
        #ifdef WITH_LUSTRE
        for(int i = 0; i<file_size_count; i++){
            uint64_t file_size = file_sizes[i];

            for(int j = 0; j<io_size_count; j++){
                uint64_t io_size = io_sizes[j];
                if(io_size>file_size) continue;
                
                for(int k=0; k<jit_prefetch_delay_count; k++){
                    int prefetch_delay = jit_prefetch_delays[k];

                    // Allocating the read buffer
                    char *buffer = malloc(sizeof(char)*io_size);

                    // Starting the campaign
                    int experiment_count;
                    uint64_t t0 = get_timestamp_us(), read_duration = 0;
                    for(experiment_count=0; get_timestamp_us() - t0 < DURATION_PER_EXPERIMENT_US; experiment_count++){

                        // Cleaning the cache at the beginning of each experiment
                        #ifdef WITH_LUSTRE
                        server_cache_evict(fileno(fp), 0, file_size);
                        #endif
                        client_cache_drop(fileno(fp));
                        // sleep(CACHE_DROP_DELAY_SECONDS); // cache drops might be async, so we do a small sleep

                        // Running the experimentation: prefetching, waiting for a bit, then reading!
                        server_cache_prefetch(fileno(fp), 0, file_size);
                        client_cache_prefetch(fileno(fp), 0, file_size);
                        usleep(prefetch_delay);
                        fseek(fp, 0, SEEK_SET);
                        uint64_t t1 = get_timestamp_us();
                        for(size_t volume = 0; volume<file_size; volume+=io_size){
                            int ret = fread(buffer, sizeof(char), io_size, fp);
                            if(__glibc_unlikely(ret < 0)){
                                printf("Error reading file \"%s\": %s\n", TARGET_FILE, strerror(errno));
                                exit(0);
                            }
                            if(io_interarrival_time_ns!=0){
                                read_duration += get_timestamp_us()-t1;
                                nsleep(io_interarrival_time_ns);
                                t1 = get_timestamp_us();
                            }
                        }
                        read_duration += get_timestamp_us()-t1;
                    }
                    fprintf(output_file, "target='%s', category='JIT prefetch', label='JIT fadvise+ladvise prefetch of the whole file', "
                        #if OUTPUT_EXPERIMENT_DESCRIPTION
                        "desc='File was prefetched to the server page cache using llu_ladvise and to the client page cache using fadvise %d seconds before the reading started', "
                        #endif
                        "file_size=%llu, interarrival_time_us=%llu, io_size=%llu, prefetch_delay=%d, throughput_gb_per_second=%.3f\n", target_file, file_size, io_interarrival_time_ns, io_size, prefetch_delay,
                        prefetch_delay, experiment_count*file_size/(read_duration*1e-6)/(1ul << 30));
                    free(buffer);
                    fflush(output_file);
                }
            }
        }
        #endif
        
        // JIT prefetched client-side
        for(int i = 0; i<file_size_count; i++){
            uint64_t file_size = file_sizes[i];

            for(int j = 0; j<io_size_count; j++){
                uint64_t io_size = io_sizes[j];
                if(io_size>file_size) continue;
                
                for(int k=0; k<jit_prefetch_delay_count; k++){
                    int prefetch_delay = jit_prefetch_delays[k];

                    // Allocating the read buffer
                    char *buffer = malloc(sizeof(char)*io_size);

                    // Starting the campaign
                    int experiment_count;
                    uint64_t t0 = get_timestamp_us(), read_duration = 0;
                    for(experiment_count=0; get_timestamp_us() - t0 < DURATION_PER_EXPERIMENT_US; experiment_count++){

                        // Cleaning the cache at the beginning of each experiment
                        #ifdef WITH_LUSTRE
                        server_cache_evict(fileno(fp), 0, file_size);
                        #endif
                        client_cache_drop(fileno(fp));
                        // sleep(CACHE_DROP_DELAY_SECONDS); // cache drops might be async, so we do a small sleep

                        // Running the experimentation: prefetching, waiting for a bit, then reading!
                        client_cache_prefetch(fileno(fp), 0, file_size);
                        usleep(prefetch_delay);
                        fseek(fp, 0, SEEK_SET);
                        uint64_t t1 = get_timestamp_us();
                        for(size_t volume = 0; volume<file_size; volume+=io_size){
                            int ret = fread(buffer, sizeof(char), io_size, fp);
                            if(__glibc_unlikely(ret < 0)){
                                printf("Error reading file \"%s\": %s\n", TARGET_FILE, strerror(errno));
                                exit(0);
                            }
                            if(io_interarrival_time_ns!=0){
                            read_duration += get_timestamp_us()-t1;
                            nsleep(io_interarrival_time_ns);
                            t1 = get_timestamp_us();
                        }
                        }
                        read_duration += get_timestamp_us()-t1;
                    }
                    fprintf(output_file, "target='%s', category='JIT prefetch', label='JIT fadvise prefetch of the whole file', "
                        #if OUTPUT_EXPERIMENT_DESCRIPTION
                        "desc='File was prefetched to the client page cache using fadvise %d seconds before the reading started', "
                        #endif
                        "file_size=%llu, interarrival_time_us=%llu, io_size=%llu, prefetch_delay=%d, throughput_gb_per_second=%.3f\n", target_file, file_size, io_interarrival_time_ns, io_size, prefetch_delay,
                        prefetch_delay, experiment_count*file_size/(read_duration*1e-6)/(1ul << 30));
                    free(buffer);
                    fflush(output_file);
                }
            }
        }

        // JIT prefetched server-side
        #ifdef WITH_LUSTRE
        for(int i = 0; i<file_size_count; i++){
            uint64_t file_size = file_sizes[i];

            for(int j = 0; j<io_size_count; j++){
                uint64_t io_size = io_sizes[j];
                if(io_size>file_size) continue;
                
                for(int k=0; k<jit_prefetch_delay_count; k++){
                    int prefetch_delay = jit_prefetch_delays[k];

                    // Allocating the read buffer
                    char *buffer = malloc(sizeof(char)*io_size);

                    // Starting the campaign
                    int experiment_count;
                    uint64_t t0 = get_timestamp_us(), read_duration = 0;
                    for(experiment_count=0; get_timestamp_us() - t0 < DURATION_PER_EXPERIMENT_US; experiment_count++){

                        // Cleaning the cache at the beginning of each experiment
                        #ifdef WITH_LUSTRE
                        server_cache_evict(fileno(fp), 0, file_size);
                        #endif
                        client_cache_drop(fileno(fp));
                        // sleep(CACHE_DROP_DELAY_SECONDS); // cache drops might be async, so we do a small sleep

                        // Running the experimentation: prefetching, waiting for a bit, then reading!
                        server_cache_prefetch(fileno(fp), 0, file_size);
                        usleep(prefetch_delay);
                        fseek(fp, 0, SEEK_SET);
                        uint64_t t1 = get_timestamp_us();
                        for(size_t volume = 0; volume<file_size; volume+=io_size){
                            int ret = fread(buffer, sizeof(char), io_size, fp);
                            if(__glibc_unlikely(ret < 0)){
                                printf("Error reading file \"%s\": %s\n", TARGET_FILE, strerror(errno));
                                exit(0);
                            }
                            if(io_interarrival_time_ns!=0){
                            read_duration += get_timestamp_us()-t1;
                            nsleep(io_interarrival_time_ns);
                            t1 = get_timestamp_us();
                        }
                        }
                        read_duration += get_timestamp_us()-t1;
                    }
                    fprintf(output_file, "target='%s', category='JIT prefetch', label='JIT ladvise prefetch of the whole file', "
                        #if OUTPUT_EXPERIMENT_DESCRIPTION
                        "desc='File was prefetched to the server page cache using llu_ladvise %d seconds before the reading started', "
                        #endif
                        "file_size=%llu, interarrival_time_us=%llu, io_size=%llu, prefetch_delay=%d, throughput_gb_per_second=%.3f\n", target_file, file_size, io_interarrival_time_ns, io_size, prefetch_delay,
                        prefetch_delay, experiment_count*file_size/(read_duration*1e-6)/(1ul << 30));
                    free(buffer);
                    fflush(output_file);
                }
            }
        }
        #endif

        // JIT forcefully prefetched using async-io
        for(int i = 0; i<file_size_count; i++){
            uint64_t file_size = file_sizes[i];

            for(int j = 0; j<io_size_count; j++){
                uint64_t io_size = io_sizes[j];
                if(io_size>file_size) continue;
                
                for(int k=0; k<jit_prefetch_delay_count; k++){
                    int prefetch_delay = jit_prefetch_delays[k];

                    // Allocating the read buffer
                    char *buffer = malloc(sizeof(char)*io_size);

                    // Starting the campaign
                    int experiment_count;
                    uint64_t t0 = get_timestamp_us(), read_duration = 0;
                    for(experiment_count=0; get_timestamp_us() - t0 < DURATION_PER_EXPERIMENT_US; experiment_count++){

                        // Cleaning the cache at the beginning of each experiment
                        #ifdef WITH_LUSTRE
                        server_cache_evict(fileno(fp), 0, file_size);
                        #endif
                        client_cache_drop(fileno(fp));
                        // sleep(CACHE_DROP_DELAY_SECONDS); // cache drops might be async, so we do a small sleep

                        // Running the experimentation: prefetching, waiting for a bit, then reading!
                        aio_prefetch(fileno(fp), 0, file_size);
                        usleep(prefetch_delay);
                        fseek(fp, 0, SEEK_SET);
                        uint64_t t1 = get_timestamp_us();
                        for(size_t volume = 0; volume<file_size; volume+=io_size){
                            int ret = fread(buffer, sizeof(char), io_size, fp);
                            if(__glibc_unlikely(ret < 0)){
                                printf("Error reading file \"%s\": %s\n", TARGET_FILE, strerror(errno));
                                exit(0);
                            }
                            if(io_interarrival_time_ns!=0){
                            read_duration += get_timestamp_us()-t1;
                            nsleep(io_interarrival_time_ns);
                            t1 = get_timestamp_us();
                        }
                        }
                        read_duration += get_timestamp_us()-t1;
                    }
                    fprintf(output_file, "target='%s', category='JIT prefetch', label='JIT async-io prefetch of the whole file', "
                        #if OUTPUT_EXPERIMENT_DESCRIPTION
                        "desc='File was prefetched to the server page cache using llu_ladvise %d seconds before the reading started', "
                        #endif
                        "file_size=%llu, interarrival_time_us=%llu, io_size=%llu, prefetch_delay=%d, throughput_gb_per_second=%.3f\n", target_file, file_size, io_interarrival_time_ns, io_size, prefetch_delay,
                        prefetch_delay, experiment_count*file_size/(read_duration*1e-6)/(1ul << 30));
                    free(buffer);
                    fflush(output_file);
                }
            }
        }
        fclose(fp);
    }
}

void perform_online_prefetch_benchmark(char *target_file, FILE *output_file){

    for(int h = 0; h<io_interarrival_time_count; h++){
        uint64_t io_interarrival_time_ns = io_interarrival_times[h];
        FILE *fp = fopen(TARGET_FILE, "r");

        // Dynamic fadvise + ladvise prefetching
        #ifdef WITH_LUSTRE
        for(int i = 0; i<file_size_count; i++){
            uint64_t file_size = file_sizes[i];

            for(int j = 0; j<io_size_count; j++){
                uint64_t io_size = io_sizes[j];
                if(io_size>file_size) continue;
                
                for(int k=0; k<io_size_count; k++){
                    uint64_t prefetch_size = io_sizes[k];
                    if(prefetch_size<=io_size) continue;

                    // Allocating the read buffer
                    char *buffer = malloc(sizeof(char)*io_size);

                    // Starting the campaign
                    int experiment_count;
                    uint64_t t0 = get_timestamp_us(), read_duration = 0;
                    for(experiment_count=0; get_timestamp_us() - t0 < DURATION_PER_EXPERIMENT_US; experiment_count++){

                        // Cleaning the cache at the beginning of each experiment
                        #ifdef WITH_LUSTRE
                        server_cache_evict(fileno(fp), 0, file_size);
                        #endif
                        client_cache_drop(fileno(fp));
                        // sleep(CACHE_DROP_DELAY_SECONDS); // cache drops might be async, so we do a small sleep

                        // Running the experimentation: reading, and sometimes prefetching!
                        fseek(fp, 0, SEEK_SET);
                        uint64_t t1 = get_timestamp_us();
                        for(size_t volume = 0; volume<file_size; volume+=io_size){
                            if(volume%prefetch_size==0){
                                server_cache_prefetch(fileno(fp), volume, prefetch_size);
                                client_cache_prefetch(fileno(fp), volume, prefetch_size);
                            }
                            int ret = fread(buffer, sizeof(char), io_size, fp);
                            if(__glibc_unlikely(ret < 0)){
                                printf("Error reading file \"%s\": %s\n", TARGET_FILE, strerror(errno));
                                exit(0);
                            }
                            if(io_interarrival_time_ns!=0){
                            read_duration += get_timestamp_us()-t1;
                            nsleep(io_interarrival_time_ns);
                            t1 = get_timestamp_us();
                        }
                        }
                        read_duration += get_timestamp_us()-t1;
                    }
                    fprintf(output_file, "target='%s', category='Online prefetch', label='fadvise+ladvise online prefetching', "
                        #if OUTPUT_EXPERIMENT_DESCRIPTION
                        "desc='The file is prefetched using %llu bytes llu_ladvise AND fadvise prefetches', "
                        #endif
                        "file_size=%llu, interarrival_time_us=%llu, io_size=%llu, prefetch_size=%llu, throughput_gb_per_second=%.3f\n", target_file, file_size, io_interarrival_time_ns, io_size, prefetch_size, 
                        prefetch_size, experiment_count*file_size/(read_duration*1e-6)/(1ul << 30));
                    free(buffer);
                    fflush(output_file);
                }
            }
        }
        #endif

        // Dynamic aio_read + ladvise prefetching
        #ifdef WITH_LUSTRE
        for(int i = 0; i<file_size_count; i++){
            uint64_t file_size = file_sizes[i];

            for(int j = 0; j<io_size_count; j++){
                uint64_t io_size = io_sizes[j];
                if(io_size>file_size) continue;
                
                for(int k=0; k<io_size_count; k++){
                    uint64_t prefetch_size = io_sizes[k];
                    if(prefetch_size<=io_size) continue;

                    // Allocating the read buffer
                    char *buffer = malloc(sizeof(char)*io_size);

                    // Starting the campaign
                    int experiment_count;
                    uint64_t t0 = get_timestamp_us(), read_duration = 0;
                    for(experiment_count=0; get_timestamp_us() - t0 < DURATION_PER_EXPERIMENT_US; experiment_count++){

                        // Cleaning the cache at the beginning of each experiment
                        #ifdef WITH_LUSTRE
                        server_cache_evict(fileno(fp), 0, file_size);
                        #endif
                        client_cache_drop(fileno(fp));
                        // sleep(CACHE_DROP_DELAY_SECONDS); // cache drops might be async, so we do a small sleep

                        // Running the experimentation: reading, and sometimes prefetching!
                        fseek(fp, 0, SEEK_SET);
                        uint64_t t1 = get_timestamp_us();
                        for(size_t volume = 0; volume<file_size; volume+=io_size){
                            if(volume%prefetch_size==0){
                                server_cache_prefetch(fileno(fp), volume, prefetch_size);
                                aio_prefetch(fileno(fp), volume, prefetch_size);
                            }
                            int ret = fread(buffer, sizeof(char), io_size, fp);
                            if(__glibc_unlikely(ret < 0)){
                                printf("Error reading file \"%s\": %s\n", TARGET_FILE, strerror(errno));
                                exit(0);
                            }
                            if(io_interarrival_time_ns!=0){
                            read_duration += get_timestamp_us()-t1;
                            nsleep(io_interarrival_time_ns);
                            t1 = get_timestamp_us();
                        }
                        }
                        read_duration += get_timestamp_us()-t1;
                    }
                    fprintf(output_file, "target='%s', category='Online prefetch', label='aio_read+ladvise online prefetching', "
                        #if OUTPUT_EXPERIMENT_DESCRIPTION
                        "desc='The file is prefetched using %llu bytes llu_ladvise AND fadvise prefetches', "
                        #endif
                        "file_size=%llu, interarrival_time_us=%llu, io_size=%llu, prefetch_size=%llu, throughput_gb_per_second=%.3f\n", target_file, file_size, io_interarrival_time_ns, io_size, prefetch_size, 
                        prefetch_size, experiment_count*file_size/(read_duration*1e-6)/(1ul << 30));
                    free(buffer);
                    fflush(output_file);
                }
            }
        }
        #endif

        // Dynamic fadvise prefetching
        for(int i = 0; i<file_size_count; i++){
            uint64_t file_size = file_sizes[i];

            for(int j = 0; j<io_size_count; j++){
                uint64_t io_size = io_sizes[j];
                if(io_size>file_size) continue;
                
                for(int k=0; k<io_size_count; k++){
                    uint64_t prefetch_size = io_sizes[k];
                    if(prefetch_size<=io_size) continue;

                    // Allocating the read buffer
                    char *buffer = malloc(sizeof(char)*io_size);

                    // Starting the campaign
                    int experiment_count;
                    uint64_t t0 = get_timestamp_us(), read_duration = 0;
                    for(experiment_count=0; get_timestamp_us() - t0 < DURATION_PER_EXPERIMENT_US; experiment_count++){

                        // Cleaning the cache at the beginning of each experiment
                        #ifdef WITH_LUSTRE
                        server_cache_evict(fileno(fp), 0, file_size);
                        #endif
                        client_cache_drop(fileno(fp));
                        // sleep(CACHE_DROP_DELAY_SECONDS); // cache drops might be async, so we do a small sleep
                        

                        // Running the experimentation: reading, and sometimes prefetching!
                        fseek(fp, 0, SEEK_SET);
                        uint64_t t1 = get_timestamp_us();
                        for(size_t volume = 0; volume<file_size; volume+=io_size){
                            if(volume%prefetch_size==0) client_cache_prefetch(fileno(fp), volume, prefetch_size);
                            int ret = fread(buffer, sizeof(char), io_size, fp);
                            if(__glibc_unlikely(ret < 0)){
                                printf("Error reading file \"%s\": %s\n", TARGET_FILE, strerror(errno));
                                exit(0);
                            }
                            if(io_interarrival_time_ns!=0){
                            read_duration += get_timestamp_us()-t1;
                            nsleep(io_interarrival_time_ns);
                            t1 = get_timestamp_us();
                        }
                        }
                        read_duration += get_timestamp_us()-t1;
                    }
                    fprintf(output_file, "target='%s', category='Online prefetch', label='fadvise online prefetching', "
                        #if OUTPUT_EXPERIMENT_DESCRIPTION
                        "desc='The file is prefetched using %llu bytes fadvise prefetches', "
                        #endif
                        "file_size=%llu, interarrival_time_us=%llu, io_size=%llu, prefetch_size=%llu, throughput_gb_per_second=%.3f\n", target_file, file_size, io_interarrival_time_ns, io_size, prefetch_size, 
                        prefetch_size, experiment_count*file_size/(read_duration*1e-6)/(1ul << 30));
                    free(buffer);
                    fflush(output_file);
                }
            }
        }

        // Dynamic llapi_ladvise prefetching
        #ifdef WITH_LUSTRE
        for(int i = 0; i<file_size_count; i++){
            uint64_t file_size = file_sizes[i];

            for(int j = 0; j<io_size_count; j++){
                uint64_t io_size = io_sizes[j];
                if(io_size>file_size) continue;
                
                for(int k=0; k<io_size_count; k++){
                    uint64_t prefetch_size = io_sizes[k];
                    if(prefetch_size<=io_size) continue;

                    // Allocating the read buffer
                    char *buffer = malloc(sizeof(char)*io_size);

                    // Starting the campaign
                    int experiment_count;
                    uint64_t t0 = get_timestamp_us(), read_duration = 0;
                    for(experiment_count=0; get_timestamp_us() - t0 < DURATION_PER_EXPERIMENT_US; experiment_count++){

                        // Cleaning the cache at the beginning of each experiment
                        #ifdef WITH_LUSTRE
                        server_cache_evict(fileno(fp), 0, file_size);
                        #endif
                        client_cache_drop(fileno(fp));
                        // sleep(CACHE_DROP_DELAY_SECONDS); // cache drops might be async, so we do a small sleep

                        // Running the experimentation: reading, and sometimes prefetching!
                        fseek(fp, 0, SEEK_SET);
                        uint64_t t1 = get_timestamp_us();
                        for(size_t volume = 0; volume<file_size; volume+=io_size){
                            if(volume%prefetch_size==0) server_cache_prefetch(fileno(fp), volume, prefetch_size);
                            int ret = fread(buffer, sizeof(char), io_size, fp);
                            if(__glibc_unlikely(ret < 0)){
                                printf("Error reading file \"%s\": %s\n", TARGET_FILE, strerror(errno));
                                exit(0);
                            }
                            if(io_interarrival_time_ns!=0){
                            read_duration += get_timestamp_us()-t1;
                            nsleep(io_interarrival_time_ns);
                            t1 = get_timestamp_us();
                        }
                        }
                        read_duration += get_timestamp_us()-t1;
                    }
                    fprintf(output_file, "target='%s', category='Online prefetch', label='ladvise online prefetching', "
                        #if OUTPUT_EXPERIMENT_DESCRIPTION
                        "desc='The file is prefetched using %llu bytes llu_ladvise prefetches', "
                        #endif
                        "file_size=%llu, interarrival_time_us=%llu, io_size=%llu, prefetch_size=%llu, throughput_gb_per_second=%.3f\n", target_file, file_size, io_interarrival_time_ns, io_size, prefetch_size, 
                        prefetch_size, experiment_count*file_size/(read_duration*1e-6)/(1ul << 30));
                    free(buffer);
                    fflush(output_file);
                }
            }
        }
        #endif

        // Dynamic aio_read prefetching
        for(int i = 0; i<file_size_count; i++){
            uint64_t file_size = file_sizes[i];

            for(int j = 0; j<io_size_count; j++){
                uint64_t io_size = io_sizes[j];
                if(io_size>file_size) continue;
                
                for(int k=0; k<io_size_count; k++){
                    uint64_t prefetch_size = io_sizes[k];
                    if(prefetch_size<=io_size) continue;

                    // Allocating the read buffer
                    char *buffer = malloc(sizeof(char)*io_size);

                    // Starting the campaign
                    int experiment_count;
                    uint64_t t0 = get_timestamp_us(), read_duration = 0;
                    for(experiment_count=0; get_timestamp_us() - t0 < DURATION_PER_EXPERIMENT_US; experiment_count++){

                        // Cleaning the cache at the beginning of each experiment
                        #ifdef WITH_LUSTRE
                        server_cache_evict(fileno(fp), 0, file_size);
                        #endif
                        client_cache_drop(fileno(fp));
                        // sleep(CACHE_DROP_DELAY_SECONDS); // cache drops might be async, so we do a small sleep

                        // Running the experimentation: reading, and sometimes prefetching!
                        fseek(fp, 0, SEEK_SET);
                        uint64_t t1 = get_timestamp_us();
                        for(size_t volume = 0; volume<file_size; volume+=io_size){
                            if(volume%prefetch_size==0) aio_prefetch(fileno(fp), volume, prefetch_size);
                            int ret = fread(buffer, sizeof(char), io_size, fp);
                            if(__glibc_unlikely(ret < 0)){
                                printf("Error reading file \"%s\": %s\n", TARGET_FILE, strerror(errno));
                                exit(0);
                            }
                            if(io_interarrival_time_ns!=0){
                            read_duration += get_timestamp_us()-t1;
                            nsleep(io_interarrival_time_ns);
                            t1 = get_timestamp_us();
                        }
                        }
                        read_duration += get_timestamp_us()-t1;
                    }
                    fprintf(output_file, "target='%s', category='Online prefetch', label='async-io online prefetching', "
                        #if OUTPUT_EXPERIMENT_DESCRIPTION
                        "desc='The file is prefetched using %llu bytes llu_ladvise prefetches', "
                        #endif
                        "file_size=%llu, interarrival_time_us=%llu, io_size=%llu, prefetch_size=%llu, throughput_gb_per_second=%.3f\n", target_file, file_size, io_interarrival_time_ns, io_size, prefetch_size, 
                        prefetch_size, experiment_count*file_size/(read_duration*1e-6)/(1ul << 30));
                    free(buffer);
                    fflush(output_file);
                }
            }
        }
        fclose(fp);
    }
}

int main(int argc, char **argv){
    #ifdef WITH_LUSTRE
    FILE *log_file = fopen("output-lustre.csv", "w");
    #else
    FILE *log_file = fopen("output.csv", "w");
    #endif
    if(log_file < 0){
        printf("Error opening file \"output.csv\": %s\n", strerror(errno));
        exit(0);
    }
    perform_baseline_benchmark(TARGET_FILE, log_file);
    perform_offline_prefetch_benchmark(TARGET_FILE, log_file);
    perform_jit_prefetch_benchmark(TARGET_FILE, log_file);
    perform_online_prefetch_benchmark(TARGET_FILE, log_file);
    fclose(log_file);
}

// Used for throughput instrumentation 
static inline uint64_t get_timestamp_us(){
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec*(uint64_t)1e6+tv.tv_usec;
}

// Use /proc/sys/vm/drop_caches to drop the client page cache 
static inline void client_cache_drop(int fd){
    sync();
    char *data = "3";
    int fd2 = open("/proc/sys/vm/drop_caches", O_WRONLY);
    if(fd2<0){
        printf("Could not open file \"/proc/sys/vm/drop_caches\": %s\n", strerror(errno));
        exit(0);
    }
    int ret = write(fd2, data, sizeof(char));
    if(ret<0){
        printf("Could not write in file \"/proc/sys/vm/drop_caches\": %s\n", strerror(errno));
        exit(0);
    }
    close(fd2);

    #ifndef WITH_LUSTRE
    char *buffer = malloc(SECONDARY_IO_SIZE);
    for(int i = 0; i<SECONDARY_IO_COUNT; i++){
        read(fd, buffer, SECONDARY_IO_SIZE);
    }
    free(buffer);
    #endif
    sync();
}

// Use fadvise to evict some data from the client page cache 
static inline void client_cache_evict(int fd, uint64_t offset, uint64_t length){
    posix_fadvise(fd, offset, offset+length, POSIX_FADV_DONTNEED);
}

// Use lla_ladvise to evict some data from the server page cache
#ifdef WITH_LUSTRE
static inline void server_cache_evict(int fd, uint64_t offset, uint64_t length){
    struct llapi_lu_ladvise advises;
    memset(&advises, 0, sizeof(struct llapi_lu_ladvise));
    advises.lla_advice = LU_LADVISE_DONTNEED;
    advises.lla_start = offset;
    advises.lla_end = offset+length;
    llapi_ladvise(fd, 0, 1, &advises);
}
#endif

// Use fadvise to prefetch some data to the client page cache  
static inline void client_cache_prefetch(int fd, uint64_t offset, uint64_t length){
    posix_fadvise(fd, offset, length, POSIX_FADV_WILLNEED);
}

// Use lla_ladvise to prefetch some data to the server page cache
#ifdef WITH_LUSTRE
static inline void server_cache_prefetch(int fd, uint64_t offset, uint64_t length){
    struct llapi_lu_ladvise advises;
    memset(&advises, 0, sizeof(struct llapi_lu_ladvise));
    advises.lla_advice = LU_LADVISE_WILLREAD;
    advises.lla_start = offset;
    advises.lla_end = offset + length;
    llapi_ladvise(fd, 0, 1, &advises);
}
#endif

// Use asynchronous I/O to forcefully prefetch some data 
static inline void aio_prefetch(int fd, uint64_t offset, uint64_t length){
    static char *buffer = 0;
    if(buffer == 0) buffer = malloc(sizeof(char)*file_sizes[file_size_count-1]);
    off_t current_offset = lseek(fd, 0, SEEK_CUR);
    struct aiocb aiocbp;
    memset(&aiocbp, 0, sizeof(struct aiocb));
    aiocbp.aio_fildes = fd;
    aiocbp.aio_buf = buffer;
    aiocbp.aio_nbytes = length;
    aiocbp.aio_offset = offset; // No need to lseek
    aiocbp.aio_sigevent.sigev_notify = SIGEV_NONE;
    aio_read(&aiocbp);
    lseek(fd, current_offset, SEEK_SET);
}
