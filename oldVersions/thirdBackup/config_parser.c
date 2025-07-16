#include "config_parser.h"
#include <ctype.h>
#include <stdarg.h>
static char g_error_buffer[1024] = "";

// Set error message
static void set_error(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(g_error_buffer, sizeof(g_error_buffer), format, args);
    va_end(args);
}

// Trim whitespace
static char* trim_whitespace(char *str) {
    char *end;
    
    // Trim leading space
    while (isspace((unsigned char)*str)) str++;
    
    if (*str == 0) return str;
    
    // Trim trailing space
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    
    // Write new null terminator
    end[1] = '\0';
    
    return str;
}

// Parse boolean value
static bool parse_bool(const char *value) {
    if (strcasecmp(value, "true") == 0 || 
        strcasecmp(value, "yes") == 0 ||
        strcasecmp(value, "1") == 0) {
        return true;
    }
    return false;
}

// Set configuration defaults
void config_parser_set_defaults(tool_config_t *config) {
    if (!config) return;
    
    memset(config, 0, sizeof(tool_config_t));
    
    // Analysis settings
    strcpy(config->mode, "full");
    config->sampling_duration = 10.0;
    config->max_samples = 100000;
    config->hotspot_threshold = 1.0;
    config->analysis_depth = 3;
    
    // Static analysis
    strcpy(config->c_standard, "c11");
    
    // Pattern detection
    config->min_confidence = 0.6;
    config->detect_false_sharing = true;
    config->correlate_static_dynamic = true;
    
    // Optimization
    config->generate_recommendations = true;
    config->min_improvement = 10.0;
    config->prefer_automatic = false;
    config->max_recommendations = 5;
    
    // Output
    strcpy(config->output_format, "html");
    strcpy(config->output_file, "report.html");
    config->include_source_snippets = true;
    config->generate_makefile = false;
    
    // Logging
    strcpy(config->log_file, "cache_optimizer.log");
    strcpy(config->log_level, "info");
    config->verbose = false;
    config->quiet = false;
    
    LOG_DEBUG("Set default configuration values");
}

