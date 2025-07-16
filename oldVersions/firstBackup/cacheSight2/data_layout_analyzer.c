#include "data_layout_analyzer.h"
#include <string.h>

static cache_info_t g_cache_info;
static bool g_initialized = false;

int data_layout_analyzer_init(const cache_info_t *cache_info) {
    if (g_initialized) {
        LOG_WARNING("Data layout analyzer already initialized");
        return 0;
    }
    
    if (!cache_info) {
        LOG_ERROR("NULL cache info provided");
        return -1;
    }
    
    g_cache_info = *cache_info;
    g_initialized = true;
    
    LOG_INFO("Data layout analyzer initialized");
    return 0;
}

void data_layout_analyzer_cleanup(void) {
    if (!g_initialized) {
        return;
    }
    
    LOG_INFO("Data layout analyzer cleanup");
    g_initialized = false;
}

int analyze_struct_layout(const struct_info_t *struct_info,
                         const static_pattern_t *accesses, int access_count,
                         struct_layout_analysis_t *analysis) {
    if (!struct_info || !accesses || !analysis || access_count <= 0) {
        LOG_ERROR("Invalid parameters for analyze_struct_layout");
        return -1;
    }
    
    LOG_INFO("Analyzing layout for struct %s with %d accesses",
             struct_info->struct_name, access_count);
    
    memset(analysis, 0, sizeof(struct_layout_analysis_t));
    
    // Copy struct info
    analysis->struct_info = MALLOC_LOGGED(sizeof(struct_info_t));
    if (!analysis->struct_info) {
        LOG_ERROR("Failed to allocate struct info");
        return -1;
    }
    *analysis->struct_info = *struct_info;
    
    // Allocate field statistics
    analysis->field_stats = CALLOC_LOGGED(struct_info->field_count, sizeof(field_stats_t));
    if (!analysis->field_stats) {
        LOG_ERROR("Failed to allocate field stats");
        FREE_LOGGED(analysis->struct_info);
        return -1;
    }
    analysis->field_count = struct_info->field_count;
    
    // Initialize field stats
    for (int i = 0; i < struct_info->field_count; i++) {
        strncpy(analysis->field_stats[i].field_name, struct_info->field_names[i],
                sizeof(analysis->field_stats[i].field_name) - 1);
        analysis->field_stats[i].field_offset = struct_info->field_offsets[i];
        analysis->field_stats[i].field_size = struct_info->field_sizes[i];
    }
    
    // Count field accesses
    int total_struct_accesses = 0;
    for (int i = 0; i < access_count; i++) {
        if (!accesses[i].is_struct_access || 
            strcmp(accesses[i].struct_name, struct_info->struct_name) != 0) {
            continue;
        }
        
        total_struct_accesses++;
        
        // Find which field is accessed
        for (int j = 0; j < struct_info->field_count; j++) {
            if (strcmp(accesses[i].variable_name, struct_info->field_names[j]) == 0) {
                analysis->field_stats[j].access_count++;
                break;
            }
        }
    }
    
    // Calculate access frequencies and identify hot/cold fields
    int hot_threshold = total_struct_accesses * 0.2;  // 20% of accesses
    int cold_threshold = total_struct_accesses * 0.05; // 5% of accesses
    
    for (int i = 0; i < analysis->field_count; i++) {
        if (total_struct_accesses > 0) {
            analysis->field_stats[i].access_frequency = 
                (double)analysis->field_stats[i].access_count / total_struct_accesses * 100;
        }
        
        analysis->field_stats[i].is_hot = 
            analysis->field_stats[i].access_count >= hot_threshold;
        analysis->field_stats[i].is_cold = 
            analysis->field_stats[i].access_count <= cold_threshold;
        
        LOG_DEBUG("Field %s: %d accesses (%.1f%%), %s",
                  analysis->field_stats[i].field_name,
                  analysis->field_stats[i].access_count,
                  analysis->field_stats[i].access_frequency,
                  analysis->field_stats[i].is_hot ? "HOT" : 
                  (analysis->field_stats[i].is_cold ? "COLD" : "WARM"));
    }
    
    // Determine current layout type
    analysis->current_layout = struct_info->is_packed ? LAYOUT_PACKED : LAYOUT_AOS;
    
    // Calculate padding
    analysis->padding_bytes = calculate_structure_padding(struct_info);
    LOG_DEBUG("Structure has %zu bytes of padding", analysis->padding_bytes);
    
    // Check for false sharing
    analysis->has_false_sharing = 
        detect_false_sharing_risk(struct_info, accesses, access_count) > 0;
    
    // Calculate current cache efficiency
    int hot_field_count = 0;
    size_t hot_field_size = 0;
    
    for (int i = 0; i < analysis->field_count; i++) {
        if (analysis->field_stats[i].is_hot) {
            hot_field_count++;
            hot_field_size += analysis->field_stats[i].field_size;
        }
    }
    
    if (struct_info->total_size > 0) {
        analysis->cache_efficiency = 
            (double)hot_field_size / struct_info->total_size * 100;
    }
    
    // Recommend layout transformation
    if (hot_field_count > 0 && hot_field_count < analysis->field_count / 2) {
        // Few hot fields - good candidate for SoA
        analysis->recommended_layout = LAYOUT_SOA;
        analysis->predicted_efficiency = 95.0;  // Near perfect for hot fields
        
        LOG_INFO("Recommending SoA transformation - only %d/%d fields are hot",
                 hot_field_count, analysis->field_count);
    } else if (analysis->padding_bytes > struct_info->total_size * 0.2) {
        // Significant padding - recommend packing or alignment
        analysis->recommended_layout = LAYOUT_PACKED;
        analysis->predicted_efficiency = 
            (double)(struct_info->total_size - analysis->padding_bytes) / 
            struct_info->total_size * 100;
        
        LOG_INFO("Recommending structure packing - %zu bytes of padding",
                 analysis->padding_bytes);
    } else if (analysis->has_false_sharing) {
        // False sharing detected - recommend aligned layout
        analysis->recommended_layout = LAYOUT_ALIGNED;
        analysis->predicted_efficiency = analysis->cache_efficiency * 1.5;
        if (analysis->predicted_efficiency > 95) {
            analysis->predicted_efficiency = 95;
        }
        
        LOG_INFO("Recommending cache-aligned layout due to false sharing risk");
    } else {
        // Current layout is reasonable
        analysis->recommended_layout = analysis->current_layout;
        analysis->predicted_efficiency = analysis->cache_efficiency;
        
        LOG_INFO("Current layout is reasonable - no transformation needed");
    }
    
    // Generate transformation code if needed
    if (analysis->recommended_layout != analysis->current_layout) {
        suggest_struct_transformation(analysis, analysis->transformation_code,
                                     sizeof(analysis->transformation_code));
    }
    
    LOG_INFO("Layout analysis complete: current efficiency=%.1f%%, predicted=%.1f%%",
             analysis->cache_efficiency, analysis->predicted_efficiency);
    
    return 0;
}

