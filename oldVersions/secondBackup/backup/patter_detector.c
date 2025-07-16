#include "pattern_detector.h"
#include <math.h>

static pattern_config_t g_config = {
    .detect_spatial_locality = true,
    .detect_temporal_locality = true,
    .detect_indirect_access = true,
    .detect_pointer_chasing = true,
    .min_stride_threshold = 1,
    .max_stride_threshold = 256
};

static bool g_initialized = false;

int pattern_detector_init(const pattern_config_t *config) {
    if (g_initialized) {
        LOG_WARNING("Pattern detector already initialized");
        return 0;
    }
    
    if (config) {
        g_config = *config;
    }
    
    LOG_INFO("Pattern detector initialized - spatial: %s, temporal: %s, indirect: %s",
             g_config.detect_spatial_locality ? "yes" : "no",
             g_config.detect_temporal_locality ? "yes" : "no",
             g_config.detect_indirect_access ? "yes" : "no");
    
    g_initialized = true;
    return 0;
}

void pattern_detector_cleanup(void) {
    if (!g_initialized) {
        return;
    }
    
    LOG_INFO("Pattern detector cleanup");
    g_initialized = false;
}

int detect_access_pattern(const static_pattern_t *pattern, pattern_detail_t *detail) {
    if (!pattern || !detail) {
        LOG_ERROR("NULL parameter passed to detect_access_pattern");
        return -1;
    }
    
    memset(detail, 0, sizeof(pattern_detail_t));
    detail->type = pattern->pattern;
    
    LOG_DEBUG("Detecting pattern for %s access at %s:%d",
              pattern->is_struct_access ? "struct" : "array",
              pattern->location.file, pattern->location.line);
    
    switch (pattern->pattern) {
        case SEQUENTIAL:
            detail->confidence_score = 95;
            detail->is_vectorizable = true;
            detail->is_prefetchable = true;
            detail->cache_line_utilization = 100;
            snprintf(detail->explanation, sizeof(detail->explanation),
                     "Sequential access pattern detected for %s with stride 1",
                     pattern->array_name);
            snprintf(detail->optimization_hint, sizeof(detail->optimization_hint),
                     "Excellent for cache performance. Consider vectorization with SIMD instructions.");
            break;
            
        case STRIDED:
            detail->confidence_score = 90;
            detail->is_vectorizable = (pattern->stride <= 4);
            detail->is_prefetchable = (pattern->stride <= 16);
            detail->cache_line_utilization = (pattern->stride <= 8) ? (100 / pattern->stride) : 12;
            snprintf(detail->explanation, sizeof(detail->explanation),
                     "Strided access pattern detected for %s with stride %d",
                     pattern->array_name, pattern->stride);
            if (pattern->stride > 8) {
                snprintf(detail->optimization_hint, sizeof(detail->optimization_hint),
                         "Large stride (%d) causing poor cache utilization. Consider loop tiling or data layout transformation.",
                         pattern->stride);
            } else {
                snprintf(detail->optimization_hint, sizeof(detail->optimization_hint),
                         "Moderate stride. May benefit from prefetching or data packing.");
            }
            break;
            
        case RANDOM:
            detail->confidence_score = 70;
            detail->is_vectorizable = false;
            detail->is_prefetchable = false;
            detail->cache_line_utilization = 25;
            snprintf(detail->explanation, sizeof(detail->explanation),
                     "Random access pattern detected for %s",
                     pattern->array_name);
            snprintf(detail->optimization_hint, sizeof(detail->optimization_hint),
                     "Poor cache performance expected. Consider data structure reorganization or caching strategies.");
            break;
            
        case INDIRECT_ACCESS:
            detail->confidence_score = 80;
            detail->is_vectorizable = false;
            detail->is_prefetchable = false;
            detail->cache_line_utilization = 30;
            snprintf(detail->explanation, sizeof(detail->explanation),
                     "Indirect access pattern detected (e.g., A[B[i]])");
            snprintf(detail->optimization_hint, sizeof(detail->optimization_hint),
                     "Consider data structure flattening or index array sorting for better locality.");
            break;
            
        case GATHER_SCATTER:
            detail->confidence_score = 75;
            detail->is_vectorizable = false;  // Possible with AVX2/AVX-512
            detail->is_prefetchable = false;
            detail->cache_line_utilization = 20;
            snprintf(detail->explanation, sizeof(detail->explanation),
                     "Gather/scatter pattern detected - non-contiguous memory access");
            snprintf(detail->optimization_hint, sizeof(detail->optimization_hint),
                     "Consider AoS to SoA transformation or data packing strategies.");
            break;
            
        case LOOP_CARRIED_DEP:
            detail->confidence_score = 85;
            detail->is_vectorizable = false;
            detail->is_prefetchable = true;
            detail->cache_line_utilization = 50;
            snprintf(detail->explanation, sizeof(detail->explanation),
                     "Loop-carried dependency detected preventing parallelization");
            snprintf(detail->optimization_hint, sizeof(detail->optimization_hint),
                     "Consider loop fission, scalar replacement, or algorithm restructuring.");
            break;
            
        case NESTED_LOOP:
            detail->confidence_score = 80;
            detail->is_vectorizable = true;  // Depends on inner pattern
            detail->is_prefetchable = true;
            detail->cache_line_utilization = 60;
            snprintf(detail->explanation, sizeof(detail->explanation),
                     "Nested loop access pattern at depth %d", pattern->loop_depth);
            snprintf(detail->optimization_hint, sizeof(detail->optimization_hint),
                     "Consider loop interchange, tiling, or blocking for better cache reuse.");
            break;
    }
    
    LOG_DEBUG("Pattern detection complete: %s (confidence: %d%%, cache utilization: %d%%)",
              access_pattern_to_string(detail->type),
              detail->confidence_score,
              detail->cache_line_utilization);
    
    return 0;
}

