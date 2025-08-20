#include "false_sharing_detector.h"
#include <math.h>

static false_sharing_config_t g_config;
static bool g_initialized = false;

// Initialize detector
int false_sharing_detector_init(const false_sharing_config_t *config) {
    if (g_initialized) {
        LOG_WARNING("False sharing detector already initialized");
        return 0;
    }
    
    if (config) {
        g_config = *config;
    } else {
        g_config = false_sharing_config_default();
    }
    
    LOG_INFO("Initialized false sharing detector (cache line: %d bytes)",
             g_config.cache_line_size);
    
    g_initialized = true;
    return 0;
}

// Cleanup detector
void false_sharing_detector_cleanup(void) {
    if (!g_initialized) {
        return;
    }
    
    LOG_INFO("Cleaning up false sharing detector");
    g_initialized = false;
}

// Get cache line address
uint64_t get_cache_line_address(uint64_t address, int cache_line_size) {
    return (address / cache_line_size) * cache_line_size;
}

// Comparison function for sorting candidates by contention score
static int compare_candidates_by_contention(const void *a, const void *b) {
    const false_sharing_candidate_t *c1 = (const false_sharing_candidate_t *)a;
    const false_sharing_candidate_t *c2 = (const false_sharing_candidate_t *)b;
    
    // Sort in descending order by contention_score
    if (c1->contention_score > c2->contention_score) return -1;
    if (c1->contention_score < c2->contention_score) return 1;
    return 0;
}

// Detect false sharing in samples
int detect_false_sharing(const cache_miss_sample_t *samples, int sample_count,
                        false_sharing_results_t *results) {
    if (!samples || sample_count <= 0 || !results) {
        LOG_ERROR("Invalid parameters for detect_false_sharing");
        return -1;
    }
    
    if (!g_initialized) {
        LOG_ERROR("False sharing detector not initialized");
        return -1;
    }
    
    LOG_INFO("Detecting false sharing in %d samples", sample_count);
    
    memset(results, 0, sizeof(false_sharing_results_t));
    
    // Group samples by cache line
    typedef struct cache_line_info {
        uint64_t cache_line;
        cache_miss_sample_t *samples;
        int sample_count;
        int sample_capacity;
    } cache_line_info_t;
    
    // Simple hash table for cache lines
    #define HASH_SIZE 1024
    cache_line_info_t *cache_lines[HASH_SIZE] = {NULL};
    
    // Group samples by cache line
    for (int i = 0; i < sample_count; i++) {
        uint64_t cache_line = get_cache_line_address(samples[i].memory_addr,
                                                    g_config.cache_line_size);
        uint64_t hash = (cache_line / g_config.cache_line_size) % HASH_SIZE;
        
        // Find or create cache line info
        cache_line_info_t *info = cache_lines[hash];
        cache_line_info_t *prev = NULL;
        
        while (info && info->cache_line != cache_line) {
            prev = info;
            info = (cache_line_info_t *)info->samples;  // Reusing pointer as next  // Reusing pointer as next
        }
        
        if (!info) {
            info = CALLOC_LOGGED(1, sizeof(cache_line_info_t));
            if (!info) continue;
            
            info->cache_line = cache_line;
            info->sample_capacity = 16;
            info->samples = MALLOC_LOGGED(info->sample_capacity * sizeof(cache_miss_sample_t));
            
            if (!info->samples) {
                FREE_LOGGED(info);
                continue;
            }
            
            if (prev) {
                prev->samples = (cache_miss_sample_t*)info;  // Chain
            } else {
                cache_lines[hash] = info;
            }
        }
        
        // Add sample to cache line
        if (info->sample_count >= info->sample_capacity) {
            info->sample_capacity *= 2;
            cache_miss_sample_t *new_samples = realloc(info->samples,
                info->sample_capacity * sizeof(cache_miss_sample_t));
            if (new_samples) {
                info->samples = new_samples;
            }
        }
        
        if (info->sample_count < info->sample_capacity) {
            info->samples[info->sample_count++] = samples[i];
        }
    }
    
    // Analyze each cache line for false sharing
    false_sharing_candidate_t *candidates = NULL;
    int candidate_capacity = 16;
    int candidate_count = 0;
    
    candidates = CALLOC_LOGGED(candidate_capacity, sizeof(false_sharing_candidate_t));
    if (!candidates) {
        // Cleanup and return
        for (int i = 0; i < HASH_SIZE; i++) {
            cache_line_info_t *info = cache_lines[i];
            while (info) {
                cache_line_info_t *next = (cache_line_info_t*)info->samples;
                if (info->samples && info->sample_count > 0) {
                    FREE_LOGGED(info->samples);
                }
                FREE_LOGGED(info);
                info = next;
            }
        }
        return -1;
    }
    
    // Check each cache line
    for (int i = 0; i < HASH_SIZE; i++) {
        cache_line_info_t *info = cache_lines[i];
        
        while (info) {
            if (info->sample_count >= g_config.min_thread_count) {
                false_sharing_candidate_t candidate = {0};
                
                if (analyze_cache_line_sharing(info->samples, info->sample_count,
                                             info->cache_line, &candidate) == 0) {
                    if (candidate.num_threads >= g_config.min_thread_count) {
                        // Calculate contention score
                        candidate.contention_score = calculate_contention_score(&candidate);
                        
                        // Verify if it's real false sharing
                        candidate.confirmed = verify_false_sharing(&candidate,
                                                                 info->samples,
                                                                 info->sample_count);
                        
                        // Add to candidates
                        if (candidate_count >= candidate_capacity) {
                            candidate_capacity *= 2;
                            false_sharing_candidate_t *new_candidates = 
                                realloc(candidates, candidate_capacity * 
                                       sizeof(false_sharing_candidate_t));
                            if (new_candidates) {
                                candidates = new_candidates;
                            }
                        }
                        
                        if (candidate_count < candidate_capacity) {
                            candidates[candidate_count++] = candidate;
                            
                            if (candidate.confirmed) {
                                results->confirmed_count++;
                                results->total_impact_score += candidate.contention_score;
                            }
                        }
                    }
                }
            }
            
            cache_line_info_t *next = (cache_line_info_t*)info->samples;
            if (info->samples && info->sample_count > 0) {
                FREE_LOGGED(info->samples);
            }
            FREE_LOGGED(info);
            info = next;
        }
    }
    
    if (candidate_count > 0) {
        qsort(candidates, candidate_count, sizeof(false_sharing_candidate_t),
            compare_candidates_by_contention);
    }
    
    results->candidates = candidates;
    results->candidate_count = candidate_count;
    
    LOG_INFO("Found %d false sharing candidates (%d confirmed)",
             candidate_count, results->confirmed_count);
    
    return 0;
}

