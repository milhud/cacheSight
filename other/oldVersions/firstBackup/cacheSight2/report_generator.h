#ifndef REPORT_GENERATOR_H
#define REPORT_GENERATOR_H

#include "common.h"
#include "hardware_detector.h"
#include "ast_analyzer.h"
#include "sample_collector.h"
#include "pattern_classifier.h"
#include "recommendation_engine.h"

// Report format types
typedef enum {
    REPORT_FORMAT_HTML,
    REPORT_FORMAT_JSON,
    REPORT_FORMAT_TEXT,
    REPORT_FORMAT_MARKDOWN
} report_format_t;

// Report configuration
typedef struct {
    report_format_t format;         // Output format
    bool include_source_snippets;   // Include code snippets
    bool include_graphs;           // Include visualizations
    bool include_raw_data;         // Include raw sample data
    bool verbose;                  // Verbose output
    int max_items_per_section;     // Limit items per section
    char css_file[256];           // Custom CSS for HTML
    char template_file[256];      // Custom template
} report_config_t;

// Report sections
typedef struct {
    char title[128];
    char content[65536];          // Large buffer for content
    int priority;                 // Display priority
    bool is_critical;            // Critical section
} report_section_t;

// Complete report
typedef struct {
    char title[256];
    char timestamp[64];
    char summary[2048];
    report_section_t *sections;
    int section_count;
    int section_capacity;
} report_t;

// API functions
report_t* report_create(const char *title);
void report_destroy(report_t *report);

// Add sections
int report_add_section(report_t *report, const char *title, 
                      const char *content, int priority);
int report_add_summary(report_t *report, const char *summary);

// Generate report from analysis results
int generate_report(const report_config_t *config,
                   const char *output_file,
                   const cache_info_t *cache_info,
                   const analysis_results_t *static_results,
                   const cache_hotspot_t *hotspots, int hotspot_count,
                   const classified_pattern_t *patterns, int pattern_count,
                   const optimization_rec_t *recommendations, int rec_count);

// Section generators
int generate_executive_summary(report_t *report,
                              const cache_info_t *cache_info,
                              int total_issues, int critical_issues,
                              double avg_miss_rate);

int generate_hardware_section(report_t *report,
                             const cache_info_t *cache_info);

int generate_static_analysis_section(report_t *report,
                                    const analysis_results_t *results);

int generate_hotspot_section(report_t *report,
                            const cache_hotspot_t *hotspots, int count,
                            bool include_snippets);

int generate_pattern_section(report_t *report,
                            const classified_pattern_t *patterns, int count);

int generate_recommendation_section(report_t *report,
                                   const optimization_rec_t *recs, int count);

// Format-specific generators
int generate_html_report(const report_t *report, const char *output_file,
                        const report_config_t *config);

int generate_json_report(const report_t *report, const char *output_file,
                        const report_config_t *config);

int generate_text_report(const report_t *report, const char *output_file,
                        const report_config_t *config);

int generate_markdown_report(const report_t *report, const char *output_file,
                            const report_config_t *config);

// Visualization helpers
int generate_cache_miss_chart(const cache_hotspot_t *hotspots, int count,
                             char *output_buffer, size_t buffer_size);

int generate_pattern_distribution_chart(const classified_pattern_t *patterns, 
                                       int count, char *output_buffer, 
                                       size_t buffer_size);

// Code snippet extraction
int extract_code_snippet(const char *filename, int start_line, int end_line,
                        char *output_buffer, size_t buffer_size);

// Configuration
report_config_t report_config_default(void);

#endif // REPORT_GENERATOR_H