int detect_loop_patterns(const loop_info_t *loop, pattern_detail_t *details, int max_details) {
    if (!loop || !details || max_details <= 0) {
        LOG_ERROR("Invalid parameters for detect_loop_patterns");
        return -1;
    }
    
    LOG_INFO("Detecting patterns in loop at %s:%d with %d accesses",
             loop->location.file, loop->location.line, loop->pattern_count);
    
    int detected_count = 0;
    
    // Analyze each pattern in the loop
    for (int i = 0; i < loop->pattern_count && detected_count < max_details; i++) {
        if (detect_access_pattern(&loop->patterns[i], &details[detected_count]) == 0) {
            
            // Additional loop-specific analysis
            if (loop->has_nested_loops) {
                strncat(details[detected_count].optimization_hint,
                       " Nested loops detected - consider loop fusion or interchange.",
                       sizeof(details[detected_count].optimization_hint) - 
                       strlen(details[detected_count].optimization_hint) - 1);
            }
            
            if (loop->estimated_iterations > 0) {
                char iter_info[128];
                snprintf(iter_info, sizeof(iter_info), 
                        " Loop has ~%zu iterations.", loop->estimated_iterations);
                strncat(details[detected_count].explanation, iter_info,
                       sizeof(details[detected_count].explanation) - 
                       strlen(details[detected_count].explanation) - 1);
            }
            
            detected_count++;
        }
    }
    
    // Check for common loop patterns
    if (loop->pattern_count >= 2) {
        // Check for array copy pattern (A[i] = B[i])
        bool is_copy_pattern = true;
        for (int i = 0; i < loop->pattern_count; i++) {
            if (loop->patterns[i].pattern != SEQUENTIAL || 
                loop->patterns[i].stride != 1) {
                is_copy_pattern = false;
                break;
            }
        }
        
        if (is_copy_pattern && detected_count < max_details) {
            details[detected_count].type = SEQUENTIAL;
            details[detected_count].confidence_score = 100;
            details[detected_count].is_vectorizable = true;
            details[detected_count].is_prefetchable = true;
            details[detected_count].cache_line_utilization = 100;
            snprintf(details[detected_count].explanation, sizeof(details[detected_count].explanation),
                     "Memory copy pattern detected in loop");
            snprintf(details[detected_count].optimization_hint, 
                    sizeof(details[detected_count].optimization_hint),
                    "Use memcpy() or vectorized copy for better performance");
            detected_count++;
        }
    }
    
    LOG_INFO("Detected %d patterns in loop", detected_count);
    return detected_count;
}