int analyze_array_layout(const static_pattern_t *accesses, int access_count,
                        array_analysis_t *analysis) {
    if (!accesses || !analysis || access_count <= 0) {
        LOG_ERROR("Invalid parameters for analyze_array_layout");
        return -1;
    }
    
    memset(analysis, 0, sizeof(array_analysis_t));
    
    // Find the dominant array being accessed
    // Simple approach: take the first array found
    for (int i = 0; i < access_count; i++) {
        if (!accesses[i].is_struct_access && strlen(accesses[i].array_name) > 0) {
            strncpy(analysis->array_name, accesses[i].array_name,
                    sizeof(analysis->array_name) - 1);
            break;
        }
    }
    
    if (strlen(analysis->array_name) == 0) {
        LOG_WARNING("No array accesses found");
        return -1;
    }
    
    LOG_INFO("Analyzing array layout for %s", analysis->array_name);
    
    // Analyze access patterns for this array
    int pattern_counts[7] = {0};  // One for each access_pattern_t
    int total_stride = 0;
    int stride_count = 0;
    
    for (int i = 0; i < access_count; i++) {
        if (strcmp(accesses[i].array_name, analysis->array_name) != 0) {
            continue;
        }
        
        pattern_counts[accesses[i].pattern]++;
        
        if (accesses[i].pattern == STRIDED) {
            total_stride += accesses[i].stride;
            stride_count++;
        }
    }
    
    // Find dominant pattern
    int max_count = 0;
    for (int i = 0; i < 7; i++) {
        if (pattern_counts[i] > max_count) {
            max_count = pattern_counts[i];
            analysis->dominant_pattern = (access_pattern_t)i;
        }
    }
    
    // Calculate average stride
    if (stride_count > 0) {
        analysis->stride = total_stride / stride_count;
    }
    
    // Calculate locality scores
    analysis->spatial_locality_score = 0;
    analysis->temporal_locality_score = 0;
    
    if (analysis->dominant_pattern == SEQUENTIAL) {
        analysis->spatial_locality_score = 100;
        analysis->temporal_locality_score = 80;
    } else if (analysis->dominant_pattern == STRIDED) {
        analysis->spatial_locality_score = 100 / (analysis->stride > 0 ? analysis->stride : 1);
        analysis->temporal_locality_score = 60;
    } else if (analysis->dominant_pattern == RANDOM) {
        analysis->spatial_locality_score = 10;
        analysis->temporal_locality_score = 20;
    }
    
    // Check if column-major would be beneficial (for 2D array patterns)
    // Look for patterns like a[i][j] where j varies in inner loop
    analysis->is_column_major_beneficial = false;
    if (analysis->dominant_pattern == STRIDED && analysis->stride > 8) {
        analysis->is_column_major_beneficial = true;
    }
    
    // Generate optimization suggestion
    switch (analysis->dominant_pattern) {
        case SEQUENTIAL:
            snprintf(analysis->optimization_suggestion,
                    sizeof(analysis->optimization_suggestion),
                    "Sequential access pattern is cache-friendly. "
                    "Consider vectorization and prefetching.");
            break;
            
        case STRIDED:
            if (analysis->stride > 8) {
                snprintf(analysis->optimization_suggestion,
                        sizeof(analysis->optimization_suggestion),
                        "Large stride (%d) detected. Consider loop tiling, "
                        "data transposition, or packing data elements.",
                        analysis->stride);
            } else {
                snprintf(analysis->optimization_suggestion,
                        sizeof(analysis->optimization_suggestion),
                        "Moderate stride (%d) detected. Consider unrolling "
                        "and software pipelining.", analysis->stride);
            }
            break;
            
        case RANDOM:
            snprintf(analysis->optimization_suggestion,
                    sizeof(analysis->optimization_suggestion),
                    "Random access pattern detected. Consider sorting data, "
                    "using hash tables, or implementing a cache.");
            break;
            
        case INDIRECT_ACCESS:
            snprintf(analysis->optimization_suggestion,
                    sizeof(analysis->optimization_suggestion),
                    "Indirect access pattern. Consider sorting index arrays "
                    "or using gather/scatter instructions.");
            break;
            
        default:
            snprintf(analysis->optimization_suggestion,
                    sizeof(analysis->optimization_suggestion),
                    "Complex access pattern. Profile further to identify "
                    "optimization opportunities.");
    }
    
    LOG_INFO("Array analysis complete: pattern=%s, spatial=%.0f%%, temporal=%.0f%%",
             access_pattern_to_string(analysis->dominant_pattern),
             analysis->spatial_locality_score,
             analysis->temporal_locality_score);
    
    return 0;
}

