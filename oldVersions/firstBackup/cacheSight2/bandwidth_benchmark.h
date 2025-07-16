#ifndef BANDWIDTH_BENCHMARK_H
#define BANDWIDTH_BENCHMARK_H

#include "common.h"
#include "hardware_detector.h"

// Benchmark results structure
typedef struct {
    double sequential_read_gbps;
    double sequential_write_gbps;
    double random_read_gbps;
    double random_write_gbps;
    double copy_bandwidth_gbps;
    double latency_ns[8];  // Latency per cache level
    double cache_bandwidth_gbps[8];  // Bandwidth per cache level
} bandwidth_results_t;

// Benchmark configuration
typedef struct {
    size_t min_size;       // Minimum buffer size
    size_t max_size;       // Maximum buffer size
    int iterations;        // Number of iterations per test
    int warmup_runs;       // Number of warmup runs
    bool use_numa_binding; // Bind to NUMA node
    int numa_node;         // NUMA node to bind to
} benchmark_config_t;

// API functions
int measure_memory_bandwidth(const cache_info_t *cache_info, bandwidth_results_t *results);
int measure_cache_latency(const cache_info_t *cache_info, bandwidth_results_t *results);
void print_bandwidth_results(const bandwidth_results_t *results);

// Individual benchmark functions
double benchmark_sequential_read(void *buffer, size_t size, int iterations);
double benchmark_sequential_write(void *buffer, size_t size, int iterations);
double benchmark_random_read(void *buffer, size_t size, int iterations);
double benchmark_random_write(void *buffer, size_t size, int iterations);
double benchmark_memory_copy(void *src, void *dst, size_t size, int iterations);
double measure_access_latency(void *buffer, size_t size, size_t stride);

// Helper functions
void* allocate_aligned_buffer(size_t size, size_t alignment);
void free_aligned_buffer(void *buffer);
void flush_cache(void *buffer, size_t size);
void warm_cache(void *buffer, size_t size);

#endif // BANDWIDTH_BENCHMARK_H