// Analyze cache line sharing
int analyze_cache_line_sharing(const cache_miss_sample_t *samples, int count,
                              uint64_t cache_line, false_sharing_candidate_t *candidate) {
    if (!samples || count <= 0 || !candidate) return -1;
    
    memset(candidate, 0, sizeof(false_sharing_candidate_t));
    candidate->cache_line_addr = cache_line;
    
    // Count accesses per thread
    for (int i = 0; i < count; i++) {
        int tid = samples[i].tid;
        
        // Find or add thread
        int thread_idx = -1;
        for (int j = 0; j < candidate->num_threads; j++) {
            if (candidate->thread_ids[j] == tid) {
                thread_idx = j;
                break;
            }
        }
        
        if (thread_idx < 0 && candidate->num_threads < 32) {
            thread_idx = candidate->num_threads++;
            candidate->thread_ids[thread_idx] = tid;
        }
        
        if (thread_idx >= 0) {
            candidate->access_counts[thread_idx]++;
            if (samples[i].is_write) {
                candidate->write_counts[thread_idx]++;
            }
            
            // Track source location
            bool found_location = false;
            for (int j = 0; j < candidate->num_locations; j++) {
                if (candidate->locations[j].line == samples[i].source_loc.line &&
                    strcmp(candidate->locations[j].file, samples[i].source_loc.file) == 0) {
                    found_location = true;
                    break;
                }
            }
            
            if (!found_location && candidate->num_locations < 32) {
                candidate->locations[candidate->num_locations++] = samples[i].source_loc;
            }
        }
    }
    
    LOG_DEBUG("Cache line 0x%lx accessed by %d threads from %d locations",
              cache_line, candidate->num_threads, candidate->num_locations);
    
    return 0;
}

