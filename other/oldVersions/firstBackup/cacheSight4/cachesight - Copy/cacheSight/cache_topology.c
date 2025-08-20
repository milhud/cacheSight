#include "cache_topology.h"

// Parse CPU topology from /sys
int get_cpu_topology(int cpu_id, cpu_topology_t *topo) {
    LOG_DEBUG("Getting topology for CPU %d", cpu_id);
    
    char path[512];
    
    // Get core ID
    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/topology/core_id", cpu_id);
    FILE *fp = fopen(path, "r");
    if (fp) {
        fscanf(fp, "%d", &topo->core_id);
        fclose(fp);
    }
    
    // Get socket/package ID
    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/topology/physical_package_id", cpu_id);
    fp = fopen(path, "r");
    if (fp) {
        fscanf(fp, "%d", &topo->socket_id);
        fclose(fp);
    }
    
    // Get NUMA node (if NUMA system)
    topo->numa_node = 0;  // Default to node 0
    for (int node = 0; node < 8; node++) {
        snprintf(path, sizeof(path), "/sys/devices/system/node/node%d/cpu%d", node, cpu_id);
        if (access(path, F_OK) == 0) {
            topo->numa_node = node;
            break;
        }
    }
    
    topo->cpu_id = cpu_id;
    
    // Get cache sharing masks
    for (int level = 0; level < 8; level++) {
        topo->cache_mask[level] = 0;
        
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cache/index%d/shared_cpu_map", cpu_id, level);
        fp = fopen(path, "r");
        if (fp) {
            // Parse CPU mask (simplified - assumes < 64 CPUs)
            char mask_str[256];
            if (fgets(mask_str, sizeof(mask_str), fp)) {
                // Convert hex string to bitmask
                topo->cache_mask[level] = strtoull(mask_str, NULL, 16);
                LOG_DEBUG("CPU %d cache level %d mask: 0x%lx", cpu_id, level, topo->cache_mask[level]);
            }
            fclose(fp);
        }
    }
    
    return 0;
}

// Get NUMA distance matrix
int get_numa_distances(int **distances, int num_nodes) {
    LOG_DEBUG("Getting NUMA distances for %d nodes", num_nodes);
    
    for (int i = 0; i < num_nodes; i++) {
        for (int j = 0; j < num_nodes; j++) {
            char path[512];
            snprintf(path, sizeof(path), "/sys/devices/system/node/node%d/distance", i);
            
            FILE *fp = fopen(path, "r");
            if (fp) {
                // Read distance values
                for (int k = 0; k <= j && k < num_nodes; k++) {
                    fscanf(fp, "%d", &distances[i][k]);
                }
                fclose(fp);
            } else {
                // Default: 10 for same node, 20 for different
                distances[i][j] = (i == j) ? 10 : 20;
            }
        }
    }
    
    return 0;
}

// Analyze complete system topology
int analyze_cache_topology(const cache_info_t *cache_info, system_topology_t *topology) {
    LOG_INFO("Analyzing cache topology for %d CPUs", cache_info->num_threads);
    
    topology->num_cpus = cache_info->num_threads;
    topology->cpus = CALLOC_LOGGED(topology->num_cpus, sizeof(cpu_topology_t));
    if (!topology->cpus) {
        LOG_ERROR("Failed to allocate CPU topology array");
        return -1;
    }
    
    // Get topology for each CPU
    int max_socket = 0;
    int max_numa = 0;
    
    for (int cpu = 0; cpu < topology->num_cpus; cpu++) {
        get_cpu_topology(cpu, &topology->cpus[cpu]);
        
        if (topology->cpus[cpu].socket_id > max_socket) {
            max_socket = topology->cpus[cpu].socket_id;
        }
        if (topology->cpus[cpu].numa_node > max_numa) {
            max_numa = topology->cpus[cpu].numa_node;
        }
        
        LOG_DEBUG("CPU %d: Core %d, Socket %d, NUMA %d",
                  cpu, topology->cpus[cpu].core_id,
                  topology->cpus[cpu].socket_id,
                  topology->cpus[cpu].numa_node);
    }
    
    topology->num_sockets = max_socket + 1;
    topology->num_numa_nodes = max_numa + 1;
    
    // Allocate and get NUMA distances
    if (topology->num_numa_nodes > 1) {
        topology->numa_distance = CALLOC_LOGGED(topology->num_numa_nodes, sizeof(int*));
        if (topology->numa_distance) {
            for (int i = 0; i < topology->num_numa_nodes; i++) {
                topology->numa_distance[i] = CALLOC_LOGGED(topology->num_numa_nodes, sizeof(int));
            }
            get_numa_distances(topology->numa_distance, topology->num_numa_nodes);
        }
    }
    
    LOG_INFO("Topology analysis complete: %d sockets, %d NUMA nodes",
             topology->num_sockets, topology->num_numa_nodes);
    
    return 0;
}

// Free topology structures
void free_system_topology(system_topology_t *topology) {
    LOG_DEBUG("Freeing system topology structures");
    
    if (topology->cpus) {
        FREE_LOGGED(topology->cpus);
        topology->cpus = NULL;
    }
    
    if (topology->numa_distance) {
        for (int i = 0; i < topology->num_numa_nodes; i++) {
            if (topology->numa_distance[i]) {
                FREE_LOGGED(topology->numa_distance[i]);
            }
        }
        FREE_LOGGED(topology->numa_distance);
        topology->numa_distance = NULL;
    }
}

