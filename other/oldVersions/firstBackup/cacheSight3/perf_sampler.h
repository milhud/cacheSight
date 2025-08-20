#ifndef PERF_SAMPLER_H
#define PERF_SAMPLER_H

#include "common.h"
#include "hardware_detector.h"

// Cache miss sample structure
typedef struct {
    uint64_t instruction_addr;   // Instruction address that caused miss
    uint64_t memory_addr;        // Memory address accessed
    uint64_t timestamp;          // Time of sample
    source_location_t source_loc; // Source code location
    int cache_level_missed;      // Which cache level (1, 2, 3)
    int cpu_id;                  // CPU core ID
    uint32_t access_size;        // Size of access in bytes
    bool is_write;               // Read or write access
    uint64_t latency_cycles;     // Access latency in cycles
    pid_t tid;                   // Thread ID
} cache_miss_sample_t;

// Sampling configuration
typedef struct {
    uint64_t sample_period;      // Sample every N events
    int max_samples;             // Maximum samples to collect
    bool sample_all_cpus;        // Sample all CPUs or just current
    bool include_kernel;         // Include kernel samples
    int cache_levels_mask;       // Bitmask of cache levels to monitor
    double sampling_duration;    // Duration in seconds (0 = until stopped)
} perf_config_t;

// Perf sampler state
typedef struct perf_sampler perf_sampler_t;

// API functions
perf_sampler_t* perf_sampler_create(const perf_config_t *config);
void perf_sampler_destroy(perf_sampler_t *sampler);

int perf_sampler_start(perf_sampler_t *sampler);
int perf_sampler_stop(perf_sampler_t *sampler);
bool perf_sampler_is_running(const perf_sampler_t *sampler);

// Get samples
int perf_sampler_get_samples(perf_sampler_t *sampler, 
                            cache_miss_sample_t **samples, int *count);
void perf_sampler_free_samples(cache_miss_sample_t *samples);

// Configuration helpers
perf_config_t perf_config_default(void);
int perf_check_permissions(void);
const char* perf_get_error_string(int error_code);

// Statistics
typedef struct {
    uint64_t total_samples;
    uint64_t l1_misses;
    uint64_t l2_misses;
    uint64_t l3_misses;
    uint64_t llc_misses;         // Last level cache
    double avg_latency;
    uint64_t sampling_duration_ns;
} perf_stats_t;

int perf_sampler_get_stats(const perf_sampler_t *sampler, perf_stats_t *stats);
void perf_print_stats(const perf_stats_t *stats);

#endif // PERF_SAMPLER_H
