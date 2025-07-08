#include "loop_analyzer.h"
#include <math.h>

static cache_info_t g_cache_info;
static bool g_initialized = false;

int loop_analyzer_init(const cache_info_t *cache_info) {
    if (g_initialized) {
        LOG_WARNING("Loop analyzer already initialized");
        return 0;
    }
    
    if (!cache_info) {
        LOG_ERROR("NULL cache info provided to loop analyzer");
        return -1;
    }
    
    g_cache_info = *cache_info;
    g_initialized = true;
    
    LOG_INFO("Loop analyzer initialized with %d cache levels", cache_info->num_levels);
    return 0;
}

void loop_analyzer_cleanup(void) {
    if (!g_initialized) {
        return;
    }
    
    LOG_INFO("Loop analyzer cleanup");
    g_initialized = false;
}

int analyze_loop_characteristics(const loop_info_t *loop, const cache_info_t *cache_info,
                                loop_characteristics_t *characteristics) {
    if (!loop || !characteristics) {
        LOG_ERROR("NULL parameters in analyze_loop_characteristics");
        return -1;
    }
    
    LOG_DEBUG("Analyzing loop at %s:%d", loop->location.file, loop->location.line);
    
    memset(characteristics, 0, sizeof(loop_characteristics_t));
    
    // Estimate working set size
    characteristics->working_set_size = estimate_working_set_size(loop);
    
    // Estimate reuse distance
    characteristics->reuse_distance = estimate_reuse_distance(loop);
    
    // Check if perfectly nested
    characteristics->is_perfectly_nested = !loop->has_function_calls && 
                                         loop->pattern_count > 0;
    
    // Check for aliasing (simplified check)
    characteristics->has_aliasing = false;
    for (int i = 0; i < loop->pattern_count; i++) {
        if (loop->patterns[i].is_pointer_access) {
            characteristics->has_aliasing = true;
            break;
        }
    }
    
    // Check parallelizability
    characteristics->is_parallelizable = true;
    characteristics->is_vectorizable = true;
    
    for (int i = 0; i < loop->pattern_count; i++) {
        if (loop->patterns[i].has_dependencies) {
            characteristics->is_parallelizable = false;
        }
        if (loop->patterns[i].pattern == ACCESS_LOOP_CARRIED_DEP ||
            loop->patterns[i].pattern == INDIRECT_ACCESS ||
            loop->patterns[i].pattern == RANDOM) {
            characteristics->is_vectorizable = false;
        }
    }
    
    // Estimate trip count
    characteristics->trip_count = loop->estimated_iterations;
    
    // Suggest unroll factor based on cache line size and access pattern
    if (cache_info && cache_info->num_levels > 0) {
        int cache_line_size = cache_info->levels[0].line_size;
        int element_size = 8;  // Assume 8-byte elements
        
        characteristics->unroll_factor = cache_line_size / element_size;
        if (characteristics->unroll_factor > 8) {
            characteristics->unroll_factor = 8;  // Cap at 8
        }
        if (characteristics->unroll_factor < 2) {
            characteristics->unroll_factor = 2;
        }
    } else {
        characteristics->unroll_factor = 4;  // Default
    }
    
    LOG_INFO("Loop characteristics: working_set=%zu, reuse_dist=%d, parallel=%s, vector=%s",
             characteristics->working_set_size,
             characteristics->reuse_distance,
             characteristics->is_parallelizable ? "yes" : "no",
             characteristics->is_vectorizable ? "yes" : "no");
    
    return 0;
}

