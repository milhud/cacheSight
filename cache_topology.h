#ifndef CACHE_TOPOLOGY_H
#define CACHE_TOPOLOGY_H

#include "common.h"
#include "hardware_detector.h"

// Cache topology analysis structures
typedef struct {
    int cpu_id;
    int core_id;
    int socket_id;
    int numa_node;
    uint64_t cache_mask[8];  // Bit mask for each cache level
} cpu_topology_t;

typedef struct {
    cpu_topology_t *cpus;
    int num_cpus;
    int num_sockets;
    int num_numa_nodes;
    int **numa_distance;     // NUMA distance matrix
} system_topology_t;

// Cache performance characteristics
typedef struct {
    double hit_rate_estimate[8];      // Estimated hit rate per level
    double effective_latency[8];      // Effective latency per level
    double bandwidth_per_level[8];    // Bandwidth per cache level
    double miss_penalty[8];           // Miss penalty per level
} cache_performance_t;

// API functions
int analyze_cache_topology(const cache_info_t *cache_info, system_topology_t *topology);
void free_system_topology(system_topology_t *topology);
int estimate_cache_performance(const cache_info_t *cache_info, cache_performance_t *perf);
void print_system_topology(const system_topology_t *topology);
void print_cache_performance(const cache_performance_t *perf);

// Helper functions
int get_cpu_topology(int cpu_id, cpu_topology_t *topo);
int get_numa_distances(int **distances, int num_nodes);
int which_cpus_share_cache(int cpu_id, int cache_level, int *cpu_list, int max_cpus);

#endif // CACHE_TOPOLOGY_H
