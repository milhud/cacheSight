#ifndef PATTERN_CLASSIFIER_H
#define PATTERN_CLASSIFIER_H

#include "common.h"
#include "ast_analyzer.h"
#include "sample_collector.h"
#include "hardware_detector.h"

// Classified pattern with detailed analysis
typedef struct {
    cache_antipattern_t type;       // Type of anti-pattern
    cache_hotspot_t *hotspot;       // Associated hotspot
    double severity_score;          // 0-100, higher is worse
    double confidence;              // 0-1, classification confidence
    char description[512];          // Human-readable description
    char root_cause[512];          // Likely cause of the problem
    miss_type_t primary_miss_type;  // Dominant miss type
    int affected_cache_levels;      // Bitmask of affected levels
    double performance_impact;      // Estimated performance loss (%)
} classified_pattern_t;

// Pattern classifier state
typedef struct pattern_classifier pattern_classifier_t;

// Classification configuration
typedef struct {
    double min_confidence_threshold;     // Minimum confidence to report
    bool enable_ml_classification;       // Use ML-based classification
    bool enable_heuristics;             // Use heuristic rules
    int analysis_depth;                 // 1-5, deeper = more thorough
    bool correlate_static_dynamic;      // Correlate with static analysis
} classifier_config_t;

// API functions
pattern_classifier_t* pattern_classifier_create(const classifier_config_t *config,
                                              const cache_info_t *cache_info);
void pattern_classifier_destroy(pattern_classifier_t *classifier);

// Classification functions
int pattern_classifier_classify_hotspot(pattern_classifier_t *classifier,
                                      const cache_hotspot_t *hotspot,
                                      classified_pattern_t *pattern);

int pattern_classifier_classify_all(pattern_classifier_t *classifier,
                                  const cache_hotspot_t *hotspots, int hotspot_count,
                                  classified_pattern_t **patterns, int *pattern_count);

// Correlation with static analysis
int pattern_classifier_correlate_static(pattern_classifier_t *classifier,
                                      const analysis_results_t *static_results,
                                      classified_pattern_t *patterns, int pattern_count);

// Specific pattern detection
bool detect_hotspot_reuse(const cache_hotspot_t *hotspot, double *severity);
bool detect_thrashing(const cache_hotspot_t *hotspot, const cache_info_t *cache_info, double *severity);
bool detect_false_sharing_pattern(const cache_hotspot_t *hotspot, double *severity);
bool detect_streaming_pattern(const cache_hotspot_t *hotspot, double *severity);
bool detect_irregular_gather_scatter(const cache_hotspot_t *hotspot, double *severity);

// Analysis helpers
miss_type_t classify_miss_type(const cache_hotspot_t *hotspot, const cache_info_t *cache_info);
double calculate_performance_impact(const classified_pattern_t *pattern, const cache_info_t *cache_info);
void generate_pattern_description(classified_pattern_t *pattern);

// Reporting
void pattern_classifier_print_results(const classified_pattern_t *patterns, int count);
int pattern_classifier_export_json(const classified_pattern_t *patterns, int count,
                                 const char *filename);

// Configuration
classifier_config_t classifier_config_default(void);

#endif // PATTERN_CLASSIFIER_H
