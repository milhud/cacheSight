#include "recommendation_engine.h"
#include <math.h>

// Internal engine structure
struct recommendation_engine {
    engine_config_t config;
    cache_info_t cache_info;
    
    // Statistics
    int total_recommendations_generated;
    double avg_expected_improvement;
    
    pthread_mutex_t mutex;
};

// Add this function after the recommendation_engine struct definition
static bool isDuplicate(optimization_rec_t *recs, int count, optimization_type_t type, 
                       const classified_pattern_t *pattern) {
    for (int i = 0; i < count; i++) {
        if (recs[i].type == type &&
            recs[i].pattern && pattern && 
            recs[i].pattern->hotspot == pattern->hotspot) {
            return true;
        }
    }
    return false;
}

// Add this helper function at the top of the file after the isDuplicate function
static bool hasConflictingPattern(optimization_rec_t *recs, int count, 
                                 const classified_pattern_t *pattern) {
    if (!pattern || !pattern->hotspot) return false;
    
    for (int i = 0; i < count; i++) {
        if (recs[i].pattern && recs[i].pattern->hotspot &&
            recs[i].pattern->hotspot->location.line == pattern->hotspot->location.line &&
            strcmp(recs[i].pattern->hotspot->location.file, pattern->hotspot->location.file) == 0) {
            
            // Check for conflicting access patterns
            access_pattern_t existing = recs[i].pattern->hotspot->dominant_pattern;
            access_pattern_t new_pattern = pattern->hotspot->dominant_pattern;
            
            // SEQUENTIAL conflicts with GATHER_SCATTER
            if ((existing == SEQUENTIAL && new_pattern == GATHER_SCATTER) ||
                (existing == GATHER_SCATTER && new_pattern == SEQUENTIAL)) {
                return true;
            }
        }
    }
    return false;
}

int recommendation_engine_save_to_file(const optimization_rec_t *recs, int count,
                                      const char *filename) {
    FILE *fp = fopen(filename, "w");
    if (!fp) {
        LOG_ERROR("Failed to open %s for writing", filename);
        return -1;
    }
    
    fprintf(fp, "Cache Optimization Recommendations\n");
    fprintf(fp, "==================================\n\n");
    fprintf(fp, "Total recommendations: %d\n\n", count);
    
    // Group by location for better readability
    int rec_num = 1;
    for (int i = 0; i < count; i++) {
        const optimization_rec_t *rec = &recs[i];
        
        // Check if this is a new location
        bool new_location = true;
        if (i > 0 && recs[i-1].pattern && rec->pattern &&
            recs[i-1].pattern->hotspot && rec->pattern->hotspot) {
            cache_hotspot_t *prev = recs[i-1].pattern->hotspot;
            cache_hotspot_t *curr = rec->pattern->hotspot;
            if (prev->location.line == curr->location.line &&
                strcmp(prev->location.file, curr->location.file) == 0) {
                new_location = false;
            }
        }
        
        if (new_location) {
            fprintf(fp, "\n========================================\n\n");
        }
        
        fprintf(fp, "Recommendation #%d\n", rec_num++);
        fprintf(fp, "-----------------\n");
        fprintf(fp, "Type: %s\n", optimization_type_to_string(rec->type));
        fprintf(fp, "Priority: %d\n", rec->priority);
        fprintf(fp, "Expected Improvement: %.1f%%\n", rec->expected_improvement);
        fprintf(fp, "Confidence: %.0f%%\n", rec->confidence_score * 100);
        fprintf(fp, "Implementation Difficulty: %d/10\n", rec->implementation_difficulty);
        
        if (rec->pattern && rec->pattern->hotspot) {
            fprintf(fp, "Location: %s:%d\n",
                   rec->pattern->hotspot->location.file,
                   rec->pattern->hotspot->location.line);
        }
        
        fprintf(fp, "\nRationale:\n%s\n", rec->rationale);
        
        if (strlen(rec->compiler_flags) > 0) {
            fprintf(fp, "\nCompiler Flags:\n%s\n", rec->compiler_flags);
        }
        
        if (strlen(rec->implementation_guide) > 0) {
            fprintf(fp, "\nImplementation Guide:\n%s\n", rec->implementation_guide);
        }
        
        if (strlen(rec->code_suggestion) > 0) {
            fprintf(fp, "\nCode Example:\n%s\n", rec->code_suggestion);
        }
        
        fprintf(fp, "\n");
    }
    
    fclose(fp);
    LOG_INFO("Saved %d recommendations to %s", count, filename);
    return 0;
}

int recommendation_engine_analyze_all(recommendation_engine_t *engine,
                                     const classified_pattern_t *patterns,
                                     int pattern_count,
                                     optimization_rec_t **all_recommendations,
                                     int *total_rec_count) {
    if (!engine || !patterns || pattern_count <= 0 || !all_recommendations || !total_rec_count) {
        LOG_ERROR("Invalid parameters for recommendation_engine_analyze_all");
        return -1;
    }
    
    // Pre-allocate space for all potential recommendations
    int max_total = pattern_count * engine->config.max_recommendations;
    optimization_rec_t *temp_recs = CALLOC_LOGGED(max_total, sizeof(optimization_rec_t));
    if (!temp_recs) {
        return -1;
    }
    
    int total_count = 0;
    
    // Process each pattern
    for (int i = 0; i < pattern_count; i++) {
        optimization_rec_t *recs = NULL;
        int count = 0;
        
        if (recommendation_engine_analyze(engine, &patterns[i], &recs, &count) == 0) {
            // Copy recommendations, checking for duplicates
            for (int j = 0; j < count; j++) {
                bool is_duplicate = false;
                
                // Check against existing recommendations
                for (int k = 0; k < total_count; k++) {
                    if (temp_recs[k].type == recs[j].type &&
                        temp_recs[k].pattern && recs[j].pattern &&
                        temp_recs[k].pattern->hotspot && recs[j].pattern->hotspot) {
                        
                        cache_hotspot_t *h1 = temp_recs[k].pattern->hotspot;
                        cache_hotspot_t *h2 = recs[j].pattern->hotspot;
                        
                        if (h1->location.line == h2->location.line &&
                            strcmp(h1->location.file, h2->location.file) == 0) {
                            is_duplicate = true;
                            break;
                        }
                    }
                }
                
                if (!is_duplicate && total_count < max_total) {
                    temp_recs[total_count++] = recs[j];
                }
            }
            FREE_LOGGED(recs);
        }
    }
    
    // Filter conflicting recommendations
    total_count = filter_conflicting_recommendations(temp_recs, total_count);
    
    // Rank all recommendations together
    rank_recommendations(temp_recs, total_count);
    
    // Copy to final array
    *all_recommendations = CALLOC_LOGGED(total_count, sizeof(optimization_rec_t));
    if (!*all_recommendations) {
        FREE_LOGGED(temp_recs);
        return -1;
    }
    
    memcpy(*all_recommendations, temp_recs, total_count * sizeof(optimization_rec_t));
    *total_rec_count = total_count;
    
    FREE_LOGGED(temp_recs);
    
    LOG_INFO("Generated %d total recommendations after deduplication", total_count);
    return 0;
}

// Create recommendation engine
recommendation_engine_t* recommendation_engine_create(const engine_config_t *config,
                                                    const cache_info_t *cache_info) {
    if (!config || !cache_info) {
        LOG_ERROR("NULL parameters for recommendation_engine_create");
        return NULL;
    }
    
    recommendation_engine_t *engine = CALLOC_LOGGED(1, sizeof(recommendation_engine_t));
    if (!engine) {
        LOG_ERROR("Failed to allocate recommendation engine");
        return NULL;
    }
    
    engine->config = *config;
    engine->cache_info = *cache_info;
    pthread_mutex_init(&engine->mutex, NULL);
    
    LOG_INFO("Created recommendation engine with min improvement threshold %.1f%%",
             config->min_expected_improvement);
    
    return engine;
}