int analyze_loop_nest(const loop_info_t *loops, int loop_count, loop_nest_t *nest) {
    if (!loops || loop_count <= 0 || !nest) {
        LOG_ERROR("Invalid parameters for analyze_loop_nest");
        return -1;
    }
    
    LOG_INFO("Analyzing loop nest with %d loops", loop_count);
    
    memset(nest, 0, sizeof(loop_nest_t));
    
    // Sort loops by nesting level
    loop_info_t **sorted_loops = CALLOC_LOGGED(loop_count, sizeof(loop_info_t*));
    if (!sorted_loops) {
        LOG_ERROR("Failed to allocate sorted loops array");
        return -1;
    }
    
    for (int i = 0; i < loop_count; i++) {
        sorted_loops[i] = (loop_info_t*)&loops[i];
    }
    
    // Simple bubble sort by nest level
    for (int i = 0; i < loop_count - 1; i++) {
        for (int j = 0; j < loop_count - i - 1; j++) {
            if (sorted_loops[j]->nest_level > sorted_loops[j+1]->nest_level) {
                loop_info_t *temp = sorted_loops[j];
                sorted_loops[j] = sorted_loops[j+1];
                sorted_loops[j+1] = temp;
            }
        }
    }
    
    nest->loops = sorted_loops;
    nest->depth = sorted_loops[loop_count-1]->nest_level;
    
    // Analyze each loop
    nest->characteristics = CALLOC_LOGGED(loop_count, sizeof(loop_characteristics_t));
    if (!nest->characteristics) {
        LOG_ERROR("Failed to allocate characteristics array");
        FREE_LOGGED(sorted_loops);
        return -1;
    }
    
    for (int i = 0; i < loop_count; i++) {
        analyze_loop_characteristics(sorted_loops[i], &g_cache_info, 
                                    &nest->characteristics[i]);
    }
    
    // Suggest optimizations
    nest->optimization_flags = suggest_loop_optimizations(nest, &g_cache_info);
    
    LOG_INFO("Loop nest analysis complete: depth=%d, optimizations=0x%x",
             nest->depth, nest->optimization_flags);
    
    return 0;
}

void free_loop_nest(loop_nest_t *nest) {
    if (!nest) return;
    
    LOG_DEBUG("Freeing loop nest structures");
    
    if (nest->loops) {
        FREE_LOGGED(nest->loops);
        nest->loops = NULL;
    }
    
    if (nest->characteristics) {
        FREE_LOGGED(nest->characteristics);
        nest->characteristics = NULL;
    }
    
    nest->depth = 0;
}

