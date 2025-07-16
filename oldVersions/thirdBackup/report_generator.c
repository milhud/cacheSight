#include "report_generator.h"
#include <time.h>
#ifndef min
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif
// Create report
report_t* report_create(const char *title) {
    report_t *report = CALLOC_LOGGED(1, sizeof(report_t));
    if (!report) {
        LOG_ERROR("Failed to allocate report");
        return NULL;
    }
    
    strncpy(report->title, title ? title : "Cache Optimization Report", 
            sizeof(report->title) - 1);
    
    // Set timestamp
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    strftime(report->timestamp, sizeof(report->timestamp),
             "%Y-%m-%d %H:%M:%S", tm);
    
    // Allocate initial sections
    report->section_capacity = 16;
    report->sections = CALLOC_LOGGED(report->section_capacity, sizeof(report_section_t));
    if (!report->sections) {
        FREE_LOGGED(report);
        return NULL;
    }
    
    LOG_INFO("Created report: %s", report->title);
    return report;
}

// Destroy report
void report_destroy(report_t *report) {
    if (!report) return;
    
    if (report->sections) {
        FREE_LOGGED(report->sections);
    }
    
    FREE_LOGGED(report);
}

// Generate static analysis section
int generate_static_analysis_section(report_t *report,
                                    const analysis_results_t *static_results) {
    if (!report || !static_results) return -1;
    
    char buffer[8192];
    snprintf(buffer, sizeof(buffer),
             "Found %d static patterns in source code analysis",
             static_results->pattern_count);
    
    return report_add_section(report, "Static Analysis", buffer, 80);
}

// Generate hotspot section - use include_source parameter
int generate_hotspot_section(report_t *report,
                            const cache_hotspot_t *hotspots,
                            int count,
                            bool include_source) {
    if (!report || !hotspots) return -1;
    
    char buffer[8192];
    char *p = buffer;
    int remaining = sizeof(buffer);
    
    int n = snprintf(p, remaining,
                    "Identified %d cache hotspots with high miss rates\n\n",
                    count);
    p += n; remaining -= n;
    
    for (int i = 0; i < count && i < 10 && remaining > 100; i++) {
        const cache_hotspot_t *hs = &hotspots[i];
        n = snprintf(p, remaining,
                    "%d. %s:%d - %.1f%% miss rate (%zu misses)\n",
                    i + 1,
                    hs->location.file,
                    hs->location.line,
                    hs->miss_rate * 100,
                    hs->total_misses);
        if (n > 0 && n < remaining) {
            p += n; remaining -= n;
        }
        
        // Include source snippet if requested
        if (include_source && hs->location.function[0] != '\0' && remaining > 50) {
            n = snprintf(p, remaining,
                        "   Function: %s\n",
                        hs->location.function);
            if (n > 0 && n < remaining) {
                p += n; remaining -= n;
            }
        }
    }
    
    return report_add_section(report, "Cache Hotspots", buffer, 95);
}


// Generate pattern section
int generate_pattern_section(report_t *report,
                            const classified_pattern_t *patterns,
                            int count) {
    if (!report || !patterns) return -1;
    
    char buffer[8192];
    char *p = buffer;
    int remaining = sizeof(buffer);
    
    int n = snprintf(p, remaining,
                    "Detected %d cache access patterns\n\n",
                    count);
    p += n; remaining -= n;
    
    for (int i = 0; i < count && i < 10 && remaining > 100; i++) {
        const classified_pattern_t *pat = &patterns[i];
        n = snprintf(p, remaining,
                    "%d. %s - Severity: %.1f, Impact: %.1f%%\n",
                    i + 1,
                    pat->description,
                    pat->severity_score,
                    pat->performance_impact);
        if (n > 0 && n < remaining) {
            p += n; remaining -= n;
        }
    }
    
    return report_add_section(report, "Access Patterns", buffer, 85);
}

