#include "evaluator.h"
#include <math.h>
#include <time.h>

// Cache simulator state
typedef struct {
    uint64_t *tags;         // Tag array
    uint64_t *lru_counters; // LRU replacement
    int num_sets;
    int associativity;
    int line_size;
    uint64_t hits;
    uint64_t misses;
} cache_level_sim_t;

// Internal evaluator structure
struct evaluator {
    evaluator_config_t config;
    cache_info_t cache_info;
    
    // Cache simulators
    cache_level_sim_t *cache_sims[4];
    int num_cache_levels;
    
    // Statistics
    double total_evaluation_time;
    int evaluations_performed;
    
    pthread_mutex_t mutex;
};

// Create cache simulator
static cache_level_sim_t* create_cache_simulator(const cache_level_t *cache) {
    cache_level_sim_t *sim = CALLOC_LOGGED(1, sizeof(cache_level_sim_t));
    if (!sim) return NULL;
    
    sim->line_size = cache->line_size;
    sim->associativity = cache->associativity;
    sim->num_sets = cache->size / (cache->line_size * cache->associativity);
    
    sim->tags = CALLOC_LOGGED(sim->num_sets * sim->associativity, sizeof(uint64_t));
    sim->lru_counters = CALLOC_LOGGED(sim->num_sets * sim->associativity, sizeof(uint64_t));
    
    if (!sim->tags || !sim->lru_counters) {
        if (sim->tags) FREE_LOGGED(sim->tags);
        if (sim->lru_counters) FREE_LOGGED(sim->lru_counters);
        FREE_LOGGED(sim);
        return NULL;
    }
    
    LOG_DEBUG("Created cache simulator: %d sets, %d-way, %zu-byte lines",
              sim->num_sets, sim->associativity, sim->line_size);
    
    return sim;
}

// Destroy cache simulator
static void destroy_cache_simulator(cache_level_sim_t *sim) {
    if (!sim) return;
    
    if (sim->tags) FREE_LOGGED(sim->tags);
    if (sim->lru_counters) FREE_LOGGED(sim->lru_counters);
    FREE_LOGGED(sim);
}

// Create evaluator
evaluator_t* evaluator_create(const evaluator_config_t *config,
                             const cache_info_t *cache_info) {
    if (!config || !cache_info) {
        LOG_ERROR("NULL parameters for evaluator_create");
        return NULL;
    }
    
    evaluator_t *evaluator = CALLOC_LOGGED(1, sizeof(evaluator_t));
    if (!evaluator) {
        LOG_ERROR("Failed to allocate evaluator");
        return NULL;
    }
    
    evaluator->config = *config;
    evaluator->cache_info = *cache_info;
    pthread_mutex_init(&evaluator->mutex, NULL);
    
    // Create cache simulators if enabled
    if (config->enable_simulation) {
        evaluator->num_cache_levels = cache_info->num_levels;

        // Since cache_sims is already a fixed array, just initialize each element:
        for (int i = 0; i < evaluator->num_cache_levels && i < 4; i++) {
            evaluator->cache_sims[i] = create_cache_simulator(&cache_info->levels[i]);
            if (!evaluator->cache_sims[i]) {
                LOG_ERROR("Failed to create cache simulator for level %d", i + 1);
                evaluator_destroy(evaluator);
                return NULL;
            }
        }

        // Initialize any remaining array elements to NULL
        for (int i = evaluator->num_cache_levels; i < 4; i++) {
            evaluator->cache_sims[i] = NULL;
        }
        
        if (evaluator->cache_sims) {
            for (int i = 0; i < evaluator->num_cache_levels; i++) {
                evaluator->cache_sims[i] = create_cache_simulator(&cache_info->levels[i]);
            }
        }
    }
    
    LOG_INFO("Created evaluator with %s and %s",
             config->enable_simulation ? "simulation" : "no simulation",
             config->enable_statistical_analysis ? "statistical analysis" : "basic analysis");
    
    return evaluator;
}

// Destroy evaluator
void evaluator_destroy(evaluator_t *evaluator) {
    if (!evaluator) return;
    
    LOG_INFO("Destroying evaluator after %d evaluations",
             evaluator->evaluations_performed);
    
    // Destroy cache simulators
    if (evaluator->cache_sims) {
        for (int i = 0; i < evaluator->num_cache_levels; i++) {
            destroy_cache_simulator(evaluator->cache_sims[i]);
        }
        FREE_LOGGED(evaluator->cache_sims);
    }
    
    pthread_mutex_destroy(&evaluator->mutex);
    FREE_LOGGED(evaluator);
}

