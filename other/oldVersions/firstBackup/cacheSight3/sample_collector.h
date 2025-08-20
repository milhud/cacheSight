#ifndef SAMPLE_COLLECTOR_H
#define SAMPLE_COLLECTOR_H

#include "common.h"
#include "perf_sampler.h"
#include "hardware_detector.h"

// Cache hotspot information
typedef struct {
    source_location_t location;      // Source code location
    uint64_t total_misses;          // Total cache misses
    uint64_t total_accesses;        // Total memory accesses
    double avg_latency_cycles;      // Average access latency
    access_pattern_t dominant_pattern; // Most common access pattern
    cache_miss_sample_t *samples;   // Raw samples for this hotspot
    size_t sample_count;            // Number of samples
    size_t sample_capacity;         // Allocated capacity
    uint64_t address_range_start;   // Start of accessed memory range
    uint64_t address_range_end;     // End of accessed memory range
    int cache_levels_affected[4];   // Miss counts per cache level
    double miss_rate;               // Cache miss rate (0-1)
    bool is_false_sharing;          // Potential false sharing detected
} cache_hotspot_t;

// Sample collector state
typedef struct sample_collector sample_collector_t;

// Collection configuration
typedef struct {
    int min_samples_per_hotspot;    // Minimum samples to form hotspot
    double hotspot_threshold;       // Miss rate threshold
    bool aggregate_by_function;     // Group by function vs line
    bool detect_false_sharing;      // Enable false sharing detection
    size_t max_hotspots;           // Maximum hotspots to track
} collector_config_t;

// API functions
sample_collector_t* sample_collector_create(const collector_config_t *config,
                                           const cache_info_t *cache_info);
void sample_collector_destroy(sample_collector_t *collector);

// Add samples
int sample_collector_add_samples(sample_collector_t *collector,
                                const cache_miss_sample_t *samples, int count);
int sample_collector_add_sample(sample_collector_t *collector,
                               const cache_miss_sample_t *sample);

// Process and aggregate samples
int sample_collector_process(sample_collector_t *collector);

// Get hotspots
int sample_collector_get_hotspots(sample_collector_t *collector,
                                 cache_hotspot_t **hotspots, int *count);
void sample_collector_free_hotspots(cache_hotspot_t *hotspots, int count);

// Analysis functions
int sample_collector_analyze_patterns(sample_collector_t *collector);
int sample_collector_detect_false_sharing(sample_collector_t *collector);

// Statistics and reporting
typedef struct {
    uint64_t total_samples_processed;
    uint64_t total_unique_addresses;
    uint64_t total_unique_instructions;
    int hotspot_count;
    double avg_samples_per_hotspot;
    uint64_t cache_line_conflicts;
} collector_stats_t;

int sample_collector_get_stats(const sample_collector_t *collector,
                              collector_stats_t *stats);
void sample_collector_print_hotspots(const cache_hotspot_t *hotspots, int count);

// Helper functions
collector_config_t collector_config_default(void);
int compare_hotspots_by_misses(const void *a, const void *b);
int compare_hotspots_by_latency(const void *a, const void *b);

#endif // SAMPLE_COLLECTOR_H
