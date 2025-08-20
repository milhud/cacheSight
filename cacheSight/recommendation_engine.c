#include "recommendation_engine.h"
#include <math.h>

// Internal engine structure
struct recommendation_engine {
    engine_config_t config;
    cache_info_t cache_info;
    
    // Statistics
    int total_recommendations_generated;
    double avg_expected_improvement;
    
    pthread_mutex_t mutex;
};

/// Current: isDuplicate is not working properly
// Fix: Improve deduplication logic
static bool isDuplicate(optimization_rec_t* recs, int count, 
                       optimization_type_t type, const classified_pattern_t* pattern) {
    if (!pattern || !pattern->hotspot) return false;
    
    for (int i = 0; i < count; i++) {
        // Check for same optimization type at same location
        if (recs[i].type == type && 
            recs[i].pattern && recs[i].pattern->hotspot) {
            
            // Exact location match
            if (recs[i].pattern->hotspot->location.line == pattern->hotspot->location.line &&
                strcmp(recs[i].pattern->hotspot->location.file, 
                       pattern->hotspot->location.file) == 0) {
                LOG_DEBUG("Found duplicate %s at %s:%d",
                         optimization_type_to_string(type),
                         pattern->hotspot->location.file,
                         pattern->hotspot->location.line);
                return true;
            }
        }
    }
    return false;
}
// Add this helper function at the top of the file after the isDuplicate function
static bool hasConflictingPattern(optimization_rec_t *recs, int count, 
                                 const classified_pattern_t *pattern) {
    if (!pattern || !pattern->hotspot) return false;
    
    for (int i = 0; i < count; i++) {
        if (recs[i].pattern && recs[i].pattern->hotspot &&
            recs[i].pattern->hotspot->location.line == pattern->hotspot->location.line &&
            strcmp(recs[i].pattern->hotspot->location.file, pattern->hotspot->location.file) == 0) {
            
            // Check for conflicting access patterns
            access_pattern_t existing = recs[i].pattern->hotspot->dominant_pattern;
            access_pattern_t new_pattern = pattern->hotspot->dominant_pattern;
            
            // SEQUENTIAL conflicts with GATHER_SCATTER
            if ((existing == SEQUENTIAL && new_pattern == GATHER_SCATTER) ||
                (existing == GATHER_SCATTER && new_pattern == SEQUENTIAL)) {
                return true;
            }
        }
    }
    return false;
}

// ============================================================================
// File: recommendation_engine.c - TARGETED DEDUPLICATION LOGIC
// ============================================================================

// 1. FUNCTION TO IDENTIFY LOGICAL SCOPE FOR RECOMMENDATIONS
typedef struct {
    char function_name[128];
    optimization_type_t opt_type;
    int start_line;
    int end_line;
} recommendation_scope_t;

bool is_same_logical_scope(const optimization_rec_t *rec1, const optimization_rec_t *rec2) {
    // Check for null pointers
    if (!rec1 || !rec2) {
        return false;
    }
    
    if (!rec1->pattern || !rec1->pattern->hotspot || 
        !rec2->pattern || !rec2->pattern->hotspot) {
        return false;
    }
    
    const char *func1 = rec1->pattern->hotspot->location.function;
    const char *func2 = rec2->pattern->hotspot->location.function;
    int line1 = rec1->pattern->hotspot->location.line;
    int line2 = rec2->pattern->hotspot->location.line;
    
    // Additional safety checks
    if (!func1 || !func2) {
        return false;
    }
    
    // Same function
    if (strcmp(func1, func2) != 0) {
        return false;
    }
    
    // Same optimization type
    if (rec1->type != rec2->type) {
        return false;
    }
    
    // For certain optimizations, treat the entire function as one scope
    switch (rec1->type) {
        case OPT_LOOP_VECTORIZE:
        case OPT_PREFETCH_HINTS:
        case OPT_CACHE_BLOCKING:
        case OPT_LOOP_TILING:
            // These should be recommended once per function, not per line
            return true;
            
        case OPT_ACCESS_REORDER:
        case OPT_DATA_LAYOUT_CHANGE:
            // These can be different for different loops within same function
            // Only consider same scope if lines are very close (same loop)
            return abs(line1 - line2) <= 5;
            
        case OPT_MEMORY_POOLING:
            // This is typically a function-level or file-level optimization
            return true;
            
        default:
            return false;
    }
}

// 2. FUNCTION TO DETERMINE IF A RECOMMENDATION MAKES SENSE
bool is_sensible_recommendation(const optimization_rec_t *rec) {
    if (!rec) {
        return false;
    }
    
    // Check for null pointers before accessing nested structures
    if (!rec->pattern || !rec->pattern->hotspot) {
        return false;
    }
    
    const char *function = rec->pattern->hotspot->location.function;
    int line = rec->pattern->hotspot->location.line;
    
    // Additional safety check
    if (!function) {
        return false;
    }
    
    // Skip main() function for loop optimizations
    if (strcmp(function, "main") == 0) {
        switch (rec->type) {
            case OPT_LOOP_VECTORIZE:
            case OPT_PREFETCH_HINTS:
            case OPT_LOOP_TILING:
            case OPT_CACHE_BLOCKING:
                return false;  // Don't optimize initialization code
            default:
                break;
        }
    }
    
    // Skip utility functions
    if (strstr(function, "printf") || strstr(function, "malloc") || 
        strstr(function, "free") || strstr(function, "init")) {
        return false;
    }
    
    // Skip recommendations with very low confidence or impact
    if (rec->confidence_score < 0.5 || rec->expected_improvement < 15.0) {
        return false;
    }
    
    // OPT_LOOP_VECTORIZE: Only makes sense for computational loops
    if (rec->type == OPT_LOOP_VECTORIZE) {
        // Skip if this looks like initialization (early lines, low impact)
        if (line < 30 && rec->expected_improvement < 30.0) {
            return false;
        }
        // Skip if very low confidence
        if (rec->confidence_score < 0.8) {
            return false;
        }
    }
    
    // OPT_PREFETCH_HINTS: Only for significant access patterns
    if (rec->type == OPT_PREFETCH_HINTS) {
        // Skip for small arrays or low-impact patterns
        if (rec->expected_improvement < 20.0) {
            return false;
        }
    }
    
    // OPT_DATA_LAYOUT_CHANGE: Only if there's actually random access
    if (rec->type == OPT_DATA_LAYOUT_CHANGE) {
        // This shouldn't be recommended for simple matrix multiplication
        if (strstr(function, "matrix") && rec->confidence_score < 0.8) {
            return false;
        }
    }
    
    return true;
}

// Forward declaration
int compare_recommendation_quality(const void *a, const void *b);

// 3. SMART DEDUPLICATION THAT KEEPS ONE PER LOGICAL SCOPE

// ============================================================================
// MEMORY STRUCTURE DEBUG - Add this to recommendation_engine.c
// ============================================================================

// Ultra-basic memory check
int check_memory_structure(optimization_rec_t *input_recs, int input_count) {
    printf("=== MEMORY STRUCTURE DEBUG ===\n");
    printf("Input recs pointer: %p\n", (void*)input_recs);
    printf("Input count: %d\n", input_count);
    printf("sizeof(optimization_rec_t): %zu\n", sizeof(optimization_rec_t));
    
    if (!input_recs) {
        printf("ERROR: input_recs is NULL!\n");
        return -1;
    }
    
    if (input_count <= 0 || input_count > 1000) {
        printf("ERROR: Suspicious input_count: %d\n", input_count);
        return -1;
    }
    
    // Try to access just the memory addresses without reading values
    printf("Memory layout check:\n");
    for (int i = 0; i < input_count && i < 5; i++) {
        void *rec_addr = &input_recs[i];
        printf("  Rec[%d] address: %p\n", i, rec_addr);
        
        // Try to access each field offset without reading the value
        void *type_addr = &(input_recs[i].type);
        void *priority_addr = &(input_recs[i].priority);
        void *improvement_addr = &(input_recs[i].expected_improvement);
        
        printf("    type field at: %p\n", type_addr);
        printf("    priority field at: %p\n", priority_addr);
        printf("    improvement field at: %p\n", improvement_addr);
    }
    
    printf("=== TRYING TO READ FIRST VALUE ===\n");
    // Try to read just one value very carefully
    volatile int test_type = input_recs[0].type;
    printf("First rec type: %d\n", test_type);
    
    return 0;
}