// Load configuration from file
int config_parser_load_file(const char *filename, tool_config_t *config) {
    if (!filename || !config) {
        set_error("Invalid parameters");
        return -1;
    }
    
    LOG_INFO("Loading configuration from %s", filename);
    
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        set_error("Failed to open config file: %s", strerror(errno));
        LOG_ERROR("Failed to open config file %s: %s", filename, strerror(errno));
        return -1;
    }
    
    char line[1024];
    char section[64] = "";
    int line_num = 0;
    
    while (fgets(line, sizeof(line), fp)) {
        line_num++;
        
        // Trim whitespace
        char *trimmed = trim_whitespace(line);
        
        // Skip empty lines and comments
        if (strlen(trimmed) == 0 || trimmed[0] == '#' || trimmed[0] == ';') {
            continue;
        }
        
        // Check for section header
        if (trimmed[0] == '[') {
            char *end = strchr(trimmed, ']');
            if (end) {
                *end = '\0';
                strncpy(section, trimmed + 1, sizeof(section) - 1);
                LOG_DEBUG("Parsing section [%s]", section);
                continue;
            }
        }
        
        // Parse key=value pairs
        char *equals = strchr(trimmed, '=');
        if (!equals) {
            LOG_WARNING("Invalid line %d: %s", line_num, trimmed);
            continue;
        }
        
        *equals = '\0';
        char *key = trim_whitespace(trimmed);
        char *value = trim_whitespace(equals + 1);
        
        // Remove quotes if present
        if (value[0] == '"' && value[strlen(value)-1] == '"') {
            value[strlen(value)-1] = '\0';
            value++;
        }
        
        // Parse based on section and key
        if (strcmp(section, "analysis") == 0) {
            if (strcmp(key, "mode") == 0) {
                strncpy(config->mode, value, sizeof(config->mode) - 1);
            } else if (strcmp(key, "sampling_duration") == 0) {
                config->sampling_duration = atof(value);
            } else if (strcmp(key, "max_samples") == 0) {
                config->max_samples = atoi(value);
            } else if (strcmp(key, "hotspot_threshold") == 0) {
                config->hotspot_threshold = atof(value);
            } else if (strcmp(key, "analysis_depth") == 0) {
                config->analysis_depth = atoi(value);
            }
        } else if (strcmp(section, "static") == 0) {
            if (strcmp(key, "c_standard") == 0) {
                strncpy(config->c_standard, value, sizeof(config->c_standard) - 1);
            } else if (strcmp(key, "include_path") == 0) {
                // Add include path
                config->num_include_paths++;
                config->include_paths = realloc(config->include_paths,
                    config->num_include_paths * sizeof(char*));
                config->include_paths[config->num_include_paths - 1] = strdup(value);
            } else if (strcmp(key, "define") == 0) {
                // Add define
                config->num_defines++;
                config->defines = realloc(config->defines,
                    config->num_defines * sizeof(char*));
                config->defines[config->num_defines - 1] = strdup(value);
            }
        } else if (strcmp(section, "dynamic") == 0) {
            if (strcmp(key, "use_papi") == 0) {
                config->use_papi = parse_bool(value);
            } else if (strcmp(key, "profile_all_cpus") == 0) {
                config->profile_all_cpus = parse_bool(value);
            } else if (strcmp(key, "perf_event") == 0 && config->num_perf_events < 8) {
                strncpy(config->perf_events[config->num_perf_events], value,
                        sizeof(config->perf_events[0]) - 1);
                config->num_perf_events++;
            }
        } else if (strcmp(section, "pattern") == 0) {
            if (strcmp(key, "min_confidence") == 0) {
                config->min_confidence = atof(value);
            } else if (strcmp(key, "detect_false_sharing") == 0) {
                config->detect_false_sharing = parse_bool(value);
            } else if (strcmp(key, "correlate_static_dynamic") == 0) {
                config->correlate_static_dynamic = parse_bool(value);
            }
        } else if (strcmp(section, "optimization") == 0) {
            if (strcmp(key, "generate_recommendations") == 0) {
                config->generate_recommendations = parse_bool(value);
            } else if (strcmp(key, "min_improvement") == 0) {
                config->min_improvement = atof(value);
            } else if (strcmp(key, "prefer_automatic") == 0) {
                config->prefer_automatic = parse_bool(value);
            } else if (strcmp(key, "max_recommendations") == 0) {
                config->max_recommendations = atoi(value);
            }
        } else if (strcmp(section, "output") == 0) {
            if (strcmp(key, "format") == 0) {
                strncpy(config->output_format, value, sizeof(config->output_format) - 1);
            } else if (strcmp(key, "output_file") == 0) {
                strncpy(config->output_file, value, sizeof(config->output_file) - 1);
            } else if (strcmp(key, "include_source_snippets") == 0) {
                config->include_source_snippets = parse_bool(value);
            } else if (strcmp(key, "generate_makefile") == 0) {
                config->generate_makefile = parse_bool(value);
            }
        } else if (strcmp(section, "logging") == 0) {
            if (strcmp(key, "log_file") == 0) {
                strncpy(config->log_file, value, sizeof(config->log_file) - 1);
            } else if (strcmp(key, "log_level") == 0) {
                strncpy(config->log_level, value, sizeof(config->log_level) - 1);
            } else if (strcmp(key, "verbose") == 0) {
                config->verbose = parse_bool(value);
            } else if (strcmp(key, "quiet") == 0) {
                config->quiet = parse_bool(value);
            }
        }
        
        LOG_DEBUG("Config: %s.%s = %s", section, key, value);
    }
    
    fclose(fp);
    
    LOG_INFO("Configuration loaded successfully");
    return 0;
}

