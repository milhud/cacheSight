#include "pattern_classifier.h"
#include <math.h>

// Internal classifier structure
struct pattern_classifier {
    classifier_config_t config;
    cache_info_t cache_info;
    
    // Statistics for classification
    double avg_miss_rate;
    double avg_latency;
    uint64_t total_samples;
    
    pthread_mutex_t mutex;
};

// Create pattern classifier
pattern_classifier_t* pattern_classifier_create(const classifier_config_t *config,
                                              const cache_info_t *cache_info) {
    if (!config || !cache_info) {
        LOG_ERROR("NULL parameters for pattern_classifier_create");
        return NULL;
    }
    
    pattern_classifier_t *classifier = CALLOC_LOGGED(1, sizeof(pattern_classifier_t));
    if (!classifier) {
        LOG_ERROR("Failed to allocate pattern classifier");
        return NULL;
    }
    
    classifier->config = *config;
    classifier->cache_info = *cache_info;
    pthread_mutex_init(&classifier->mutex, NULL);
    
    LOG_INFO("Created pattern classifier with confidence threshold %.2f",
             config->min_confidence_threshold);
    
    return classifier;
}

// Destroy pattern classifier
void pattern_classifier_destroy(pattern_classifier_t *classifier) {
    if (!classifier) return;
    
    LOG_INFO("Destroying pattern classifier");
    pthread_mutex_destroy(&classifier->mutex);
    FREE_LOGGED(classifier);
}

// Classify single hotspot
int pattern_classifier_classify_hotspot(pattern_classifier_t *classifier,
                                      const cache_hotspot_t *hotspot,
                                      classified_pattern_t *pattern) {
    if (!classifier || !hotspot || !pattern) {
        LOG_ERROR("Invalid parameters for classify_hotspot");
        return -1;
    }
    
    LOG_DEBUG("Classifying hotspot at %s:%d", 
              hotspot->location.file, hotspot->location.line);
    
    memset(pattern, 0, sizeof(classified_pattern_t));
    pattern->hotspot = (cache_hotspot_t*)hotspot;
    
    // Run detection algorithms
    double max_severity = 0;
    cache_antipattern_t detected_type = HOTSPOT_REUSE;  // Default
    double severity;
    
    // Check for false sharing
    if (hotspot->is_false_sharing || 
        detect_false_sharing_pattern(hotspot, &severity)) {
        if (severity > max_severity) {
            max_severity = severity;
            detected_type = FALSE_SHARING;
            pattern->confidence = 0.95;
        }
    }
    
    // Check for thrashing
    if (detect_thrashing(hotspot, &classifier->cache_info, &severity)) {
        if (severity > max_severity) {
            max_severity = severity;
            detected_type = THRASHING;
            pattern->confidence = 0.85;
        }
    }
    
    // Check for streaming pattern
    if (detect_streaming_pattern(hotspot, &severity)) {
        if (severity > max_severity) {
            max_severity = severity;
            detected_type = STREAMING_EVICTION;
            pattern->confidence = 0.80;
        }
    }
    
    // Check for irregular gather/scatter
    if (detect_irregular_gather_scatter(hotspot, &severity)) {
        if (severity > max_severity) {
            max_severity = severity;
            detected_type = IRREGULAR_GATHER_SCATTER;
            pattern->confidence = 0.75;
        }
    }
    
    // Check access pattern
    switch (hotspot->dominant_pattern) {
        case LOOP_CARRIED_DEP:
            detected_type = LOOP_CARRIED_DEP;
            max_severity = 70;
            pattern->confidence = 0.90;
            break;
            
        case INDIRECT_ACCESS:
            if (detected_type != FALSE_SHARING) {
                detected_type = IRREGULAR_GATHER_SCATTER;
                max_severity = 60;
                pattern->confidence = 0.70;
            }
            break;
            
        case RANDOM:
            if (max_severity < 50) {
                detected_type = UNCOALESCED_ACCESS;
                max_severity = 50;
                pattern->confidence = 0.65;
            }
            break;
            
        default:
            break;
    }
    
    // Set pattern type and severity
    pattern->type = detected_type;
    pattern->severity_score = max_severity;
    
    // Classify miss type
    pattern->primary_miss_type = classify_miss_type(hotspot, &classifier->cache_info);
    
    // Determine affected cache levels
    pattern->affected_cache_levels = 0;
    for (int i = 0; i < 4; i++) {
        if (hotspot->cache_levels_affected[i] > 0) {
            pattern->affected_cache_levels |= (1 << i);
        }
    }
    
    // Calculate performance impact
    pattern->performance_impact = calculate_performance_impact(pattern, &classifier->cache_info);
    
    // Generate human-readable description
    generate_pattern_description(pattern);
    
    LOG_INFO("Classified pattern: %s (severity: %.1f, confidence: %.2f)",
             cache_antipattern_to_string(pattern->type),
             pattern->severity_score, pattern->confidence);
    
    return 0;
}