int suggest_struct_transformation(const struct_layout_analysis_t *analysis,
                                 char *transformation_code, size_t code_size) {
    if (!analysis || !transformation_code || code_size == 0) {
        LOG_ERROR("Invalid parameters for suggest_struct_transformation");
        return -1;
    }
    
    transformation_code[0] = '\0';
    
    if (analysis->recommended_layout == LAYOUT_SOA) {
        // Generate SoA transformation
        char soa_def[1024];
        generate_soa_definition(analysis->struct_info, soa_def, sizeof(soa_def));
        
        snprintf(transformation_code, code_size,
                "// Structure of Arrays transformation for %s\n"
                "// Original AoS definition:\n"
                "// struct %s { ... };\n\n"
                "// Recommended SoA definition:\n"
                "%s\n"
                "// Access hot fields directly: soa.%s[i]\n"
                "// This improves cache efficiency from %.1f%% to %.1f%%\n",
                analysis->struct_info->struct_name,
                analysis->struct_info->struct_name,
                soa_def,
                analysis->field_stats[0].field_name,  // Assume first is hot
                analysis->cache_efficiency,
                analysis->predicted_efficiency);
                
    } else if (analysis->recommended_layout == LAYOUT_PACKED) {
        snprintf(transformation_code, code_size,
                "// Packed structure to eliminate padding\n"
                "#pragma pack(push, 1)\n"
                "struct %s_packed {\n",
                analysis->struct_info->struct_name);
                
        // Add fields sorted by size (largest first)
        // This is simplified - real implementation would sort properly
        for (int i = 0; i < analysis->field_count; i++) {
            char field_line[256];
            snprintf(field_line, sizeof(field_line),
                    "    type %s;  // size: %zu, offset: %zu\n",
                    analysis->field_stats[i].field_name,
                    analysis->field_stats[i].field_size,
                    analysis->field_stats[i].field_offset);
            strncat(transformation_code, field_line, 
                    code_size - strlen(transformation_code) - 1);
        }
        
        strncat(transformation_code, "};\n#pragma pack(pop)\n",
                code_size - strlen(transformation_code) - 1);
                
    } else if (analysis->recommended_layout == LAYOUT_ALIGNED) {
        snprintf(transformation_code, code_size,
                "// Cache-aligned structure to prevent false sharing\n"
                "struct alignas(%d) %s_aligned {\n",
                (int)g_cache_info.levels[0].line_size,
                analysis->struct_info->struct_name);
                
        // Group hot fields together
        strncat(transformation_code, "    // Hot fields grouped together:\n",
                code_size - strlen(transformation_code) - 1);
                
        for (int i = 0; i < analysis->field_count; i++) {
            if (analysis->field_stats[i].is_hot) {
                char field_line[256];
                snprintf(field_line, sizeof(field_line),
                        "    type %s;  // HOT - %d accesses\n",
                        analysis->field_stats[i].field_name,
                        analysis->field_stats[i].access_count);
                strncat(transformation_code, field_line,
                        code_size - strlen(transformation_code) - 1);
            }
        }
        
        strncat(transformation_code, "\n    // Cold fields:\n",
                code_size - strlen(transformation_code) - 1);
                
        for (int i = 0; i < analysis->field_count; i++) {
            if (!analysis->field_stats[i].is_hot) {
                char field_line[256];
                snprintf(field_line, sizeof(field_line),
                        "    type %s;\n",
                        analysis->field_stats[i].field_name);
                strncat(transformation_code, field_line,
                        code_size - strlen(transformation_code) - 1);
            }
        }
        
        strncat(transformation_code, "};\n",
                code_size - strlen(transformation_code) - 1);
    }
    
    LOG_DEBUG("Generated transformation code for %s layout",
              analysis->recommended_layout == LAYOUT_SOA ? "SoA" :
              analysis->recommended_layout == LAYOUT_PACKED ? "packed" :
              analysis->recommended_layout == LAYOUT_ALIGNED ? "aligned" : "unknown");
    
    return 0;
}