// Replace your deduplicate_by_scope function with this minimal version:
int deduplicate_by_scope(optimization_rec_t *input_recs, int input_count,
                        optimization_rec_t **output_recs, int *output_count) {
    
    printf("=== MEMORY DEBUG DEDUPLICATION ===\n");
    
    // Check memory structure first
    if (check_memory_structure(input_recs, input_count) != 0) {
        return -1;
    }
    
    // If we get here, just return the input unchanged for now
    optimization_rec_t *output = malloc(input_count * sizeof(optimization_rec_t));
    if (!output) {
        printf("Failed to allocate output array\n");
        return -1;
    }
    
    // Simple copy without any processing
    memcpy(output, input_recs, input_count * sizeof(optimization_rec_t));
    
    *output_recs = output;
    *output_count = input_count;
    
    printf("Memory debug complete - returning %d recommendations unchanged\n", input_count);
    return 0;
}



/*
int deduplicate_by_scope(optimization_rec_t *input_recs, int input_count,
                        optimization_rec_t **output_recs, int *output_count) {
    
    // Step 1: Filter out non-sensible recommendations
    optimization_rec_t *sensible_recs = malloc(input_count * sizeof(optimization_rec_t));
    int sensible_count = 0;
    
    printf("=== FILTERING RECOMMENDATIONS ===\n");
    for (int i = 0; i < input_count; i++) {
        // Safe access with null checks
        const char *func = "unknown";
        int line = -1;
        
        if (input_recs[i].pattern && input_recs[i].pattern->hotspot) {
            func = input_recs[i].pattern->hotspot->location.function;
            line = input_recs[i].pattern->hotspot->location.line;
        }
        
        bool keep = is_sensible_recommendation(&input_recs[i]);
        
        printf("Rec #%d: %s at %s:%d (%.1f%% improvement) - %s\n",
               i + 1,
               optimization_type_to_string(input_recs[i].type),
               func ? func : "null", line,
               input_recs[i].expected_improvement,
               keep ? "KEEP" : "FILTER");
               
        if (keep) {
            sensible_recs[sensible_count++] = input_recs[i];
        }
    }
    
    // Step 2: Remove duplicates within same logical scope
    optimization_rec_t *unique_recs = malloc(sensible_count * sizeof(optimization_rec_t));
    int unique_count = 0;
    
    printf("\n=== DEDUPLICATING BY SCOPE ===\n");
    for (int i = 0; i < sensible_count; i++) {
        bool is_duplicate = false;
        
        // Check if we already have this optimization for this scope
        for (int j = 0; j < unique_count; j++) {
            if (is_same_logical_scope(&sensible_recs[i], &unique_recs[j])) {
                // Keep the better recommendation
                if (sensible_recs[i].priority < unique_recs[j].priority ||
                    (sensible_recs[i].priority == unique_recs[j].priority && 
                     sensible_recs[i].expected_improvement > unique_recs[j].expected_improvement)) {
                    
                    printf("Replacing %s with better version (%.1f%% vs %.1f%%)\n",
                           optimization_type_to_string(sensible_recs[i].type),
                           sensible_recs[i].expected_improvement,
                           unique_recs[j].expected_improvement);
                           
                    unique_recs[j] = sensible_recs[i];  // Replace with better
                } else {
                    printf("Skipping duplicate %s (worse than existing)\n",
                           optimization_type_to_string(sensible_recs[i].type));
                }
                is_duplicate = true;
                break;
            }
        }
        
        if (!is_duplicate) {
            const char *func = "unknown";
            int line = -1;
            
            if (sensible_recs[i].pattern && sensible_recs[i].pattern->hotspot) {
                func = sensible_recs[i].pattern->hotspot->location.function;
                line = sensible_recs[i].pattern->hotspot->location.line;
            }
            
            printf("Adding unique %s for %s:%d\n",
                   optimization_type_to_string(sensible_recs[i].type),
                   func ? func : "null", line);
                   
            unique_recs[unique_count++] = sensible_recs[i];
        }
    }
    
    // Step 3: Sort by priority and impact
    qsort(unique_recs, unique_count, sizeof(optimization_rec_t), compare_recommendation_quality);
    
    *output_recs = unique_recs;
    *output_count = unique_count;
    
    printf("\n=== FINAL RESULT ===\n");
    printf("Original: %d recommendations\n", input_count);
    printf("After filtering: %d recommendations\n", sensible_count);
    printf("After deduplication: %d recommendations\n", unique_count);
    
    free(sensible_recs);
    return 0;
}
*/

// 4. COMPARISON FUNCTION FOR SORTING
int compare_recommendation_quality(const void *a, const void *b) {
    const optimization_rec_t *rec_a = (const optimization_rec_t *)a;
    const optimization_rec_t *rec_b = (const optimization_rec_t *)b;
    
    // Priority first (lower number = higher priority)
    if (rec_a->priority != rec_b->priority) {
        return rec_a->priority - rec_b->priority;
    }
    
    // Expected improvement second (higher = better)
    if (rec_a->expected_improvement > rec_b->expected_improvement) return -1;
    if (rec_a->expected_improvement < rec_b->expected_improvement) return 1;
    
    // Confidence third (higher = better)  
    if (rec_a->confidence_score > rec_b->confidence_score) return -1;
    if (rec_a->confidence_score < rec_b->confidence_score) return 1;
    
    return 0;
}

// Safe string helper
const char* safe_string(const char* str) {
    return str ? str : "NULL";
}

// Safe optimization type to string (avoid external function)
const char* safe_opt_type_string(optimization_type_t type) {
    switch (type) {
        case OPT_LOOP_TILING: return "LOOP_TILING";
        case OPT_ACCESS_REORDER: return "ACCESS_REORDER"; 
        case OPT_CACHE_BLOCKING: return "CACHE_BLOCKING";
        case OPT_LOOP_VECTORIZE: return "LOOP_VECTORIZE";
        case OPT_PREFETCH_HINTS: return "PREFETCH_HINTS";
        case OPT_DATA_LAYOUT_CHANGE: return "DATA_LAYOUT_CHANGE";
        case OPT_MEMORY_POOLING: return "MEMORY_POOLING";
        case OPT_MEMORY_ALIGNMENT: return "MEMORY_ALIGNMENT";
        case OPT_LOOP_UNROLL: return "LOOP_UNROLL";
        case OPT_NUMA_BINDING: return "NUMA_BINDING";
        default: return "UNKNOWN";
    }
}