// Collect metrics from hotspots
int evaluator_collect_metrics(evaluator_t *evaluator,
                             const cache_hotspot_t *hotspots, int hotspot_count,
                             evaluation_metrics_t *metrics) {
    if (!evaluator || !hotspots || !metrics || hotspot_count <= 0) {
        LOG_ERROR("Invalid parameters for collect_metrics");
        return -1;
    }
    
    LOG_INFO("Collecting metrics from %d hotspots", hotspot_count);
    
    pthread_mutex_lock(&evaluator->mutex);
    
    memset(metrics, 0, sizeof(evaluation_metrics_t));
    
    // Aggregate metrics from hotspots
    uint64_t total_accesses = 0;
    uint64_t total_misses = 0;
    uint64_t cache_misses[4] = {0};
    double total_latency = 0;
    
    // Calculate spatial access pattern
    uint64_t min_addr = UINT64_MAX;
    uint64_t max_addr = 0;
    
    for (int i = 0; i < hotspot_count; i++) {
        const cache_hotspot_t *h = &hotspots[i];
        
        total_accesses += h->total_accesses;
        total_misses += h->total_misses;
        total_latency += h->avg_latency_cycles * h->total_misses;
        
        // Track address range
        if (h->address_range_start < min_addr) {
            min_addr = h->address_range_start;
        }
        if (h->address_range_end > max_addr) {
            max_addr = h->address_range_end;
        }
        
        // Count misses per cache level
        for (int j = 0; j < 4; j++) {
            cache_misses[j] += h->cache_levels_affected[j];
        }
        
        // Update latency histogram
        int latency_bucket = (int)(log2(h->avg_latency_cycles + 1));
        if (latency_bucket < 32) {
            metrics->miss_latency_histogram[latency_bucket] += h->total_misses;
        }
    }
    
    // Calculate overall metrics
    if (total_accesses > 0) {
        // Cache miss rates
        for (int i = 0; i < 4; i++) {
            metrics->cache_miss_rate[i] = (double)cache_misses[i] / total_accesses;
        }
        
        // Average cycles per element
        metrics->cycles_per_element = total_latency / total_accesses;
        
        // Memory footprint
        metrics->loop_footprint_bytes = max_addr - min_addr;
        
        // Cache line utilization (simplified)
        size_t cache_lines_touched = metrics->loop_footprint_bytes / 
                                    evaluator->cache_info.levels[0].line_size;
        size_t useful_bytes = total_accesses * 8;  // Assume 8-byte elements
        
        if (cache_lines_touched > 0) {
            metrics->cache_line_utilization = 
                (double)useful_bytes / (cache_lines_touched * 
                evaluator->cache_info.levels[0].line_size) * 100;
            if (metrics->cache_line_utilization > 100) {
                metrics->cache_line_utilization = 100;
            }
        }
    }
    
    // Analyze access patterns for locality scores
    int sequential_count = 0;
    int strided_count = 0;
    int random_count = 0;
    
    for (int i = 0; i < hotspot_count; i++) {
        switch (hotspots[i].dominant_pattern) {
            case SEQUENTIAL:
                sequential_count++;
                break;
            case STRIDED:
                strided_count++;
                break;
            case RANDOM:
            case INDIRECT_ACCESS:
                random_count++;
                break;
            default:
                break;
        }
    }
    
    // Calculate locality scores
    if (hotspot_count > 0) {
        metrics->spatial_locality_score = 
            (sequential_count * 100.0 + strided_count * 50.0) / hotspot_count;
        
        // Temporal locality based on reuse (simplified)
        if (total_misses > 0 && total_accesses > total_misses) {
            metrics->temporal_locality_score = 
                (1.0 - (double)total_misses / total_accesses) * 100;
        }
    }
    
    // Transformability score based on pattern types
    metrics->transformability_score = 
        (sequential_count + strided_count) * 100.0 / hotspot_count;
    
    pthread_mutex_unlock(&evaluator->mutex);
    
    LOG_INFO("Metrics collected: miss_rate=%.2f%%, footprint=%zu KB, "
             "spatial_locality=%.1f, temporal_locality=%.1f",
             metrics->cache_miss_rate[0] * 100,
             metrics->loop_footprint_bytes / 1024,
             metrics->spatial_locality_score,
             metrics->temporal_locality_score);
    
    return 0;
}

// Simulate cache behavior
static void simulate_cache_access(cache_level_sim_t *sim, uint64_t address) {
    uint64_t tag = address / sim->line_size;
    int set_index = (tag % sim->num_sets);
    
    // Check for hit
    bool hit = false;
    int lru_way = 0;
    uint64_t max_lru = 0;
    
    for (int way = 0; way < sim->associativity; way++) {
        int idx = set_index * sim->associativity + way;
        
        if (sim->tags[idx] == tag) {
            // Hit
            hit = true;
            sim->hits++;
            sim->lru_counters[idx] = 0;  // Reset LRU counter
            break;
        }
        
        // Track LRU way
        if (sim->lru_counters[idx] > max_lru) {
            max_lru = sim->lru_counters[idx];
            lru_way = way;
        }
    }
    
    if (!hit) {
        // Miss - replace LRU
        sim->misses++;
        int idx = set_index * sim->associativity + lru_way;
        sim->tags[idx] = tag;
        sim->lru_counters[idx] = 0;
    }
    
    // Update LRU counters
    for (int way = 0; way < sim->associativity; way++) {
        int idx = set_index * sim->associativity + way;
        sim->lru_counters[idx]++;
    }
}