int suggest_loop_optimizations(const loop_nest_t *nest, const cache_info_t *cache_info) {
    if (!nest || !cache_info) {
        LOG_ERROR("NULL parameters in suggest_loop_optimizations");
        return LOOP_OPT_NONE;
    }
    
    int optimizations = LOOP_OPT_NONE;
    char notes[1024] = "";
    
    LOG_INFO("Suggesting optimizations for loop nest of depth %d", nest->depth);
    
    // Check for tiling opportunities
    bool should_tile = false;
    for (int i = 0; i < nest->depth; i++) {
        size_t working_set = nest->characteristics[i].working_set_size;
        size_t l1_size = cache_info->levels[0].size;
        size_t l2_size = cache_info->num_levels > 1 ? cache_info->levels[1].size : 0;
        
        if (working_set > l1_size) {
            should_tile = true;
            strcat(notes, "Working set exceeds L1 cache. ");
            
            if (working_set > l2_size && l2_size > 0) {
                strcat(notes, "Working set exceeds L2 cache - aggressive tiling needed. ");
            }
        }
    }
    
    if (should_tile) {
        optimizations |= LOOP_OPT_TILE;
        LOG_DEBUG("Suggesting loop tiling");
    }
    
    // Check for vectorization
    bool can_vectorize = true;
    for (int i = 0; i < nest->depth; i++) {
        if (!nest->characteristics[i].is_vectorizable) {
            can_vectorize = false;
            break;
        }
    }
    
    if (can_vectorize) {
        optimizations |= LOOP_OPT_VECTORIZE;
        strcat(notes, "Loops are vectorizable. ");
        LOG_DEBUG("Suggesting vectorization");
    }
    
    // Check for parallelization
    bool can_parallelize = true;
    for (int i = 0; i < nest->depth; i++) {
        if (!nest->characteristics[i].is_parallelizable) {
            can_parallelize = false;
            break;
        }
    }
    
    if (can_parallelize && nest->characteristics[0].trip_count > 100) {
        optimizations |= LOOP_OPT_PARALLELIZE;
        strcat(notes, "Outer loop is parallelizable with sufficient work. ");
        LOG_DEBUG("Suggesting parallelization");
    }
    
    // Check for unrolling
    for (int i = 0; i < nest->depth; i++) {
        if (nest->characteristics[i].unroll_factor > 1 &&
            nest->characteristics[i].trip_count > 10) {
            optimizations |= LOOP_OPT_UNROLL;
            strcat(notes, "Inner loops can benefit from unrolling. ");
            LOG_DEBUG("Suggesting unrolling");
            break;
        }
    }
    
    // Check for prefetching
    bool needs_prefetch = false;
    for (int i = 0; i < nest->depth; i++) {
        if (nest->loops[i]->patterns) {
            for (int j = 0; j < nest->loops[i]->pattern_count; j++) {
                if (nest->loops[i]->patterns[j].pattern == STRIDED &&
                    nest->loops[i]->patterns[j].stride > 1) {
                    needs_prefetch = true;
                    break;
                }
            }
        }
    }
    
    if (needs_prefetch) {
        optimizations |= LOOP_OPT_PREFETCH;
        strcat(notes, "Strided access patterns can benefit from prefetching. ");
        LOG_DEBUG("Suggesting prefetching");
    }
    
    // Check for interchange opportunities
    if (nest->depth >= 2) {
        // Simple heuristic: interchange if inner loop has larger stride
        bool should_interchange = false;
        
        for (int i = 0; i < nest->loops[0]->pattern_count; i++) {
            for (int j = 0; j < nest->loops[1]->pattern_count; j++) {
                if (nest->loops[0]->patterns[i].stride > 
                    nest->loops[1]->patterns[j].stride) {
                    should_interchange = true;
                    break;
                }
            }
        }
        
        if (should_interchange) {
            optimizations |= LOOP_OPT_INTERCHANGE;
            strcat(notes, "Loop interchange can improve access patterns. ");
            LOG_DEBUG("Suggesting loop interchange");
        }
    }
    
    // Copy notes to nest structure
    strncpy((char*)nest->optimization_notes, notes, sizeof(nest->optimization_notes) - 1);
    //strncpy(optimization, buffer, sizeof(optimization) - 1);
    //optimization[sizeof(optimization) - 1] = '\0';  // Ensure null termination
    
    LOG_INFO("Suggested optimizations: 0x%x", optimizations);
    return optimizations;
}

