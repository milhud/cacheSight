#ifndef PATTERN_DETECTOR_H
#define PATTERN_DETECTOR_H

#include "common.h"
#include "ast_analyzer.h"

// Pattern detection configuration
typedef struct {
    bool detect_spatial_locality;
    bool detect_temporal_locality;
    bool detect_indirect_access;
    bool detect_pointer_chasing;
    int min_stride_threshold;
    int max_stride_threshold;
} pattern_config_t;

// Detected pattern details
typedef struct {
    access_pattern_t type;
    int confidence_score;  // 0-100
    char explanation[512];
    char optimization_hint[512];
    bool is_vectorizable;
    bool is_prefetchable;
    int cache_line_utilization;  // Percentage
} pattern_detail_t;

// API functions
int pattern_detector_init(const pattern_config_t *config);
void pattern_detector_cleanup(void);

int detect_access_pattern(const static_pattern_t *pattern, pattern_detail_t *detail);
int detect_loop_patterns(const loop_info_t *loop, pattern_detail_t *details, int max_details);
int detect_struct_access_patterns(const struct_info_t *struct_info, 
                                 const static_pattern_t *accesses, int access_count,
                                 pattern_detail_t *detail);

// Analysis functions
bool is_aos_pattern(const static_pattern_t *patterns, int count);
bool is_soa_candidate(const struct_info_t *struct_info, const static_pattern_t *patterns, int count);
int calculate_spatial_locality_score(const static_pattern_t *patterns, int count);
int calculate_temporal_locality_score(const static_pattern_t *patterns, int count);

// Helper functions
const char* get_optimization_suggestion(access_pattern_t pattern);
int estimate_cache_efficiency(const static_pattern_t *pattern, int cache_line_size);

#endif // PATTERN_DETECTOR_H