// Destroy recommendation engine
void recommendation_engine_destroy(recommendation_engine_t *engine) {
    if (!engine) return;
    
    LOG_INFO("Destroying recommendation engine");
    pthread_mutex_destroy(&engine->mutex);
    FREE_LOGGED(engine);
}




// Analyze single pattern
// Analyze single pattern with comprehensive pattern-specific recommendations
int recommendation_engine_analyze(recommendation_engine_t *engine,
                                 const classified_pattern_t *pattern,
                                 optimization_rec_t **recommendations,
                                 int *rec_count) {
    if (!engine || !pattern || !recommendations || !rec_count) {
        LOG_ERROR("Invalid parameters for recommendation_engine_analyze");
        return -1;
    }
    
    LOG_INFO("Analyzing pattern %s for optimizations (access pattern: %s)",
             cache_antipattern_to_string(pattern->type),
             pattern->hotspot ? access_pattern_to_string(pattern->hotspot->dominant_pattern) : "unknown");
    
    pthread_mutex_lock(&engine->mutex);

     // Replace or enhance the existing DEBUG log with:
    LOG_DEBUG("=== RECOMMENDATION ANALYSIS ===");
    LOG_DEBUG("Pattern type: %s (enum: %d)", 
            cache_antipattern_to_string(pattern->type), pattern->type);
    if (pattern->hotspot) {
        LOG_DEBUG("Access pattern: %s (enum: %d)", 
                access_pattern_to_string(pattern->hotspot->dominant_pattern),
                pattern->hotspot->dominant_pattern);
        LOG_DEBUG("Location: %s:%d", 
                pattern->hotspot->location.file,
                pattern->hotspot->location.line);
        LOG_DEBUG("Total misses: %lu, Miss rate: %.2f%%", 
                pattern->hotspot->total_misses,
                pattern->hotspot->miss_rate * 100);
    } else {
        LOG_DEBUG("WARNING: No hotspot data!");
    }

    // Log which case we're entering
    LOG_DEBUG("Entering switch case for access pattern: %d", 
            pattern->hotspot ? pattern->hotspot->dominant_pattern : -1);
    LOG_DEBUG("=== END REC ANALYSIS ===\n");
    
    // Allocate recommendations array
    optimization_rec_t *recs = CALLOC_LOGGED(engine->config.max_recommendations,
                                            sizeof(optimization_rec_t));
    if (!recs) {
        LOG_ERROR("Failed to allocate recommendations");
        pthread_mutex_unlock(&engine->mutex);
        return -1;
    }
    
    int count = 0;
    
    // First, check the underlying access pattern for more specific recommendations
    if (pattern->hotspot) {
        access_pattern_t access_pattern = pattern->hotspot->dominant_pattern;
        
        // Pattern-specific recommendations based on ACCESS pattern
        switch (access_pattern) {
            case SEQUENTIAL:
                // Sequential access - vectorization is key
                if (count < engine->config.max_recommendations) {
                    optimization_rec_t *rec = &recs[count];
                    rec->type = OPT_LOOP_VECTORIZE;
                    rec->pattern = (classified_pattern_t*)pattern;
                    rec->expected_improvement = 40.0;
                    rec->confidence_score = 0.9;
                    rec->implementation_difficulty = 3;
                    rec->priority = 1;
                    
                    snprintf(rec->code_suggestion, sizeof(rec->code_suggestion),
                            "// Vectorize sequential access\n"
                            "#pragma omp simd\n"
                            "for (int i = 0; i < n; i++) {\n"
                            "    sum += data[i];\n"
                            "}\n\n"
                            "// Or use intrinsics for more control:\n"
                            "#include <immintrin.h>\n"
                            "__m256d vsum = _mm256_setzero_pd();\n"
                            "for (int i = 0; i < n; i += 4) {\n"
                            "    __m256d vdata = _mm256_load_pd(&data[i]);\n"
                            "    vsum = _mm256_add_pd(vsum, vdata);\n"
                            "}");
                    
                    snprintf(rec->implementation_guide, sizeof(rec->implementation_guide),
                            "1. Ensure data is aligned to 32-byte boundaries\n"
                            "2. Use -march=native for auto-vectorization\n"
                            "3. Consider #pragma omp simd for explicit vectorization\n"
                            "4. Check vectorization report with -fopt-info-vec");
                    
                    snprintf(rec->rationale, sizeof(rec->rationale),
                            "Sequential access patterns are ideal for SIMD vectorization. "
                            "Processing 4-8 elements simultaneously can improve performance by 4-8x.");
                    
                    snprintf(rec->compiler_flags, sizeof(rec->compiler_flags),
                            "-O3 -march=native -ftree-vectorize -fopt-info-vec");
                    
                    // Check for duplicate before incrementing
                    if (!isDuplicate(recs, count, recs[count].type, pattern)) {
                        // Set priority based on expected improvement
                        if (recs[count].expected_improvement > 50) {
                            recs[count].priority = 1;
                        } else if (recs[count].expected_improvement > 30) {
                            recs[count].priority = 2;
                        } else {
                            recs[count].priority = 3;
                        }
                        count++;
                    };
                }
                
                // Also add prefetching for sequential
                if (count < engine->config.max_recommendations) {
                    if (generate_prefetch_recommendation(pattern, &recs[count]) == 0) {
                        recs[count].expected_improvement = 15.0; // Lower for sequential
                        // Check for duplicate before incrementing
                        if (!isDuplicate(recs, count, recs[count].type, pattern)) {
                            // Set priority based on expected improvement
                            if (recs[count].expected_improvement > 50) {
                                recs[count].priority = 1;
                            } else if (recs[count].expected_improvement > 30) {
                                recs[count].priority = 2;
                            } else {
                                recs[count].priority = 3;
                            }
                            // Check for duplicate before incrementing
                            if (!isDuplicate(recs, count, recs[count].type, pattern)) {
                                // Set priority based on expected improvement
                                if (recs[count].expected_improvement > 50) {
                                    recs[count].priority = 1;
                                } else if (recs[count].expected_improvement > 30) {
                                    recs[count].priority = 2;
                                } else {
                                    recs[count].priority = 3;
                                }
                                count++;
                            }
                        }
                    }
                }
                break;

                case ACCESS_LOOP_CARRIED_DEP:
                    // Loop-carried dependencies - need to break dependencies
                    if (count < engine->config.max_recommendations) {
                        optimization_rec_t *rec = &recs[count];
                        rec->type = OPT_LOOP_UNROLL;
                        rec->pattern = (classified_pattern_t*)pattern;
                        rec->expected_improvement = 25.0;
                        rec->confidence_score = 0.7;
                        rec->implementation_difficulty = 5;
                        rec->priority = 2;
                        
                        snprintf(rec->code_suggestion, sizeof(rec->code_suggestion),
                                "// Break loop-carried dependencies with unrolling\n"
                                "// Original loop with dependency:\n"
                                "// for (int i = 1; i < n; i++) {\n"
                                "//     a[i] = a[i-1] + b[i];\n"
                                "// }\n\n"
                                "// Unrolled version:\n"
                                "for (int i = 1; i < n-3; i += 4) {\n"
                                "    a[i] = a[i-1] + b[i];\n"
                                "    a[i+1] = a[i] + b[i+1];\n"
                                "    a[i+2] = a[i+1] + b[i+2];\n"
                                "    a[i+3] = a[i+2] + b[i+3];\n"
                                "}\n"
                                "// Handle remainder\n"
                                "for (int i = n - (n-1)%%4; i < n; i++) {\n"
                                "    a[i] = a[i-1] + b[i];\n"
                                "}");
                        
                        snprintf(rec->implementation_guide, sizeof(rec->implementation_guide),
                                "1. Identify the dependency chain in the loop\n"
                                "2. Unroll by a factor that allows parallel execution\n"
                                "3. Consider using reduction operations if possible\n"
                                "4. Profile to ensure unrolling improves performance");
                        
                        snprintf(rec->rationale, sizeof(rec->rationale),
                                "Loop-carried dependencies prevent parallelization and vectorization. "
                                "Unrolling can expose instruction-level parallelism.");
                        
                        snprintf(rec->compiler_flags, sizeof(rec->compiler_flags),
                                "-funroll-loops --param max-unroll-times=4");
                        
                        // Check for duplicate before incrementing
                        if (!isDuplicate(recs, count, recs[count].type, pattern)) {
                            // Set priority based on expected improvement
                            if (recs[count].expected_improvement > 50) {
                                recs[count].priority = 1;
                            } else if (recs[count].expected_improvement > 30) {
                                recs[count].priority = 2;
                            } else {
                                recs[count].priority = 3;
                            }
                            count++;
                        }
                    }
                    break;
                
            case STRIDED:
                // Strided access - depends on stride size
                if (pattern->hotspot->access_stride > 8) {
                    // Large stride - loop tiling crucial
                    if (count < engine->config.max_recommendations) {
                        if (generate_loop_tiling_recommendation(pattern, &engine->cache_info,
                                                               &recs[count]) == 0) {
                            // Check for duplicate before incrementing
                            if (!isDuplicate(recs, count, recs[count].type, pattern)) {
                                // Set priority based on expected improvement
                                if (recs[count].expected_improvement > 50) {
                                    recs[count].priority = 1;
                                } else if (recs[count].expected_improvement > 30) {
                                    recs[count].priority = 2;
                                } else {
                                    recs[count].priority = 3;
                                }
                                count++;
                            }
                        }
                    }
                    
                    // Consider gather operations
                    if (count < engine->config.max_recommendations) {
                        optimization_rec_t *rec = &recs[count];
                        rec->type = OPT_LOOP_VECTORIZE;
                        rec->pattern = (classified_pattern_t*)pattern;
                        rec->expected_improvement = 25.0;
                        rec->confidence_score = 0.7;
                        rec->implementation_difficulty = 5;
                        rec->priority = 2;
                        
                        snprintf(rec->code_suggestion, sizeof(rec->code_suggestion),
                                "// Use gather instructions for strided access\n"
                                "#include <immintrin.h>\n"
                                "__m256i vindices = _mm256_set_epi32(7*stride, 6*stride, 5*stride, 4*stride,\n"
                                "                                     3*stride, 2*stride, stride, 0);\n"
                                "for (int i = 0; i < n; i += 8) {\n"
                                "    __m256d vdata = _mm256_i32gather_pd(&data[i], vindices, 8);\n"
                                "    // Process vdata\n"
                                "}");
                        
                        snprintf(rec->rationale, sizeof(rec->rationale),
                                "Large stride (%d) causes cache line waste. "
                                "Gather instructions can improve efficiency.",
                                pattern->hotspot->access_stride);
                        
                        // Check for duplicate before incrementing
                        if (!isDuplicate(recs, count, recs[count].type, pattern)) {
                            // Set priority based on expected improvement
                            if (recs[count].expected_improvement > 50) {
                                recs[count].priority = 1;
                            } else if (recs[count].expected_improvement > 30) {
                                recs[count].priority = 2;
                            } else {
                                recs[count].priority = 3;
                            }
                            count++;
                        }
                    }
                }
                break;
                
            case RANDOM:
                // Random access - prefetching won't help
                if (count < engine->config.max_recommendations) {
                    optimization_rec_t *rec = &recs[count];
                    rec->type = OPT_DATA_LAYOUT_CHANGE;
                    rec->pattern = (classified_pattern_t*)pattern;
                    rec->expected_improvement = 35.0;
                    rec->confidence_score = 0.6;
                    rec->implementation_difficulty = 8;
                    rec->priority = 1;
                    
                    snprintf(rec->code_suggestion, sizeof(rec->code_suggestion),
                            "// Option 1: Sort indices for better locality\n"
                            "int sorted_indices[N];\n"
                            "memcpy(sorted_indices, indices, N * sizeof(int));\n"
                            "qsort(sorted_indices, N, sizeof(int), compare_int);\n"
                            "for (int i = 0; i < N; i++) {\n"
                            "    sum += data[sorted_indices[i]];\n"
                            "}\n\n"
                            "// Option 2: Use software cache/memoization\n"
                            "struct cache_line {\n"
                            "    int tag;\n"
                            "    double values[8];\n"
                            "} sw_cache[CACHE_SIZE];");
                    
                    snprintf(rec->implementation_guide, sizeof(rec->implementation_guide),
                            "1. Sort indices if possible to improve locality\n"
                            "2. Implement software caching for frequently accessed data\n"
                            "3. Consider data structure reorganization\n"
                            "4. Use smaller data types if possible");
                    
                    snprintf(rec->rationale, sizeof(rec->rationale),
                            "Random access patterns cannot benefit from hardware prefetching. "
                            "Reorganizing access order or data layout is necessary.");
                    
                   // Check for duplicate before incrementing
                    if (!isDuplicate(recs, count, recs[count].type, pattern)) {
                        // Set priority based on expected improvement
                        if (recs[count].expected_improvement > 50) {
                            recs[count].priority = 1;
                        } else if (recs[count].expected_improvement > 30) {
                            recs[count].priority = 2;
                        } else {
                            recs[count].priority = 3;
                        }
                        count++;
                    }
                }
                
                // Memory pooling for random access
                if (count < engine->config.max_recommendations) {
                    optimization_rec_t *rec = &recs[count];
                    rec->type = OPT_MEMORY_POOLING;
                    rec->pattern = (classified_pattern_t*)pattern;
                    rec->expected_improvement = 20.0;
                    rec->confidence_score = 0.7;
                    rec->implementation_difficulty = 6;
                    rec->priority = 2;
                    
                    snprintf(rec->code_suggestion, sizeof(rec->code_suggestion),
                            "// Use memory pool to improve locality\n"
                            "typedef struct {\n"
                            "    void* blocks[MAX_BLOCKS];\n"
                            "    size_t block_size;\n"
                            "    int free_list[MAX_BLOCKS];\n"
                            "} memory_pool_t;\n\n"
                            "// Allocate from pool instead of malloc\n"
                            "data = pool_alloc(&pool, size);");
                    
                    snprintf(rec->rationale, sizeof(rec->rationale),
                            "Memory pooling keeps related data together, "
                            "improving cache locality for random access.");
                    
                    // Check for duplicate before incrementing
                    if (!isDuplicate(recs, count, recs[count].type, pattern)) {
                        // Set priority based on expected improvement
                        if (recs[count].expected_improvement > 50) {
                            recs[count].priority = 1;
                        } else if (recs[count].expected_improvement > 30) {
                            recs[count].priority = 2;
                        } else {
                            recs[count].priority = 3;
                        }
                        count++;
                    }
                }
                break;
                
            case GATHER_SCATTER:
                // Gather/scatter - needs SoA transformation
                if (count < engine->config.max_recommendations) {
                    if (generate_data_layout_recommendation(pattern, &recs[count]) == 0) {
                        // Check for duplicate before incrementing
                        if (!isDuplicate(recs, count, recs[count].type, pattern)) {
                            // Set priority based on expected improvement
                            if (recs[count].expected_improvement > 50) {
                                recs[count].priority = 1;
                            } else if (recs[count].expected_improvement > 30) {
                                recs[count].priority = 2;
                            } else {
                                recs[count].priority = 3;
                            }
                            count++;
                        }
                    }
                }
                
                // Gather prefetch for modern CPUs
                if (count < engine->config.max_recommendations) {
                    optimization_rec_t *rec = &recs[count];
                    rec->type = OPT_PREFETCH_HINTS;
                    rec->pattern = (classified_pattern_t*)pattern;
                    rec->expected_improvement = 18.0;
                    rec->confidence_score = 0.6;
                    rec->implementation_difficulty = 7;
                    rec->priority = 3;
                    
                    snprintf(rec->code_suggestion, sizeof(rec->code_suggestion),
                            "// Gather prefetch for indirect access\n"
                            "#ifdef __AVX512PF__\n"
                            "_mm512_prefetch_i32gather_pd(vindices, base_addr, 8, _MM_HINT_T0);\n"
                            "#else\n"
                            "// Manual gather prefetch\n"
                            "for (int i = 0; i < n; i++) {\n"
                            "    __builtin_prefetch(&data[indices[i+8]], 0, 1);\n"
                            "    result[i] = data[indices[i]];\n"
                            "}\n"
                            "#endif");
                    
                    snprintf(rec->rationale, sizeof(rec->rationale),
                            "Gather/scatter patterns can benefit from specialized "
                            "prefetch instructions on modern CPUs.");
                    
                    // Check for duplicate before incrementing
                    if (!isDuplicate(recs, count, recs[count].type, pattern)) {
                        // Set priority based on expected improvement
                        if (recs[count].expected_improvement > 50) {
                            recs[count].priority = 1;
                        } else if (recs[count].expected_improvement > 30) {
                            recs[count].priority = 2;
                        } else {
                            recs[count].priority = 3;
                        }
                        count++;
                    }
                }
                break;
                
            case NESTED_LOOP:
                // Poor loop nesting - need interchange
                if (count < engine->config.max_recommendations) {
                    optimization_rec_t *rec = &recs[count];
                    rec->type = OPT_ACCESS_REORDER;
                    rec->pattern = (classified_pattern_t*)pattern;
                    rec->expected_improvement = 60.0;
                    rec->confidence_score = 0.95;
                    rec->implementation_difficulty = 2;
                    rec->priority = 1;
                    
                    snprintf(rec->code_suggestion, sizeof(rec->code_suggestion),
                            "// Original column-major access (poor)\n"
                            "// for (int j = 0; j < N; j++)\n"
                            "//     for (int i = 0; i < M; i++)\n"
                            "//         sum += matrix[i][j];\n\n"
                            "// Optimized row-major access\n"
                            "for (int i = 0; i < M; i++) {\n"
                            "    for (int j = 0; j < N; j++) {\n"
                            "        sum += matrix[i][j];  // Sequential in memory\n"
                            "    }\n"
                            "}\n\n"
                            "// Or use loop interchange pragma\n"
                            "#pragma GCC ivdep\n"
                            "#pragma GCC loop interchange");
                    
                    snprintf(rec->implementation_guide, sizeof(rec->implementation_guide),
                            "1. Swap loop order to access memory sequentially\n"
                            "2. Inner loop should iterate over contiguous memory\n"
                            "3. Use compiler pragmas for automatic interchange\n"
                            "4. Consider cache-oblivious algorithms");
                    
                    snprintf(rec->rationale, sizeof(rec->rationale),
                            "Column-major access in row-major layout causes "
                            "cache misses on every access. Loop interchange provides "
                            "immediate and significant improvement.");
                    
                    // Check for duplicate before incrementing
                    if (!isDuplicate(recs, count, recs[count].type, pattern)) {
                        // Set priority based on expected improvement
                        if (recs[count].expected_improvement > 50) {
                            recs[count].priority = 1;
                        } else if (recs[count].expected_improvement > 30) {
                            recs[count].priority = 2;
                        } else {
                            recs[count].priority = 3;
                        }
                        count++;
                    }
                }
                break;
                
            case INDIRECT_ACCESS:
                // Indirect access through pointers
                if (count < engine->config.max_recommendations) {
                    optimization_rec_t *rec = &recs[count];
                    rec->type = OPT_CACHE_BLOCKING;
                    rec->pattern = (classified_pattern_t*)pattern;
                    rec->expected_improvement = 30.0;
                    rec->confidence_score = 0.7;
                    rec->implementation_difficulty = 5;
                    rec->priority = 2;
                    
                    snprintf(rec->code_suggestion, sizeof(rec->code_suggestion),
                            "// Cache blocking for indirect access\n"
                            "#define BLOCK_SIZE 64\n"
                            "// Process in cache-sized blocks\n"
                            "for (int block = 0; block < n; block += BLOCK_SIZE) {\n"
                            "    int block_end = min(block + BLOCK_SIZE, n);\n"
                            "    // First pass: prefetch\n"
                            "    for (int i = block; i < block_end; i++) {\n"
                            "        __builtin_prefetch(pointers[i], 0, 3);\n"
                            "    }\n"
                            "    // Second pass: process\n"
                            "    for (int i = block; i < block_end; i++) {\n"
                            "        sum += *pointers[i];\n"
                            "    }\n"
                            "}");
                    
                    snprintf(rec->rationale, sizeof(rec->rationale),
                            "Indirect pointer access benefits from blocking "
                            "to keep pointers in cache during processing.");
                    
                    // Check for duplicate before incrementing
                    if (!isDuplicate(recs, count, recs[count].type, pattern)) {
                        // Set priority based on expected improvement
                        if (recs[count].expected_improvement > 50) {
                            recs[count].priority = 1;
                        } else if (recs[count].expected_improvement > 30) {
                            recs[count].priority = 2;
                        } else {
                            recs[count].priority = 3;
                        }
                        count++;
                    }
                }
                break;
        }
    }
    
    // Now handle cache antipattern-specific recommendations
    switch (pattern->type) {
        case THRASHING:
            // Working set too large
            if (count < engine->config.max_recommendations) {
                if (generate_loop_tiling_recommendation(pattern, &engine->cache_info,
                                                       &recs[count]) == 0) {
                    // Check for duplicate before incrementing
                    if (!isDuplicate(recs, count, recs[count].type, pattern)) {
                        // Set priority based on expected improvement
                        if (recs[count].expected_improvement > 50) {
                            recs[count].priority = 1;
                        } else if (recs[count].expected_improvement > 30) {
                            recs[count].priority = 2;
                        } else {
                            recs[count].priority = 3;
                        }
                        count++;
                    }
                }
            }
            
            // Cache blocking
            if (count < engine->config.max_recommendations) {
                optimization_rec_t *rec = &recs[count];
                rec->type = OPT_CACHE_BLOCKING;
                rec->pattern = (classified_pattern_t*)pattern;
                rec->expected_improvement = 45.0;
                rec->confidence_score = 0.85;
                rec->implementation_difficulty = 5;
                rec->priority = 1;
                
                snprintf(rec->code_suggestion, sizeof(rec->code_suggestion),
                        "// Cache blocking to reduce working set\n"
                        "const int L1_BLOCK = 32;   // Fit in L1\n"
                        "const int L2_BLOCK = 128;  // Fit in L2\n"
                        "const int L3_BLOCK = 512;  // Fit in L3\n\n"
                        "for (int l3 = 0; l3 < n; l3 += L3_BLOCK) {\n"
                        "    for (int l2 = l3; l2 < min(l3 + L3_BLOCK, n); l2 += L2_BLOCK) {\n"
                        "        for (int l1 = l2; l1 < min(l2 + L2_BLOCK, n); l1 += L1_BLOCK) {\n"
                        "            // Process L1-sized block\n"
                        "        }\n"
                        "    }\n"
                        "}");
                
                snprintf(rec->rationale, sizeof(rec->rationale),
                        "Multi-level cache blocking keeps data in appropriate "
                        "cache levels, preventing thrashing.");
                
                // Check for duplicate before incrementing
                if (!isDuplicate(recs, count, recs[count].type, pattern)) {
                    // Set priority based on expected improvement
                    if (recs[count].expected_improvement > 50) {
                        recs[count].priority = 1;
                    } else if (recs[count].expected_improvement > 30) {
                        recs[count].priority = 2;
                    } else {
                        recs[count].priority = 3;
                    }
                    count++;
                }
            }
            break;
            
        case FALSE_SHARING:
            // Multiple threads accessing same cache line
            if (count < engine->config.max_recommendations) {
                if (generate_alignment_recommendation(pattern, &recs[count]) == 0) {
                    // Check for duplicate before incrementing
                    if (!isDuplicate(recs, count, recs[count].type, pattern)) {
                        // Set priority based on expected improvement
                        if (recs[count].expected_improvement > 50) {
                            recs[count].priority = 1;
                        } else if (recs[count].expected_improvement > 30) {
                            recs[count].priority = 2;
                        } else {
                            recs[count].priority = 3;
                        }
                        count++;
                    }
                }
            }
            
            // Thread-local storage
            if (count < engine->config.max_recommendations) {
                optimization_rec_t *rec = &recs[count];
                rec->type = OPT_ACCESS_REORDER;
                rec->pattern = (classified_pattern_t*)pattern;
                rec->expected_improvement = 40.0;
                rec->confidence_score = 0.9;
                rec->implementation_difficulty = 3;
                rec->priority = 1;
                
                snprintf(rec->code_suggestion, sizeof(rec->code_suggestion),
                        "// Use thread-local storage\n"
                        "__thread int local_counter = 0;\n\n"
                        "// Or use thread-local accumulation\n"
                        "int local_results[NUM_THREADS];\n"
                        "#pragma omp parallel\n"
                        "{\n"
                        "    int tid = omp_get_thread_num();\n"
                        "    int local_sum = 0;\n"
                        "    #pragma omp for\n"
                        "    for (int i = 0; i < n; i++) {\n"
                        "        local_sum += data[i];\n"
                        "    }\n"
                        "    local_results[tid] = local_sum;\n"
                        "}");
                
                snprintf(rec->rationale, sizeof(rec->rationale),
                        "Thread-local storage eliminates false sharing by giving "
                        "each thread its own cache lines.");
                
                // Check for duplicate before incrementing
                if (!isDuplicate(recs, count, recs[count].type, pattern)) {
                    // Set priority based on expected improvement
                    if (recs[count].expected_improvement > 50) {
                        recs[count].priority = 1;
                    } else if (recs[count].expected_improvement > 30) {
                        recs[count].priority = 2;
                    } else {
                        recs[count].priority = 3;
                    }
                    count++;
                }
            }
            break;
            
        case STREAMING_EVICTION:
            // Streaming data evicts useful data
            if (count < engine->config.max_recommendations) {
                optimization_rec_t *rec = &recs[count];
                rec->type = OPT_PREFETCH_HINTS;
                rec->pattern = (classified_pattern_t*)pattern;
                rec->expected_improvement = 25.0;
                rec->confidence_score = 0.8;
                rec->implementation_difficulty = 4;
                rec->priority = 2;
                
                snprintf(rec->code_suggestion, sizeof(rec->code_suggestion),
                        "// Non-temporal stores for streaming data\n"
                        "#include <immintrin.h>\n"
                        "for (int i = 0; i < large_n; i += 4) {\n"
                        "    __m256d vdata = _mm256_load_pd(&input[i]);\n"
                        "    // Process vdata\n"
                        "    _mm256_stream_pd(&output[i], vdata);  // Bypass cache\n"
                        "}\n"
                        "_mm_sfence();  // Ensure completion\n\n"
                        "// Or use compiler intrinsics\n"
                        "#pragma GCC ivdep\n"
                        "#pragma vector nontemporal");
                
                snprintf(rec->implementation_guide, sizeof(rec->implementation_guide),
                        "1. Use non-temporal stores for data not reused\n"
                        "2. Keep frequently accessed data in cache\n"
                        "3. Process in chunks to maintain useful data\n"
                        "4. Consider cache partitioning if available");
                
                snprintf(rec->rationale, sizeof(rec->rationale),
                        "Non-temporal hints prevent streaming data from evicting "
                        "useful cached data, preserving performance.");
                
                // Check for duplicate before incrementing
                if (!isDuplicate(recs, count, recs[count].type, pattern)) {
                    // Set priority based on expected improvement
                    if (recs[count].expected_improvement > 50) {
                        recs[count].priority = 1;
                    } else if (recs[count].expected_improvement > 30) {
                        recs[count].priority = 2;
                    } else {
                        recs[count].priority = 3;
                    }
                    count++;
                }
            }
            break;
            
        case BANK_CONFLICTS:
            // Power-of-2 strides causing conflicts
            if (count < engine->config.max_recommendations) {
                optimization_rec_t *rec = &recs[count];
                rec->type = OPT_MEMORY_ALIGNMENT;
                rec->pattern = (classified_pattern_t*)pattern;
                rec->expected_improvement = 30.0;
                rec->confidence_score = 0.8;
                rec->implementation_difficulty = 4;
                rec->priority = 2;
                
                snprintf(rec->code_suggestion, sizeof(rec->code_suggestion),
                        "// Add padding to avoid bank conflicts\n"
                        "#define ORIGINAL_SIZE 1024\n"
                        "#define PAD 1  // Break power-of-2 stride\n"
                        "float matrix[ORIGINAL_SIZE][ORIGINAL_SIZE + PAD];\n\n"
                        "// Or use prime number dimensions\n"
                        "#define PRIME_SIZE 1021  // Prime number\n"
                        "float matrix[PRIME_SIZE][PRIME_SIZE];");
                
                snprintf(rec->rationale, sizeof(rec->rationale),
                        "Power-of-2 dimensions cause bank conflicts. Adding padding "
                        "or using prime dimensions eliminates conflicts.");
                
                // Check for duplicate before incrementing
                if (!isDuplicate(recs, count, recs[count].type, pattern)) {
                    // Set priority based on expected improvement
                    if (recs[count].expected_improvement > 50) {
                        recs[count].priority = 1;
                    } else if (recs[count].expected_improvement > 30) {
                        recs[count].priority = 2;
                    } else {
                        recs[count].priority = 3;
                    }
                    count++;
                }
            }
            break;
            
        case CACHE_LOOP_CARRIED_DEP:
            // Already handled above
            if (count == 0) {  // Fallback if not handled
                optimization_rec_t *rec = &recs[count];
                rec->type = OPT_LOOP_UNROLL;
                rec->pattern = (classified_pattern_t*)pattern;
                rec->expected_improvement = 20.0;
                rec->confidence_score = 0.7;
                rec->implementation_difficulty = 4;
                rec->priority = 2;
                
                snprintf(rec->code_suggestion, sizeof(rec->code_suggestion),
                        "// Software pipelining to hide latency\n"
                        "double a0 = data[0];\n"
                        "double a1 = data[1];\n"
                        "double a2 = data[2];\n"
                        "double a3 = data[3];\n"
                        "for (int i = 4; i < n; i += 4) {\n"
                        "    double t0 = func(a0);\n"
                        "    double t1 = func(a1);\n"
                        "    double t2 = func(a2);\n"
                        "    double t3 = func(a3);\n"
                        "    a0 = data[i+0];\n"
                        "    a1 = data[i+1];\n"
                        "    a2 = data[i+2];\n"
                        "    a3 = data[i+3];\n"
                        "    result[i-4] = t0;\n"
                        "    result[i-3] = t1;\n"
                        "    result[i-2] = t2;\n"
                        "    result[i-1] = t3;\n"
                        "}");
                
                snprintf(rec->rationale, sizeof(rec->rationale),
                        "Software pipelining overlaps memory access with computation, "
                        "hiding dependency latencies.");
                
                // Check for duplicate before incrementing
                if (!isDuplicate(recs, count, recs[count].type, pattern)) {
                    // Set priority based on expected improvement
                    if (recs[count].expected_improvement > 50) {
                        recs[count].priority = 1;
                    } else if (recs[count].expected_improvement > 30) {
                        recs[count].priority = 2;
                    } else {
                        recs[count].priority = 3;
                    }
                    count++;
                }
            }
            break;
            
        default:
            // Generic optimizations based on severity
            if (pattern->severity_score > 70 && count < engine->config.max_recommendations) {
                // High severity - suggest profiling
                optimization_rec_t *rec = &recs[count];
                rec->type = OPT_ACCESS_REORDER;
                rec->pattern = (classified_pattern_t*)pattern;
                rec->expected_improvement = 15.0;
                rec->confidence_score = 0.5;
                rec->implementation_difficulty = 8;
                rec->priority = 3;
                
                snprintf(rec->code_suggestion, sizeof(rec->code_suggestion),
                        "// Profile-guided optimization\n"
                        "1. Compile with -fprofile-generate\n"
                        "2. Run representative workload\n"
                        "3. Recompile with -fprofile-use\n\n"
                        "// Manual profiling\n"
                        "#ifdef PROFILE\n"
                        "uint64_t start = rdtsc();\n"
                        "// Hot code here\n"
                        "uint64_t cycles = rdtsc() - start;\n"
                        "profile_record(cycles);\n"
                        "#endif");
                
                snprintf(rec->rationale, sizeof(rec->rationale),
                        "High severity pattern requires detailed profiling "
                        "to identify the best optimization strategy.");
                
                // Check for duplicate before incrementing
                if (!isDuplicate(recs, count, recs[count].type, pattern)) {
                    // Set priority based on expected improvement
                    if (recs[count].expected_improvement > 50) {
                        recs[count].priority = 1;
                    } else if (recs[count].expected_improvement > 30) {
                        recs[count].priority = 2;
                    } else {
                        recs[count].priority = 3;
                    }
                    count++;
                }
            }
            break;
    }
    
    // Add NUMA optimizations if multiple NUMA nodes
    if (engine->cache_info.numa_nodes > 1 && count < engine->config.max_recommendations) {
        optimization_rec_t *rec = &recs[count];
        rec->type = OPT_NUMA_BINDING;
        rec->pattern = (classified_pattern_t*)pattern;
        rec->expected_improvement = 25.0;
        rec->confidence_score = 0.8;
        rec->implementation_difficulty = 5;
        rec->priority = 2;
        
        snprintf(rec->code_suggestion, sizeof(rec->code_suggestion),
                "// NUMA-aware memory allocation\n"
                "#include <numa.h>\n"
                "// Bind to NUMA node\n"
                "numa_set_preferred(0);\n"
                "// Allocate on specific node\n"
                "void* data = numa_alloc_onnode(size, 0);\n\n"
                "// Thread pinning\n"
                "#pragma omp parallel\n"
                "{\n"
                "    int tid = omp_get_thread_num();\n"
                "    int node = tid %% numa_num_nodes();\n"
                "    numa_run_on_node(node);\n"
                "}");
        
        snprintf(rec->rationale, sizeof(rec->rationale),
                "NUMA optimization ensures data is accessed from local memory, "
                "reducing cross-node traffic.");
        
        // Check for duplicate before incrementing
        if (!isDuplicate(recs, count, recs[count].type, pattern)) {
            // Set priority based on expected improvement
            if (recs[count].expected_improvement > 50) {
                recs[count].priority = 1;
            } else if (recs[count].expected_improvement > 30) {
                recs[count].priority = 2;
            } else {
                recs[count].priority = 3;
            }
            count++;
        }
    }
    
    // Add compiler flag suggestions
    if (engine->config.consider_compiler_flags) {
        for (int i = 0; i < count; i++) {
            // Specific flags per optimization type
            switch (recs[i].type) {
                case OPT_LOOP_TILING:
                    snprintf(recs[i].compiler_flags, sizeof(recs[i].compiler_flags),
                             "-floop-block -floop-strip-mine -floop-interchange");
                    break;
                case OPT_LOOP_VECTORIZE:
                    snprintf(recs[i].compiler_flags, sizeof(recs[i].compiler_flags),
                             "-O3 -march=native -ftree-vectorize -mavx2 -mfma -fopt-info-vec");
                    break;
                case OPT_PREFETCH_HINTS:
                    snprintf(recs[i].compiler_flags, sizeof(recs[i].compiler_flags),
                             "-fprefetch-loop-arrays -msse4.2");
                    break;
                case OPT_CACHE_BLOCKING:
                    snprintf(recs[i].compiler_flags, sizeof(recs[i].compiler_flags),
                             "-floop-block --param l1-cache-size=32 --param l2-cache-size=512");
                    break;
                case OPT_ACCESS_REORDER:
                    snprintf(recs[i].compiler_flags, sizeof(recs[i].compiler_flags),
                             "-floop-interchange -ftree-loop-distribution -ftree-loop-im");
                    break;
                default:
                    snprintf(recs[i].compiler_flags, sizeof(recs[i].compiler_flags),
                             "-O3 -march=native -mtune=native");
                    break;
            }
        }
    }
    
    // Filter by minimum expected improvement
    int filtered_count = 0;
    for (int i = 0; i < count; i++) {
        if (recs[i].expected_improvement >= engine->config.min_expected_improvement) {
            if (i != filtered_count) {
                recs[filtered_count] = recs[i];
            }
            filtered_count++;
        }
    }
    
    // Rank recommendations
    rank_recommendations(recs, filtered_count);
    
    *recommendations = recs;
    *rec_count = filtered_count;
    
    engine->total_recommendations_generated += filtered_count;
    
    pthread_mutex_unlock(&engine->mutex);
    
    LOG_INFO("Generated %d recommendations for pattern", filtered_count);
    return 0;
}