void free_layout_analysis(struct_layout_analysis_t *analysis) {
    if (!analysis) return;
    
    LOG_DEBUG("Freeing layout analysis structures");
    
    if (analysis->struct_info) {
        FREE_LOGGED(analysis->struct_info);
        analysis->struct_info = NULL;
    }
    
    if (analysis->field_stats) {
        FREE_LOGGED(analysis->field_stats);
        analysis->field_stats = NULL;
    }
}

bool should_transform_aos_to_soa(const struct_layout_analysis_t *analysis) {
    if (!analysis) return false;
    
    // Count hot fields
    int hot_count = 0;
    for (int i = 0; i < analysis->field_count; i++) {
        if (analysis->field_stats[i].is_hot) {
            hot_count++;
        }
    }
    
    // Transform if:
    // 1. Less than half the fields are hot
    // 2. Current efficiency is below 50%
    // 3. Predicted improvement is significant (>20%)
    
    bool few_hot_fields = hot_count < analysis->field_count / 2;
    bool low_efficiency = analysis->cache_efficiency < 50;
    bool significant_improvement = 
        (analysis->predicted_efficiency - analysis->cache_efficiency) > 20;
    
    bool should_transform = few_hot_fields && (low_efficiency || significant_improvement);
    
    LOG_INFO("AoS to SoA transformation %s: hot_fields=%d/%d, efficiency=%.1f%%, improvement=%.1f%%",
             should_transform ? "recommended" : "not recommended",
             hot_count, analysis->field_count,
             analysis->cache_efficiency,
             analysis->predicted_efficiency - analysis->cache_efficiency);
    
    return should_transform;
}