int calculate_tiling_parameters(const loop_nest_t *nest, const cache_info_t *cache_info,
                               tiling_params_t *params) {
    if (!nest || !cache_info || !params) {
        LOG_ERROR("NULL parameters in calculate_tiling_parameters");
        return -1;
    }
    
    memset(params, 0, sizeof(tiling_params_t));
    
    LOG_INFO("Calculating tiling parameters for %d-deep loop nest", nest->depth);
    
    // Get cache sizes
    size_t l1_size = cache_info->levels[0].size;
    size_t l2_size = cache_info->num_levels > 1 ? cache_info->levels[1].size : l1_size;
    size_t l3_size = cache_info->num_levels > 2 ? cache_info->levels[2].size : l2_size;
    
    // Account for multiple arrays and safety margin
    size_t effective_l1 = l1_size * 0.8 / 3;  // Assume 3 arrays, 80% utilization
    size_t effective_l2 = l2_size * 0.8 / 3;
    size_t effective_l3 = l3_size * 0.8 / 3;
    
    // Calculate tile sizes based on cache hierarchy
    int element_size = 8;  // Assume double precision
    
    if (nest->depth >= 1) {
        // Innermost tile fits in L1
        params->tile_sizes[0] = sqrt(effective_l1 / element_size);
        params->num_dimensions = 1;
    }
    
    if (nest->depth >= 2) {
        // Second level tile fits in L2
        params->tile_sizes[1] = sqrt(effective_l2 / element_size);
        params->num_dimensions = 2;
    }
    
    if (nest->depth >= 3) {
        // Third level tile fits in L3
        params->tile_sizes[2] = sqrt(effective_l3 / element_size);
        params->num_dimensions = 3;
    }
    
    // Round to nice numbers
    for (int i = 0; i < params->num_dimensions; i++) {
        if (params->tile_sizes[i] > 256) {
            params->tile_sizes[i] = 256;
        } else if (params->tile_sizes[i] > 128) {
            params->tile_sizes[i] = 128;
        } else if (params->tile_sizes[i] > 64) {
            params->tile_sizes[i] = 64;
        } else if (params->tile_sizes[i] > 32) {
            params->tile_sizes[i] = 32;
        } else {
            params->tile_sizes[i] = 16;
        }
    }
    
    // Estimate speedup based on reuse improvement
    size_t original_misses = nest->characteristics[0].working_set_size / 
                            cache_info->levels[0].line_size;
    size_t tiled_misses = (params->tile_sizes[0] * element_size) / 
                         cache_info->levels[0].line_size;
    
    if (original_misses > 0) {
        params->estimated_speedup = (original_misses * 100) / (tiled_misses + 1);
        if (params->estimated_speedup > 500) {
            params->estimated_speedup = 500;  // Cap at 5x
        }
    } else {
        params->estimated_speedup = 100;  // No improvement
    }
    
    snprintf(params->rationale, sizeof(params->rationale),
             "Tiling with sizes %dx%dx%d to fit in L1/L2/L3 caches. "
             "Expected %zu%% speedup from improved cache reuse.",
             params->tile_sizes[0], params->tile_sizes[1], params->tile_sizes[2],
             params->estimated_speedup);
    
    LOG_INFO("Calculated tile sizes: %dx%dx%d, expected speedup: %zu%%",
             params->tile_sizes[0], params->tile_sizes[1], params->tile_sizes[2],
             params->estimated_speedup);
    
    return 0;
}

bool can_interchange_loops(const loop_info_t *outer, const loop_info_t *inner) {
    if (!outer || !inner) return false;
    
    LOG_DEBUG("Checking if loops can be interchanged");
    
    // Check if loops are adjacent in nesting
    if (inner->nest_level != outer->nest_level + 1) {
        LOG_DEBUG("Loops not adjacent in nesting");
        return false;
    }
    
    // Check for dependencies that prevent interchange
    // This is a simplified check - real implementation would need dependency analysis
    
    // If inner loop has function calls, generally can't interchange
    if (inner->has_function_calls) {
        LOG_DEBUG("Inner loop has function calls");
        return false;
    }
    
    // Check if loop bounds are independent
    // Simple check: if inner loop bound depends on outer loop variable
    if (strstr(inner->condition_expr, outer->loop_var) != NULL) {
        LOG_DEBUG("Inner loop bound depends on outer loop variable");
        return false;
    }
    
    LOG_INFO("Loops can be interchanged");
    return true;
}

bool can_fuse_loops(const loop_info_t *loop1, const loop_info_t *loop2) {
    if (!loop1 || !loop2) return false;
    
    LOG_DEBUG("Checking if loops can be fused");
    
    // Loops must be at same nesting level
    if (loop1->nest_level != loop2->nest_level) {
        LOG_DEBUG("Loops at different nesting levels");
        return false;
    }
    
    // Loops must have same trip count (or close)
    if (loop1->estimated_iterations != loop2->estimated_iterations) {
        // Allow some flexibility
        int diff = abs((int)loop1->estimated_iterations - (int)loop2->estimated_iterations);
        if (diff > 10 || diff > (int)(loop1->estimated_iterations / 10)) {
            LOG_DEBUG("Loop trip counts differ significantly");
            return false;
        }
    }
    
    // Check for data dependencies (simplified)
    // Would need proper dependency analysis in real implementation
    
    LOG_INFO("Loops can potentially be fused");
    return true;
}