// Generate loop tiling recommendation
int generate_loop_tiling_recommendation(const classified_pattern_t *pattern,
                                       const cache_info_t *cache_info,
                                       optimization_rec_t *rec) {
    if (!pattern || !cache_info || !rec) return -1;
    
    rec->type = OPT_LOOP_TILING;
    rec->pattern = (classified_pattern_t*)pattern;
    
    // Calculate tile sizes based on cache
    int l1_tile = sqrt(cache_info->levels[0].size / (3 * sizeof(double)));
    int l2_tile = sqrt(cache_info->levels[1].size / (3 * sizeof(double)));
    
    // Round to nice values
    if (l1_tile > 32) l1_tile = 32;
    if (l2_tile > 128) l2_tile = 128;
    
    rec->expected_improvement = 40 + (pattern->severity_score / 2);
    rec->confidence_score = 0.85;
    rec->implementation_difficulty = 6;
    
    snprintf(rec->code_suggestion, sizeof(rec->code_suggestion),
             "// Original nested loops with poor cache behavior\n"
             "// for (int i = 0; i < N; i++)\n"
             "//   for (int j = 0; j < M; j++)\n"
             "//     C[i][j] = A[i][j] + B[i][j];\n\n"
             "// Tiled version for better cache reuse\n"
             "#define TILE_SIZE %d  // Fits in L1 cache\n\n"
             "for (int ii = 0; ii < N; ii += TILE_SIZE) {\n"
             "    for (int jj = 0; jj < M; jj += TILE_SIZE) {\n"
             "        // Process one tile\n"
             "        for (int i = ii; i < min(ii + TILE_SIZE, N); i++) {\n"
             "            for (int j = jj; j < min(jj + TILE_SIZE, M); j++) {\n"
             "                C[i][j] = A[i][j] + B[i][j];\n"
             "            }\n"
             "        }\n"
             "    }\n"
             "}", l1_tile);
    
    snprintf(rec->implementation_guide, sizeof(rec->implementation_guide),
             "1. Identify loop bounds and array dimensions\n"
             "2. Choose tile size to fit in L1 cache (%d elements)\n"
             "3. Add outer loops with tile-sized steps\n"
             "4. Ensure inner loops handle boundary conditions\n"
             "5. Test with different tile sizes for optimal performance",
             l1_tile);
    
    snprintf(rec->rationale, sizeof(rec->rationale),
             "Loop tiling improves temporal locality by processing data in "
             "cache-sized blocks. Working set of %zu KB exceeds L%d cache (%zu KB). "
             "Tiling reduces cache misses by ~%.0f%%.",
             (pattern->hotspot->address_range_end - 
              pattern->hotspot->address_range_start) / 1024,
             pattern->affected_cache_levels & 1 ? 1 : 2,
             cache_info->levels[0].size / 1024,
             rec->expected_improvement);
    
    rec->priority = 1;  // High priority
    rec->is_automatic = false;
    
    LOG_DEBUG("Generated loop tiling recommendation with tile size %d", l1_tile);
    return 0;
}