int calculate_structure_padding(const struct_info_t *struct_info) {
    if (!struct_info) return 0;
    
    size_t expected_size = 0;
    size_t actual_size = struct_info->total_size;
    
    // Calculate expected size without padding
    for (int i = 0; i < struct_info->field_count; i++) {
        expected_size += struct_info->field_sizes[i];
    }
    
    size_t padding = actual_size - expected_size;
    
    LOG_DEBUG("Structure %s: expected size=%zu, actual size=%zu, padding=%zu",
              struct_info->struct_name, expected_size, actual_size, padding);
    
    return padding;
}

int detect_false_sharing_risk(const struct_info_t *struct_info, 
                             const static_pattern_t *accesses, int access_count) {
    if (!struct_info || !accesses || access_count <= 0) return 0;
    
    int cache_line_size = g_cache_info.levels[0].line_size;
    int risk_count = 0;
    
    // Check if different fields that might be accessed by different threads
    // fall within the same cache line
    for (int i = 0; i < struct_info->field_count - 1; i++) {
        size_t field1_start = struct_info->field_offsets[i];
        size_t field1_end = field1_start + struct_info->field_sizes[i];
        
        for (int j = i + 1; j < struct_info->field_count; j++) {
            size_t field2_start = struct_info->field_offsets[j];
            
            // Check if fields are in same cache line
            if (field1_start / cache_line_size == field2_start / cache_line_size ||
                field1_end / cache_line_size == field2_start / cache_line_size) {
                
                // Check if both fields are accessed
                bool field1_accessed = false;
                bool field2_accessed = false;
                
                for (int k = 0; k < access_count; k++) {
                    if (strcmp(accesses[k].variable_name, struct_info->field_names[i]) == 0) {
                        field1_accessed = true;
                    }
                    if (strcmp(accesses[k].variable_name, struct_info->field_names[j]) == 0) {
                        field2_accessed = true;
                    }
                }
                
                if (field1_accessed && field2_accessed) {
                    risk_count++;
                    LOG_DEBUG("False sharing risk: fields %s and %s in same cache line",
                              struct_info->field_names[i], struct_info->field_names[j]);
                }
            }
        }
    }
    
    LOG_INFO("Detected %d potential false sharing risks in struct %s",
             risk_count, struct_info->struct_name);
    
    return risk_count;
}

