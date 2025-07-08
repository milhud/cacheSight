#ifndef HARDWARE_DETECTOR_H
#define HARDWARE_DETECTOR_H

#include "common.h"

// Cache level information
typedef struct {
    int level;                  // Cache level (1, 2, 3, etc.)
    size_t size;               // Cache size in bytes
    size_t line_size;          // Cache line size in bytes
    int associativity;         // Associativity (e.g., 8-way)
    char type[16];             // "data", "instruction", "unified"
    int latency_cycles;        // Estimated latency in CPU cycles
    bool inclusive;            // Is this cache inclusive of lower levels?
    int sets;                  // Number of sets
    bool shared;               // Shared across cores?
    int sharing_cpu_count;     // Number of CPUs sharing this cache
} cache_level_t;

// Complete cache hierarchy information
typedef struct {
    cache_level_t levels[8];    // Support up to 8 cache levels
    int num_levels;             // Actual number of cache levels
    int num_cores;              // Number of CPU cores
    int num_threads;            // Number of hardware threads
    char arch[32];              // Architecture: "x86_64", "aarch64"
    int page_size;              // Memory page size
    size_t memory_bandwidth_gbps; // Estimated memory bandwidth
    size_t total_memory;        // Total system memory
    int numa_nodes;             // Number of NUMA nodes
    char cpu_model[256];        // CPU model name
    int cpu_family;             // CPU family
    int cpu_model_num;          // CPU model number
    double cpu_frequency_ghz;   // CPU frequency in GHz
} cache_info_t;

// Main API functions
int hardware_detector_init(void);
void hardware_detector_cleanup(void);
int detect_cache_hierarchy(cache_info_t *info);
void print_cache_info(const cache_info_t *info);
int save_cache_info_to_file(const cache_info_t *info, const char *filename);
int load_cache_info_from_file(cache_info_t *info, const char *filename);

// Helper functions
int get_cpu_count(void);
int get_numa_node_count(void);
size_t get_total_memory(void);
int get_page_size(void);
const char* get_architecture(void);

#endif // HARDWARE_DETECTOR_H
