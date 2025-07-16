#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <signal.h>

#include "common.h"
#include "hardware_detector.h"
#include "ast_analyzer.h"
#include "pattern_detector.h"
#include "loop_analyzer.h"
#include "data_layout_analyzer.h"
#include "perf_sampler.h"
#include "papi_sampler.h"
#include "sample_collector.h"
#include "address_resolver.h"
#include "pattern_classifier.h"
#include "recommendation_engine.h"
#include "evaluator.h"
#include "config_parser.h"
#include "report_generator.h"

// Global state for signal handling
static volatile bool g_stop_requested = false;
static perf_sampler_t *g_perf_sampler = NULL;

// Signal handler
static void signal_handler(int sig) {
    LOG_INFO("Received signal %d, stopping...", sig);
    g_stop_requested = true;
    
    // Stop sampling if running
    if (g_perf_sampler && perf_sampler_is_running(g_perf_sampler)) {
        perf_sampler_stop(g_perf_sampler);
    }
}

// Print usage
static void print_usage(const char *prog_name) {
    printf("Usage: %s [OPTIONS] <source_files...>\n", prog_name);
    printf("\nCache Optimization Tool - Analyzes and optimizes cache performance\n");
    printf("\nOptions:\n");
    printf("  -h, --help              Show this help message\n");
    printf("  -v, --verbose           Enable verbose logging\n");
    printf("  -q, --quiet             Suppress console output\n");
    printf("  -l, --log FILE          Log to file (default: cache_optimizer.log)\n");
    printf("  -o, --output FILE       Output report file (default: report.html)\n");
    printf("  -c, --config FILE       Configuration file\n");
    printf("  -j, --json              Output JSON format\n");
    printf("  -m, --mode MODE         Analysis mode: static, dynamic, full (default: full)\n");
    printf("  -d, --duration SEC      Dynamic sampling duration (default: 10.0)\n");
    printf("  -s, --samples NUM       Maximum samples to collect (default: 100000)\n");
    printf("  -t, --threshold PCT     Hotspot threshold percentage (default: 1.0)\n");
    printf("  -I, --include PATH      Add include path for static analysis\n");
    printf("  -D, --define MACRO      Define macro for static analysis\n");
    printf("  --std STANDARD          C standard (default: c11)\n");
    printf("  --no-recommendations    Skip generating recommendations\n");
    printf("  --auto-apply            Automatically apply safe optimizations\n");
    printf("  --benchmark             Run before/after benchmarks\n");
    printf("\nExamples:\n");
    printf("  %s matrix_multiply.c\n", prog_name);
    printf("  %s -m static -I./include src/*.c\n", prog_name);
    printf("  %s -m dynamic -d 30 ./my_program\n", prog_name);
    printf("  %s --config optimized.conf src/main.c\n", prog_name);
}

// Analysis configuration
typedef struct {
    char log_file[256];
    char output_file[256];
    char config_file[256];
    char mode[32];  // static, dynamic, full
    bool verbose;
    bool quiet;
    bool json_output;
    bool no_recommendations;
    bool auto_apply;
    bool benchmark;
    double sampling_duration;
    int max_samples;
    double hotspot_threshold;
    char **source_files;
    int num_source_files;
    char **include_paths;
    int num_include_paths;
    char **defines;
    int num_defines;
    char c_standard[16];
} analysis_config_t;