// Simulate cache with samples
int evaluator_simulate_cache(evaluator_t *evaluator,
                            const cache_miss_sample_t *samples, int sample_count,
                            evaluation_metrics_t *metrics) {
    if (!evaluator || !samples || !metrics || sample_count <= 0) {
        LOG_ERROR("Invalid parameters for simulate_cache");
        return -1;
    }
    
    if (!evaluator->config.enable_simulation || !evaluator->cache_sims) {
        LOG_WARNING("Cache simulation not enabled");
        return -1;
    }
    
    LOG_INFO("Simulating cache behavior with %d samples", sample_count);
    
    pthread_mutex_lock(&evaluator->mutex);
    
    // Reset simulators
    for (int i = 0; i < evaluator->num_cache_levels; i++) {
        if (evaluator->cache_sims[i]) {
            evaluator->cache_sims[i]->hits = 0;
            evaluator->cache_sims[i]->misses = 0;
            // Clear tags
            memset(evaluator->cache_sims[i]->tags, 0,
                   evaluator->cache_sims[i]->num_sets * 
                   evaluator->cache_sims[i]->associativity * sizeof(uint64_t));
        }
    }
    
    // Run simulation
    for (int i = 0; i < sample_count; i++) {
        uint64_t addr = samples[i].memory_addr;
        
        // Simulate through cache hierarchy
        bool hit = false;
        for (int level = 0; level < evaluator->num_cache_levels; level++) {
            if (evaluator->cache_sims[level]) {
                uint64_t prev_hits = evaluator->cache_sims[level]->hits;
                simulate_cache_access(evaluator->cache_sims[level], addr);
                
                if (evaluator->cache_sims[level]->hits > prev_hits) {
                    hit = true;
                    break;  // Hit at this level
                }
            }
        }
    }
    
    // Calculate metrics from simulation
    for (int i = 0; i < evaluator->num_cache_levels && i < 4; i++) {
        if (evaluator->cache_sims[i]) {
            uint64_t total = evaluator->cache_sims[i]->hits + 
                           evaluator->cache_sims[i]->misses;
            if (total > 0) {
                metrics->cache_miss_rate[i] = 
                    (double)evaluator->cache_sims[i]->misses / total;
            }
            
            LOG_DEBUG("L%d simulation: hits=%lu, misses=%lu, miss_rate=%.2f%%",
                      i + 1, evaluator->cache_sims[i]->hits,
                      evaluator->cache_sims[i]->misses,
                      metrics->cache_miss_rate[i] * 100);
        }
    }
    
    pthread_mutex_unlock(&evaluator->mutex);
    
    return 0;
}

// Measure performance with timing
double evaluator_measure_performance(evaluator_t *evaluator,
                                   void (*test_function)(void *),
                                   void *test_data,
                                   int iterations) {
    if (!evaluator || !test_function || iterations <= 0) {
        LOG_ERROR("Invalid parameters for measure_performance");
        return -1;
    }
    
    LOG_DEBUG("Measuring performance over %d iterations", iterations);
    
    // Warmup
    for (int i = 0; i < 5; i++) {
        test_function(test_data);
    }
    
    // Time the iterations
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    for (int i = 0; i < iterations; i++) {
        test_function(test_data);
    }
    
    clock_gettime(CLOCK_MONOTONIC, &end);
    
    double elapsed = (end.tv_sec - start.tv_sec) + 
                    (end.tv_nsec - start.tv_nsec) / 1e9;
    double avg_time = elapsed / iterations;
    
    LOG_DEBUG("Average time per iteration: %.6f seconds", avg_time);
    
    return avg_time;
}