// Classify all hotspots
int pattern_classifier_classify_all(pattern_classifier_t *classifier,
                                  const cache_hotspot_t *hotspots, int hotspot_count,
                                  classified_pattern_t **patterns, int *pattern_count) {
    if (!classifier || !hotspots || !patterns || !pattern_count || hotspot_count <= 0) {
        LOG_ERROR("Invalid parameters for classify_all");
        return -1;
    }
    
    LOG_INFO("Classifying %d hotspots", hotspot_count);
    
    pthread_mutex_lock(&classifier->mutex);
    
    // Allocate patterns array
    *patterns = CALLOC_LOGGED(hotspot_count, sizeof(classified_pattern_t));
    if (!*patterns) {
        LOG_ERROR("Failed to allocate patterns array");
        pthread_mutex_unlock(&classifier->mutex);
        return -1;
    }
    
    // Classify each hotspot
    int classified = 0;
    for (int i = 0; i < hotspot_count; i++) {
        classified_pattern_t *pattern = &(*patterns)[classified];
        
        if (pattern_classifier_classify_hotspot(classifier, &hotspots[i], pattern) == 0) {
            // Only keep patterns above confidence threshold
            if (pattern->confidence >= classifier->config.min_confidence_threshold) {
                classified++;
            }
        }
    }
    
    *pattern_count = classified;
    
    // Sort by severity
    qsort(*patterns, classified, sizeof(classified_pattern_t),
          [](const void *a, const void *b) {
              const classified_pattern_t *pa = (const classified_pattern_t *)a;
              const classified_pattern_t *pb = (const classified_pattern_t *)b;
              if (pa->severity_score > pb->severity_score) return -1;
              if (pa->severity_score < pb->severity_score) return 1;
              return 0;
          });
    
    pthread_mutex_unlock(&classifier->mutex);
    
    LOG_INFO("Classified %d patterns above confidence threshold", classified);
    return 0;
}

// Detect hotspot reuse pattern
bool detect_hotspot_reuse(const cache_hotspot_t *hotspot, double *severity) {
    if (!hotspot || !severity) return false;
    
    // High miss rate with small address range indicates hotspot reuse
    uint64_t range = hotspot->address_range_end - hotspot->address_range_start;
    
    if (hotspot->miss_rate > 0.5 && range < 4096) {  // Within a page
        *severity = hotspot->miss_rate * 100;
        LOG_DEBUG("Detected hotspot reuse: range=%lu, miss_rate=%.2f",
                  range, hotspot->miss_rate);
        return true;
    }
    
    return false;
}

// Detect thrashing pattern
bool detect_thrashing(const cache_hotspot_t *hotspot, const cache_info_t *cache_info, 
                     double *severity) {
    if (!hotspot || !cache_info || !severity) return false;
    
    // Check if working set exceeds cache size
    uint64_t working_set = hotspot->address_range_end - hotspot->address_range_start;
    
    // Check each cache level
    for (int i = 0; i < cache_info->num_levels; i++) {
        if (hotspot->cache_levels_affected[i] > 0) {
            if (working_set > cache_info->levels[i].size) {
                *severity = 80 + (20 * (i + 1) / cache_info->num_levels);
                LOG_DEBUG("Detected thrashing at L%d: working_set=%lu > cache_size=%zu",
                          i + 1, working_set, cache_info->levels[i].size);
                return true;
            }
        }
    }
    
    // Also check for high miss rate with regular pattern
    if (hotspot->miss_rate > 0.7 && 
        (hotspot->dominant_pattern == SEQUENTIAL || hotspot->dominant_pattern == STRIDED)) {
        *severity = hotspot->miss_rate * 100;
        return true;
    }
    
    return false;
}