// Calculate contention score
double calculate_contention_score(const false_sharing_candidate_t *candidate) {
    if (!candidate || candidate->num_threads < 2) return 0;
    
    double score = 0;
    
    // Factor 1: Number of threads (more threads = worse)
    score += (candidate->num_threads - 1) * 20;
    
    // Factor 2: Write ratio (more writes = worse)
    uint64_t total_accesses = 0;
    uint64_t total_writes = 0;
    
    for (int i = 0; i < candidate->num_threads; i++) {
        total_accesses += candidate->access_counts[i];
        total_writes += candidate->write_counts[i];
    }
    
    if (total_accesses > 0) {
        double write_ratio = (double)total_writes / total_accesses;
        score += write_ratio * 40;
    }
    
    // Factor 3: Access imbalance (uneven access = worse)
    if (candidate->num_threads > 1) {
        double mean_access = (double)total_accesses / candidate->num_threads;
        double variance = 0;
        
        for (int i = 0; i < candidate->num_threads; i++) {
            double diff = candidate->access_counts[i] - mean_access;
            variance += diff * diff;
        }
        
        variance /= candidate->num_threads;
        double std_dev = sqrt(variance);
        double cv = mean_access > 0 ? std_dev / mean_access : 0;
        
        score += cv * 20;
    }
    
    // Factor 4: Multiple source locations (different code = likely false sharing)
    if (candidate->num_locations > 1) {
        score += 20;
    }
    
    // Cap at 100
    if (score > 100) score = 100;
    
    LOG_DEBUG("Contention score: %.1f", score);
    return score;
}

// Verify false sharing
bool verify_false_sharing(const false_sharing_candidate_t *candidate,
                         const cache_miss_sample_t *samples, int sample_count) {
    if (!candidate || !samples || sample_count <= 0) return false;
    
    // Verification criteria:
    // 1. Multiple threads with writes
    // 2. High miss rate
    // 3. Different variables (if detectable)
    
    int writing_threads = 0;
    for (int i = 0; i < candidate->num_threads; i++) {
        if (candidate->write_counts[i] > 0) {
            writing_threads++;
        }
    }
    
    if (writing_threads < 2) {
        LOG_DEBUG("Not false sharing: only %d thread(s) writing", writing_threads);
        return false;
    }
    
    // Check write ratio
    uint64_t total_accesses = 0;
    uint64_t total_writes = 0;
    
    for (int i = 0; i < candidate->num_threads; i++) {
        total_accesses += candidate->access_counts[i];
        total_writes += candidate->write_counts[i];
    }
    
    double write_ratio = total_accesses > 0 ? (double)total_writes / total_accesses : 0;
    
    if (write_ratio < g_config.min_write_ratio) {
        LOG_DEBUG("Not false sharing: low write ratio %.2f", write_ratio);
        return false;
    }
    
    // Check for different source locations
    if (g_config.require_different_vars && candidate->num_locations < 2) {
        LOG_DEBUG("Not false sharing: single source location");
        return false;
    }
    
    LOG_INFO("Confirmed false sharing at cache line 0x%lx", candidate->cache_line_addr);
    return true;
}