// UPDATE THIS - SHOULD PROVIDE LOCATIONS
// UPDATE THIS - SHOULD PROVIDE LOCATIONS AND DETAILS
int generate_recommendation_section(report_t *report,
                                   const optimization_rec_t *recommendations,
                                   int count) {
    if (!report || !recommendations) return -1;
    
    char buffer[8192];
    char *p = buffer;
    int remaining = sizeof(buffer);
    
    int n = snprintf(p, remaining,
                    "Generated %d optimization recommendations\n\n",
                    count);
    p += n; remaining -= n;
    
    // Add each recommendation with details
    for (int i = 0; i < count && remaining > 200; i++) {
        const optimization_rec_t *rec = &recommendations[i];
        
        // Add location if available
        const char *file = "unknown";
        int line = 0;
        if (rec->pattern && rec->pattern->hotspot) {
            file = rec->pattern->hotspot->location.file;
            line = rec->pattern->hotspot->location.line;
        }
        
        n = snprintf(p, remaining,
                    "%d. %s (Priority: %d)\n"
                    "   Location: %s:%d\n"
                    "   Expected Improvement: %.1f%% (Confidence: %.0f%%)\n"
                    "   Difficulty: %d/10\n"
                    "   Rationale: %s\n",
                    i + 1,
                    optimization_type_to_string(rec->type),
                    rec->priority,
                    file, line,
                    rec->expected_improvement,
                    rec->confidence_score * 100,
                    rec->implementation_difficulty,
                    rec->rationale);
        
        if (n > 0 && n < remaining) {
            p += n; remaining -= n;
        }
        
        // Add compiler flags if present
        if (strlen(rec->compiler_flags) > 0 && remaining > 50) {
            n = snprintf(p, remaining,
                        "   Compiler flags: %s\n",
                        rec->compiler_flags);
            if (n > 0 && n < remaining) {
                p += n; remaining -= n;
            }
        }
        
        // Add a separator
        if (remaining > 10) {
            n = snprintf(p, remaining, "\n");
            if (n > 0 && n < remaining) {
                p += n; remaining -= n;
            }
        }
    }
    
    return report_add_section(report, "Recommendations", buffer, 100);
}

// Generate markdown report - fix the unused parameter warning
int generate_markdown_report(const report_t *report, const char *output_file,
                           const report_config_t *config) {
    FILE *fp = fopen(output_file, "w");
    if (!fp) return -1;
    
    // Use config parameter to avoid warning
    bool verbose = config ? config->verbose : false;
    
    fprintf(fp, "# %s\n\n", report->title);
    fprintf(fp, "*Generated: %s*\n\n", report->timestamp);
    
    if (strlen(report->summary) > 0) {
        fprintf(fp, "## Summary\n\n%s\n\n", report->summary);
    }
    
    for (int i = 0; i < report->section_count; i++) {
        fprintf(fp, "## %s\n\n", report->sections[i].title);
        if (report->sections[i].is_critical) {
            fprintf(fp, "**⚠️ CRITICAL**\n\n");
        }
        fprintf(fp, "%s\n\n", report->sections[i].content);
        
        if (verbose) {
            fprintf(fp, "*Priority: %d*\n\n", report->sections[i].priority);
        }
    }
    
    fclose(fp);
    return 0;
}

// Add section
int report_add_section(report_t *report, const char *title, 
                      const char *content, int priority) {
    if (!report || !title || !content) return -1;
    
    // Grow sections array if needed
    if (report->section_count >= report->section_capacity) {
        report->section_capacity *= 2;
        report_section_t *new_sections = realloc(report->sections,
            report->section_capacity * sizeof(report_section_t));
        if (!new_sections) {
            LOG_ERROR("Failed to grow sections array");
            return -1;
        }
        report->sections = new_sections;
    }
    
    report_section_t *section = &report->sections[report->section_count++];
    strncpy(section->title, title, sizeof(section->title) - 1);
    strncpy(section->content, content, sizeof(section->content) - 1);
    section->priority = priority;
    section->is_critical = (priority >= 90);
    
    LOG_DEBUG("Added report section: %s (priority: %d)", title, priority);
    return 0;
}