// Detect false sharing pattern
bool detect_false_sharing_pattern(const cache_hotspot_t *hotspot, double *severity) {
    if (!hotspot || !severity) return false;
    
    // Already flagged
    if (hotspot->is_false_sharing) {
        *severity = 90;
        return true;
    }
    
    // Additional heuristics
    // Small address range with high miss rate from multiple CPUs
    uint64_t range = hotspot->address_range_end - hotspot->address_range_start;
    
    if (range <= 128 && hotspot->miss_rate > 0.4 && hotspot->sample_count > 100) {
        // Check CPU diversity in samples
        int cpu_mask = 0;
        for (size_t i = 0; i < hotspot->sample_count && i < 100; i++) {
            cpu_mask |= (1 << hotspot->samples[i].cpu_id);
        }
        
        int cpu_count = __builtin_popcount(cpu_mask);
        if (cpu_count >= 2) {
            *severity = 70 + (cpu_count * 5);
            LOG_DEBUG("Detected false sharing: %d CPUs, range=%lu", cpu_count, range);
            return true;
        }
    }
    
    return false;
}

// Detect streaming pattern
bool detect_streaming_pattern(const cache_hotspot_t *hotspot, double *severity) {
    if (!hotspot || !severity) return false;
    
    // Sequential access with high miss rate indicates streaming
    if (hotspot->dominant_pattern == SEQUENTIAL && hotspot->miss_rate > 0.6) {
        *severity = 60 + (hotspot->miss_rate - 0.6) * 100;
        
        // Check if it's a large range (streaming through memory)
        uint64_t range = hotspot->address_range_end - hotspot->address_range_start;
        if (range > 1024 * 1024) {  // > 1MB
            *severity += 10;
            LOG_DEBUG("Detected streaming pattern: range=%lu MB, miss_rate=%.2f",
                      range / (1024 * 1024), hotspot->miss_rate);
            return true;
        }
    }
    
    return false;
}

// Detect irregular gather/scatter
bool detect_irregular_gather_scatter(const cache_hotspot_t *hotspot, double *severity) {
    if (!hotspot || !severity) return false;
    
    // Random or gather/scatter pattern with poor locality
    if (hotspot->dominant_pattern == RANDOM || 
        hotspot->dominant_pattern == GATHER_SCATTER ||
        hotspot->dominant_pattern == INDIRECT_ACCESS) {
        
        // Calculate address entropy (simplified)
        if (hotspot->sample_count >= 10) {
            uint64_t total_distance = 0;
            int distance_count = 0;
            
            for (size_t i = 1; i < hotspot->sample_count && i < 100; i++) {
                uint64_t dist = labs((long)(hotspot->samples[i].memory_addr - 
                                           hotspot->samples[i-1].memory_addr));
                if (dist > 0) {
                    total_distance += dist;
                    distance_count++;
                }
            }
            
            if (distance_count > 0) {
                uint64_t avg_distance = total_distance / distance_count;
                if (avg_distance > 4096) {  // Larger than page size
                    *severity = 50 + (log2(avg_distance / 4096) * 10);
                    if (*severity > 90) *severity = 90;
                    
                    LOG_DEBUG("Detected gather/scatter: avg_distance=%lu", avg_distance);
                    return true;
                }
            }
        }
    }
    
    return false;
}

// Classify miss type
miss_type_t classify_miss_type(const cache_hotspot_t *hotspot, const cache_info_t *cache_info) {
    if (!hotspot || !cache_info) return MISS_COMPULSORY;
    
    // Analyze miss characteristics
    uint64_t working_set = hotspot->address_range_end - hotspot->address_range_start;
    
    // Compulsory miss: first access to data
    if (hotspot->total_accesses < 2 * hotspot->total_misses) {
        return MISS_COMPULSORY;
    }
    
    // Capacity miss: working set exceeds cache size
    for (int i = 0; i < cache_info->num_levels; i++) {
        if (working_set > cache_info->levels[i].size &&
            hotspot->cache_levels_affected[i] > 0) {
            return MISS_CAPACITY;
        }
    }
    
    // Conflict miss: poor access pattern despite small working set
    if (working_set < cache_info->levels[0].size && hotspot->miss_rate > 0.3) {
        return MISS_CONFLICT;
    }
    
    // Coherence miss: multiple CPUs involved
    if (hotspot->is_false_sharing) {
        return MISS_COHERENCE;
    }
    
    // Default to conflict
    return MISS_CONFLICT;
}