// Save configuration to file
int config_parser_save_file(const char *filename, const tool_config_t *config) {
    if (!filename || !config) {
        set_error("Invalid parameters");
        return -1;
    }
    
    LOG_INFO("Saving configuration to %s", filename);
    
    FILE *fp = fopen(filename, "w");
    if (!fp) {
        set_error("Failed to create config file: %s", strerror(errno));
        LOG_ERROR("Failed to create config file %s: %s", filename, strerror(errno));
        return -1;
    }
    
    // Write header
    fprintf(fp, "# Cache Optimizer Tool Configuration\n");
    fprintf(fp, "# Generated by cache_optimizer\n\n");
    
    // Analysis section
    fprintf(fp, "[analysis]\n");
    fprintf(fp, "mode = %s\n", config->mode);
    fprintf(fp, "sampling_duration = %.1f\n", config->sampling_duration);
    fprintf(fp, "max_samples = %d\n", config->max_samples);
    fprintf(fp, "hotspot_threshold = %.2f\n", config->hotspot_threshold);
    fprintf(fp, "analysis_depth = %d\n\n", config->analysis_depth);
    
    // Static analysis section
    fprintf(fp, "[static]\n");
    fprintf(fp, "c_standard = %s\n", config->c_standard);
    for (int i = 0; i < config->num_include_paths; i++) {
        fprintf(fp, "include_path = %s\n", config->include_paths[i]);
    }
    for (int i = 0; i < config->num_defines; i++) {
        fprintf(fp, "define = %s\n", config->defines[i]);
    }
    fprintf(fp, "\n");
    
    // Dynamic profiling section
    fprintf(fp, "[dynamic]\n");
    fprintf(fp, "use_papi = %s\n", config->use_papi ? "true" : "false");
    fprintf(fp, "profile_all_cpus = %s\n", config->profile_all_cpus ? "true" : "false");
    for (int i = 0; i < config->num_perf_events; i++) {
        fprintf(fp, "perf_event = %s\n", config->perf_events[i]);
    }
    fprintf(fp, "\n");
    
    // Pattern detection section
    fprintf(fp, "[pattern]\n");
    fprintf(fp, "min_confidence = %.2f\n", config->min_confidence);
    fprintf(fp, "detect_false_sharing = %s\n", config->detect_false_sharing ? "true" : "false");
    fprintf(fp, "correlate_static_dynamic = %s\n\n", config->correlate_static_dynamic ? "true" : "false");
    
    // Optimization section
    fprintf(fp, "[optimization]\n");
    fprintf(fp, "generate_recommendations = %s\n", config->generate_recommendations ? "true" : "false");
    fprintf(fp, "min_improvement = %.1f\n", config->min_improvement);
    fprintf(fp, "prefer_automatic = %s\n", config->prefer_automatic ? "true" : "false");
    fprintf(fp, "max_recommendations = %d\n\n", config->max_recommendations);
    
    // Output section
    fprintf(fp, "[output]\n");
    fprintf(fp, "format = %s\n", config->output_format);
    fprintf(fp, "output_file = %s\n", config->output_file);
    fprintf(fp, "include_source_snippets = %s\n", config->include_source_snippets ? "true" : "false");
    fprintf(fp, "generate_makefile = %s\n\n", config->generate_makefile ? "true" : "false");
    
    // Logging section
    fprintf(fp, "[logging]\n");
    fprintf(fp, "log_file = %s\n", config->log_file);
    fprintf(fp, "log_level = %s\n", config->log_level);
    fprintf(fp, "verbose = %s\n", config->verbose ? "true" : "false");
    fprintf(fp, "quiet = %s\n", config->quiet ? "true" : "false");
    
    fclose(fp);
    
    LOG_INFO("Configuration saved successfully");
    return 0;
}

// Validate configuration
int config_parser_validate(const tool_config_t *config) {
    if (!config) {
        set_error("NULL configuration");
        return -1;
    }
    
    // Validate mode
    if (strcmp(config->mode, "static") != 0 &&
        strcmp(config->mode, "dynamic") != 0 &&
        strcmp(config->mode, "full") != 0) {
        set_error("Invalid mode: %s (must be static, dynamic, or full)", config->mode);
        return -1;
    }
    
    // Validate numeric ranges
    if (config->sampling_duration <= 0) {
        set_error("Invalid sampling duration: %.1f (must be positive)", config->sampling_duration);
        return -1;
    }
    
    if (config->max_samples <= 0) {
        set_error("Invalid max samples: %d (must be positive)", config->max_samples);
        return -1;
    }
    
    if (config->hotspot_threshold < 0 || config->hotspot_threshold > 100) {
        set_error("Invalid hotspot threshold: %.1f (must be 0-100)", config->hotspot_threshold);
        return -1;
    }
    
    if (config->analysis_depth < 1 || config->analysis_depth > 5) {
        set_error("Invalid analysis depth: %d (must be 1-5)", config->analysis_depth);
        return -1;
    }
    
    if (config->min_confidence < 0 || config->min_confidence > 1) {
        set_error("Invalid min confidence: %.2f (must be 0-1)", config->min_confidence);
        return -1;
    }
    
    // Validate output format
    if (strcmp(config->output_format, "html") != 0 &&
        strcmp(config->output_format, "json") != 0 &&
        strcmp(config->output_format, "text") != 0) {
        set_error("Invalid output format: %s (must be html, json, or text)", config->output_format);
        return -1;
    }
    
    // Validate log level
    if (strcmp(config->log_level, "debug") != 0 &&
        strcmp(config->log_level, "info") != 0 &&
        strcmp(config->log_level, "warning") != 0 &&
        strcmp(config->log_level, "error") != 0) {
        set_error("Invalid log level: %s (must be debug, info, warning, or error)", config->log_level);
        return -1;
    }
    
    LOG_DEBUG("Configuration validated successfully");
    return 0;
}