// Generate prefetch recommendation
int generate_prefetch_recommendation(const classified_pattern_t *pattern,
                                    optimization_rec_t *rec) {
    if (!pattern || !rec) return -1;
    
    rec->type = OPT_PREFETCH_HINTS;
    rec->pattern = (classified_pattern_t*)pattern;
    
    int prefetch_distance = 8;  // Default prefetch distance
    
    // Adjust based on pattern
    if (pattern->hotspot->dominant_pattern == STRIDED) {
        prefetch_distance = 16;  // Larger distance for strided
    } else if (pattern->hotspot->dominant_pattern == SEQUENTIAL) {
        prefetch_distance = 4;   // Smaller for sequential
    }
    
    rec->expected_improvement = 15 + (pattern->hotspot->miss_rate * 20);
    rec->confidence_score = 0.75;
    rec->implementation_difficulty = 3;
    
    snprintf(rec->code_suggestion, sizeof(rec->code_suggestion),
             "// Add software prefetch hints\n"
             "#include <xmmintrin.h>  // For _mm_prefetch\n\n"
             "for (int i = 0; i < n; i++) {\n"
             "    // Prefetch future data\n"
             "    if (i + %d < n) {\n"
             "        _mm_prefetch(&data[i + %d], _MM_HINT_T0);  // Prefetch to L1\n"
             "    }\n"
             "    \n"
             "    // Process current element\n"
             "    result[i] = process(data[i]);\n"
             "}\n\n"
             "// Alternative: Use compiler builtin\n"
             "for (int i = 0; i < n; i++) {\n"
             "    __builtin_prefetch(&data[i + %d], 0, 3);\n"
             "    result[i] = process(data[i]);\n"
             "}",
             prefetch_distance, prefetch_distance, prefetch_distance);
    
    snprintf(rec->implementation_guide, sizeof(rec->implementation_guide),
             "1. Identify the access pattern and stride\n"
             "2. Calculate prefetch distance (typically 4-16 iterations ahead)\n"
             "3. Insert prefetch intrinsics or builtins\n"
             "4. Use _MM_HINT_T0 for L1, _MM_HINT_T1 for L2\n"
             "5. Profile to find optimal prefetch distance");
    
    snprintf(rec->rationale, sizeof(rec->rationale),
             "Software prefetching can hide memory latency by bringing data "
             "into cache before it's needed. With %.1f%% miss rate and "
             "%s access pattern, prefetching can reduce stalls.",
             pattern->hotspot->miss_rate * 100,
             access_pattern_to_string(pattern->hotspot->dominant_pattern));
    
    rec->priority = 2;  // Medium priority
    rec->is_automatic = false;
    
    LOG_DEBUG("Generated prefetch recommendation with distance %d", prefetch_distance);
    return 0;
}