// Estimate cache performance characteristics
int estimate_cache_performance(const cache_info_t *cache_info, cache_performance_t *perf) {
    LOG_INFO("Estimating cache performance characteristics");
    
    memset(perf, 0, sizeof(cache_performance_t));
    
    for (int i = 0; i < cache_info->num_levels && i < 8; i++) {
        const cache_level_t *cache = &cache_info->levels[i];
        
        // Estimate hit rates (typical values)
        switch (cache->level) {
            case 1:
                perf->hit_rate_estimate[i] = 0.95;  // L1 typically 95%
                break;
            case 2:
                perf->hit_rate_estimate[i] = 0.80;  // L2 typically 80%
                break;
            case 3:
                perf->hit_rate_estimate[i] = 0.50;  // L3 typically 50%
                break;
            default:
                perf->hit_rate_estimate[i] = 0.30;
        }
        
        // Calculate effective latency
        perf->effective_latency[i] = cache->latency_cycles / cache_info->cpu_frequency_ghz;
        
        // Estimate bandwidth (GB/s) - rough approximation
        // Assumes 2 loads per cycle at peak
        perf->bandwidth_per_level[i] = (cache_info->cpu_frequency_ghz * 2 * cache->line_size) / 1e9;
        
        // Miss penalty is the latency to next level
        if (i < cache_info->num_levels - 1) {
            perf->miss_penalty[i] = cache_info->levels[i+1].latency_cycles - cache->latency_cycles;
        } else {
            // Last level - penalty is main memory latency
            perf->miss_penalty[i] = 200;  // Typical DRAM latency in cycles
        }
        
        LOG_DEBUG("L%d performance: hit_rate=%.2f%%, latency=%.2fns, bandwidth=%.1fGB/s, miss_penalty=%d cycles",
                  cache->level, perf->hit_rate_estimate[i] * 100,
                  perf->effective_latency[i], perf->bandwidth_per_level[i],
                  (int)perf->miss_penalty[i]);
    }
    
    return 0;
}

// Find which CPUs share a cache level
int which_cpus_share_cache(int cpu_id, int cache_level, int *cpu_list, int max_cpus) {
    char path[512];
    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cache/index%d/shared_cpu_list", 
             cpu_id, cache_level);
    
    FILE *fp = fopen(path, "r");
    if (!fp) {
        LOG_DEBUG("Cannot open %s", path);
        return 0;
    }
    
    char buffer[1024];
    if (fgets(buffer, sizeof(buffer), fp) == NULL) {
        fclose(fp);
        return 0;
    }
    fclose(fp);
    
    // Parse CPU list (e.g., "0-3,8-11")
    int count = 0;
    char *token = strtok(buffer, ",");
    while (token && count < max_cpus) {
        int start, end;
        if (sscanf(token, "%d-%d", &start, &end) == 2) {
            for (int cpu = start; cpu <= end && count < max_cpus; cpu++) {
                cpu_list[count++] = cpu;
            }
        } else if (sscanf(token, "%d", &start) == 1) {
            cpu_list[count++] = start;
        }
        token = strtok(NULL, ",");
    }
    
    return count;
}

// Print system topology
void print_system_topology(const system_topology_t *topology) {
    printf("\n=== System Topology ===\n");
    printf("Total CPUs: %d\n", topology->num_cpus);
    printf("Sockets: %d\n", topology->num_sockets);
    printf("NUMA Nodes: %d\n", topology->num_numa_nodes);
    
    if (topology->num_numa_nodes > 1 && topology->numa_distance) {
        printf("\nNUMA Distance Matrix:\n");
        printf("     ");
        for (int j = 0; j < topology->num_numa_nodes; j++) {
            printf("N%d  ", j);
        }
        printf("\n");
        
        for (int i = 0; i < topology->num_numa_nodes; i++) {
            printf("N%d:  ", i);
            for (int j = 0; j < topology->num_numa_nodes; j++) {
                printf("%-4d", topology->numa_distance[i][j]);
            }
            printf("\n");
        }
    }
    
    printf("\nCPU Layout:\n");
    for (int socket = 0; socket < topology->num_sockets; socket++) {
        printf("Socket %d:\n", socket);
        for (int cpu = 0; cpu < topology->num_cpus; cpu++) {
            if (topology->cpus[cpu].socket_id == socket) {
                printf("  CPU %d (Core %d, NUMA %d)\n",
                       cpu, topology->cpus[cpu].core_id,
                       topology->cpus[cpu].numa_node);
            }
        }
    }
}

// Print cache performance estimates
void print_cache_performance(const cache_performance_t *perf) {
    printf("\n=== Cache Performance Estimates ===\n");
    
    for (int i = 0; i < 8 && perf->hit_rate_estimate[i] > 0; i++) {
        printf("Level %d:\n", i + 1);
        printf("  Estimated Hit Rate: %.1f%%\n", perf->hit_rate_estimate[i] * 100);
        printf("  Effective Latency: %.2f ns\n", perf->effective_latency[i]);
        printf("  Bandwidth: %.1f GB/s\n", perf->bandwidth_per_level[i]);
        printf("  Miss Penalty: %d cycles\n", (int)perf->miss_penalty[i]);
        printf("\n");
    }
}