// Statistical comparison (simplified t-test)
int evaluator_compare_performance(evaluator_t *evaluator,
                                 double *baseline_times, int baseline_count,
                                 double *optimized_times, int optimized_count,
                                 double *speedup, double *p_value) {
    if (!evaluator || !baseline_times || !optimized_times || 
        !speedup || !p_value || baseline_count <= 0 || optimized_count <= 0) {
        LOG_ERROR("Invalid parameters for compare_performance");
        return -1;
    }
    
    // Calculate means
    double baseline_mean = 0, optimized_mean = 0;
    
    for (int i = 0; i < baseline_count; i++) {
        baseline_mean += baseline_times[i];
    }
    baseline_mean /= baseline_count;
    
    for (int i = 0; i < optimized_count; i++) {
        optimized_mean += optimized_times[i];
    }
    optimized_mean /= optimized_count;
    
    *speedup = baseline_mean / optimized_mean;
    
    // Calculate standard deviations
    double baseline_var = 0, optimized_var = 0;
    
    for (int i = 0; i < baseline_count; i++) {
        double diff = baseline_times[i] - baseline_mean;
        baseline_var += diff * diff;
    }
    baseline_var /= (baseline_count - 1);
    
    for (int i = 0; i < optimized_count; i++) {
        double diff = optimized_times[i] - optimized_mean;
        optimized_var += diff * diff;
    }
    optimized_var /= (optimized_count - 1);
    
    // Welch's t-test (unequal variances)
    double se = sqrt(baseline_var / baseline_count + optimized_var / optimized_count);
    double t_stat = (baseline_mean - optimized_mean) / se;
    
    // Approximate p-value (simplified - assumes normal distribution)
    // Real implementation would use t-distribution
    double z = fabs(t_stat);
    *p_value = 2 * (1 - 0.5 * (1 + erf(z / sqrt(2))));
    
    LOG_INFO("Performance comparison: speedup=%.2fx, p-value=%.4f",
             *speedup, *p_value);
    
    return 0;
}

// Print evaluation metrics
void evaluator_print_metrics(const evaluation_metrics_t *metrics) {
    if (!metrics) return;
    
    printf("\n=== Evaluation Metrics ===\n");
    
    printf("\nCache Performance:\n");
    printf("  Cache line utilization: %.1f%%\n", metrics->cache_line_utilization);
    printf("  Miss rates: L1=%.2f%%, L2=%.2f%%, L3=%.2f%%\n",
           metrics->cache_miss_rate[0] * 100,
           metrics->cache_miss_rate[1] * 100,
           metrics->cache_miss_rate[2] * 100);
    
    printf("\nLocality Scores:\n");
    printf("  Spatial locality: %.1f/100\n", metrics->spatial_locality_score);
    printf("  Temporal locality: %.1f/100\n", metrics->temporal_locality_score);
    
    printf("\nPerformance Metrics:\n");
    printf("  Cycles per element: %.2f\n", metrics->cycles_per_element);
    printf("  Memory bandwidth utilization: %.1f%%\n", 
           metrics->memory_bandwidth_utilization);
    
    printf("\nMemory Footprint:\n");
    char size_str[32];
    format_bytes(metrics->loop_footprint_bytes, size_str, sizeof(size_str));
    printf("  Working set size: %s\n", size_str);
    
    printf("\nOptimization Potential:\n");
    printf("  Transformability score: %.1f/100\n", metrics->transformability_score);
    
    // Print latency histogram
    printf("\nMiss Latency Distribution:\n");
    for (int i = 0; i < 32; i++) {
        if (metrics->miss_latency_histogram[i] > 0) {
            printf("  %d-%d cycles: %lu misses\n",
                   1 << i, 1 << (i + 1), metrics->miss_latency_histogram[i]);
        }
    }
}

// Print comparison results
void evaluator_print_comparison(const benchmark_result_t *result) {
    if (!result) return;
    
    printf("\n=== Benchmark: %s ===\n", result->test_name);
    printf("Baseline time: %.6f seconds\n", result->baseline_time);
    printf("Optimized time: %.6f seconds\n", result->optimized_time);
    printf("Speedup: %.2fx\n", result->speedup);
    
    if (result->is_significant) {
        printf("Result is statistically significant (p=%.4f)\n", result->p_value);
    } else {
        printf("Result is NOT statistically significant (p=%.4f)\n", result->p_value);
    }
    
    printf("\nCache miss reduction:\n");
    for (int i = 0; i < 4; i++) {
        if (result->baseline_metrics.cache_miss_rate[i] > 0) {
            double reduction = (result->baseline_metrics.cache_miss_rate[i] -
                              result->optimized_metrics.cache_miss_rate[i]) /
                              result->baseline_metrics.cache_miss_rate[i] * 100;
            printf("  L%d: %.1f%% reduction\n", i + 1, reduction);
        }
    }
    
    printf("\nLocality improvements:\n");
    printf("  Spatial: %.1f → %.1f\n",
           result->baseline_metrics.spatial_locality_score,
           result->optimized_metrics.spatial_locality_score);
    printf("  Temporal: %.1f → %.1f\n",
           result->baseline_metrics.temporal_locality_score,
           result->optimized_metrics.temporal_locality_score);
}

// Get default configuration
evaluator_config_t evaluator_config_default(void) {
    evaluator_config_t config = {
        .enable_before_after = true,
        .enable_simulation = false,  // Disabled by default for performance
        .enable_statistical_analysis = true,
        .sample_iterations = 100,
        .confidence_level = 0.95
    };
    
    return config;
}
