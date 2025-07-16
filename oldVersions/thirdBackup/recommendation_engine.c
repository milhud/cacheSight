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

int recommendation_engine_analyze_all(recommendation_engine_t *engine,
                                     const classified_pattern_t *patterns,
                                     int pattern_count,
                                     optimization_rec_t **all_recommendations,
                                     int *total_rec_count) {
    if (!engine || !patterns || !all_recommendations || !total_rec_count) {
        LOG_ERROR("Invalid parameters for recommendation_engine_analyze_all");
        return -1;
    }
    
    *all_recommendations = CALLOC_LOGGED(pattern_count * 5, sizeof(optimization_rec_t));
    if (!*all_recommendations) {
        return -1;
    }
    
    *total_rec_count = 0;
    
    for (int i = 0; i < pattern_count; i++) {
        optimization_rec_t *recs = NULL;
        int count = 0;
        
        if (recommendation_engine_analyze(engine, &patterns[i], &recs, &count) == 0) {
            for (int j = 0; j < count; j++) {
                (*all_recommendations)[(*total_rec_count)++] = recs[j];
            }
            FREE_LOGGED(recs);
        }
    }
    
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
int recommendation_engine_analyze(recommendation_engine_t *engine,
                                 const classified_pattern_t *pattern,
                                 optimization_rec_t **recommendations,
                                 int *rec_count) {
    if (!engine || !pattern || !recommendations || !rec_count) {
        LOG_ERROR("Invalid parameters for recommendation_engine_analyze");
        return -1;
    }
    
    LOG_INFO("Analyzing pattern %s for optimizations",
             cache_antipattern_to_string(pattern->type));
    
    pthread_mutex_lock(&engine->mutex);
    
    // Allocate recommendations array
    optimization_rec_t *recs = CALLOC_LOGGED(engine->config.max_recommendations,
                                            sizeof(optimization_rec_t));
    if (!recs) {
        LOG_ERROR("Failed to allocate recommendations");
        pthread_mutex_unlock(&engine->mutex);
        return -1;
    }
    
    int count = 0;
    
    // Generate recommendations based on pattern type
    switch (pattern->type) {
        case THRASHING:
        case STREAMING_EVICTION:
            // Loop tiling is effective for these patterns
            if (count < engine->config.max_recommendations) {
                if (generate_loop_tiling_recommendation(pattern, &engine->cache_info,
                                                       &recs[count]) == 0) {
                    count++;
                }
            }
            // Also suggest prefetching
            if (count < engine->config.max_recommendations) {
                if (generate_prefetch_recommendation(pattern, &recs[count]) == 0) {
                    count++;
                }
            }
            break;
            
        case FALSE_SHARING:
            // Alignment and padding recommendations
            if (count < engine->config.max_recommendations) {
                if (generate_alignment_recommendation(pattern, &recs[count]) == 0) {
                    count++;
                }
            }
            break;
            
        case IRREGULAR_GATHER_SCATTER:
        case UNCOALESCED_ACCESS:
            // case ACCESS_LOOP_CARRIED_DEP:  // Same value as UNCOALESCED_ACCESS
            // Handle both cases since they have the same enum value
            if (*rec_count < engine->config.max_recommendations) {
                optimization_rec_t *rec = &recs[*rec_count];  // Add this line to get the pointer
                rec->type = OPT_LOOP_UNROLL;  // Use appropriate enum value
                rec->priority = 7;
                rec->expected_improvement = 30.0;  // Change from expected_speedup to expected_improvement
                rec->confidence_score = 0.7;  // Add this field
                rec->implementation_difficulty = 6;  // Add this field
                rec->pattern = (classified_pattern_t *)pattern;  // Add pattern reference
                
                snprintf(rec->code_suggestion, sizeof(rec->code_suggestion),  // Note: field name might be different
                        "Memory access pattern issue detected (uncoalesced access or loop dependency)");
                snprintf(rec->implementation_guide, sizeof(rec->implementation_guide),  // Changed from 'implementation'
                        "Consider: 1) Aligning data structures, 2) Using SOA instead of AOS, "
                        "3) Breaking loop dependencies with temporary arrays");
                snprintf(rec->rationale, sizeof(rec->rationale),
                        "Improving memory access patterns can significantly reduce cache misses");
                
                (*rec_count)++;  // Don't forget to increment the counter
            }
            break;
                   
           
           /* // case ACCESS_LOOP_CARRIED_DEP:  // Same value as UNCOALESCED_ACCESS
            // Handle both cases since they have the same enum value
            recs[*rec_count].type = LOOP_OPT_VECTORIZE;  // NEED TO UPDATE ENUM VALUE 
            rec->priority = 7;
            rec->expected_speedup = 1.3;
            snprintf(rec->description, sizeof(rec->description),
                    "Memory access pattern issue detected (uncoalesced access or loop dependency)");
            snprintf(rec->implementation, sizeof(rec->implementation),
                    "Consider: 1) Aligning data structures, 2) Using SOA instead of AOS, "
                    "3) Breaking loop dependencies with temporary arrays");
            
            / Data layout transformation
            if (count < engine->config.max_recommendations) {
                if (generate_data_layout_recommendation(pattern, &recs[count]) == 0) {
                    count++;
                }
            }
            break;
        
        case ACCESS_LOOP_CARRIED_DEP:
            // Loop transformations
            if (count < engine->config.max_recommendations) {
                optimization_rec_t *rec = &recs[count];
                rec->type = OPT_LOOP_UNROLL;
                rec->pattern = (classified_pattern_t*)pattern;
                rec->expected_improvement = 20;
                rec->confidence_score = 0.7;
                rec->implementation_difficulty = 3;
                
                snprintf(rec->code_suggestion, sizeof(rec->code_suggestion),
                         "// Unroll loop to expose more parallelism\n"
                         "#pragma unroll 4\n"
                         "for (int i = 0; i < n; i += 4) {\n"
                         "    // Process 4 elements per iteration\n"
                         "    result[i] = compute(data[i]);\n"
                         "    result[i+1] = compute(data[i+1]);\n"
                         "    result[i+2] = compute(data[i+2]);\n"
                         "    result[i+3] = compute(data[i+3]);\n"
                         "}");
                
                snprintf(rec->rationale, sizeof(rec->rationale),
                         "Loop unrolling can help break dependencies and expose "
                         "instruction-level parallelism");
                
                count++;
            }
            */

        case CACHE_LOOP_CARRIED_DEP:
            // Loop transformations for cache-specific dependencies
            if (count < engine->config.max_recommendations) {
                optimization_rec_t *rec = &recs[count];
                rec->type = OPT_LOOP_UNROLL;
                rec->pattern = (classified_pattern_t*)pattern;
                rec->expected_improvement = 25;
                rec->confidence_score = 0.8;
                rec->implementation_difficulty = 4;
                rec->priority = 2;
                
                snprintf(rec->code_suggestion, sizeof(rec->code_suggestion),
                        "// Break cache dependencies with loop transformations\n"
                        "#pragma unroll 4\n"
                        "for (int i = 0; i < n; i += 4) {\n"
                        "    // Process multiple elements to hide latency\n"
                        "    process(data[i]);\n"
                        "    process(data[i+1]);\n"
                        "    process(data[i+2]);\n"
                        "    process(data[i+3]);\n"
                        "}");
                
                snprintf(rec->implementation_guide, sizeof(rec->implementation_guide),
                        "1. Identify loop-carried dependencies\n"
                        "2. Unroll loops to expose parallelism\n"
                        "3. Use temporary variables to break dependencies\n"
                        "4. Consider software pipelining");
                
                snprintf(rec->rationale, sizeof(rec->rationale),
                        "Cache-related loop dependencies limit performance. "
                        "Unrolling can help hide memory latency.");
                
                count++;
            }
            break;
             
        default:
            // Generic optimizations
            if (pattern->hotspot->dominant_pattern == STRIDED &&
                count < engine->config.max_recommendations) {
                if (generate_prefetch_recommendation(pattern, &recs[count]) == 0) {
                    count++;
                }
            }
            break;
    }
    
    // Add compiler flag suggestions if enabled
    if (engine->config.consider_compiler_flags) {
        for (int i = 0; i < count; i++) {
            switch (recs[i].type) {
                case OPT_LOOP_TILING:
                    snprintf(recs[i].compiler_flags, sizeof(recs[i].compiler_flags),
                             "-floop-block -floop-interchange");
                    break;
                case OPT_PREFETCH_HINTS:
                    snprintf(recs[i].compiler_flags, sizeof(recs[i].compiler_flags),
                             "-fprefetch-loop-arrays");
                    break;
                case LOOP_OPT_VECTORIZE:
                    snprintf(recs[i].compiler_flags, sizeof(recs[i].compiler_flags),
                             "-ftree-vectorize -mavx2");
                    break;
                default:
                    snprintf(recs[i].compiler_flags, sizeof(recs[i].compiler_flags),
                             "-O3 -march=native");
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

// Rank recommendations by priority and expected improvement
void rank_recommendations(optimization_rec_t *recommendations, int count) {
    if (!recommendations || count <= 0) return;
    
    // Calculate priority scores
    for (int i = 0; i < count; i++) {
        optimization_rec_t *rec = &recommendations[i];
        
        // Priority score based on multiple factors
        double score = rec->expected_improvement * rec->confidence_score;
        
        // Boost score for automatic transformations
        if (rec->is_automatic) {
            score *= 1.2;
        }
        
        // Reduce score for high difficulty
        score *= (11.0 - rec->implementation_difficulty) / 10.0;
        
        // Set priority based on score
        if (score > 50) {
            rec->priority = 1;
        } else if (score > 30) {
            rec->priority = 2;
        } else {
            rec->priority = 3;
        }
    }
    
    qsort(recommendations, count, sizeof(optimization_rec_t),
      compare_recommendations);
      
    LOG_DEBUG("Ranked %d recommendations", count);
}

// Filter conflicting recommendations
int filter_conflicting_recommendations(optimization_rec_t *recommendations, int count) {
    if (!recommendations || count <= 0) return count;
    
    // Mark conflicting recommendations
    bool *keep = CALLOC_LOGGED(count, sizeof(bool));
    if (!keep) return count;
    
    for (int i = 0; i < count; i++) {
        keep[i] = true;
    }
    
    // Check for conflicts
    for (int i = 0; i < count - 1; i++) {
        if (!keep[i]) continue;
        
        for (int j = i + 1; j < count; j++) {
            if (!keep[j]) continue;
            
            // Check if recommendations conflict
            bool conflict = false;
            
            // Loop tiling conflicts with loop unrolling
            if ((recommendations[i].type == OPT_LOOP_TILING &&
                 recommendations[j].type == OPT_LOOP_UNROLL) ||
                (recommendations[j].type == OPT_LOOP_TILING &&
                 recommendations[i].type == OPT_LOOP_UNROLL)) {
                conflict = true;
            }
            
            // Multiple data layout changes conflict
            if (recommendations[i].type == OPT_DATA_LAYOUT_CHANGE &&
                recommendations[j].type == OPT_DATA_LAYOUT_CHANGE) {
                conflict = true;
            }
            
            if (conflict) {
                // Keep the one with higher priority/improvement
                if (recommendations[i].priority < recommendations[j].priority ||
                    (recommendations[i].priority == recommendations[j].priority &&
                     recommendations[i].expected_improvement > 
                     recommendations[j].expected_improvement)) {
                    keep[j] = false;
                    LOG_DEBUG("Filtered conflicting recommendation: %s",
                              optimization_type_to_string(recommendations[j].type));
                } else {
                    keep[i] = false;
                    LOG_DEBUG("Filtered conflicting recommendation: %s",
                              optimization_type_to_string(recommendations[i].type));
                    break;
                }
            }
        }
    }
    
    // Compact array
    int kept = 0;
    for (int i = 0; i < count; i++) {
        if (keep[i]) {
            if (i != kept) {
                recommendations[kept] = recommendations[i];
            }
            kept++;
        }
    }
    
    FREE_LOGGED(keep);
    
    LOG_INFO("Filtered %d conflicting recommendations, kept %d",
             count - kept, kept);
    
    return kept;
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
