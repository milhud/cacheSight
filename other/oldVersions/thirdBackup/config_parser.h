#ifndef CONFIG_PARSER_H
#define CONFIG_PARSER_H

#include "common.h"

// Configuration sections
typedef struct {
    // Analysis settings
    char mode[32];                  // static, dynamic, full
    double sampling_duration;       // Dynamic profiling duration
    int max_samples;               // Maximum samples to collect
    double hotspot_threshold;      // Hotspot detection threshold (%)
    int analysis_depth;            // 1-5, deeper = more thorough
    
    // Static analysis
    char **include_paths;
    int num_include_paths;
    char **defines;
    int num_defines;
    char c_standard[16];
    
    // Dynamic profiling
    bool use_papi;                 // Use PAPI instead of perf
    bool profile_all_cpus;         // Profile all CPUs
    char perf_events[8][64];       // Custom perf events
    int num_perf_events;
    
    // Pattern detection
    double min_confidence;         // Minimum confidence for patterns
    bool detect_false_sharing;     // Enable false sharing detection
    bool correlate_static_dynamic; // Correlate analyses
    
    // Optimization
    bool generate_recommendations; // Generate optimization suggestions
    double min_improvement;        // Minimum improvement threshold (%)
    bool prefer_automatic;         // Prefer automatic transformations
    int max_recommendations;       // Max recommendations per pattern
    
    // Output
    char output_format[32];        // html, json, text
    char output_file[256];         // Output file path
    bool include_source_snippets;  // Include code in report
    bool generate_makefile;        // Generate optimized Makefile
    
    // Logging
    char log_file[256];
    char log_level[16];            // debug, info, warning, error
    bool verbose;
    bool quiet;
} tool_config_t;

// API functions
int config_parser_load_file(const char *filename, tool_config_t *config);
int config_parser_save_file(const char *filename, const tool_config_t *config);

// Parse individual sections
int config_parser_parse_string(const char *config_string, tool_config_t *config);
int config_parser_parse_args(int argc, char *argv[], tool_config_t *config);

// Validation
int config_parser_validate(const tool_config_t *config);
void config_parser_set_defaults(tool_config_t *config);

// Helper functions
void config_parser_print(const tool_config_t *config);
void config_parser_free(tool_config_t *config);
int config_parser_merge(tool_config_t *dest, const tool_config_t *src);

// Error handling
const char* config_parser_get_error(void);

#endif // CONFIG_PARSER_H