// Step-by-step debugging function
int debug_recommendations_safely(optimization_rec_t *input_recs, int input_count) {
    printf("=== ULTRA-SAFE DEBUG START ===\n");
    printf("Input count: %d\n", input_count);
    
    if (!input_recs) {
        printf("ERROR: input_recs is NULL!\n");
        return -1;
    }
    
    for (int i = 0; i < input_count; i++) {
        printf("--- Rec #%d ---\n", i + 1);
        
        // Check basic structure
        printf("  Rec pointer: %p\n", (void*)&input_recs[i]);
        printf("  Type: %d\n", (int)input_recs[i].type);
        printf("  Type string: %s\n", safe_opt_type_string(input_recs[i].type));
        printf("  Expected improvement: %.1f\n", input_recs[i].expected_improvement);
        printf("  Confidence: %.1f\n", input_recs[i].confidence_score);
        
        // Check pattern pointer
        printf("  Pattern: %p\n", (void*)input_recs[i].pattern);
        if (input_recs[i].pattern) {
            printf("  Hotspot: %p\n", (void*)input_recs[i].pattern->hotspot);
            if (input_recs[i].pattern->hotspot) {
                printf("  Function: %s\n", safe_string(input_recs[i].pattern->hotspot->location.function));
                printf("  Line: %d\n", input_recs[i].pattern->hotspot->location.line);
            } else {
                printf("  Hotspot is NULL\n");
            }
        } else {
            printf("  Pattern is NULL\n");
        }
        
        printf("  --- End Rec #%d ---\n", i + 1);
        
        // Stop if we detect any major issues
        if (i > 20) {
            printf("Stopping debug after 20 items to prevent overflow\n");
            break;
        }
    }
    
    printf("=== ULTRA-SAFE DEBUG END ===\n");
    return 0;
}

// Ultra-simple deduplication that just filters main()
int simple_filter_main_only(optimization_rec_t *input_recs, int input_count,
                            optimization_rec_t **output_recs, int *output_count) {
    
    printf("=== SIMPLE FILTER (MAIN ONLY) ===\n");
    
    // First, debug the input safely
    if (debug_recommendations_safely(input_recs, input_count) != 0) {
        return -1;
    }
    
    // Allocate output array
    optimization_rec_t *filtered = malloc(input_count * sizeof(optimization_rec_t));
    if (!filtered) {
        printf("ERROR: Failed to allocate filtered array\n");
        return -1;
    }
    
    int filtered_count = 0;
    
    printf("Starting filtering loop...\n");
    for (int i = 0; i < input_count; i++) {
        bool keep = true;
        
        printf("Processing rec #%d: ", i + 1);
        
        // Only filter out main() function recommendations
        if (input_recs[i].pattern && 
            input_recs[i].pattern->hotspot && 
            input_recs[i].pattern->hotspot->location.function) {
            
            const char *func = input_recs[i].pattern->hotspot->location.function;
            printf("Function: %s ", func);
            
            if (strcmp(func, "main") == 0) {
                printf("- FILTERED (main function)");
                keep = false;
            } else {
                printf("- KEPT");
            }
        } else {
            printf("- KEPT (no function info)");
        }
        
        printf("\n");
        
        if (keep) {
            filtered[filtered_count] = input_recs[i];
            filtered_count++;
        }
    }
    
    printf("Filtered: %d -> %d recommendations\n", input_count, filtered_count);
    
    *output_recs = filtered;
    *output_count = filtered_count;
    
    return 0;
}


int recommendation_engine_save_to_file(const optimization_rec_t *recs, int count,
                                      const char *filename) {
    FILE *fp = fopen(filename, "w");
    if (!fp) {
        LOG_ERROR("Failed to open %s for writing", filename);
        return -1;
    }
    
    fprintf(fp, "Cache Optimization Recommendations\n");
    fprintf(fp, "==================================\n\n");
    fprintf(fp, "Total recommendations: %d\n\n", count);
    
    // Group by location for better readability
    int rec_num = 1;
    for (int i = 0; i < count; i++) {
        const optimization_rec_t *rec = &recs[i];
        
        // Check if this is a new location
        bool new_location = true;
        if (i > 0 && recs[i-1].pattern && rec->pattern &&
            recs[i-1].pattern->hotspot && rec->pattern->hotspot) {
            cache_hotspot_t *prev = recs[i-1].pattern->hotspot;
            cache_hotspot_t *curr = rec->pattern->hotspot;
            if (prev->location.line == curr->location.line &&
                strcmp(prev->location.file, curr->location.file) == 0) {
                new_location = false;
            }
        }
        
        if (new_location) {
            fprintf(fp, "\n========================================\n\n");
        }
        
        fprintf(fp, "Recommendation #%d\n", rec_num++);
        fprintf(fp, "-----------------\n");
        fprintf(fp, "Type: %s\n", optimization_type_to_string(rec->type));
        fprintf(fp, "Priority: %d\n", rec->priority);
        fprintf(fp, "Expected Improvement: %.1f%%\n", rec->expected_improvement);
        fprintf(fp, "Confidence: %.0f%%\n", rec->confidence_score * 100);
        fprintf(fp, "Implementation Difficulty: %d/10\n", rec->implementation_difficulty);
        
        if (rec->pattern && rec->pattern->hotspot) {
            fprintf(fp, "Location: %s:%d\n",
                   rec->pattern->hotspot->location.file,
                   rec->pattern->hotspot->location.line);
        }
        
        fprintf(fp, "\nRationale:\n%s\n", rec->rationale);
        
        if (strlen(rec->compiler_flags) > 0) {
            fprintf(fp, "\nCompiler Flags:\n%s\n", rec->compiler_flags);
        }
        
        if (strlen(rec->implementation_guide) > 0) {
            fprintf(fp, "\nImplementation Guide:\n%s\n", rec->implementation_guide);
        }
        
        if (strlen(rec->code_suggestion) > 0) {
            fprintf(fp, "\nCode Example:\n%s\n", rec->code_suggestion);
        }
        
        fprintf(fp, "\n");
    }
    
    fclose(fp);
    LOG_INFO("Saved %d recommendations to %s", count, filename);
    return 0;
}



// In recommendation_engine.c, replace the entire recommendation_engine_analyze_all function:

int recommendation_engine_analyze_all(recommendation_engine_t *engine,
                                     const classified_pattern_t *patterns,
                                     int pattern_count,
                                     optimization_rec_t **all_recommendations,
                                     int *total_rec_count) {
    if (!engine || !patterns || pattern_count <= 0 || !all_recommendations || !total_rec_count) {
        LOG_ERROR("Invalid parameters for recommendation_engine_analyze_all");
        return -1;
    }
    
    // Track which locations we've already generated recommendations for
    typedef struct {
        char file[256];
        int line;
        optimization_type_t types[10];  // Track which types we've recommended
        int type_count;
    } location_tracker_t;
    
    location_tracker_t *tracked_locations = CALLOC_LOGGED(pattern_count * 5, sizeof(location_tracker_t));
    int tracked_count = 0;
    
    // Pre-allocate space for all potential recommendations
    int max_total = pattern_count * engine->config.max_recommendations;
    optimization_rec_t *temp_recs = CALLOC_LOGGED(max_total, sizeof(optimization_rec_t));
    if (!temp_recs || !tracked_locations) {
        if (temp_recs) FREE_LOGGED(temp_recs);
        if (tracked_locations) FREE_LOGGED(tracked_locations);
        return -1;
    }
    
    int total_count = 0;
    
    // Helper function to check if we already have this type at this location
    auto already_has_recommendation = [&](const char* file, int line, optimization_type_t type) -> bool {
        for (int i = 0; i < tracked_count; i++) {
            if (tracked_locations[i].line == line && 
                strcmp(tracked_locations[i].file, file) == 0) {
                // Check if we already have this type
                for (int j = 0; j < tracked_locations[i].type_count; j++) {
                    if (tracked_locations[i].types[j] == type) {
                        return true;
                    }
                }
                return false;
            }
        }
        return false;
    };
    
    // Helper function to add a tracked recommendation
    auto track_recommendation = [&](const char* file, int line, optimization_type_t type) {
        // Find or create location entry
        int loc_idx = -1;
        for (int i = 0; i < tracked_count; i++) {
            if (tracked_locations[i].line == line && 
                strcmp(tracked_locations[i].file, file) == 0) {
                loc_idx = i;
                break;
            }
        }
        
        if (loc_idx == -1) {
            // New location
            loc_idx = tracked_count++;
            strncpy(tracked_locations[loc_idx].file, file, 255);
            tracked_locations[loc_idx].line = line;
            tracked_locations[loc_idx].type_count = 0;
        }
        
        // Add type if there's room
        if (tracked_locations[loc_idx].type_count < 10) {
            tracked_locations[loc_idx].types[tracked_locations[loc_idx].type_count++] = type;
        }
    };
    
    // Process each pattern
    for (int i = 0; i < pattern_count; i++) {
        // Skip patterns without valid hotspot data
        if (!patterns[i].hotspot) {
            LOG_WARNING("Pattern %d has no hotspot data, skipping", i);
            continue;
        }
        
        // Validate line number - skip if it's beyond reasonable bounds
        if (patterns[i].hotspot->location.line <= 0 || 
            patterns[i].hotspot->location.line > 1000) {
            LOG_WARNING("Invalid line number %d for pattern %d, skipping",
                       patterns[i].hotspot->location.line, i);
            continue;
        }
        
        optimization_rec_t *recs = NULL;
        int count = 0;
        
        if (recommendation_engine_analyze(engine, &patterns[i], &recs, &count) == 0) {
            // Filter recommendations based on location tracking
            for (int j = 0; j < count; j++) {
                if (!recs[j].pattern || !recs[j].pattern->hotspot) {
                    continue;
                }
                
                const char* file = recs[j].pattern->hotspot->location.file;
                int line = recs[j].pattern->hotspot->location.line;
                optimization_type_t type = recs[j].type;
                
                // Skip if we already have this type of recommendation at this location
                if (already_has_recommendation(file, line, type)) {
                    LOG_DEBUG("Skipping duplicate %s at %s:%d",
                             optimization_type_to_string(type), file, line);
                    continue;
                }
                
                // Additional filtering based on context
                bool should_include = true;
                
                // Filter out memory pooling for static arrays
                if (type == OPT_MEMORY_POOLING) {
                    // Check if this is actually dynamic allocation context
                    // For now, skip memory pooling for matrix multiplication
                    if (strstr(file, "matrix") != NULL) {
                        LOG_DEBUG("Skipping memory pooling for matrix code");
                        should_include = false;
                    }
                }
                
                // Filter out SoA transformation unless it's actually struct access
                if (type == OPT_DATA_LAYOUT_CHANGE) {
                    // Check if the pattern is actually about struct access
                    if (patterns[i].hotspot->dominant_pattern != GATHER_SCATTER &&
                        patterns[i].hotspot->dominant_pattern != RANDOM) {
                        LOG_DEBUG("Skipping data layout change for non-gather pattern");
                        should_include = false;
                    }
                }
                
                // Only include if it passes all filters
                if (should_include && total_count < max_total) {
                    temp_recs[total_count] = recs[j];
                    track_recommendation(file, line, type);
                    total_count++;
                    
                    LOG_DEBUG("Added %s recommendation for %s:%d (total: %d)",
                             optimization_type_to_string(type), file, line, total_count);
                }
            }
            FREE_LOGGED(recs);
        }
    }
    
    // Sort by priority and expected improvement
    qsort(temp_recs, total_count, sizeof(optimization_rec_t), 
          [](const void* a, const void* b) -> int {
              const optimization_rec_t* ra = (const optimization_rec_t*)a;
              const optimization_rec_t* rb = (const optimization_rec_t*)b;
              
              // First sort by priority (lower number = higher priority)
              if (ra->priority != rb->priority) {
                  return ra->priority - rb->priority;
              }
              
              // Then by expected improvement (higher is better)
              if (ra->expected_improvement > rb->expected_improvement) return -1;
              if (ra->expected_improvement < rb->expected_improvement) return 1;
              
              return 0;
          });
    
    // Final allocation and copy
    *all_recommendations = CALLOC_LOGGED(total_count, sizeof(optimization_rec_t));
    if (!*all_recommendations) {
        FREE_LOGGED(temp_recs);
        FREE_LOGGED(tracked_locations);
        return -1;
    }
    
    memcpy(*all_recommendations, temp_recs, total_count * sizeof(optimization_rec_t));
    *total_rec_count = total_count;
    
    FREE_LOGGED(temp_recs);
    FREE_LOGGED(tracked_locations);
    
    LOG_INFO("Generated %d unique recommendations after deduplication", total_count);
    
    return 0;
}

// Also fix the synthetic pattern generation in main.c to avoid invalid line numbers:
// In main.c, replace the synthetic pattern generation section:

    // If no dynamic profiling data, create synthetic patterns from static analysis
    if (sample_count == 0 && static_results.pattern_count > 0) {
        LOG_INFO("No dynamic profiling data - generating patterns from static analysis");
        
        // Count valid patterns (with reasonable line numbers)
        int valid_pattern_count = 0;
        for (int i = 0; i < static_results.pattern_count; i++) {
            if (static_results.patterns[i].location.line > 0 && 
                static_results.patterns[i].location.line <= 1000) {
                valid_pattern_count++;
            }
        }
        
        if (valid_pattern_count == 0) {
            LOG_WARNING("No valid static patterns found");
            goto cleanup;
        }
        
        hotspot_count = valid_pattern_count;
        hotspots = CALLOC_LOGGED(hotspot_count, sizeof(cache_hotspot_t));
        patterns = CALLOC_LOGGED(hotspot_count, sizeof(classified_pattern_t));
        pattern_count = hotspot_count;
        
        int idx = 0;
        for (int i = 0; i < static_results.pattern_count && idx < hotspot_count; i++) {
            static_pattern_t *sp = &static_results.patterns[i];
            
            // Skip patterns with invalid line numbers
            if (sp->location.line <= 0 || sp->location.line > 1000) {
                LOG_DEBUG("Skipping pattern with invalid line %d", sp->location.line);
                continue;
            }
            
            cache_hotspot_t *hs = &hotspots[idx];
            classified_pattern_t *pat = &patterns[idx];
            
            // Copy location and set dominant pattern
            hs->location = sp->location;
            hs->dominant_pattern = sp->pattern;
            hs->access_stride = (sp->stride > 0 && sp->stride < 1000) ? sp->stride : 1;
            pat->hotspot = hs;
            
            // Map static patterns to cache antipatterns
            switch(sp->pattern) {
                case SEQUENTIAL:
                    pat->type = HOTSPOT_REUSE;
                    hs->miss_rate = 0.05;
                    hs->avg_latency_cycles = 4;
                    break;
                    
                case STRIDED:
                    // For matrix multiplication, strided usually means poor loop order
                    if (strstr(sp->location.file, "matrix") != NULL) {
                        pat->type = LOOP_INTERCHANGE_NEEDED;
                        hs->miss_rate = 0.6;
                    } else if (sp->stride > 8) {
                        pat->type = STREAMING_EVICTION;
                        hs->miss_rate = 0.5;
                    } else {
                        pat->type = BANK_CONFLICTS;
                        hs->miss_rate = 0.4;
                    }
                    hs->avg_latency_cycles = 50 + sp->stride * 2;
                    break;
                    
                case RANDOM:
                    pat->type = IRREGULAR_GATHER_SCATTER;
                    hs->miss_rate = 0.8;
                    hs->avg_latency_cycles = 200;
                    break;
                    
                case GATHER_SCATTER:
                    pat->type = IRREGULAR_GATHER_SCATTER;
                    hs->miss_rate = 0.7;
                    hs->avg_latency_cycles = 150;
                    break;
                    
                default:
                    pat->type = UNCLASSIFIED;
                    hs->miss_rate = 0.5;
                    hs->avg_latency_cycles = 100;
                    break;
            }
            
            // Set other pattern fields
            pat->severity_score = hs->miss_rate * 100;
            pat->confidence = 0.7;  // Lower confidence for synthetic
            pat->performance_impact = hs->miss_rate * 50;
            
            snprintf(pat->description, sizeof(pat->description),
                     "Static analysis pattern: %s access with stride %d at line %d",
                     access_pattern_to_string(sp->pattern), 
                     hs->access_stride,
                     sp->location.line);
            snprintf(pat->root_cause, sizeof(pat->root_cause),
                     "Detected from static code analysis at %s:%d",
                     sp->location.file, sp->location.line);
            
            idx++;
        }
        
        // Adjust counts if we skipped any
        hotspot_count = idx;
        pattern_count = idx;
        
        LOG_INFO("Generated %d synthetic patterns from static analysis", pattern_count);
    }