// Calculate performance impact
double calculate_performance_impact(const classified_pattern_t *pattern, 
                                   const cache_info_t *cache_info) {
    if (!pattern || !cache_info) return 0;
    
    double impact = 0;
    const cache_hotspot_t *hotspot = pattern->hotspot;
    
    // Base impact from miss rate and latency
    double miss_penalty = hotspot->avg_latency_cycles;
    if (miss_penalty < 10) miss_penalty = 10;  // Minimum penalty
    
    // Calculate cycles lost per access
    double cycles_lost = hotspot->miss_rate * miss_penalty;
    
    // Convert to percentage (assume 1 cycle per hit)
    impact = (cycles_lost / (1 + cycles_lost)) * 100;
    
    // Adjust based on pattern type
    switch (pattern->type) {
        case FALSE_SHARING:
            impact *= 1.5;  // False sharing has additional coherence overhead
            break;
            
        case THRASHING:
            impact *= 1.3;  // Thrashing affects entire working set
            break;
            
        case STREAMING_EVICTION:
            impact *= 0.8;  // Streaming can be prefetched
            break;
            
        default:
            break;
    }
    
    // Cap at reasonable maximum
    if (impact > 90) impact = 90;
    
    LOG_DEBUG("Calculated performance impact: %.1f%% for %s pattern",
              impact, cache_antipattern_to_string(pattern->type));
    
    return impact;
}

// Generate pattern description
void generate_pattern_description(classified_pattern_t *pattern) {
    if (!pattern) return;
    
    const cache_hotspot_t *h = pattern->hotspot;
    
    switch (pattern->type) {
        case HOTSPOT_REUSE:
            snprintf(pattern->description, sizeof(pattern->description),
                     "Hotspot reuse detected: The same memory location is accessed repeatedly "
                     "with %.1f%% miss rate, causing performance degradation.",
                     h->miss_rate * 100);
            snprintf(pattern->root_cause, sizeof(pattern->root_cause),
                     "Likely caused by poor temporal locality or cache contention "
                     "from other memory accesses.");
            break;
            
        case THRASHING:
            snprintf(pattern->description, sizeof(pattern->description),
                     "Cache thrashing detected: Working set size exceeds cache capacity, "
                     "causing %.1f%% miss rate with continuous evictions.",
                     h->miss_rate * 100);
            snprintf(pattern->root_cause, sizeof(pattern->root_cause),
                     "Working set of %zu KB exceeds cache capacity. Consider loop tiling "
                     "or data blocking.",
                     (h->address_range_end - h->address_range_start) / 1024);
            break;
            
        case FALSE_SHARING:
            snprintf(pattern->description, sizeof(pattern->description),
                     "False sharing detected: Multiple threads accessing different data "
                     "in the same cache line, causing coherence misses.");
            snprintf(pattern->root_cause, sizeof(pattern->root_cause),
                     "Different threads are modifying data within the same %d-byte cache line. "
                     "Consider padding or alignment.", 64);  // Typical cache line size
            break;
            
        case IRREGULAR_GATHER_SCATTER:
            snprintf(pattern->description, sizeof(pattern->description),
                     "Irregular memory access pattern: Non-contiguous accesses with poor "
                     "spatial locality (%.1f%% miss rate).", h->miss_rate * 100);
            snprintf(pattern->root_cause, sizeof(pattern->root_cause),
                     "Caused by indirect addressing or scattered data access. "
                     "Consider data structure reorganization.");
            break;
            
        case UNCOALESCED_ACCESS:
            snprintf(pattern->description, sizeof(pattern->description),
                     "Uncoalesced memory accesses: Multiple small accesses that could be "
                     "combined for better cache utilization.");
            snprintf(pattern->root_cause, sizeof(pattern->root_cause),
                     "Small, scattered accesses waste cache bandwidth. "
                     "Consider grouping accesses or using vector operations.");
            break;
            
        case LOOP_CARRIED_DEP:
            snprintf(pattern->description, sizeof(pattern->description),
                     "Loop-carried dependency: Data dependencies between iterations "
                     "prevent efficient caching and parallelization.");
            snprintf(pattern->root_cause, sizeof(pattern->root_cause),
                     "Each iteration depends on previous results, limiting optimization "
                     "opportunities. Consider algorithm restructuring.");
            break;
            
        case STREAMING_EVICTION:
            snprintf(pattern->description, sizeof(pattern->description),
                     "Streaming access pattern: Sequential access through large data "
                     "evicts useful cache contents (%.1f%% miss rate).", h->miss_rate * 100);
            snprintf(pattern->root_cause, sizeof(pattern->root_cause),
                     "Large sequential accesses evict reusable data. "
                     "Consider non-temporal hints or cache bypassing.");
            break;
            
        default:
            snprintf(pattern->description, sizeof(pattern->description),
                     "Cache performance issue detected with %.1f%% miss rate.",
                     h->miss_rate * 100);
            snprintf(pattern->root_cause, sizeof(pattern->root_cause),
                     "Review memory access patterns for optimization opportunities.");
            break;
    }
}