int detect_struct_access_patterns(const struct_info_t *struct_info, 
                                 const static_pattern_t *accesses, int access_count,
                                 pattern_detail_t *detail) {
    if (!struct_info || !accesses || !detail || access_count <= 0) {
        LOG_ERROR("Invalid parameters for detect_struct_access_patterns");
        return -1;
    }
    
    LOG_INFO("Analyzing struct access patterns for %s with %d accesses",
             struct_info->struct_name, access_count);
    
    memset(detail, 0, sizeof(pattern_detail_t));
    
    // Count accesses per field
    int field_access_count[32] = {0};
    int total_field_accesses = 0;
    
    for (int i = 0; i < access_count; i++) {
        if (!accesses[i].is_struct_access) continue;
        
        // Find which field is being accessed
        for (int j = 0; j < struct_info->field_count; j++) {
            if (strcmp(accesses[i].variable_name, struct_info->field_names[j]) == 0) {
                field_access_count[j]++;
                total_field_accesses++;
                break;
            }
        }
    }
    
    // Determine if this is AoS pattern
    int fields_accessed = 0;
    for (int i = 0; i < struct_info->field_count; i++) {
        if (field_access_count[i] > 0) {
            fields_accessed++;
        }
    }
    
    if (fields_accessed == 1) {
        // Only one field accessed - good candidate for SoA
        detail->type = GATHER_SCATTER;
        detail->confidence_score = 95;
        detail->is_vectorizable = true;
        detail->is_prefetchable = false;
        detail->cache_line_utilization = 100 / struct_info->field_count;
        
        snprintf(detail->explanation, sizeof(detail->explanation),
                 "Single field access pattern in struct %s - only %d%% cache utilization",
                 struct_info->struct_name, detail->cache_line_utilization);
        snprintf(detail->optimization_hint, sizeof(detail->optimization_hint),
                 "Strong candidate for Structure of Arrays (SoA) transformation");
    } else if (fields_accessed == struct_info->field_count) {
        // All fields accessed - AoS might be optimal
        detail->type = SEQUENTIAL;
        detail->confidence_score = 90;
        detail->is_vectorizable = false;
        detail->is_prefetchable = true;
        detail->cache_line_utilization = 100;
        
        snprintf(detail->explanation, sizeof(detail->explanation),
                 "Full struct access pattern in %s - all fields used",
                 struct_info->struct_name);
        snprintf(detail->optimization_hint, sizeof(detail->optimization_hint),
                 "Current AoS layout is appropriate for this access pattern");
    } else {
        // Partial field access
        detail->type = GATHER_SCATTER;
        detail->confidence_score = 80;
        detail->is_vectorizable = false;
        detail->is_prefetchable = false;
        detail->cache_line_utilization = (fields_accessed * 100) / struct_info->field_count;
        
        snprintf(detail->explanation, sizeof(detail->explanation),
                 "Partial struct access - %d of %d fields accessed (%d%% utilization)",
                 fields_accessed, struct_info->field_count, detail->cache_line_utilization);
        snprintf(detail->optimization_hint, sizeof(detail->optimization_hint),
                 "Consider struct splitting or hot/cold field separation");
    }
    
    LOG_INFO("Struct pattern analysis complete: %s (utilization: %d%%)",
             access_pattern_to_string(detail->type), detail->cache_line_utilization);
    
    return 0;
}

bool is_aos_pattern(const static_pattern_t *patterns, int count) {
    if (!patterns || count <= 0) return false;
    
    int struct_accesses = 0;
    for (int i = 0; i < count; i++) {
        if (patterns[i].is_struct_access) {
            struct_accesses++;
        }
    }
    
    // If more than 50% are struct accesses, likely AoS
    return (struct_accesses * 2 > count);
}

