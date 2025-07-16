#ifndef EVALUATOR_H
#define EVALUATOR_H

#include "common.h"
#include "hardware_detector.h"
#include "recommendation_engine.h"

// Comprehensive evaluation metrics
typedef struct {
    // Cache metrics
    double cache_line_utilization;      // Percentage of cache line used
    double temporal_locality_score;     // 0-100, reuse over time
    double spatial_locality_score;      // 0-100, adjacent access
    uint64_t miss_latency_histogram[32]; // Distribution of miss latencies
    double access_density_map[1024];    // Spatial access distribution
    size_t loop_footprint_bytes;        // Working set size
    double prefetch_accuracy;           // Successful prefetches
    double prefetch_coverage;           // Fraction prefetched
    double thread_contention_score;     // Multi-thread interference
    double transformability_score;      // Ease of optimization
    
    // Performance metrics
    double cycles_per_element;          // Average cycles per data element
    double instructions_per_cycle;      // IPC
    double memory_bandwidth_utilization; // Percentage of peak bandwidth
    double cache_miss_rate[4];          // Miss rate per cache level
    
    // Before/after comparison
    double speedup_ratio;               // After/before performance
    double miss_reduction[4];           // Miss reduction per level
    double bandwidth_reduction;         // Memory bandwidth saved
} evaluation_metrics_t;

// Evaluator state
typedef struct evaluator evaluator_t;

// Evaluation configuration
typedef struct {
    bool enable_before_after;           // Compare before/after optimization
    bool enable_simulation;             // Use cache simulation
    bool enable_statistical_analysis;   // Statistical significance testing
    int sample_iterations;              // Iterations for timing
    double confidence_level;            // Statistical confidence (e.g., 0.95)
} evaluator_config_t;

// Benchmark results
typedef struct {
    char test_name[128];
    double baseline_time;               // Original code timing
    double optimized_time;              // Optimized code timing
    double speedup;                     // Speedup factor
    evaluation_metrics_t baseline_metrics;
    evaluation_metrics_t optimized_metrics;
    bool is_significant;                // Statistically significant
    double p_value;                     // Statistical p-value
} benchmark_result_t;

// API functions
evaluator_t* evaluator_create(const evaluator_config_t *config,
                             const cache_info_t *cache_info);
void evaluator_destroy(evaluator_t *evaluator);

// Evaluate optimization recommendations
int evaluator_evaluate_recommendation(evaluator_t *evaluator,
                                     const optimization_rec_t *rec,
                                     evaluation_metrics_t *metrics);

int evaluator_evaluate_all(evaluator_t *evaluator,
                          const optimization_rec_t *recs, int rec_count,
                          benchmark_result_t **results, int *result_count);

// Metrics collection
int evaluator_collect_metrics(evaluator_t *evaluator,
                             const cache_hotspot_t *hotspots, int hotspot_count,
                             evaluation_metrics_t *metrics);

// Performance measurement
double evaluator_measure_performance(evaluator_t *evaluator,
                                   void (*test_function)(void *),
                                   void *test_data,
                                   int iterations);

// Statistical analysis
int evaluator_compare_performance(evaluator_t *evaluator,
                                 double *baseline_times, int baseline_count,
                                 double *optimized_times, int optimized_count,
                                 double *speedup, double *p_value);

// Cache simulation
int evaluator_simulate_cache(evaluator_t *evaluator,
                            const cache_miss_sample_t *samples, int sample_count,
                            evaluation_metrics_t *metrics);

// Reporting
void evaluator_print_metrics(const evaluation_metrics_t *metrics);
void evaluator_print_comparison(const benchmark_result_t *result);
int evaluator_export_results(const benchmark_result_t *results, int count,
                           const char *filename);

// Configuration
evaluator_config_t evaluator_config_default(void);

#endif // EVALUATOR_H
