#ifndef STATISTICAL_ANALYZER_H
#define STATISTICAL_ANALYZER_H

#include "common.h"
#include "sample_collector.h"

// Statistical metrics
typedef struct {
    double mean;
    double median;
    double std_dev;
    double variance;
    double min;
    double max;
    double percentile_25;
    double percentile_75;
    double percentile_95;
    double percentile_99;
    double skewness;
    double kurtosis;
} statistics_t;

// Access pattern statistics
typedef struct {
    statistics_t stride_stats;      // Stride distribution
    statistics_t reuse_distance;    // Temporal reuse distance
    statistics_t access_interval;   // Time between accesses
    double entropy;                 // Randomness measure
    double autocorrelation;         // Pattern correlation
    int dominant_stride;            // Most common stride
    double stride_regularity;       // 0-1, how regular is stride
} pattern_statistics_t;

// Correlation analysis
typedef struct {
    double correlation_coefficient;
    double p_value;
    bool is_significant;
    char description[256];
} correlation_result_t;

// API functions
int statistical_analyzer_init(void);
void statistical_analyzer_cleanup(void);

// Basic statistics
int calculate_statistics(const double *data, int count, statistics_t *stats);
int calculate_pattern_statistics(const cache_miss_sample_t *samples, int count,
                                pattern_statistics_t *stats);

// Pattern analysis
double calculate_entropy(const uint64_t *addresses, int count);
double calculate_autocorrelation(const uint64_t *addresses, int count, int lag);
int detect_stride_pattern(const uint64_t *addresses, int count,
                         int *stride, double *confidence);

// Correlation analysis
int analyze_correlation(const double *x, const double *y, int count,
                       correlation_result_t *result);
int find_correlated_metrics(const cache_hotspot_t *hotspots, int count,
                           correlation_result_t **results, int *result_count);

// Distribution analysis
typedef enum {
    DIST_NORMAL,
    DIST_EXPONENTIAL,
    DIST_POISSON,
    DIST_UNIFORM,
    DIST_UNKNOWN
} distribution_type_t;

distribution_type_t identify_distribution(const double *data, int count);
double kolmogorov_smirnov_test(const double *data, int count,
                               distribution_type_t dist);

// Time series analysis
int analyze_time_series(const cache_miss_sample_t *samples, int count,
                       double *trend, double *seasonality);
int detect_phase_behavior(const cache_miss_sample_t *samples, int count,
                         int *num_phases);

// Clustering
int cluster_access_patterns(const cache_hotspot_t *hotspots, int count,
                           int **cluster_labels, int *num_clusters);

// Helper functions
void print_statistics(const statistics_t *stats, const char *name);
void print_pattern_statistics(const pattern_statistics_t *stats);

#endif // STATISTICAL_ANALYZER_H