bool is_soa_candidate(const struct_info_t *struct_info, const static_pattern_t *patterns, int count) {
    if (!struct_info || !patterns || count <= 0) return false;
    
    // Count unique fields accessed
    bool field_accessed[32] = {false};
    int unique_fields = 0;
    
    for (int i = 0; i < count; i++) {
        if (!patterns[i].is_struct_access) continue;
        
        for (int j = 0; j < struct_info->field_count; j++) {
            if (strcmp(patterns[i].variable_name, struct_info->field_names[j]) == 0) {
                if (!field_accessed[j]) {
                    field_accessed[j] = true;
                    unique_fields++;
                }
                break;
            }
        }
    }
    
    // Good SoA candidate if accessing < 50% of fields
    return (unique_fields * 2 < struct_info->field_count);
}

int calculate_spatial_locality_score(const static_pattern_t *patterns, int count) {
    if (!patterns || count <= 0) return 0;
    
    int sequential_count = 0;
    int small_stride_count = 0;
    int total_stride = 0;
    
    for (int i = 0; i < count; i++) {
        if (patterns[i].pattern == SEQUENTIAL) {
            sequential_count++;
        } else if (patterns[i].pattern == STRIDED && patterns[i].stride <= 8) {
            small_stride_count++;
            total_stride += patterns[i].stride;
        }
    }
    
    // Score based on sequential and small-stride accesses
    int score = (sequential_count * 100 + small_stride_count * 50) / count;
    
    LOG_DEBUG("Spatial locality score: %d (seq: %d, small stride: %d)",
              score, sequential_count, small_stride_count);
    
    return score;
}

int calculate_temporal_locality_score(const static_pattern_t *patterns, int count) {
    if (!patterns || count <= 0) return 0;
    
    // Simple heuristic: patterns accessing same array/struct have temporal locality
    int reuse_count = 0;
    
    for (int i = 0; i < count; i++) {
        for (int j = i + 1; j < count; j++) {
            if (strcmp(patterns[i].array_name, patterns[j].array_name) == 0 ||
                (patterns[i].is_struct_access && patterns[j].is_struct_access &&
                 strcmp(patterns[i].struct_name, patterns[j].struct_name) == 0)) {
                reuse_count++;
            }
        }
    }
    
    // Normalize to 0-100 scale
    int max_reuse = (count * (count - 1)) / 2;
    int score = max_reuse > 0 ? (reuse_count * 100) / max_reuse : 0;
    
    LOG_DEBUG("Temporal locality score: %d (reuse count: %d)", score, reuse_count);
    
    return score;
}

const char* get_optimization_suggestion(access_pattern_t pattern) {
    switch (pattern) {
        case SEQUENTIAL:
            return "Use vectorization, prefetching, and ensure proper alignment";
        case STRIDED:
            return "Consider loop tiling, data packing, or gather operations";
        case RANDOM:
            return "Use caching, memoization, or data structure reorganization";
        case GATHER_SCATTER:
            return "Transform AoS to SoA or use specialized gather/scatter instructions";
        case LOOP_CARRIED_DEP:
            return "Break dependencies with scalar replacement or algorithm redesign";
        case NESTED_LOOP:
            return "Apply loop blocking, interchange, or fusion techniques";
        case INDIRECT_ACCESS:
            return "Sort indices, use bucketing, or implement software prefetching";
        default:
            return "Profile further to identify optimization opportunities";
    }
}

int estimate_cache_efficiency(const static_pattern_t *pattern, int cache_line_size) {
    if (!pattern) return 0;
    
    int efficiency = 0;
    
    switch (pattern->pattern) {
        case SEQUENTIAL:
            efficiency = 100;  // Full cache line utilization
            break;
        case STRIDED:
            if (pattern->stride * 8 <= cache_line_size) {  // Assuming 8-byte elements
                efficiency = 100 / pattern->stride;
            } else {
                efficiency = (8 * 100) / cache_line_size;  // One element per line
            }
            break;
        case RANDOM:
        case INDIRECT_ACCESS:
            efficiency = (8 * 100) / cache_line_size;  // Worst case
            break;
        case GATHER_SCATTER:
            efficiency = 25;  // Typical for scattered access
            break;
        case LOOP_CARRIED_DEP:
        case NESTED_LOOP:
            efficiency = 50;  // Depends on actual pattern
            break;
    }
    
    LOG_DEBUG("Cache efficiency for %s pattern: %d%%",
              access_pattern_to_string(pattern->pattern), efficiency);
    
    return efficiency;
}