// Add summary
int report_add_summary(report_t *report, const char *summary) {
    if (!report || !summary) return -1;
    
    strncpy(report->summary, summary, sizeof(report->summary) - 1);
    return 0;
}

// Generate complete report
int generate_report(const report_config_t *config,
                   const char *output_file,
                   const cache_info_t *cache_info,
                   const analysis_results_t *static_results,
                   const cache_hotspot_t *hotspots, int hotspot_count,
                   const classified_pattern_t *patterns, int pattern_count,
                   const optimization_rec_t *recommendations, int rec_count) {
    
    if (!config || !output_file) {
        LOG_ERROR("Invalid parameters for generate_report");
        return -1;
    }
    
    LOG_INFO("Generating %s report to %s",
             config->format == REPORT_FORMAT_HTML ? "HTML" :
             config->format == REPORT_FORMAT_JSON ? "JSON" :
             config->format == REPORT_FORMAT_TEXT ? "text" : "markdown",
             output_file);
    
    // Create report
    report_t *report = report_create("Cache Optimization Analysis Report");
    if (!report) {
        return -1;
    }
    
    // Calculate summary statistics
    int total_issues = pattern_count;
    int critical_issues = 0;
    double total_miss_rate = 0;
    
    for (int i = 0; i < pattern_count; i++) {
        if (patterns[i].severity_score > 80) {
            critical_issues++;
        }
    }
    
    for (int i = 0; i < hotspot_count; i++) {
        total_miss_rate += hotspots[i].miss_rate;
    }
    double avg_miss_rate = hotspot_count > 0 ? total_miss_rate / hotspot_count : 0;
    
    // Generate sections
    generate_executive_summary(report, cache_info, total_issues, 
                              critical_issues, avg_miss_rate);
    
    if (cache_info) {
        generate_hardware_section(report, cache_info);
    }
    
    if (static_results && static_results->pattern_count > 0) {
        generate_static_analysis_section(report, static_results);
    }
    
    if (hotspots && hotspot_count > 0) {
        generate_hotspot_section(report, hotspots, 
                                min(hotspot_count, config->max_items_per_section),
                                config->include_source_snippets);
    }
    
    if (patterns && pattern_count > 0) {
        generate_pattern_section(report, patterns,
                                min(pattern_count, config->max_items_per_section));
    }
    
    if (recommendations && rec_count > 0) {
        generate_recommendation_section(report, recommendations,
                                       min(rec_count, config->max_items_per_section));
    }
    
    // Sort sections by priority
    for (int i = 0; i < report->section_count - 1; i++) {
        for (int j = i + 1; j < report->section_count; j++) {
            if (report->sections[i].priority < report->sections[j].priority) {
                report_section_t temp = report->sections[i];
                report->sections[i] = report->sections[j];
                report->sections[j] = temp;
            }
        }
    }
    
    // Generate output based on format
    int ret = -1;
    switch (config->format) {
        case REPORT_FORMAT_HTML:
            ret = generate_html_report(report, output_file, config);
            break;
        case REPORT_FORMAT_JSON:
            ret = generate_json_report(report, output_file, config);
            break;
        case REPORT_FORMAT_TEXT:
            ret = generate_text_report(report, output_file, config);
            break;
        case REPORT_FORMAT_MARKDOWN:
            ret = generate_markdown_report(report, output_file, config);
            break;
    }
    
    report_destroy(report);
    
    if (ret == 0) {
        LOG_INFO("Report generated successfully: %s", output_file);
    } else {
        LOG_ERROR("Failed to generate report");
    }
    
    return ret;
}

