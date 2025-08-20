#ifndef RECOMMENDATION_ENGINE_H
#define RECOMMENDATION_ENGINE_H

#include "common.h"
#include "pattern_classifier.h"
#include "hardware_detector.h"
#include "loop_analyzer.h"

// Optimization recommendation
typedef struct {
    optimization_type_t type;           // Type of optimization
    classified_pattern_t *pattern;      // Associated pattern
    char code_suggestion[2048];         // Suggested code transformation
    char implementation_guide[1024];    // How to implement
    double expected_improvement;        // Performance improvement (%)
    double confidence_score;            // Confidence in recommendation (0-1)
    int implementation_difficulty;      // 1-10 scale
    char rationale[1024];              // Why this optimization helps
    int priority;                      // Priority ranking
    bool is_automatic;                 // Can be automatically applied
    char compiler_flags[256];          // Suggested compiler flags
} optimization_rec_t;

// Recommendation engine state
typedef struct recommendation_engine recommendation_engine_t;

// Engine configuration
typedef struct {
    bool generate_code_examples;        // Include code examples
    bool consider_compiler_flags;       // Suggest compiler optimizations
    bool prefer_automatic;              // Prioritize automatic transformations
    int max_recommendations;            // Maximum recommendations per pattern
    double min_expected_improvement;    // Minimum improvement threshold (%)
} engine_config_t;

// API functions
recommendation_engine_t* recommendation_engine_create(const engine_config_t *config,
                                                    const cache_info_t *cache_info);
void recommendation_engine_destroy(recommendation_engine_t *engine);

// Generate recommendations
int recommendation_engine_analyze(recommendation_engine_t *engine,
                                 const classified_pattern_t *pattern,
                                 optimization_rec_t **recommendations,
                                 int *rec_count);

int recommendation_engine_analyze_all(recommendation_engine_t *engine,
                                     const classified_pattern_t *patterns,
                                     int pattern_count,
                                     optimization_rec_t **all_recommendations,
                                     int *total_count);

// Specific optimization generators
int generate_loop_tiling_recommendation(const classified_pattern_t *pattern,
                                       const cache_info_t *cache_info,
                                       optimization_rec_t *rec);

int generate_prefetch_recommendation(const classified_pattern_t *pattern,
                                    optimization_rec_t *rec);

int generate_data_layout_recommendation(const classified_pattern_t *pattern,
                                       optimization_rec_t *rec);

int generate_alignment_recommendation(const classified_pattern_t *pattern,
                                     optimization_rec_t *rec);

// Code transformation helpers
int generate_tiling_code(const loop_nest_t *nest, const tiling_params_t *params,
                        char *code, size_t code_size);

int generate_prefetch_code(const cache_hotspot_t *hotspot,
                          char *code, size_t code_size);

int generate_soa_transformation_code(const struct_info_t *struct_info,
                                    char *code, size_t code_size);

// Recommendation ranking and filtering
void rank_recommendations(optimization_rec_t *recommendations, int count);
int filter_conflicting_recommendations(optimization_rec_t *recommendations, int count);

// Reporting
void recommendation_engine_print_recommendations(const optimization_rec_t *recs, int count);
int recommendation_engine_export_makefile(const optimization_rec_t *recs, int count,
                                        const char *filename);

// Configuration
engine_config_t engine_config_default(void);

#endif // RECOMMENDATION_ENGINE_H
