#ifndef LOOP_ANALYZER_H
#define LOOP_ANALYZER_H

#include "common.h"
#include "ast_analyzer.h"
#include "hardware_detector.h"

// Loop characteristics
typedef struct {
    size_t working_set_size;      // Estimated memory footprint
    int reuse_distance;           // Average reuse distance
    bool is_perfectly_nested;     // All statements in innermost loop
    bool has_aliasing;            // Potential pointer aliasing
    bool is_parallelizable;       // Can be parallelized
    bool is_vectorizable;         // Can be vectorized
    int trip_count;               // Estimated iterations
    int unroll_factor;            // Suggested unroll factor
} loop_characteristics_t;

// Loop optimization opportunities
typedef enum {
    LOOP_OPT_NONE = 0,
    LOOP_OPT_UNROLL = 1 << 0,
    LOOP_OPT_TILE = 1 << 1,
    LOOP_OPT_FUSE = 1 << 2,
    LOOP_OPT_SPLIT = 1 << 3,
    LOOP_OPT_INTERCHANGE = 1 << 4,
    LOOP_OPT_VECTORIZE = 1 << 5,
    LOOP_OPT_PARALLELIZE = 1 << 6,
    LOOP_OPT_PREFETCH = 1 << 7
} loop_optimization_t;

// Loop nest information
typedef struct {
    loop_info_t **loops;          // Array of loops in nest order
    int depth;                    // Nest depth
    loop_characteristics_t *characteristics;  // Per-loop characteristics
    int optimization_flags;       // Bitmask of loop_optimization_t
    char optimization_notes[1024];
} loop_nest_t;

// Tiling parameters
typedef struct {
    int tile_sizes[3];            // Tile sizes for up to 3 dimensions
    int num_dimensions;           // Number of tiled dimensions
    size_t estimated_speedup;     // Percentage speedup expected
    char rationale[512];
} tiling_params_t;

// API functions
int loop_analyzer_init(const cache_info_t *cache_info);
void loop_analyzer_cleanup(void);

int analyze_loop_nest(const loop_info_t *loops, int loop_count, loop_nest_t *nest);
int analyze_loop_characteristics(const loop_info_t *loop, const cache_info_t *cache_info,
                                loop_characteristics_t *characteristics);
void free_loop_nest(loop_nest_t *nest);

// Optimization analysis
int suggest_loop_optimizations(const loop_nest_t *nest, const cache_info_t *cache_info);
int calculate_tiling_parameters(const loop_nest_t *nest, const cache_info_t *cache_info,
                               tiling_params_t *params);
bool can_interchange_loops(const loop_info_t *outer, const loop_info_t *inner);
bool can_fuse_loops(const loop_info_t *loop1, const loop_info_t *loop2);

// Helper functions
size_t estimate_working_set_size(const loop_info_t *loop);
int estimate_reuse_distance(const loop_info_t *loop);
void print_loop_analysis(const loop_nest_t *nest);

#endif // LOOP_ANALYZER_H
