# IO-Hints-Benchmark
# Requirements
* librt for asynchronous I/Os
* liblustreapi for ladvise
* CMake
* GCC
* libasan for memory safety (only when compiling in Debug mode)
# Installation
Run the command `cmake -DCMAKE_BUILD_TYPE=Release .` to compile.
# Configuration
Two benchmarks binaries are created: one for the sequential I/O pattern one for the random I/O pattern. Benchmark parameters are available as constants at the top of each benchmark source file.
