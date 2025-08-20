# cacheSight: Cache Optimization Tool

## Work In Progress

This tool is very much a work in progress, mainly concerning deduplication issues that must be rectified. Forthcoming fixes should fix these problems. Stay tuned, and feel free to contribute when able. All noted issues should be documented in the "Issues" tab.

## Features
- static analysis: AST-based pattern detection with Clang
- dynamic profiling: perfomance counter sampling with perf_event
- hardware detection: automatic cache hierarchy discovery
- pattern classification: identifies 15+ cache access patterns
- provides actionable recommendations

## Detected Cache Patterns
- sequential - recommends vectorization, prefetching
- strided - recommends loop tiling, gather instructions
- random - recommends data strcuture reorgnization, sorting
- gather_scatter - recommends AoS to SoA transformation
- loop_carried_dep - recommends loop unrolling, scalar replacement
- nested_loop - recommends loop interchange, tiling
- indirect_access - recommends prefetching, data layout changes
- cache_thrashing - recommends loop tiling, blocking
- false_sharing - recommends paddling, alignment
- streaming_eviction - gives non-temporal hints
- bank_conflicts - recommends paddling, prime dimensions
- capacity_misses - recommends data compression, tiling
- conflict_misses - recommends array padding
- coherency_misses - recommends thread-local storage

## Requirements

System Requirements:

- Linux (kernel 3.2+ with perf_event support)
- x86_64 architecture
- GCC 7+ or Clang 8+
- GNU Make

## Dependencies
```bash
# Ubuntu/Debian
sudo apt-get install \
    build-essential \
    cmake \
    libclang-dev \
    llvm-dev \
    libpfm4-dev \
    libomp-dev

# RHEL/CentOS
sudo yum install \
    gcc \
    gcc-c++ \
    cmake \
    clang-devel \
    llvm-devel \
    libpfm-devel \
    libomp-devel
```

## To Use

```bash
git clone https://github.com/milhud/cacheSight.git
cd cacheSight
make
```
This will generate an executable for you to use.

## Command Line Options

```bash
Usage: cache_analyzer [options] <source files>

Options:
  -o, --output <file>      Output report filename (default: report.html)
  -c, --config <file>      Configuration file
  -t, --time <seconds>     Profiling duration (default: 10.0)
  -s, --static-only        Run static analysis only
  -d, --dynamic-only       Run dynamic profiling only
  -v, --verbose           Enable verbose output
  -q, --quiet             Suppress non-error output
  --version               Show version information
  -h, --help              Show this help message

Advanced Options:
  --sample-rate <rate>     Perf sampling rate (default: 1000)
  --min-samples <count>    Minimum samples for hotspot (default: 100)
  --cache-info <file>      Override detected cache info
  --no-recommendations     Skip recommendation generation

```

Can also use a config file called `analyzer.conf`:

```
[general]
output_format = html
verbose_level = 1
log_file = analyzer.log

[static_analysis]
enabled = true
detect_patterns = all
min_confidence = 0.7

[dynamic_profiling]
enabled = true
duration = 10.0
sample_rate = 1000
events = LLC-MISSES,L1-DCACHE-MISSES

[recommendations]
enabled = true
min_severity = medium
max_recommendations = 100

[hardware]
auto_detect = true
l1_size = 32768
l2_size = 262144
l3_size = 8388608
cache_line_size = 64
```