// Print classification results
void pattern_classifier_print_results(const classified_pattern_t *patterns, int count) {
    printf("\n=== Cache Pattern Classification Results ===\n");
    printf("Found %d significant patterns:\n\n", count);
    
    for (int i = 0; i < count; i++) {
        const classified_pattern_t *p = &patterns[i];
        const cache_hotspot_t *h = p->hotspot;
        
        printf("[%d] %s at %s:%d\n", i + 1,
               cache_antipattern_to_string(p->type),
               h->location.file, h->location.line);
        
        printf("    Severity: %.1f/100 (Confidence: %.0f%%)\n",
               p->severity_score, p->confidence * 100);
        
        printf("    Description: %s\n", p->description);
        printf("    Root cause: %s\n", p->root_cause);
        
        printf("    Miss type: %s, Affected levels: ",
               miss_type_to_string(p->primary_miss_type));
        for (int j = 0; j < 4; j++) {
            if (p->affected_cache_levels & (1 << j)) {
                printf("L%d ", j + 1);
            }
        }
        printf("\n");
        
        printf("    Performance impact: %.1f%%\n", p->performance_impact);
        printf("\n");
    }
}

// Export results to JSON
int pattern_classifier_export_json(const classified_pattern_t *patterns, int count,
                                 const char *filename) {
    if (!patterns || count <= 0 || !filename) {
        LOG_ERROR("Invalid parameters for export_json");
        return -1;
    }
    
    FILE *fp = fopen(filename, "w");
    if (!fp) {
        LOG_ERROR("Failed to open %s for writing: %s", filename, strerror(errno));
        return -1;
    }
    
    fprintf(fp, "{\n");
    fprintf(fp, "  \"pattern_count\": %d,\n", count);
    fprintf(fp, "  \"patterns\": [\n");
    
    for (int i = 0; i < count; i++) {
        const classified_pattern_t *p = &patterns[i];
        const cache_hotspot_t *h = p->hotspot;
        
        fprintf(fp, "    {\n");
        fprintf(fp, "      \"type\": \"%s\",\n", cache_antipattern_to_string(p->type));
        fprintf(fp, "      \"location\": {\n");
        fprintf(fp, "        \"file\": \"%s\",\n", h->location.file);
        fprintf(fp, "        \"line\": %d,\n", h->location.line);
        fprintf(fp, "        \"function\": \"%s\"\n", h->location.function);
        fprintf(fp, "      },\n");
        fprintf(fp, "      \"severity\": %.1f,\n", p->severity_score);
        fprintf(fp, "      \"confidence\": %.2f,\n", p->confidence);
        fprintf(fp, "      \"performance_impact\": %.1f,\n", p->performance_impact);
        fprintf(fp, "      \"miss_rate\": %.3f,\n", h->miss_rate);
        fprintf(fp, "      \"total_misses\": %lu,\n", h->total_misses);
        fprintf(fp, "      \"description\": \"%s\",\n", p->description);
        fprintf(fp, "      \"root_cause\": \"%s\"\n", p->root_cause);
        fprintf(fp, "    }%s\n", (i < count - 1) ? "," : "");
    }
    
    fprintf(fp, "  ]\n");
    fprintf(fp, "}\n");
    
    fclose(fp);
    
    LOG_INFO("Exported %d patterns to %s", count, filename);
    return 0;
}

// Get default configuration
classifier_config_t classifier_config_default(void) {
    classifier_config_t config = {
        .min_confidence_threshold = 0.6,
        .enable_ml_classification = false,  // Not implemented yet
        .enable_heuristics = true,
        .analysis_depth = 3,
        .correlate_static_dynamic = true
    };
    
    return config;
}
