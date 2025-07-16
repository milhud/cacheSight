#ifndef AST_ANALYZER_H
#define AST_ANALYZER_H

#include "common.h"



#ifdef __cplusplus
extern "C" {
#endif

// Forward declarations for C++ types
typedef struct ast_analyzer ast_analyzer_t;
typedef struct ast_analysis_result ast_analysis_result_t;

// Static pattern information
typedef struct {
    source_location_t location;
    access_pattern_t pattern;
    int stride;
    int loop_depth;
    size_t estimated_footprint;
    bool has_dependencies;
    char variable_name[64];
    char array_name[64];
    char struct_name[64];
    bool is_pointer_access;
    bool is_struct_access;
    int access_count;
} static_pattern_t;

// Loop information
typedef struct {
    source_location_t location;
    char loop_var[32];
    char init_expr[128];
    char condition_expr[128];
    char increment_expr[128];
    int nest_level;
    bool has_function_calls;
    bool has_nested_loops;
    size_t estimated_iterations;
    static_pattern_t *patterns;
    int pattern_count;
} loop_info_t;

// Data structure information
typedef struct {
    char struct_name[128];
    char field_names[32][64];
    size_t field_offsets[32];
    size_t field_sizes[32];
    int field_count;
    size_t total_size;
    bool has_pointer_fields;
    bool is_packed;
    source_location_t location;
} struct_info_t;

// Analysis results
typedef struct {
    static_pattern_t *patterns;
    int pattern_count;
    loop_info_t *loops;
    int loop_count;
    struct_info_t *structs;
    int struct_count;
    char *diagnostics;
    int diagnostic_count;
} analysis_results_t;

// API functions
ast_analyzer_t* ast_analyzer_create(void);
void ast_analyzer_destroy(ast_analyzer_t *analyzer);

int ast_analyzer_add_include_path(ast_analyzer_t *analyzer, const char *path);
int ast_analyzer_add_define(ast_analyzer_t *analyzer, const char *define);
int ast_analyzer_set_std(ast_analyzer_t *analyzer, const char *std);

int ast_analyzer_analyze_file(ast_analyzer_t *analyzer, const char *filename,
                             analysis_results_t *results);
int ast_analyzer_analyze_files(ast_analyzer_t *analyzer, const char **filenames,
                              int file_count, analysis_results_t *results);

void ast_analyzer_free_results(analysis_results_t *results);
void ast_analyzer_print_results(const analysis_results_t *results);

// Helper functions
const char* get_pattern_description(static_pattern_t *pattern);
int estimate_cache_footprint(loop_info_t *loop);

#ifdef __cplusplus
}
#endif

#endif // AST_ANALYZER_H