// Generate executive summary
int generate_executive_summary(report_t *report,
                              const cache_info_t *cache_info,
                              int total_issues, int critical_issues,
                              double avg_miss_rate) {
    char buffer[4096];
    
    snprintf(buffer, sizeof(buffer),
            "Cache optimization analysis completed on %s\n\n"
            "System Configuration:\n"
            "- Architecture: %s\n"
            "- CPU: %s\n"
            "- Cache Levels: %d (L1: %zu KB, L2: %zu KB, L3: %zu KB)\n"
            "- Total Memory: %.1f GB\n\n"
            "Analysis Summary:\n"
            "- Total Issues Found: %d\n"
            "- Critical Issues: %d\n"
            "- Average Cache Miss Rate: %.1f%%\n"
            "- Estimated Performance Impact: %.1f%%\n\n"
            "%s",
            report->timestamp,
            cache_info ? cache_info->arch : "Unknown",
            cache_info ? cache_info->cpu_model : "Unknown",
            cache_info ? cache_info->num_levels : 0,
            cache_info && cache_info->num_levels > 0 ? cache_info->levels[0].size / 1024 : 0,
            cache_info && cache_info->num_levels > 1 ? cache_info->levels[1].size / 1024 : 0,
            cache_info && cache_info->num_levels > 2 ? cache_info->levels[2].size / 1024 : 0,
            cache_info ? cache_info->total_memory / (1024.0 * 1024 * 1024) : 0,
            total_issues,
            critical_issues,
            avg_miss_rate * 100,
            avg_miss_rate * 150,  // Rough estimate
            critical_issues > 0 ? 
            "CRITICAL: Immediate optimization recommended to improve performance." :
            total_issues > 0 ?
            "Several optimization opportunities identified." :
            "No significant cache performance issues detected.");
    
    report_add_summary(report, buffer);
    report_add_section(report, "Executive Summary", buffer, 100);
    
    return 0;
}

// Generate hardware section
int generate_hardware_section(report_t *report, const cache_info_t *cache_info) {
    char buffer[4096];
    char *p = buffer;
    int remaining = sizeof(buffer);
    
    int n = snprintf(p, remaining,
                    "Hardware Configuration Details\n"
                    "==============================\n\n");
    p += n; remaining -= n;
    
    n = snprintf(p, remaining,
                "CPU Information:\n"
                "- Model: %s\n"
                "- Architecture: %s\n"
                "- Cores: %d physical, %d logical\n"
                "- Frequency: %.2f GHz\n"
                "- NUMA Nodes: %d\n\n",
                cache_info->cpu_model,
                cache_info->arch,
                cache_info->num_cores,
                cache_info->num_threads,
                cache_info->cpu_frequency_ghz,
                cache_info->numa_nodes);
    p += n; remaining -= n;
    
    n = snprintf(p, remaining, "Cache Hierarchy:\n");
    p += n; remaining -= n;
    
    for (int i = 0; i < cache_info->num_levels; i++) {
        const cache_level_t *level = &cache_info->levels[i];
        n = snprintf(p, remaining,
                    "- L%d %s Cache:\n"
                    "  - Size: %zu KB\n"
                    "  - Line Size: %zu bytes\n"
                    "  - Associativity: %d-way\n"
                    "  - Latency: ~%d cycles\n"
                    "  - Shared: %s\n",
                    level->level,
                    level->type,
                    level->size / 1024,
                    level->line_size,
                    level->associativity,
                    level->latency_cycles,
                    level->shared ? "Yes" : "No");
        p += n; remaining -= n;
    }
    
    n = snprintf(p, remaining,
                "\nMemory Configuration:\n"
                "- Total Memory: %.1f GB\n"
                "- Page Size: %d KB\n"
                "- Estimated Bandwidth: %zu GB/s\n",
                cache_info->total_memory / (1024.0 * 1024 * 1024),
                cache_info->page_size / 1024,
                cache_info->memory_bandwidth_gbps);
    
    report_add_section(report, "Hardware Configuration", buffer, 90);
    return 0;
}