// Generate data layout recommendation
int generate_data_layout_recommendation(const classified_pattern_t *pattern,
                                       optimization_rec_t *rec) {
    if (!pattern || !rec) return -1;
    
    rec->type = OPT_DATA_LAYOUT_CHANGE;
    rec->pattern = (classified_pattern_t*)pattern;
    
    rec->expected_improvement = 50;  // SoA can be very effective
    rec->confidence_score = 0.80;
    rec->implementation_difficulty = 7;
    
    snprintf(rec->code_suggestion, sizeof(rec->code_suggestion),
             "// Original Array of Structures (AoS)\n"
             "struct Particle {\n"
             "    double x, y, z;\n"
             "    double vx, vy, vz;\n"
             "    double mass;\n"
             "};\n"
             "Particle particles[N];\n\n"
             "// Transformed to Structure of Arrays (SoA)\n"
             "struct ParticleArray {\n"
             "    double *x, *y, *z;\n"
             "    double *vx, *vy, *vz;\n"
             "    double *mass;\n"
             "    size_t count;\n"
             "};\n\n"
             "// Access pattern changes from:\n"
             "// for (i = 0; i < N; i++) \n"
             "//     particles[i].x += particles[i].vx * dt;\n"
             "// To:\n"
             "for (i = 0; i < N; i++)\n"
             "    particle_array.x[i] += particle_array.vx[i] * dt;");
    
    snprintf(rec->implementation_guide, sizeof(rec->implementation_guide),
             "1. Identify fields that are accessed together\n"
             "2. Group hot fields in separate arrays\n"
             "3. Allocate arrays with proper alignment\n"
             "4. Update all access patterns in code\n"
             "5. Consider SIMD opportunities with SoA layout");
    
    snprintf(rec->rationale, sizeof(rec->rationale),
             "Structure of Arrays (SoA) improves cache efficiency for "
             "scattered field access. Current layout wastes %.0f%% of "
             "cache line transfers. SoA enables vectorization.",
             (1.0 - pattern->hotspot->miss_rate) * 100);
    
    rec->priority = 1;  // High priority for gather/scatter patterns
    rec->is_automatic = false;
    
    LOG_DEBUG("Generated data layout transformation recommendation");
    return 0;
}