// Create recommendation engine
recommendation_engine_t* recommendation_engine_create(const engine_config_t *config,
                                                    const cache_info_t *cache_info) {
    if (!config || !cache_info) {
        LOG_ERROR("NULL parameters for recommendation_engine_create");
        return NULL;
    }
    
    recommendation_engine_t *engine = CALLOC_LOGGED(1, sizeof(recommendation_engine_t));
    if (!engine) {
        LOG_ERROR("Failed to allocate recommendation engine");
        return NULL;
    }
    
    engine->config = *config;
    engine->cache_info = *cache_info;
    pthread_mutex_init(&engine->mutex, NULL);
    
    LOG_INFO("Created recommendation engine with min improvement threshold %.1f%%",
             config->min_expected_improvement);
    
    return engine;
}

// Destroy recommendation engine
void recommendation_engine_destroy(recommendation_engine_t *engine) {
    if (!engine) return;
    
    LOG_INFO("Destroying recommendation engine");
    pthread_mutex_destroy(&engine->mutex);
    FREE_LOGGED(engine);
}




// Analyze single pattern
// Analyze single pattern with comprehensive pattern-specific recommendations
// Fix 2: Improved recommendation analysis with better stride handling
int recommendation_engine_analyze(recommendation_engine_t *engine,
                                 const classified_pattern_t *pattern,
                                 optimization_rec_t **recommendations,
                                 int *rec_count) {
    if (!engine || !pattern || !recommendations || !rec_count) {
        LOG_ERROR("Invalid parameters for recommendation_engine_analyze");
        return -1;
    }
    
    LOG_INFO("Analyzing pattern %s for optimizations (access pattern: %s)",
             cache_antipattern_to_string(pattern->type),
             pattern->hotspot ? access_pattern_to_string(pattern->hotspot->dominant_pattern) : "unknown");
    
    pthread_mutex_lock(&engine->mutex);
    
    // Allocate space for recommendations
    optimization_rec_t *recs = CALLOC_LOGGED(engine->config.max_recommendations, 
                                            sizeof(optimization_rec_t));
    if (!recs) {
        pthread_mutex_unlock(&engine->mutex);
        return -1;
    }
    
    int count = 0;
    
    // Skip if no hotspot data
    if (!pattern->hotspot) {
        LOG_WARNING("Pattern has no hotspot data, skipping");
        *recommendations = recs;
        *rec_count = 0;
        pthread_mutex_unlock(&engine->mutex);
        return 0;
    }
    
    // Validate and sanitize stride value
    int stride = pattern->hotspot->access_stride;
    if (stride < 0 || stride > 10000) {
        LOG_DEBUG("Invalid stride %d, using default based on pattern", stride);
        // Set reasonable defaults based on pattern
        switch (pattern->hotspot->dominant_pattern) {
            case SEQUENTIAL:
                stride = 1;
                break;
            case STRIDED:
                stride = 8;  // Common stride for doubles
                break;
            case RANDOM:
            case GATHER_SCATTER:
                stride = 0;  // No regular stride
                break;
            default:
                stride = 1;
        }
    }
    
    // Generate recommendations based on access pattern
    switch (pattern->hotspot->dominant_pattern) {
        case SEQUENTIAL:
            // Vectorization for sequential access
            if (count < engine->config.max_recommendations) {
                optimization_rec_t *rec = &recs[count];
                rec->type = OPT_LOOP_VECTORIZE;
                rec->pattern = (classified_pattern_t*)pattern;
                rec->expected_improvement = 40.0;
                rec->confidence_score = 0.9;
                rec->implementation_difficulty = 3;
                rec->priority = 2;
                
                snprintf(rec->code_suggestion, sizeof(rec->code_suggestion),
                        "// Vectorize sequential access\n"
                        "#pragma omp simd\n"
                        "for (int i = 0; i < n; i++) {\n"
                        "    sum += data[i];\n"
                        "}\n\n"
                        "// Or use intrinsics for more control:\n"
                        "#include <immintrin.h>\n"
                        "__m256d vsum = _mm256_setzero_pd();\n"
                        "for (int i = 0; i < n; i += 4) {\n"
                        "    __m256d vdata = _mm256_load_pd(&data[i]);\n"
                        "    vsum = _mm256_add_pd(vsum, vdata);\n"
                        "}");
                
                snprintf(rec->implementation_guide, sizeof(rec->implementation_guide),
                        "1. Ensure data is aligned to 32-byte boundaries\n"
                        "2. Use -march=native for auto-vectorization\n"
                        "3. Consider #pragma omp simd for explicit vectorization\n"
                        "4. Check vectorization report with -fopt-info-vec");
                
                snprintf(rec->rationale, sizeof(rec->rationale),
                        "Sequential access patterns are ideal for SIMD vectorization. "
                        "Processing 4-8 elements simultaneously can improve performance by 4-8x.");
                
                snprintf(rec->compiler_flags, sizeof(rec->compiler_flags),
                        "-O3 -march=native -ftree-vectorize -mavx2 -mfma -fopt-info-vec");
                
                if (!isDuplicate(recs, count, rec->type, pattern)) {
                    count++;
                }
            }
            
            // Prefetching for sequential (less important)
            if (count < engine->config.max_recommendations && pattern->hotspot->miss_rate > 0.05) {
                if (generate_prefetch_recommendation(pattern, &recs[count]) == 0) {
                    recs[count].expected_improvement = 15.0;
                    recs[count].priority = 3;
                    if (!isDuplicate(recs, count, recs[count].type, pattern)) {
                        count++;
                    }
                }
            }
            break;
            
        case STRIDED:
            // Loop tiling for strided access in nested loops
            if (count < engine->config.max_recommendations) {
                if (generate_loop_tiling_recommendation(pattern, &engine->cache_info, &recs[count]) == 0) {
                    if (!isDuplicate(recs, count, recs[count].type, pattern)) {
                        count++;
                    }
                }
            }
            
            // Access reordering for strided patterns
            if (count < engine->config.max_recommendations) {
                optimization_rec_t *rec = &recs[count];
                rec->type = OPT_ACCESS_REORDER;
                rec->pattern = (classified_pattern_t*)pattern;
                rec->expected_improvement = 60.0;
                rec->confidence_score = 0.95;
                rec->implementation_difficulty = 2;
                rec->priority = 1;
                
                snprintf(rec->code_suggestion, sizeof(rec->code_suggestion),
                        "// Original column-major access (poor)\n"
                        "// for (int j = 0; j < N; j++)\n"
                        "//     for (int i = 0; i < M; i++)\n"
                        "//         sum += matrix[i][j];\n\n"
                        "// Optimized row-major access\n"
                        "for (int i = 0; i < M; i++) {\n"
                        "    for (int j = 0; j < N; j++) {\n"
                        "        sum += matrix[i][j];  // Sequential in memory\n"
                        "    }\n"
                        "}\n\n"
                        "// Or use loop interchange pragma\n"
                        "#pragma GCC ivdep\n"
                        "#pragma GCC loop interchange");
                
                snprintf(rec->implementation_guide, sizeof(rec->implementation_guide),
                        "1. Swap loop order to access memory sequentially\n"
                        "2. Inner loop should iterate over contiguous memory\n"
                        "3. Use compiler pragmas for automatic interchange\n"
                        "4. Consider cache-oblivious algorithms");
                
                snprintf(rec->rationale, sizeof(rec->rationale),
                        "Column-major access in row-major layout causes cache misses on every access. "
                        "Loop interchange provides immediate and significant improvement.");
                
                snprintf(rec->compiler_flags, sizeof(rec->compiler_flags),
                        "-floop-interchange -ftree-loop-distribution -ftree-loop-im");
                
                if (!isDuplicate(recs, count, rec->type, pattern)) {
                    count++;
                }
            }
            
            // Cache blocking
            if (count < engine->config.max_recommendations) {
                optimization_rec_t *rec = &recs[count];
                rec->type = OPT_CACHE_BLOCKING;
                rec->pattern = (classified_pattern_t*)pattern;
                rec->expected_improvement = 45.0;
                rec->confidence_score = 0.85;
                rec->implementation_difficulty = 5;
                rec->priority = 2;
                
                snprintf(rec->code_suggestion, sizeof(rec->code_suggestion),
                        "// Cache blocking to reduce working set\n"
                        "const int L1_BLOCK = 32;   // Fit in L1\n"
                        "const int L2_BLOCK = 128;  // Fit in L2\n"
                        "const int L3_BLOCK = 512;  // Fit in L3\n\n"
                        "for (int l3 = 0; l3 < n; l3 += L3_BLOCK) {\n"
                        "    for (int l2 = l3; l2 < min(l3 + L3_BLOCK, n); l2 += L2_BLOCK) {\n"
                        "        for (int l1 = l2; l1 < min(l2 + L2_BLOCK, n); l1 += L1_BLOCK) {\n"
                        "            // Process L1-sized block\n"
                        "        }\n"
                        "    }\n"
                        "}");
                
                snprintf(rec->rationale, sizeof(rec->rationale),
                        "Multi-level cache blocking keeps data in appropriate cache levels, "
                        "preventing thrashing.");
                
                snprintf(rec->compiler_flags, sizeof(rec->compiler_flags),
                        "-floop-block --param l1-cache-size=32 --param l2-cache-size=512");
                
                if (!isDuplicate(recs, count, rec->type, pattern)) {
                    count++;
                }
            }
            break;
            
        case RANDOM:
        case GATHER_SCATTER:
            // Data layout transformation for random/gather-scatter
            if (count < engine->config.max_recommendations) {
                if (generate_data_layout_recommendation(pattern, &recs[count]) == 0) {
                    recs[count].expected_improvement = 35.0;  // Lower for random
                    recs[count].confidence_score = 0.6;
                    recs[count].implementation_difficulty = 8;
                    if (!isDuplicate(recs, count, recs[count].type, pattern)) {
                        count++;
                    }
                }
            }
            
            // Memory pooling for random access
            if (count < engine->config.max_recommendations) {
                optimization_rec_t *rec = &recs[count];
                rec->type = OPT_MEMORY_POOLING;
                rec->pattern = (classified_pattern_t*)pattern;
                rec->expected_improvement = 20.0;
                rec->confidence_score = 0.7;
                rec->implementation_difficulty = 6;
                rec->priority = 3;
                
                snprintf(rec->code_suggestion, sizeof(rec->code_suggestion),
                        "// Use memory pool to improve locality\n"
                        "typedef struct {\n"
                        "    void* blocks[MAX_BLOCKS];\n"
                        "    size_t block_size;\n"
                        "    int free_list[MAX_BLOCKS];\n"
                        "} memory_pool_t;\n\n"
                        "// Allocate from pool instead of malloc\n"
                        "data = pool_alloc(&pool, size);");
                
                snprintf(rec->rationale, sizeof(rec->rationale),
                        "Memory pooling keeps related data together, improving cache locality "
                        "for random access.");
                
                if (!isDuplicate(recs, count, rec->type, pattern)) {
                    count++;
                }
            }
            break;
            
        default:
            LOG_WARNING("Unknown access pattern: %d", pattern->hotspot->dominant_pattern);
            break;
    }
    
    // Add non-temporal hints for streaming patterns with high miss rates
    if (pattern->hotspot->miss_rate > 0.7 && count < engine->config.max_recommendations) {
        optimization_rec_t *rec = &recs[count];
        rec->type = OPT_PREFETCH_HINTS;
        rec->pattern = (classified_pattern_t*)pattern;
        rec->expected_improvement = 25.0;
        rec->confidence_score = 0.8;
        rec->implementation_difficulty = 4;
        rec->priority = 3;
        
        snprintf(rec->code_suggestion, sizeof(rec->code_suggestion),
                "// Non-temporal stores for streaming data\n"
                "#include <immintrin.h>\n"
                "for (int i = 0; i < large_n; i += 4) {\n"
                "    __m256d vdata = _mm256_load_pd(&input[i]);\n"
                "    // Process vdata\n"
                "    _mm256_stream_pd(&output[i], vdata);  // Bypass cache\n"
                "}\n"
                "_mm_sfence();  // Ensure completion\n\n"
                "// Or use compiler intrinsics\n"
                "#pragma GCC ivdep\n"
                "#pragma vector nontemporal");
        
        snprintf(rec->implementation_guide, sizeof(rec->implementation_guide),
                "1. Use non-temporal stores for data not reused\n"
                "2. Keep frequently accessed data in cache\n"
                "3. Process in chunks to maintain useful data\n"
                "4. Consider cache partitioning if available");
        
        snprintf(rec->rationale, sizeof(rec->rationale),
                "Non-temporal hints prevent streaming data from evicting useful cached data, "
                "preserving performance.");
        
        if (!isDuplicate(recs, count, rec->type, pattern)) {
            count++;
        }
    }
    
    // Filter by minimum expected improvement
    int filtered_count = 0;
    for (int i = 0; i < count; i++) {
        if (recs[i].expected_improvement >= engine->config.min_expected_improvement) {
            if (i != filtered_count) {
                recs[filtered_count] = recs[i];
            }
            filtered_count++;
        }
    }
    
    // Rank recommendations
    rank_recommendations(recs, filtered_count);
    
    *recommendations = recs;
    *rec_count = filtered_count;
    
    engine->total_recommendations_generated += filtered_count;
    
    pthread_mutex_unlock(&engine->mutex);
    
    LOG_INFO("Generated %d recommendations for pattern at %s:%d", 
             filtered_count,
             pattern->hotspot ? pattern->hotspot->location.file : "unknown",
             pattern->hotspot ? pattern->hotspot->location.line : 0);
    
    return 0;
}