size_t estimate_working_set_size(const loop_info_t *loop) {
    if (!loop) return 0;
    
    size_t total_size = 0;
    
    // Sum up footprints of all accessed arrays
    for (int i = 0; i < loop->pattern_count; i++) {
        total_size += loop->patterns[i].estimated_footprint;
    }
    
    // Scale by loop characteristics
    if (loop->has_nested_loops) {
        total_size *= 2;  // Nested loops typically access more data
    }
    
    LOG_DEBUG("Estimated working set size: %zu bytes", total_size);
    return total_size;
}

int estimate_reuse_distance(const loop_info_t *loop) {
    if (!loop || loop->pattern_count == 0) return -1;
    
    // Simple heuristic based on access patterns
    int total_distance = 0;
    int pattern_count = 0;
    
    for (int i = 0; i < loop->pattern_count; i++) {
        int distance;
        
        switch (loop->patterns[i].pattern) {
            case SEQUENTIAL:
                distance = 1;  // Immediate reuse
                break;
            case STRIDED:
                distance = loop->patterns[i].stride;
                break;
            case RANDOM:
            case INDIRECT_ACCESS:
                distance = 1000;  // Large reuse distance
                break;
            default:
                distance = 10;
        }
        
        total_distance += distance;
        pattern_count++;
    }
    
    int avg_distance = pattern_count > 0 ? total_distance / pattern_count : -1;
    
    LOG_DEBUG("Estimated average reuse distance: %d", avg_distance);
    return avg_distance;
}

void print_loop_analysis(const loop_nest_t *nest) {
    if (!nest) return;
    
    printf("\n=== Loop Nest Analysis ===\n");
    printf("Nest depth: %d\n", nest->depth);
    printf("Optimization opportunities: 0x%x\n", nest->optimization_flags);
    
    if (nest->optimization_flags & LOOP_OPT_TILE) printf("  - Loop tiling\n");
    if (nest->optimization_flags & LOOP_OPT_VECTORIZE) printf("  - Vectorization\n");
    if (nest->optimization_flags & LOOP_OPT_PARALLELIZE) printf("  - Parallelization\n");
    if (nest->optimization_flags & LOOP_OPT_UNROLL) printf("  - Loop unrolling\n");
    if (nest->optimization_flags & LOOP_OPT_PREFETCH) printf("  - Prefetching\n");
    if (nest->optimization_flags & LOOP_OPT_INTERCHANGE) printf("  - Loop interchange\n");
    if (nest->optimization_flags & LOOP_OPT_FUSE) printf("  - Loop fusion\n");
    if (nest->optimization_flags & LOOP_OPT_SPLIT) printf("  - Loop splitting\n");
    
    printf("\nOptimization notes:\n%s\n", nest->optimization_notes);
    
    printf("\nLoop details:\n");
    for (int i = 0; i < nest->depth; i++) {
        printf("Level %d: %s:%d\n", i, 
               nest->loops[i]->location.file,
               nest->loops[i]->location.line);
        printf("  Variable: %s\n", nest->loops[i]->loop_var);
        printf("  Iterations: %zu\n", nest->loops[i]->estimated_iterations);
        printf("  Working set: ");
        
        char size_str[32];
        format_bytes(nest->characteristics[i].working_set_size, size_str, sizeof(size_str));
        printf("%s\n", size_str);
        
        printf("  Parallelizable: %s\n", 
               nest->characteristics[i].is_parallelizable ? "Yes" : "No");
        printf("  Vectorizable: %s\n",
               nest->characteristics[i].is_vectorizable ? "Yes" : "No");
        printf("  Suggested unroll factor: %d\n",
               nest->characteristics[i].unroll_factor);
    }
}