// Generate alignment recommendation
int generate_alignment_recommendation(const classified_pattern_t *pattern,
                                     optimization_rec_t *rec) {
    if (!pattern || !rec) return -1;
    
    rec->type = OPT_MEMORY_ALIGNMENT;
    rec->pattern = (classified_pattern_t*)pattern;
    
    rec->expected_improvement = 30;
    rec->confidence_score = 0.90;
    rec->implementation_difficulty = 4;
    
    snprintf(rec->code_suggestion, sizeof(rec->code_suggestion),
             "// Align data structures to cache line boundaries\n"
             "#define CACHE_LINE_SIZE 64\n\n"
             "// Method 1: Aligned allocation\n"
             "void* aligned_data;\n"
             "if (posix_memalign(&aligned_data, CACHE_LINE_SIZE, \n"
             "                   sizeof(DataType) * count) != 0) {\n"
             "    // Handle allocation failure\n"
             "}\n\n"
             "// Method 2: Compiler attributes\n"
             "struct alignas(CACHE_LINE_SIZE) AlignedData {\n"
             "    double values[8];  // One cache line\n"
             "};\n\n"
             "// Method 3: Padding to prevent false sharing\n"
             "struct PaddedData {\n"
             "    double value;\n"
             "    char padding[CACHE_LINE_SIZE - sizeof(double)];\n"
             "} __attribute__((packed));");
    
    snprintf(rec->implementation_guide, sizeof(rec->implementation_guide),
             "1. Identify shared data structures\n"
             "2. Add padding or alignment attributes\n"
             "3. Use posix_memalign for dynamic allocation\n"
             "4. Ensure each thread's data is in separate cache lines\n"
             "5. Verify alignment with address checks");
    
    snprintf(rec->rationale, sizeof(rec->rationale),
             "False sharing occurs when multiple threads access different data "
             "in the same cache line. Alignment and padding ensure each thread's "
             "data occupies separate cache lines, eliminating coherence traffic.");
    
    rec->priority = 1;  // High priority for false sharing
    rec->is_automatic = true;  // Can be automated with padding
    
    LOG_DEBUG("Generated alignment recommendation for false sharing");
    return 0;
}