// Generate loop tiling recommendation
int generate_loop_tiling_recommendation(const classified_pattern_t *pattern,
                                       const cache_info_t *cache_info,
                                       optimization_rec_t *rec) {
    if (!pattern || !cache_info || !rec) return -1;
    
    rec->type = OPT_LOOP_TILING;
    rec->pattern = (classified_pattern_t*)pattern;
    
    // Calculate tile sizes based on cache
    int l1_tile = sqrt(cache_info->levels[0].size / (3 * sizeof(double)));
    int l2_tile = sqrt(cache_info->levels[1].size / (3 * sizeof(double)));
    
    // Round to nice values
    if (l1_tile > 32) l1_tile = 32;
    if (l2_tile > 128) l2_tile = 128;
    
    rec->expected_improvement = 40 + (pattern->severity_score / 2);
    rec->confidence_score = 0.85;
    rec->implementation_difficulty = 6;
    
    snprintf(rec->code_suggestion, sizeof(rec->code_suggestion),
             "// Original nested loops with poor cache behavior\n"
             "// for (int i = 0; i < N; i++)\n"
             "//   for (int j = 0; j < M; j++)\n"
             "//     C[i][j] = A[i][j] + B[i][j];\n\n"
             "// Tiled version for better cache reuse\n"
             "#define TILE_SIZE %d  // Fits in L1 cache\n\n"
             "for (int ii = 0; ii < N; ii += TILE_SIZE) {\n"
             "    for (int jj = 0; jj < M; jj += TILE_SIZE) {\n"
             "        // Process one tile\n"
             "        for (int i = ii; i < min(ii + TILE_SIZE, N); i++) {\n"
             "            for (int j = jj; j < min(jj + TILE_SIZE, M); j++) {\n"
             "                C[i][j] = A[i][j] + B[i][j];\n"
             "            }\n"
             "        }\n"
             "    }\n"
             "}", l1_tile);
    
    snprintf(rec->implementation_guide, sizeof(rec->implementation_guide),
             "1. Identify loop bounds and array dimensions\n"
             "2. Choose tile size to fit in L1 cache (%d elements)\n"
             "3. Add outer loops with tile-sized steps\n"
             "4. Ensure inner loops handle boundary conditions\n"
             "5. Test with different tile sizes for optimal performance",
             l1_tile);
    
    snprintf(rec->rationale, sizeof(rec->rationale),
             "Loop tiling improves temporal locality by processing data in "
             "cache-sized blocks. Working set of %zu KB exceeds L%d cache (%zu KB). "
             "Tiling reduces cache misses by ~%.0f%%.",
             (pattern->hotspot->address_range_end - 
              pattern->hotspot->address_range_start) / 1024,
             pattern->affected_cache_levels & 1 ? 1 : 2,
             cache_info->levels[0].size / 1024,
             rec->expected_improvement);
    
    rec->priority = 1;  // High priority
    rec->is_automatic = false;
    
    LOG_DEBUG("Generated loop tiling recommendation with tile size %d", l1_tile);
    return 0;
}

