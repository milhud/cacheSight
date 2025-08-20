#ifndef DATA_LAYOUT_ANALYZER_H
#define DATA_LAYOUT_ANALYZER_H

#include "common.h"
#include "ast_analyzer.h"
#include "hardware_detector.h"

// Data layout types
typedef enum {
    LAYOUT_AOS,        // Array of Structures
    LAYOUT_SOA,        // Structure of Arrays
    LAYOUT_AOSOA,      // Hybrid - Array of Structure of Arrays
    LAYOUT_PACKED,     // Packed structure
    LAYOUT_ALIGNED,    // Cache-aligned structure
    LAYOUT_CUSTOM      // Custom layout
} data_layout_t;

// Field access statistics
typedef struct {
    char field_name[64];
    int access_count;
    double access_frequency;  // Percentage of total accesses
    bool is_hot;             // Frequently accessed field
    bool is_cold;            // Rarely accessed field
    size_t field_offset;
    size_t field_size;
} field_stats_t;

// Structure layout analysis
typedef struct {
    struct_info_t *struct_info;
    field_stats_t *field_stats;
    int field_count;
    data_layout_t current_layout;
    data_layout_t recommended_layout;
    double cache_efficiency;      // Current efficiency (0-100)
    double predicted_efficiency;  // After transformation
    size_t padding_bytes;         // Wasted bytes due to padding
    bool has_false_sharing;       // Potential false sharing detected
    char transformation_code[2048];
} struct_layout_analysis_t;

// Array access analysis
typedef struct {
    char array_name[128];
    size_t element_size;
    size_t total_size;
    access_pattern_t dominant_pattern;
    int stride;
    double spatial_locality_score;
    double temporal_locality_score;
    bool is_column_major_beneficial;  // For 2D arrays
    char optimization_suggestion[512];
} array_analysis_t;

// API functions
int data_layout_analyzer_init(const cache_info_t *cache_info);
void data_layout_analyzer_cleanup(void);

int analyze_struct_layout(const struct_info_t *struct_info,
                         const static_pattern_t *accesses, int access_count,
                         struct_layout_analysis_t *analysis);

int analyze_array_layout(const static_pattern_t *accesses, int access_count,
                        array_analysis_t *analysis);

int suggest_struct_transformation(const struct_layout_analysis_t *analysis,
                                 char *transformation_code, size_t code_size);

void free_layout_analysis(struct_layout_analysis_t *analysis);

// Helper functions
bool should_transform_aos_to_soa(const struct_layout_analysis_t *analysis);
int calculate_structure_padding(const struct_info_t *struct_info);
int detect_false_sharing_risk(const struct_info_t *struct_info, 
                             const static_pattern_t *accesses, int access_count);
void print_layout_analysis(const struct_layout_analysis_t *analysis);

// Code generation helpers
int generate_soa_definition(const struct_info_t *struct_info, char *code, size_t code_size);
int generate_aos_to_soa_conversion(const struct_info_t *struct_info, 
                                  const char *aos_var, const char *soa_var,
                                  int array_size, char *code, size_t code_size);

#endif // DATA_LAYOUT_ANALYZER_H