// Comparison function for sorting recommendations
static int compare_recommendations(const void *a, const void *b) {
    const optimization_rec_t *r1 = (const optimization_rec_t *)a;
    const optimization_rec_t *r2 = (const optimization_rec_t *)b;
    
    // First sort by priority (descending)
    if (r1->priority > r2->priority) return -1;
    if (r1->priority < r2->priority) return 1;
    
    // Then by expected improvement (descending)
    if (r1->expected_improvement > r2->expected_improvement) return -1;
    if (r1->expected_improvement < r2->expected_improvement) return 1;
    
    return 0;
}

void rank_recommendations(optimization_rec_t *recommendations, int count) {
    if (!recommendations || count <= 1) return;
    
    // Sort by:
    // 1. Priority (lower is better)
    // 2. Expected improvement (higher is better)
    // 3. Confidence score (higher is better)
    // 4. Implementation difficulty (lower is better)
    
    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            bool swap = false;
            
            // First by priority
            if (recommendations[i].priority > recommendations[j].priority) {
                swap = true;
            } else if (recommendations[i].priority == recommendations[j].priority) {
                // Then by expected improvement
                if (recommendations[i].expected_improvement < recommendations[j].expected_improvement) {
                    swap = true;
                } else if (recommendations[i].expected_improvement == recommendations[j].expected_improvement) {
                    // Then by confidence
                    if (recommendations[i].confidence_score < recommendations[j].confidence_score) {
                        swap = true;
                    } else if (recommendations[i].confidence_score == recommendations[j].confidence_score) {
                        // Finally by difficulty
                        if (recommendations[i].implementation_difficulty > recommendations[j].implementation_difficulty) {
                            swap = true;
                        }
                    }
                }
            }
            
            if (swap) {
                optimization_rec_t temp = recommendations[i];
                recommendations[i] = recommendations[j];
                recommendations[j] = temp;
            }
        }
    }
}