// Generate mitigation suggestions
int generate_mitigation_suggestions(const false_sharing_candidate_t *candidate,
                                   mitigation_suggestion_t **suggestions,
                                   int *suggestion_count) {
    if (!candidate || !suggestions || !suggestion_count) return -1;
    
    *suggestions = CALLOC_LOGGED(4, sizeof(mitigation_suggestion_t));
    if (!*suggestions) return -1;
    
    *suggestion_count = 0;
    
    // Suggestion 1: Padding
    mitigation_suggestion_t *sug = &(*suggestions)[(*suggestion_count)++];
    sug->priority = 5;
    sug->expected_improvement = 50 + candidate->contention_score / 2;
    
    snprintf(sug->suggestion, sizeof(sug->suggestion),
             "Add padding to separate variables into different cache lines");
    
    snprintf(sug->code_example, sizeof(sug->code_example),
             "// Before:\n"
             "struct shared_data {\n"
             "    int thread1_counter;\n"
             "    int thread2_counter;  // False sharing!\n"
             "};\n\n"
             "// After:\n"
             "struct shared_data {\n"
             "    int thread1_counter;\n"
             "    char padding[%d];  // Cache line size - sizeof(int)\n"
             "    int thread2_counter;  // Now in different cache line\n"
             "};",
             g_config.cache_line_size - (int)sizeof(int));
    
    // Suggestion 2: Alignment
    sug = &(*suggestions)[(*suggestion_count)++];
    sug->priority = 4;
    sug->expected_improvement = 40 + candidate->contention_score / 3;
    
    snprintf(sug->suggestion, sizeof(sug->suggestion),
             "Use cache-aligned allocation for thread-local data");
    
    snprintf(sug->code_example, sizeof(sug->code_example),
             "// Align each thread's data to cache line boundary\n"
             "struct alignas(%d) thread_data {\n"
             "    // Thread-specific fields\n"
             "    int counter;\n"
             "    double values[8];\n"
             "};\n\n"
             "// Or use aligned allocation:\n"
             "void *aligned_data;\n"
             "posix_memalign(&aligned_data, %d, sizeof(thread_data));",
             g_config.cache_line_size, g_config.cache_line_size);
    
    // Suggestion 3: Data restructuring
    if (candidate->num_locations > 1) {
        sug = &(*suggestions)[(*suggestion_count)++];
        sug->priority = 3;
        sug->expected_improvement = 30;
        
        snprintf(sug->suggestion, sizeof(sug->suggestion),
                 "Restructure data to group thread-local fields together");
        
        snprintf(sug->code_example, sizeof(sug->code_example),
                 "// Instead of interleaved fields:\n"
                 "// struct { int a1; int b1; int a2; int b2; };\n\n"
                 "// Group by thread:\n"
                 "struct {\n"
                 "    struct { int a1; int a2; } thread1_data;\n"
                 "    char padding[%d];\n"
                 "    struct { int b1; int b2; } thread2_data;\n"
                 "};",
                 g_config.cache_line_size);
    }
    
    LOG_DEBUG("Generated %d mitigation suggestions", *suggestion_count);
    return 0;
}

// Print false sharing results
void print_false_sharing_results(const false_sharing_results_t *results) {
    if (!results) return;
    
    printf("\n=== False Sharing Detection Results ===\n");
    printf("Total candidates: %d\n", results->candidate_count);
    printf("Confirmed cases: %d\n", results->confirmed_count);
    printf("Total impact score: %.1f\n", results->total_impact_score);
    
    if (results->candidate_count > 0) {
        printf("\nTop False Sharing Candidates:\n");
        
        int print_count = results->candidate_count < 10 ? results->candidate_count : 10;
        for (int i = 0; i < print_count; i++) {
            printf("\n[%d] ", i + 1);
            print_false_sharing_candidate(&results->candidates[i]);
        }
    }
}

// Print single candidate
void print_false_sharing_candidate(const false_sharing_candidate_t *candidate) {
    if (!candidate) return;
    
    printf("Cache line 0x%lx %s\n", 
           candidate->cache_line_addr,
           candidate->confirmed ? "[CONFIRMED]" : "[SUSPECTED]");
    
    printf("  Contention score: %.1f/100\n", candidate->contention_score);
    printf("  Threads involved: %d\n", candidate->num_threads);
    
    for (int i = 0; i < candidate->num_threads && i < 5; i++) {
        printf("    Thread %d: %lu accesses (%lu writes)\n",
               candidate->thread_ids[i],
               candidate->access_counts[i],
               candidate->write_counts[i]);
    }
    
    printf("  Source locations: %d\n", candidate->num_locations);
    for (int i = 0; i < candidate->num_locations && i < 3; i++) {
        printf("    %s:%d\n",
               candidate->locations[i].file,
               candidate->locations[i].line);
    }
    
    if (strlen(candidate->description) > 0) {
        printf("  Description: %s\n", candidate->description);
    }
}

// Get default configuration
false_sharing_config_t false_sharing_config_default(void) {
    false_sharing_config_t config = {
        .min_thread_count = 2,
        .min_write_ratio = 0.1,
        .cache_line_size = 64,  // Common x86_64 cache line size
        .time_window_ms = 100.0,
        .require_different_vars = false
    };
    
    return config;
}

// Free results
void free_false_sharing_results(false_sharing_results_t *results) {
    if (!results) return;
    
    if (results->candidates) {
        FREE_LOGGED(results->candidates);
        results->candidates = NULL;
    }
    
    results->candidate_count = 0;
    results->confirmed_count = 0;
    results->total_impact_score = 0;
}