// Generate HTML report
int generate_html_report(const report_t *report, const char *output_file,
                        const report_config_t *config) {
    FILE *fp = fopen(output_file, "w");
    if (!fp) {
        LOG_ERROR("Failed to open output file: %s", output_file);
        return -1;
    }
    
    // HTML header
    fprintf(fp, "<!DOCTYPE html>\n<html>\n<head>\n");
    fprintf(fp, "<meta charset=\"UTF-8\">\n");
    fprintf(fp, "<title>%s</title>\n", report->title);
    
    // Embedded CSS or link to external
    if (strlen(config->css_file) > 0) {
        fprintf(fp, "<link rel=\"stylesheet\" href=\"%s\">\n", config->css_file);
    } else {
        // Embed default CSS
        fprintf(fp, "<style>\n");
        fprintf(fp, "body { font-family: Arial, sans-serif; margin: 40px; "
                   "background-color: #f5f5f5; }\n");
        fprintf(fp, ".container { max-width: 1200px; margin: 0 auto; "
                   "background-color: white; padding: 20px; box-shadow: 0 0 10px rgba(0,0,0,0.1); }\n");
        fprintf(fp, "h1 { color: #333; border-bottom: 3px solid #007bff; padding-bottom: 10px; }\n");
        fprintf(fp, "h2 { color: #555; margin-top: 30px; }\n");
        fprintf(fp, ".summary { background-color: #e9ecef; padding: 15px; "
                   "border-radius: 5px; margin-bottom: 20px; }\n");
        fprintf(fp, ".critical { background-color: #f8d7da; color: #721c24; "
                   "padding: 10px; border-radius: 5px; margin: 10px 0; }\n");
        fprintf(fp, ".recommendation { background-color: #d4edda; color: #155724; "
                   "padding: 10px; border-radius: 5px; margin: 10px 0; }\n");
        fprintf(fp, "pre { background-color: #f8f9fa; padding: 10px; "
                   "border: 1px solid #dee2e6; border-radius: 5px; overflow-x: auto; }\n");
        fprintf(fp, "table { border-collapse: collapse; width: 100%%; margin: 15px 0; }\n");
        fprintf(fp, "th, td { border: 1px solid #ddd; padding: 8px; text-align: left; }\n");
        fprintf(fp, "th { background-color: #007bff; color: white; }\n");
        fprintf(fp, "tr:nth-child(even) { background-color: #f2f2f2; }\n");
        fprintf(fp, ".chart { margin: 20px 0; }\n");
        fprintf(fp, "</style>\n");
    }
    
    fprintf(fp, "</head>\n<body>\n<div class=\"container\">\n");
    
    // Title and timestamp
    fprintf(fp, "<h1>%s</h1>\n", report->title);
    fprintf(fp, "<p>Generated: %s</p>\n", report->timestamp);
    
    // Summary
    if (strlen(report->summary) > 0) {
        fprintf(fp, "<div class=\"summary\">\n");
        fprintf(fp, "<h2>Summary</h2>\n");
        
        // Convert newlines to <br> for HTML
        char *summary = strdup(report->summary);
        char *line = strtok(summary, "\n");
        while (line) {
            fprintf(fp, "%s<br>\n", line);
            line = strtok(NULL, "\n");
        }
        free(summary);
        
        fprintf(fp, "</div>\n");
    }
    
    // Sections
    for (int i = 0; i < report->section_count; i++) {
        report_section_t *section = &report->sections[i];
        
        fprintf(fp, "<div class=\"section%s\">\n",
                section->is_critical ? " critical" : "");
        fprintf(fp, "<h2>%s</h2>\n", section->title);
        
        // Convert content (simple text to HTML)
        char *content = strdup(section->content);
        char *line = strtok(content, "\n");
        bool in_code = false;
        
        while (line) {
            // Simple code detection
            if (strstr(line, "```") || strstr(line, "//") || strstr(line, "/*")) {
                if (!in_code) {
                    fprintf(fp, "<pre>");
                    in_code = true;
                }
                fprintf(fp, "%s\n", line);
            } else if (in_code && strlen(line) == 0) {
                fprintf(fp, "</pre>\n");
                in_code = false;
            } else if (in_code) {
                fprintf(fp, "%s\n", line);
            } else {
                fprintf(fp, "<p>%s</p>\n", line);
            }
            
            line = strtok(NULL, "\n");
        }
        
        if (in_code) {
            fprintf(fp, "</pre>\n");
        }
        
        free(content);
        fprintf(fp, "</div>\n");
    }
    
    // Footer
    fprintf(fp, "<hr>\n");
    fprintf(fp, "<p><small>Generated by Cache Optimizer Tool</small></p>\n");
    fprintf(fp, "</div>\n</body>\n</html>\n");
    
    fclose(fp);
    return 0;
}