//
int filter_conflicting_recommendations(optimization_rec_t *recommendations, int count) {
    if (!recommendations || count <= 1) return count;
    
    // Mark conflicting recommendations for removal
    bool *to_remove = calloc(count, sizeof(bool));
    
    for (int i = 0; i < count - 1; i++) {
        if (to_remove[i]) continue;
        
        for (int j = i + 1; j < count; j++) {
            if (to_remove[j]) continue;
            
            // Check if they target the same location
            if (recommendations[i].pattern && recommendations[j].pattern &&
                recommendations[i].pattern->hotspot && recommendations[j].pattern->hotspot) {
                
                cache_hotspot_t *h1 = recommendations[i].pattern->hotspot;
                cache_hotspot_t *h2 = recommendations[j].pattern->hotspot;
                
                if (h1->location.line == h2->location.line &&
                    strcmp(h1->location.file, h2->location.file) == 0) {
                    
                    // Same location - check for conflicts
                    if ((recommendations[i].type == OPT_LOOP_VECTORIZE && 
                         recommendations[j].type == OPT_DATA_LAYOUT_CHANGE) ||
                        (recommendations[j].type == OPT_LOOP_VECTORIZE && 
                         recommendations[i].type == OPT_DATA_LAYOUT_CHANGE)) {
                        
                        // Keep the one with higher expected improvement
                        if (recommendations[i].expected_improvement < 
                            recommendations[j].expected_improvement) {
                            to_remove[i] = true;
                        } else {
                            to_remove[j] = true;
                        }
                    }
                }
            }
        }
    }
    
    // Compact the array
    int new_count = 0;
    for (int i = 0; i < count; i++) {
        if (!to_remove[i]) {
            if (i != new_count) {
                recommendations[new_count] = recommendations[i];
            }
            new_count++;
        }
    }
    
    free(to_remove);
    return new_count;
}

// Print recommendations
void recommendation_engine_print_recommendations(const optimization_rec_t *recs, int count) {
    printf("\n=== Optimization Recommendations ===\n");
    printf("Found %d optimization opportunities:\n\n", count);
    
    for (int i = 0; i < count; i++) {
        const optimization_rec_t *rec = &recs[i];
        
        printf("[%d] %s (Priority: %d)\n", i + 1,
               optimization_type_to_string(rec->type), rec->priority);
        
        printf("    Expected improvement: %.1f%%\n", rec->expected_improvement);
        printf("    Confidence: %.0f%%\n", rec->confidence_score * 100);
        printf("    Difficulty: %d/10\n", rec->implementation_difficulty);
        
        if (rec->pattern && rec->pattern->hotspot) {
            printf("    Location: %s:%d\n",
                   rec->pattern->hotspot->location.file,
                   rec->pattern->hotspot->location.line);
        }
        
        printf("\n    Rationale: %s\n", rec->rationale);
        
        if (strlen(rec->compiler_flags) > 0) {
            printf("\n    Compiler flags: %s\n", rec->compiler_flags);
        }
        
        if (strlen(rec->implementation_guide) > 0) {
            printf("\n    Implementation guide:\n%s\n", rec->implementation_guide);
        }
        
        if (strlen(rec->code_suggestion) > 0) {
            printf("\n    Code example:\n%s\n", rec->code_suggestion);
        }
        
        printf("\n" "─" "─" "─" "─" "─" "─" "─" "─" "─" "─" "\n\n");
    }
}

// Get default configuration
engine_config_t engine_config_default(void) {
    engine_config_t config = {
        .generate_code_examples = true,
        .consider_compiler_flags = true,
        .prefer_automatic = false,
        .max_recommendations = 5,
        .min_expected_improvement = 10.0
    };
    
    return config;
}