void print_layout_analysis(const struct_layout_analysis_t *analysis) {
    if (!analysis) return;
    
    printf("\n=== Structure Layout Analysis: %s ===\n",
           analysis->struct_info->struct_name);
    printf("Current layout: %s\n",
           analysis->current_layout == LAYOUT_AOS ? "Array of Structures (AoS)" :
           analysis->current_layout == LAYOUT_PACKED ? "Packed" : "Unknown");
    printf("Cache efficiency: %.1f%%\n", analysis->cache_efficiency);
    printf("Padding bytes: %zu\n", analysis->padding_bytes);
    printf("False sharing risk: %s\n", analysis->has_false_sharing ? "Yes" : "No");
    
    printf("\nField Access Statistics:\n");
    for (int i = 0; i < analysis->field_count; i++) {
        printf("  %-20s: %4d accesses (%5.1f%%) %s\n",
               analysis->field_stats[i].field_name,
               analysis->field_stats[i].access_count,
               analysis->field_stats[i].access_frequency,
               analysis->field_stats[i].is_hot ? "[HOT]" :
               analysis->field_stats[i].is_cold ? "[COLD]" : "");
    }
    
    printf("\nRecommended layout: %s\n",
           analysis->recommended_layout == LAYOUT_SOA ? "Structure of Arrays (SoA)" :
           analysis->recommended_layout == LAYOUT_PACKED ? "Packed" :
           analysis->recommended_layout == LAYOUT_ALIGNED ? "Cache-aligned" :
           "No change");
    printf("Predicted efficiency: %.1f%% (%.1f%% improvement)\n",
           analysis->predicted_efficiency,
           analysis->predicted_efficiency - analysis->cache_efficiency);
    
    if (strlen(analysis->transformation_code) > 0) {
        printf("\nSuggested transformation:\n%s\n", analysis->transformation_code);
    }
}

int generate_soa_definition(const struct_info_t *struct_info, char *code, size_t code_size) {
    if (!struct_info || !code || code_size == 0) return -1;
    
    snprintf(code, code_size,
            "struct %s_SoA {\n"
            "    size_t count;\n",
            struct_info->struct_name);
    
    // Add array for each field
    for (int i = 0; i < struct_info->field_count; i++) {
        char field_line[256];
        snprintf(field_line, sizeof(field_line),
                "    type *%s;  // Array of %s values\n",
                struct_info->field_names[i],
                struct_info->field_names[i]);
        strncat(code, field_line, code_size - strlen(code) - 1);
    }
    
    strncat(code, "};\n", code_size - strlen(code) - 1);
    
    return 0;
}

int generate_aos_to_soa_conversion(const struct_info_t *struct_info, 
                                  const char *aos_var, const char *soa_var,
                                  int array_size, char *code, size_t code_size) {
    if (!struct_info || !aos_var || !soa_var || !code || code_size == 0) return -1;
    
    snprintf(code, code_size,
            "// Convert AoS to SoA\n"
            "void convert_%s_aos_to_soa(struct %s *%s, struct %s_SoA *%s, size_t count) {\n"
            "    %s->count = count;\n",
            struct_info->struct_name,
            struct_info->struct_name, aos_var,
            struct_info->struct_name, soa_var,
            soa_var);
    
    // Allocate arrays
    for (int i = 0; i < struct_info->field_count; i++) {
        char alloc_line[256];
        snprintf(alloc_line, sizeof(alloc_line),
                "    %s->%s = malloc(count * sizeof(type));\n",
                soa_var, struct_info->field_names[i]);
        strncat(code, alloc_line, code_size - strlen(code) - 1);
    }
    
    // Copy data
    strncat(code, "    \n    // Copy data from AoS to SoA\n"
                  "    for (size_t i = 0; i < count; i++) {\n",
            code_size - strlen(code) - 1);
    
    for (int i = 0; i < struct_info->field_count; i++) {
        char copy_line[256];
        snprintf(copy_line, sizeof(copy_line),
                "        %s->%s[i] = %s[i].%s;\n",
                soa_var, struct_info->field_names[i],
                aos_var, struct_info->field_names[i]);
        strncat(code, copy_line, code_size - strlen(code) - 1);
    }
    
    strncat(code, "    }\n}\n", code_size - strlen(code) - 1);
    
    return 0;
}