// Generate JSON report - use config parameter
int generate_json_report(const report_t *report, const char *output_file,
                        const report_config_t *config) {
    FILE *fp = fopen(output_file, "w");
    if (!fp) {
        LOG_ERROR("Failed to open output file: %s", output_file);
        return -1;
    }
    
    fprintf(fp, "{\n");
    fprintf(fp, "  \"title\": \"%s\",\n", report->title);
    fprintf(fp, "  \"timestamp\": \"%s\",\n", report->timestamp);
    fprintf(fp, "  \"summary\": \"%s\",\n", report->summary);
    
    // Include metadata if verbose
    if (config && config->verbose) {
        fprintf(fp, "  \"format_version\": \"1.0\",\n");
        fprintf(fp, "  \"section_count\": %d,\n", report->section_count);
    }
    
    fprintf(fp, "  \"sections\": [\n");
    
    for (int i = 0; i < report->section_count; i++) {
        report_section_t *section = &report->sections[i];
        
        fprintf(fp, "    {\n");
        fprintf(fp, "      \"title\": \"%s\",\n", section->title);
        fprintf(fp, "      \"priority\": %d,\n", section->priority);
        fprintf(fp, "      \"is_critical\": %s,\n", 
                section->is_critical ? "true" : "false");
        
        // Escape content for JSON
        fprintf(fp, "      \"content\": \"");
        for (const char *p = section->content; *p; p++) {
            switch (*p) {
                case '"': fprintf(fp, "\\\""); break;
                case '\\': fprintf(fp, "\\\\"); break;
                case '\n': fprintf(fp, "\\n"); break;
                case '\r': fprintf(fp, "\\r"); break;
                case '\t': fprintf(fp, "\\t"); break;
                default: fputc(*p, fp);
            }
        }
        fprintf(fp, "\"\n");
        
        fprintf(fp, "    }%s\n", i < report->section_count - 1 ? "," : "");
    }
    
    fprintf(fp, "  ]\n");
    fprintf(fp, "}\n");
    
    fclose(fp);
    return 0;
}

// Generate text report - use config parameter
int generate_text_report(const report_t *report, const char *output_file,
                        const report_config_t *config) {
    FILE *fp = fopen(output_file, "w");
    if (!fp) {
        LOG_ERROR("Failed to open output file: %s", output_file);
        return -1;
    }
    
    // Header
    fprintf(fp, "%s\n", report->title);
    for (size_t i = 0; i < strlen(report->title); i++) {
        fprintf(fp, "=");
    }
    fprintf(fp, "\n\n");
    
    fprintf(fp, "Generated: %s\n\n", report->timestamp);
    
    // Summary
    if (strlen(report->summary) > 0) {
        fprintf(fp, "SUMMARY\n");
        fprintf(fp, "-------\n");
        fprintf(fp, "%s\n\n", report->summary);
    }
    
    // Sections
    for (int i = 0; i < report->section_count; i++) {
        report_section_t *section = &report->sections[i];
        
        fprintf(fp, "\n%s\n", section->title);
        for (size_t j = 0; j < strlen(section->title); j++) {
            fprintf(fp, "-");
        }
        fprintf(fp, "\n\n");
        
        if (section->is_critical) {
            fprintf(fp, "*** CRITICAL ***\n\n");
        }
        
        fprintf(fp, "%s\n", section->content);
        
        // Include raw data if configured
        if (config && config->include_raw_data) {
            fprintf(fp, "\n[Priority: %d]\n", section->priority);
        }
    }
    
    fclose(fp);
    return 0;
}

// Get default configuration
report_config_t report_config_default(void) {
    report_config_t config = {
        .format = REPORT_FORMAT_HTML,
        .include_source_snippets = true,
        .include_graphs = true,
        .include_raw_data = false,
        .verbose = false,
        .max_items_per_section = 20,
        .css_file = "",
        .template_file = ""
    };
    
    return config;
}
