#ifndef FALSE_SHARING_DETECTOR_H
#define FALSE_SHARING_DETECTOR_H

#include "common.h"
#include "sample_collector.h"
#include "hardware_detector.h"

// False sharing candidate
typedef struct {
    uint64_t cache_line_addr;       // Cache line address
    int num_threads;                // Number of threads accessing
    int thread_ids[32];             // Thread IDs (up to 32)
    uint64_t access_counts[32];     // Access count per thread
    uint64_t write_counts[32];      // Write count per thread
    double contention_score;        // 0-100, severity of contention
    source_location_t locations[32]; // Source locations per thread
    int num_locations;              // Number of unique locations
    bool confirmed;                 // Confirmed false sharing
    char description[512];          // Human-readable description
} false_sharing_candidate_t;

// Detection configuration
typedef struct {
    int min_thread_count;           // Minimum threads for detection (default: 2)
    double min_write_ratio;         // Minimum write ratio (default: 0.1)
    int cache_line_size;            // Cache line size in bytes
    double time_window_ms;          // Time window for correlation
    bool require_different_vars;    // Require different variable names
} false_sharing_config_t;

// Detection results
typedef struct {
    false_sharing_candidate_t *candidates;
    int candidate_count;
    int confirmed_count;
    double total_impact_score;      // Overall performance impact
} false_sharing_results_t;

// API functions
int false_sharing_detector_init(const false_sharing_config_t *config);
void false_sharing_detector_cleanup(void);

// Detection functions
int detect_false_sharing(const cache_miss_sample_t *samples, int sample_count,
                        false_sharing_results_t *results);

int detect_false_sharing_hotspots(const cache_hotspot_t *hotspots, int hotspot_count,
                                 false_sharing_results_t *results);

// Analysis functions
int analyze_cache_line_sharing(const cache_miss_sample_t *samples, int count,
                              uint64_t cache_line, false_sharing_candidate_t *candidate);

double calculate_contention_score(const false_sharing_candidate_t *candidate);

bool verify_false_sharing(const false_sharing_candidate_t *candidate,
                         const cache_miss_sample_t *samples, int sample_count);

// Mitigation suggestions
typedef struct {
    char suggestion[512];
    char code_example[1024];
    int priority;                   // 1-5, higher is more important
    double expected_improvement;    // Percentage improvement
} mitigation_suggestion_t;

int generate_mitigation_suggestions(const false_sharing_candidate_t *candidate,
                                   mitigation_suggestion_t **suggestions,
                                   int *suggestion_count);

// Reporting
void print_false_sharing_results(const false_sharing_results_t *results);
void print_false_sharing_candidate(const false_sharing_candidate_t *candidate);
int export_false_sharing_report(const false_sharing_results_t *results,
                               const char *filename);

// Helper functions
false_sharing_config_t false_sharing_config_default(void);
void free_false_sharing_results(false_sharing_results_t *results);
uint64_t get_cache_line_address(uint64_t address, int cache_line_size);

#endif // FALSE_SHARING_DETECTOR_H