// Generate prefetch recommendation
int generate_prefetch_recommendation(const classified_pattern_t *pattern,
                                    optimization_rec_t *rec) {
    if (!pattern || !rec) return -1;
    
    rec->type = OPT_PREFETCH_HINTS;
    rec->pattern = (classified_pattern_t*)pattern;
    
    int prefetch_distance = 8;  // Default prefetch distance
    
    // Adjust based on pattern
    if (pattern->hotspot->dominant_pattern == STRIDED) {
        prefetch_distance = 16;  // Larger distance for strided
    } else if (pattern->hotspot->dominant_pattern == SEQUENTIAL) {
        prefetch_distance = 4;   // Smaller for sequential
    }
    
    rec->expected_improvement = 15 + (pattern->hotspot->miss_rate * 20);
    rec->confidence_score = 0.75;
    rec->implementation_difficulty = 3;
    
    snprintf(rec->code_suggestion, sizeof(rec->code_suggestion),
             "// Add software prefetch hints\n"
             "#include <xmmintrin.h>  // For _mm_prefetch\n\n"
             "for (int i = 0; i < n; i++) {\n"
             "    // Prefetch future data\n"
             "    if (i + %d < n) {\n"
             "        _mm_prefetch(&data[i + %d], _MM_HINT_T0);  // Prefetch to L1\n"
             "    }\n"
             "    \n"
             "    // Process current element\n"
             "    result[i] = process(data[i]);\n"
             "}\n\n"
             "// Alternative: Use compiler builtin\n"
             "for (int i = 0; i < n; i++) {\n"
             "    __builtin_prefetch(&data[i + %d], 0, 3);\n"
             "    result[i] = process(data[i]);\n"
             "}",
             prefetch_distance, prefetch_distance, prefetch_distance);
    
    snprintf(rec->implementation_guide, sizeof(rec->implementation_guide),
             "1. Identify the access pattern and stride\n"
             "2. Calculate prefetch distance (typically 4-16 iterations ahead)\n"
             "3. Insert prefetch intrinsics or builtins\n"
             "4. Use _MM_HINT_T0 for L1, _MM_HINT_T1 for L2\n"
             "5. Profile to find optimal prefetch distance");
    
    snprintf(rec->rationale, sizeof(rec->rationale),
             "Software prefetching can hide memory latency by bringing data "
             "into cache before it's needed. With %.1f%% miss rate and "
             "%s access pattern, prefetching can reduce stalls.",
             pattern->hotspot->miss_rate * 100,
             access_pattern_to_string(pattern->hotspot->dominant_pattern));
    
    rec->priority = 2;  // Medium priority
    rec->is_automatic = false;
    
    LOG_DEBUG("Generated prefetch recommendation with distance %d", prefetch_distance);
    return 0;
}

// Generate data layout recommendation
int generate_data_layout_recommendation(const classified_pattern_t *pattern,
                                       optimization_rec_t *rec) {
    if (!pattern || !rec) return -1;
    
    rec->type = OPT_DATA_LAYOUT_CHANGE;
    rec->pattern = (classified_pattern_t*)pattern;
    
    rec->expected_improvement = 50;  // SoA can be very effective
    rec->confidence_score = 0.80;
    rec->implementation_difficulty = 7;
    
    snprintf(rec->code_suggestion, sizeof(rec->code_suggestion),
             "// Original Array of Structures (AoS)\n"
             "struct Particle {\n"
             "    double x, y, z;\n"
             "    double vx, vy, vz;\n"
             "    double mass;\n"
             "};\n"
             "Particle particles[N];\n\n"
             "// Transformed to Structure of Arrays (SoA)\n"
             "struct ParticleArray {\n"
             "    double *x, *y, *z;\n"
             "    double *vx, *vy, *vz;\n"
             "    double *mass;\n"
             "    size_t count;\n"
             "};\n\n"
             "// Access pattern changes from:\n"
             "// for (i = 0; i < N; i++) \n"
             "//     particles[i].x += particles[i].vx * dt;\n"
             "// To:\n"
             "for (i = 0; i < N; i++)\n"
             "    particle_array.x[i] += particle_array.vx[i] * dt;");
    
    snprintf(rec->implementation_guide, sizeof(rec->implementation_guide),
             "1. Identify fields that are accessed together\n"
             "2. Group hot fields in separate arrays\n"
             "3. Allocate arrays with proper alignment\n"
             "4. Update all access patterns in code\n"
             "5. Consider SIMD opportunities with SoA layout");
    
    snprintf(rec->rationale, sizeof(rec->rationale),
             "Structure of Arrays (SoA) improves cache efficiency for "
             "scattered field access. Current layout wastes %.0f%% of "
             "cache line transfers. SoA enables vectorization.",
             (1.0 - pattern->hotspot->miss_rate) * 100);
    
    rec->priority = 1;  // High priority for gather/scatter patterns
    rec->is_automatic = false;
    
    LOG_DEBUG("Generated data layout transformation recommendation");
    return 0;
}