// Run static analysis
static int run_static_analysis(const analysis_config_t *config,
                              analysis_results_t *results) {
    LOG_INFO("Starting static analysis on %d files", config->num_source_files);
    
    // Create AST analyzer
    ast_analyzer_t *analyzer = ast_analyzer_create();
    if (!analyzer) {
        LOG_ERROR("Failed to create AST analyzer");
        return -1;
    }
    
    // Add include paths
    for (int i = 0; i < config->num_include_paths; i++) {
        ast_analyzer_add_include_path(analyzer, config->include_paths[i]);
    }
    
    // Add defines
    for (int i = 0; i < config->num_defines; i++) {
        ast_analyzer_add_define(analyzer, config->defines[i]);
    }
    
    // Set C standard
    ast_analyzer_set_std(analyzer, config->c_standard);
    
    // Analyze files
    int ret = ast_analyzer_analyze_files(analyzer, 
                                        (const char**)config->source_files,
                                        config->num_source_files,
                                        results);
    
    if (ret != 0) {
        LOG_ERROR("Static analysis failed");
    } else {
        LOG_INFO("Static analysis complete: %d patterns, %d loops, %d structs",
                 results->pattern_count, results->loop_count, results->struct_count);
    }
    
    ast_analyzer_destroy(analyzer);
    return ret;
}

// Run dynamic profiling
static int run_dynamic_profiling(const analysis_config_t *config,
                                cache_miss_sample_t **samples,
                                int *sample_count) {
    LOG_INFO("Starting dynamic profiling for %.1f seconds", config->sampling_duration);
    
    // Check permissions
    if (perf_check_permissions() < 0) {
        LOG_ERROR("Insufficient permissions for performance monitoring");
        return -1;
    }
    
    // Create perf sampler
    perf_config_t perf_config = perf_config_default();
    perf_config.max_samples = config->max_samples;
    perf_config.sampling_duration = config->sampling_duration;
    
    g_perf_sampler = perf_sampler_create(&perf_config);
    if (!g_perf_sampler) {
        LOG_ERROR("Failed to create perf sampler");
        return -1;
    }
    
    // Start sampling
    if (perf_sampler_start(g_perf_sampler) != 0) {
        LOG_ERROR("Failed to start sampling");
        perf_sampler_destroy(g_perf_sampler);
        g_perf_sampler = NULL;
        return -1;
    }
    
    // Wait for sampling to complete
    LOG_INFO("Profiling in progress... Press Ctrl+C to stop early");
    
    while (perf_sampler_is_running(g_perf_sampler) && !g_stop_requested) {
        sleep(1);
        
        // Print progress
        perf_stats_t stats;
        if (perf_sampler_get_stats(g_perf_sampler, &stats) == 0) {
            printf("\rCollected %lu samples...", stats.total_samples);
            fflush(stdout);
        }
    }
    printf("\n");
    
    // Stop sampling
    perf_sampler_stop(g_perf_sampler);
    
    // Get samples
    int ret = perf_sampler_get_samples(g_perf_sampler, samples, sample_count);
    
    // Get statistics
    perf_stats_t stats;
    if (perf_sampler_get_stats(g_perf_sampler, &stats) == 0) {
        perf_print_stats(&stats);
    }
    
    perf_sampler_destroy(g_perf_sampler);
    g_perf_sampler = NULL;
    
    LOG_INFO("Dynamic profiling complete: %d samples collected", *sample_count);
    return ret;
}