// Print configuration
void config_parser_print(const tool_config_t *config) {
    if (!config) return;
    
    printf("\n=== Cache Optimizer Configuration ===\n");
    
    printf("\nAnalysis Settings:\n");
    printf("  Mode: %s\n", config->mode);
    printf("  Sampling duration: %.1f seconds\n", config->sampling_duration);
    printf("  Max samples: %d\n", config->max_samples);
    printf("  Hotspot threshold: %.1f%%\n", config->hotspot_threshold);
    printf("  Analysis depth: %d\n", config->analysis_depth);
    
    printf("\nStatic Analysis:\n");
    printf("  C standard: %s\n", config->c_standard);
    printf("  Include paths: %d\n", config->num_include_paths);
    for (int i = 0; i < config->num_include_paths; i++) {
        printf("    %s\n", config->include_paths[i]);
    }
    printf("  Defines: %d\n", config->num_defines);
    for (int i = 0; i < config->num_defines; i++) {
        printf("    %s\n", config->defines[i]);
    }
    
    printf("\nDynamic Profiling:\n");
    printf("  Use PAPI: %s\n", config->use_papi ? "yes" : "no");
    printf("  Profile all CPUs: %s\n", config->profile_all_cpus ? "yes" : "no");
    printf("  Custom events: %d\n", config->num_perf_events);
    
    printf("\nPattern Detection:\n");
    printf("  Min confidence: %.2f\n", config->min_confidence);
    printf("  Detect false sharing: %s\n", config->detect_false_sharing ? "yes" : "no");
    printf("  Correlate analyses: %s\n", config->correlate_static_dynamic ? "yes" : "no");
    
    printf("\nOptimization:\n");
    printf("  Generate recommendations: %s\n", config->generate_recommendations ? "yes" : "no");
    printf("  Min improvement: %.1f%%\n", config->min_improvement);
    printf("  Prefer automatic: %s\n", config->prefer_automatic ? "yes" : "no");
    printf("  Max recommendations: %d\n", config->max_recommendations);
    
    printf("\nOutput:\n");
    printf("  Format: %s\n", config->output_format);
    printf("  Output file: %s\n", config->output_file);
    printf("  Include source: %s\n", config->include_source_snippets ? "yes" : "no");
    printf("  Generate makefile: %s\n", config->generate_makefile ? "yes" : "no");
    
    printf("\nLogging:\n");
    printf("  Log file: %s\n", config->log_file);
    printf("  Log level: %s\n", config->log_level);
    printf("  Verbose: %s\n", config->verbose ? "yes" : "no");
    printf("  Quiet: %s\n", config->quiet ? "yes" : "no");
}

// Free configuration resources
void config_parser_free(tool_config_t *config) {
    if (!config) return;
    
    // Free include paths
    for (int i = 0; i < config->num_include_paths; i++) {
        free(config->include_paths[i]);
    }
    free(config->include_paths);
    config->include_paths = NULL;
    config->num_include_paths = 0;
    
    // Free defines
    for (int i = 0; i < config->num_defines; i++) {
        free(config->defines[i]);
    }
    free(config->defines);
    config->defines = NULL;
    config->num_defines = 0;
}

// Get last error
const char* config_parser_get_error(void) {
    return g_error_buffer;
}