// Generate alignment recommendation
int generate_alignment_recommendation(const classified_pattern_t *pattern,
                                     optimization_rec_t *rec) {
    if (!pattern || !rec) return -1;
    
    rec->type = OPT_MEMORY_ALIGNMENT;
    rec->pattern = (classified_pattern_t*)pattern;
    
    rec->expected_improvement = 30;
    rec->confidence_score = 0.90;
    rec->implementation_difficulty = 4;
    
    snprintf(rec->code_suggestion, sizeof(rec->code_suggestion),
             "// Align data structures to cache line boundaries\n"
             "#define CACHE_LINE_SIZE 64\n\n"
             "// Method 1: Aligned allocation\n"
             "void* aligned_data;\n"
             "if (posix_memalign(&aligned_data, CACHE_LINE_SIZE, \n"
             "                   sizeof(DataType) * count) != 0) {\n"
             "    // Handle allocation failure\n"
             "}\n\n"
             "// Method 2: Compiler attributes\n"
             "struct alignas(CACHE_LINE_SIZE) AlignedData {\n"
             "    double values[8];  // One cache line\n"
             "};\n\n"
             "// Method 3: Padding to prevent false sharing\n"
             "struct PaddedData {\n"
             "    double value;\n"
             "    char padding[CACHE_LINE_SIZE - sizeof(double)];\n"
             "} __attribute__((packed));");
    
    snprintf(rec->implementation_guide, sizeof(rec->implementation_guide),
             "1. Identify shared data structures\n"
             "2. Add padding or alignment attributes\n"
             "3. Use posix_memalign for dynamic allocation\n"
             "4. Ensure each thread's data is in separate cache lines\n"
             "5. Verify alignment with address checks");
    
    snprintf(rec->rationale, sizeof(rec->rationale),
             "False sharing occurs when multiple threads access different data "
             "in the same cache line. Alignment and padding ensure each thread's "
             "data occupies separate cache lines, eliminating coherence traffic.");
    
    rec->priority = 1;  // High priority for false sharing
    rec->is_automatic = true;  // Can be automated with padding
    
    LOG_DEBUG("Generated alignment recommendation for false sharing");
    return 0;
}

// Comparison function for sorting recommendations
static int compare_recommendations(const void *a, const void *b) {
    const optimization_rec_t *r1 = (const optimization_rec_t *)a;
    const optimization_rec_t *r2 = (const optimization_rec_t *)b;
    
    // First sort by priority (descending)
    if (r1->priority > r2->priority) return -1;
    if (r1->priority < r2->priority) return 1;
    
    // Then by expected improvement (descending)
    if (r1->expected_improvement > r2->expected_improvement) return -1;
    if (r1->expected_improvement < r2->expected_improvement) return 1;
    
    return 0;
}

void rank_recommendations(optimization_rec_t *recommendations, int count) {
    if (!recommendations || count <= 1) return;
    
    // Sort by:
    // 1. Priority (lower is better)
    // 2. Expected improvement (higher is better)
    // 3. Confidence score (higher is better)
    // 4. Implementation difficulty (lower is better)
    
    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            bool swap = false;
            
            // First by priority
            if (recommendations[i].priority > recommendations[j].priority) {
                swap = true;
            } else if (recommendations[i].priority == recommendations[j].priority) {
                // Then by expected improvement
                if (recommendations[i].expected_improvement < recommendations[j].expected_improvement) {
                    swap = true;
                } else if (recommendations[i].expected_improvement == recommendations[j].expected_improvement) {
                    // Then by confidence
                    if (recommendations[i].confidence_score < recommendations[j].confidence_score) {
                        swap = true;
                    } else if (recommendations[i].confidence_score == recommendations[j].confidence_score) {
                        // Finally by difficulty
                        if (recommendations[i].implementation_difficulty > recommendations[j].implementation_difficulty) {
                            swap = true;
                        }
                    }
                }
            }
            
            if (swap) {
                optimization_rec_t temp = recommendations[i];
                recommendations[i] = recommendations[j];
                recommendations[j] = temp;
            }
        }
    }
}

//
int filter_conflicting_recommendations(optimization_rec_t *recommendations, int count) {
    if (!recommendations || count <= 1) return count;
    
    // Mark conflicting recommendations for removal
    bool *to_remove = calloc(count, sizeof(bool));
    
    for (int i = 0; i < count - 1; i++) {
        if (to_remove[i]) continue;
        
        for (int j = i + 1; j < count; j++) {
            if (to_remove[j]) continue;
            
            // Check if they target the same location
            if (recommendations[i].pattern && recommendations[j].pattern &&
                recommendations[i].pattern->hotspot && recommendations[j].pattern->hotspot) {
                
                cache_hotspot_t *h1 = recommendations[i].pattern->hotspot;
                cache_hotspot_t *h2 = recommendations[j].pattern->hotspot;
                
                if (h1->location.line == h2->location.line &&
                    strcmp(h1->location.file, h2->location.file) == 0) {
                    
                    // Same location - check for conflicts
                    if ((recommendations[i].type == OPT_LOOP_VECTORIZE && 
                         recommendations[j].type == OPT_DATA_LAYOUT_CHANGE) ||
                        (recommendations[j].type == OPT_LOOP_VECTORIZE && 
                         recommendations[i].type == OPT_DATA_LAYOUT_CHANGE)) {
                        
                        // Keep the one with higher expected improvement
                        if (recommendations[i].expected_improvement < 
                            recommendations[j].expected_improvement) {
                            to_remove[i] = true;
                        } else {
                            to_remove[j] = true;
                        }
                    }
                }
            }
        }
    }
    
    // Compact the array
    int new_count = 0;
    for (int i = 0; i < count; i++) {
        if (!to_remove[i]) {
            if (i != new_count) {
                recommendations[new_count] = recommendations[i];
            }
            new_count++;
        }
    }
    
    free(to_remove);
    return new_count;
}

// Print recommendations
void recommendation_engine_print_recommendations(const optimization_rec_t *recs, int count) {
    printf("\n=== Optimization Recommendations ===\n");
    printf("Found %d optimization opportunities:\n\n", count);
    
    for (int i = 0; i < count; i++) {
        const optimization_rec_t *rec = &recs[i];
        
        printf("[%d] %s (Priority: %d)\n", i + 1,
               optimization_type_to_string(rec->type), rec->priority);
        
        printf("    Expected improvement: %.1f%%\n", rec->expected_improvement);
        printf("    Confidence: %.0f%%\n", rec->confidence_score * 100);
        printf("    Difficulty: %d/10\n", rec->implementation_difficulty);
        
        if (rec->pattern && rec->pattern->hotspot) {
            printf("    Location: %s:%d\n",
                   rec->pattern->hotspot->location.file,
                   rec->pattern->hotspot->location.line);
        }
        
        printf("\n    Rationale: %s\n", rec->rationale);
        
        if (strlen(rec->compiler_flags) > 0) {
            printf("\n    Compiler flags: %s\n", rec->compiler_flags);
        }
        
        if (strlen(rec->implementation_guide) > 0) {
            printf("\n    Implementation guide:\n%s\n", rec->implementation_guide);
        }
        
        if (strlen(rec->code_suggestion) > 0) {
            printf("\n    Code example:\n%s\n", rec->code_suggestion);
        }
        
        printf("\n" "─" "─" "─" "─" "─" "─" "─" "─" "─" "─" "\n\n");
    }
}

// Get default configuration
engine_config_t engine_config_default(void) {
    engine_config_t config = {
        .generate_code_examples = true,
        .consider_compiler_flags = true,
        .prefer_automatic = false,
        .max_recommendations = 5,
        .min_expected_improvement = 10.0
    };
    
    return config;
}