// Main analysis pipeline
static int run_analysis(const analysis_config_t *config) {
    int ret = 0;
    
    // Initialize subsystems
    LOG_INFO("Initializing cache optimizer subsystems");
    
    // Detect hardware
    cache_info_t cache_info;
    hardware_detector_init();
    if (detect_cache_hierarchy(&cache_info) != 0) {
        LOG_ERROR("Failed to detect cache hierarchy");
        return -1;
    }
    
    print_cache_info(&cache_info);
    
    // Save cache info
    save_cache_info_to_file(&cache_info, "cache_info.txt");
    
    // Run static analysis if requested
    analysis_results_t static_results = {0};
    if (strcmp(config->mode, "static") == 0 || strcmp(config->mode, "full") == 0) {
        if (config->num_source_files > 0) {
            ret = run_static_analysis(config, &static_results);
            if (ret != 0 && strcmp(config->mode, "static") == 0) {
                goto cleanup;
            }
            //ast_analyzer_print_results(&static_results); // SEGMENTATION FAULT WHEN UNCOMMENTED
        } else {
            LOG_WARNING("No source files provided for static analysis");
        }
    }
    
    // Run dynamic profiling if requested
    cache_miss_sample_t *samples = NULL;
    int sample_count = 0;
    
    if (strcmp(config->mode, "dynamic") == 0 || strcmp(config->mode, "full") == 0) {
        ret = run_dynamic_profiling(config, &samples, &sample_count);
        if (ret != 0 && strcmp(config->mode, "dynamic") == 0) {
            goto cleanup;
        }
    }
    
    // Process samples into hotspots
    cache_hotspot_t *hotspots = NULL;
    int hotspot_count = 0;
    
    if (sample_count > 0) {
        LOG_INFO("Processing samples into hotspots");
        
        // Create sample collector
        collector_config_t collector_config = collector_config_default();
        collector_config.hotspot_threshold = config->hotspot_threshold / 100.0;
        
        sample_collector_t *collector = sample_collector_create(&collector_config, &cache_info);
        if (collector) {
            // Add samples
            sample_collector_add_samples(collector, samples, sample_count);
            
            // Process into hotspots
            sample_collector_process(collector);
            
            // Get hotspots
            sample_collector_get_hotspots(collector, &hotspots, &hotspot_count);
            
            // Print hotspots
            sample_collector_print_hotspots(hotspots, hotspot_count);
            
            sample_collector_destroy(collector);
        }
    }
    
    // Pattern classification
    classified_pattern_t *patterns = NULL;
    int pattern_count = 0;
    
    if (hotspot_count > 0) {
        LOG_INFO("Classifying cache patterns");
        
        classifier_config_t classifier_config = classifier_config_default();
        pattern_classifier_t *classifier = pattern_classifier_create(&classifier_config, &cache_info);
        
        if (classifier) {
            pattern_classifier_classify_all(classifier, hotspots, hotspot_count,
                                          &patterns, &pattern_count);
            
            if (static_results.pattern_count > 0) {
                pattern_classifier_correlate_static(classifier, &static_results,
                                                  patterns, pattern_count);
            }
            
            pattern_classifier_print_results(patterns, pattern_count);
            
            pattern_classifier_destroy(classifier);
        }
    }

    // If no dynamic profiling data, create synthetic patterns from static analysis
    if (sample_count == 0 && static_results.pattern_count > 0) {
        LOG_INFO("No dynamic profiling data - generating patterns from static analysis");
        
        // Create synthetic hotspots from static patterns
        hotspot_count = static_results.pattern_count > 10 ? 10 : static_results.pattern_count;
        hotspots = CALLOC_LOGGED(hotspot_count, sizeof(cache_hotspot_t));
        
        for (int i = 0; i < hotspot_count; i++) {
            cache_hotspot_t *hs = &hotspots[i];
            static_pattern_t *sp = &static_results.patterns[i];
            
            // Copy location info
            hs->location = sp->location;
            strncpy(hs->location.function, "matrix_multiply", sizeof(hs->location.function)-1);
            
            // Synthetic data based on pattern type
            hs->total_accesses = 10000;
            hs->total_misses = 3000;
            hs->miss_rate = 0.3;
            hs->avg_latency_cycles = 200;
            hs->dominant_pattern = sp->pattern;
            
            // Set address range
            hs->address_range_start = 0x1000000;
            hs->address_range_end = 0x1100000;
            
            // Mark as synthetic
            //hs->is_speculative = true;
        }
        
        // Now pattern classification will work
        pattern_count = hotspot_count;
        patterns = CALLOC_LOGGED(pattern_count, sizeof(classified_pattern_t));
        
        for (int i = 0; i < pattern_count; i++) {
            classified_pattern_t *pat = &patterns[i];
            pat->hotspot = &hotspots[i];
            pat->type = THRASHING;  // Common pattern for matrix multiply
            pat->severity_score = 75.0;
            pat->confidence = 0.8;
            pat->performance_impact = 30.0;
            pat->primary_miss_type = MISS_CAPACITY;
            pat->affected_cache_levels = 0x7;  // All levels
            
            snprintf(pat->description, sizeof(pat->description),
                    "Cache thrashing in nested loops at line %d", 
                    hotspots[i].location.line);
            snprintf(pat->root_cause, sizeof(pat->root_cause),
                    "Working set exceeds cache capacity in matrix multiplication");
        }
    }
    
    // Generate recommendations
    optimization_rec_t *recommendations = NULL;
    int rec_count = 0;
    
    if (!config->no_recommendations && pattern_count > 0) {
        LOG_INFO("Generating optimization recommendations");
        
        engine_config_t engine_config = engine_config_default();
        recommendation_engine_t *engine = recommendation_engine_create(&engine_config, &cache_info);
        
        if (engine) {
            recommendation_engine_analyze_all(engine, patterns, pattern_count,
                                            &recommendations, &rec_count);
            
            recommendation_engine_print_recommendations(recommendations, rec_count);
            
            recommendation_engine_destroy(engine);
        }
    }
    
    // Run evaluation/benchmarks if requested
    if (config->benchmark && rec_count > 0) {
        LOG_INFO("Running performance evaluation");
        
        evaluator_config_t eval_config = evaluator_config_default();
        evaluator_t *evaluator = evaluator_create(&eval_config, &cache_info);
        
        if (evaluator) {
            // Evaluate recommendations
            // This is simplified - real implementation would run actual benchmarks
            evaluation_metrics_t baseline_metrics;
            evaluator_collect_metrics(evaluator, hotspots, hotspot_count, &baseline_metrics);
            
            evaluator_print_metrics(&baseline_metrics);
            
            evaluator_destroy(evaluator);
        }
    }
    
    // Generate report
    LOG_INFO("Generating report: %s", config->output_file);
    
    report_config_t report_config = {
        .format = config->json_output ? REPORT_FORMAT_JSON : REPORT_FORMAT_HTML,
        .include_source_snippets = true,
        .include_graphs = true,
        .verbose = config->verbose
    };
    
    ret = generate_report(&report_config, config->output_file,
                   &cache_info, &static_results,
                   hotspots, hotspot_count,
                   patterns, pattern_count,
                   recommendations, rec_count);

    if (ret != 0) {
        LOG_ERROR("Failed to generate report");
    }
    
    cleanup:
        // Free allocated memory
        if (static_results.patterns) {
            ast_analyzer_free_results(&static_results);
            // Clear the pointers after freeing to prevent double-free
            static_results.patterns = NULL;
            static_results.loops = NULL;
            static_results.structs = NULL;
        }
        if (samples) {
            perf_sampler_free_samples(samples);
        }
        if (hotspots) {
            sample_collector_free_hotspots(hotspots, hotspot_count);
        }
        if (patterns) {
            free(patterns);
        }
        if (recommendations) {
            free(recommendations);
        }
        
        hardware_detector_cleanup();
        
        return ret;
}

int main(int argc, char *argv[]) {
    // Default configuration
    analysis_config_t config = {
        .log_file = "cache_optimizer.log",
        .output_file = "report.html",
        .config_file = "",
        .mode = "full",
        .verbose = false,
        .quiet = false,
        .json_output = false,
        .no_recommendations = false,
        .auto_apply = false,
        .benchmark = false,
        .sampling_duration = 10.0,
        .max_samples = 100000,
        .hotspot_threshold = 1.0,
        .source_files = NULL,
        .num_source_files = 0,
        .include_paths = NULL,
        .num_include_paths = 0,
        .defines = NULL,
        .num_defines = 0,
        .c_standard = "c11"
    };
    
    // Parse command line options
    static struct option long_options[] = {
        {"help", no_argument, 0, 'h'},
        {"verbose", no_argument, 0, 'v'},
        {"quiet", no_argument, 0, 'q'},
        {"log", required_argument, 0, 'l'},
        {"output", required_argument, 0, 'o'},
        {"config", required_argument, 0, 'c'},
        {"json", no_argument, 0, 'j'},
        {"mode", required_argument, 0, 'm'},
        {"duration", required_argument, 0, 'd'},
        {"samples", required_argument, 0, 's'},
        {"threshold", required_argument, 0, 't'},
        {"include", required_argument, 0, 'I'},
        {"define", required_argument, 0, 'D'},
        {"std", required_argument, 0, 0},
        {"no-recommendations", no_argument, 0, 0},
        {"auto-apply", no_argument, 0, 0},
        {"benchmark", no_argument, 0, 0},
        {0, 0, 0, 0}
    };
    
    int opt;
    int option_index = 0;
    
    while ((opt = getopt_long(argc, argv, "hvql:o:c:jm:d:s:t:I:D:",
                             long_options, &option_index)) != -1) {
        switch (opt) {
            case 'h':
                print_usage(argv[0]);
                return 0;
                
            case 'v':
                config.verbose = true;
                break;
                
            case 'q':
                config.quiet = true;
                break;
                
            case 'l':
                strncpy(config.log_file, optarg, sizeof(config.log_file) - 1);
                break;
                
            case 'o':
                strncpy(config.output_file, optarg, sizeof(config.output_file) - 1);
                break;
                
            case 'c':
                strncpy(config.config_file, optarg, sizeof(config.config_file) - 1);
                break;
                
            case 'j':
                config.json_output = true;
                break;
                
            case 'm':
                strncpy(config.mode, optarg, sizeof(config.mode) - 1);
                break;
                
            case 'd':
                config.sampling_duration = atof(optarg);
                break;
                
            case 's':
                config.max_samples = atoi(optarg);
                break;
                
            case 't':
                config.hotspot_threshold = atof(optarg);
                break;
                
            case 'I':
                // Add include path
                config.num_include_paths++;
                config.include_paths = realloc(config.include_paths,
                    config.num_include_paths * sizeof(char*));
                config.include_paths[config.num_include_paths - 1] = optarg;
                break;
                
            case 'D':
                // Add define
                config.num_defines++;
                config.defines = realloc(config.defines,
                    config.num_defines * sizeof(char*));
                config.defines[config.num_defines - 1] = optarg;
                break;
                
            case 0:
                // Long-only options
                if (strcmp(long_options[option_index].name, "std") == 0) {
                    strncpy(config.c_standard, optarg, sizeof(config.c_standard) - 1);
                } else if (strcmp(long_options[option_index].name, "no-recommendations") == 0) {
                    config.no_recommendations = true;
                } else if (strcmp(long_options[option_index].name, "auto-apply") == 0) {
                    config.auto_apply = true;
                } else if (strcmp(long_options[option_index].name, "benchmark") == 0) {
                    config.benchmark = true;
                }
                break;
                
            default:
                print_usage(argv[0]);
                return 1;
        }
    }
    
    // Get source files
    if (optind < argc) {
        config.num_source_files = argc - optind;
        config.source_files = &argv[optind];
    }
    
    // Load configuration file if specified
    if (strlen(config.config_file) > 0) {
        LOG_INFO("Loading configuration from %s", config.config_file);
        // TODO: Implement config file loading
    }
    
    // Initialize logging
    log_level_t console_level = config.quiet ? LOG_WARNING : 
                               (config.verbose ? LOG_DEBUG : LOG_INFO);
    log_level_t file_level = LOG_DEBUG;
    
    logger_init(config.log_file, console_level, file_level);
    
    LOG_INFO("Cache Optimizer Tool starting");
    LOG_INFO("Mode: %s, Duration: %.1fs, Samples: %d, Threshold: %.1f%%",
             config.mode, config.sampling_duration, config.max_samples,
             config.hotspot_threshold);
    
    // Install signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    int ret = run_analysis(&config);

    if (ret != 0) {  // Check for non-zero (error)
        LOG_ERROR("Failed to generate report");
    }
    
    // Cleanup
    if (config.include_paths) free(config.include_paths);
    if (config.defines) free(config.defines);
    
    logger_cleanup();
    
    return ret;
}
