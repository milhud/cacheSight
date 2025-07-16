#include "bank_conflict_analyzer.h"
#include <math.h>

static bank_config_t g_config;
static bool g_initialized = false;

// Initialize analyzer
int bank_conflict_analyzer_init(const bank_config_t *config) {
    if (g_initialized) {
        LOG_WARNING("Bank conflict analyzer already initialized");
        return 0;
    }
    
    if (config) {
        g_config = *config;
    } else {
        // Default to CPU configuration
        g_config = bank_config_default_cpu();
    }
    
    LOG_INFO("Initialized bank conflict analyzer: %d banks, %d-byte width",
             g_config.num_memory_banks, g_config.bank_width_bytes);
    
    g_initialized = true;
    return 0;
}

// Cleanup analyzer
void bank_conflict_analyzer_cleanup(void) {
    if (!g_initialized) {
        return;
    }
    
    LOG_INFO("Cleaning up bank conflict analyzer");
    g_initialized = false;
}

// Calculate memory bank for address
int calculate_memory_bank(uint64_t address, const bank_config_t *config) {
    if (!config || config->num_memory_banks <= 0) return 0;
    
    // Simple interleaved banking scheme
    uint64_t bank_offset = address / config->bank_interleave_bytes;
    return bank_offset % config->num_memory_banks;
}

// Calculate cache bank (for banked caches)
int calculate_cache_bank(uint64_t address, int cache_level, 
                        const cache_info_t *cache_info) {
    if (!cache_info || cache_level < 0 || cache_level >= cache_info->num_levels) {
        return 0;
    }
    
    // Many modern CPUs have banked L1 caches
    if (cache_level == 0 && g_config.l1_banks > 0) {
        // Simple hash based on address bits
        return (address / 64) % g_config.l1_banks;  // 64-byte cache lines
    }
    
    return 0;
}

// Analyze bank conflicts
int analyze_bank_conflicts(const cache_miss_sample_t *samples, int sample_count,
                          bank_conflict_t **conflicts, int *conflict_count) {
    if (!samples || sample_count <= 0 || !conflicts || !conflict_count) {
        LOG_ERROR("Invalid parameters for analyze_bank_conflicts");
        return -1;
    }
    
    if (!g_initialized) {
        LOG_ERROR("Bank conflict analyzer not initialized");
        return -1;
    }
    
    if (!g_config.has_bank_conflicts) {
        LOG_INFO("Architecture does not have bank conflicts");
        *conflicts = NULL;
        *conflict_count = 0;
        return 0;
    }
    
    LOG_INFO("Analyzing bank conflicts in %d samples", sample_count);
    
    // Bank access tracking
    typedef struct {
        uint64_t *access_times;
        int access_count;
        int access_capacity;
        int thread_mask;
    } bank_access_info_t;
    
    bank_access_info_t *bank_info = CALLOC_LOGGED(g_config.num_memory_banks,
                                                  sizeof(bank_access_info_t));
    if (!bank_info) {
        LOG_ERROR("Failed to allocate bank info");
        return -1;
    }
    
    // Track accesses per bank
    for (int i = 0; i < sample_count; i++) {
        int bank = calculate_memory_bank(samples[i].memory_addr, &g_config);
        
        if (bank >= 0 && bank < g_config.num_memory_banks) {
            bank_access_info_t *info = &bank_info[bank];
            
            // Grow array if needed
            if (info->access_count >= info->access_capacity) {
                info->access_capacity = info->access_capacity ? info->access_capacity * 2 : 16;
                uint64_t *new_times = realloc(info->access_times,
                                            info->access_capacity * sizeof(uint64_t));
                if (new_times) {
                    info->access_times = new_times;
                } else {
                    continue;
                }
            }
            
            if (info->access_count < info->access_capacity) {
                info->access_times[info->access_count++] = samples[i].timestamp;
                info->thread_mask |= (1 << (samples[i].tid % 32));
            }
        }
    }
    
    // Detect conflicts
    bank_conflict_t *conflict_list = NULL;
    int conflict_capacity = 8;
    *conflict_count = 0;
    
    conflict_list = CALLOC_LOGGED(conflict_capacity, sizeof(bank_conflict_t));
    if (!conflict_list) {
        // Cleanup
        for (int i = 0; i < g_config.num_memory_banks; i++) {
            if (bank_info[i].access_times) {
                FREE_LOGGED(bank_info[i].access_times);
            }
        }
        FREE_LOGGED(bank_info);
        return -1;
    }
    
    // Look for banks with high contention
    for (int bank = 0; bank < g_config.num_memory_banks; bank++) {
        bank_access_info_t *info = &bank_info[bank];
        
        if (info->access_count < 10) continue;  // Skip lightly used banks
        
        // Check for temporal clustering (many accesses in short time)
        bool has_conflict = false;
        int conflict_window_ns = 1000;  // 1 microsecond window
        
        for (int i = 1; i < info->access_count; i++) {
            if (info->access_times[i] - info->access_times[i-1] < conflict_window_ns) {
                has_conflict = true;
                break;
            }
        }
        
        // Check for multiple threads
        int thread_count = __builtin_popcount(info->thread_mask);
        if (thread_count > 1) {
            has_conflict = true;
        }
        
        if (has_conflict) {
            if (*conflict_count >= conflict_capacity) {
                conflict_capacity *= 2;
                bank_conflict_t *new_list = realloc(conflict_list,
                    conflict_capacity * sizeof(bank_conflict_t));
                if (new_list) {
                    conflict_list = new_list;
                } else {
                    break;
                }
            }
            
            bank_conflict_t *conflict = &conflict_list[(*conflict_count)++];
            memset(conflict, 0, sizeof(bank_conflict_t));
            
            // Fill conflict info
            conflict->num_banks = 1;  // Can be extended to multi-bank conflicts
            conflict->banks = CALLOC_LOGGED(1, sizeof(bank_info_t));
            
            if (conflict->banks) {
                conflict->banks[0].bank_id = bank;
                conflict->banks[0].access_count = info->access_count;
                conflict->banks[0].num_conflicting_threads = thread_count;
                
                // Calculate access rate
                if (info->access_count > 1) {
                    uint64_t time_span = info->access_times[info->access_count-1] - 
                                        info->access_times[0];
                    if (time_span > 0) {
                        conflict->banks[0].access_rate = 
                            (double)info->access_count * 1e9 / time_span;
                    }
                }
                
                conflict->total_conflicts = info->access_count;
                conflict->conflict_severity = calculate_bank_conflict_severity(conflict);
                
                snprintf(conflict->pattern_description, 
                        sizeof(conflict->pattern_description),
                        "Bank %d conflict: %d accesses from %d threads",
                        bank, info->access_count, thread_count);
            }
        }
    }
    
    // Cleanup
    for (int i = 0; i < g_config.num_memory_banks; i++) {
        if (bank_info[i].access_times) {
            FREE_LOGGED(bank_info[i].access_times);
        }
    }
    FREE_LOGGED(bank_info);
    
    *conflicts = conflict_list;
    
    LOG_INFO("Found %d bank conflicts", *conflict_count);
    return 0;
}

// Calculate conflict severity
double calculate_bank_conflict_severity(const bank_conflict_t *conflict) {
    if (!conflict || conflict->num_banks <= 0) return 0;
    
    double severity = 0;
    
    // Factor 1: Access rate (higher rate = worse)
    double max_access_rate = 0;
    for (int i = 0; i < conflict->num_banks; i++) {
        if (conflict->banks[i].access_rate > max_access_rate) {
            max_access_rate = conflict->banks[i].access_rate;
        }
    }
    
    // Normalize to 0-40 range
    if (max_access_rate > 1e6) {  // More than 1M accesses/sec
        severity += 40;
    } else {
        severity += (max_access_rate / 1e6) * 40;
    }
    
    // Factor 2: Thread contention (more threads = worse)
    int max_threads = 0;
    for (int i = 0; i < conflict->num_banks; i++) {
        if (conflict->banks[i].num_conflicting_threads > max_threads) {
            max_threads = conflict->banks[i].num_conflicting_threads;
        }
    }
    
    severity += min(max_threads * 10, 40);
    
    // Factor 3: Total conflicts
    if (conflict->total_conflicts > 10000) {
        severity += 20;
    } else {
        severity += (conflict->total_conflicts / 10000.0) * 20;
    }
    
    // Cap at 100
    if (severity > 100) severity = 100;
    
    LOG_DEBUG("Bank conflict severity: %.1f", severity);
    return severity;
}

// Detect strided bank conflict pattern
bool detect_strided_bank_conflict(const uint64_t *addresses, int count,
                                 int *stride, int *conflicting_banks) {
    if (!addresses || count < 2 || !stride || !conflicting_banks) return false;
    
    // Calculate strides
    int common_stride = 0;
    int stride_count = 0;
    
    for (int i = 1; i < count && i < 100; i++) {
        int current_stride = addresses[i] - addresses[i-1];
        if (current_stride > 0) {
            if (common_stride == 0) {
                common_stride = current_stride;
                stride_count = 1;
            } else if (current_stride == common_stride) {
                stride_count++;
            }
        }
    }
    
    if (stride_count < count / 2) {
        return false;  // No dominant stride
    }
    
    *stride = common_stride;
    
    // Check if stride causes bank conflicts
    int banks_hit = 0;
    bool bank_used[32] = {false};  // Track up to 32 banks
    
    for (int i = 0; i < count && i < 100; i++) {
        int bank = calculate_memory_bank(addresses[i], &g_config);
        if (bank >= 0 && bank < 32 && !bank_used[bank]) {
            bank_used[bank] = true;
            banks_hit++;
        }
    }
    
    *conflicting_banks = banks_hit;
    
    // Conflict if we're hitting fewer banks than we should
    if (banks_hit < g_config.num_memory_banks && count > g_config.num_memory_banks) {
        LOG_DEBUG("Strided bank conflict detected: stride=%d, banks=%d/%d",
                  common_stride, banks_hit, g_config.num_memory_banks);
        return true;
    }
    
    return false;
}

// Detect power-of-two conflict pattern
bool detect_power_of_two_conflict(const uint64_t *addresses, int count) {
    if (!addresses || count < 2) return false;
    
    // Common problem: arrays with power-of-two sizes causing conflicts
    // Check if addresses are separated by power-of-two distances
    
    int power_of_two_strides = 0;
    int total_strides = 0;
    
    for (int i = 1; i < count && i < 100; i++) {
        uint64_t stride = addresses[i] - addresses[i-1];
        if (stride > 0) {
            total_strides++;
            
            // Check if stride is power of two
            if ((stride & (stride - 1)) == 0) {
                power_of_two_strides++;
            }
        }
    }
    
    if (total_strides > 0 && power_of_two_strides > total_strides * 0.8) {
        LOG_DEBUG("Power-of-two bank conflict pattern detected");
        return true;
    }
    
    return false;
}

// Suggest mitigations
int suggest_bank_conflict_mitigation(const bank_conflict_t *conflict,
                                    bank_conflict_mitigation_t **mitigations,
                                    int *mitigation_count) {
    if (!conflict || !mitigations || !mitigation_count) return -1;
    
    *mitigations = CALLOC_LOGGED(4, sizeof(bank_conflict_mitigation_t));
    if (!*mitigations) return -1;
    
    *mitigation_count = 0;
    
    // Mitigation 1: Padding
    bank_conflict_mitigation_t *mit = &(*mitigations)[(*mitigation_count)++];
    
    strncpy(mit->technique, "Array Padding", sizeof(mit->technique) - 1);
    mit->expected_improvement = 30 + conflict->conflict_severity / 3;
    
    snprintf(mit->description, sizeof(mit->description),
             "Add padding to array dimensions to avoid power-of-two sizes "
             "that cause bank conflicts");
    
    snprintf(mit->code_example, sizeof(mit->code_example),
             "// Before: Power-of-two size causes conflicts\n"
             "float matrix[1024][1024];\n\n"
             "// After: Add padding to avoid conflicts\n"
             "float matrix[1024][1024 + %d];  // Padding breaks pattern\n\n"
             "// Access normally, ignore padding:\n"
             "for (int i = 0; i < 1024; i++)\n"
             "    for (int j = 0; j < 1024; j++)\n"
             "        sum += matrix[i][j];",
             g_config.num_memory_banks);
    
    // Mitigation 2: Access pattern change
    mit = &(*mitigations)[(*mitigation_count)++];
    
    strncpy(mit->technique, "Access Pattern Optimization", sizeof(mit->technique) - 1);
    mit->expected_improvement = 40;
    
    snprintf(mit->description, sizeof(mit->description),
             "Change access pattern to distribute accesses across banks evenly");
    
    snprintf(mit->code_example, sizeof(mit->code_example),
             "// Diagonal access pattern to avoid conflicts\n"
             "for (int k = 0; k < n; k++) {\n"
             "    for (int i = 0; i < n; i++) {\n"
             "        int j = (i + k) %% n;  // Diagonal offset\n"
             "        process(array[i][j]);\n"
             "    }\n"
             "}");
    
    // GPU-specific mitigation
    if (conflict->is_gpu_related) {
        mit = &(*mitigations)[(*mitigation_count)++];
        
        strncpy(mit->technique, "Shared Memory Padding (GPU)", sizeof(mit->technique) - 1);
        mit->expected_improvement = 50;
        
        snprintf(mit->description, sizeof(mit->description),
                 "Add padding to shared memory arrays to avoid bank conflicts "
                 "in GPU kernels");
        
        snprintf(mit->code_example, sizeof(mit->code_example),
                 "__shared__ float tile[TILE_SIZE][TILE_SIZE + 1];\n"
                 "// The +1 padding ensures consecutive threads\n"
                 "// access different banks");
    }
    
    LOG_DEBUG("Generated %d bank conflict mitigations", *mitigation_count);
    return 0;
}

// Print bank conflicts
void print_bank_conflicts(const bank_conflict_t *conflicts, int count) {
    if (!conflicts || count <= 0) return;
    
    printf("\n=== Bank Conflict Analysis ===\n");
    printf("Found %d bank conflicts\n", count);
    
    for (int i = 0; i < count && i < 10; i++) {
        printf("\n[%d] ", i + 1);
        print_bank_conflict_details(&conflicts[i]);
    }
}

// Print conflict details
void print_bank_conflict_details(const bank_conflict_t *conflict) {
    if (!conflict) return;
    
    printf("%s\n", conflict->pattern_description);
    printf("  Severity: %.1f/100\n", conflict->conflict_severity);
    printf("  Total conflicts: %lu\n", conflict->total_conflicts);
    
    if (conflict->location.file[0]) {
        printf("  Location: %s:%d\n", conflict->location.file, conflict->location.line);
    }
    
    printf("  Banks affected:\n");
    for (int i = 0; i < conflict->num_banks && i < 5; i++) {
        printf("    Bank %d: %lu accesses, %.0f accesses/sec, %d threads\n",
               conflict->banks[i].bank_id,
               conflict->banks[i].access_count,
               conflict->banks[i].access_rate,
               conflict->banks[i].num_conflicting_threads);
    }
    
    if (conflict->performance_impact > 0) {
        printf("  Estimated performance impact: %.1f%%\n", conflict->performance_impact);
    }
}

// Default CPU configuration
bank_config_t bank_config_default_cpu(void) {
    bank_config_t config = {
        .num_memory_banks = 8,        // Typical for DDR4
        .bank_width_bytes = 8,        // 64-bit wide
        .bank_interleave_bytes = 64,  // Cache line size
        .has_bank_conflicts = true,
        .l1_banks = 4,               // Common for modern CPUs
        .shared_memory_banks = 0      // Not applicable for CPU
    };
    
    return config;
}

// Default GPU configuration
bank_config_t bank_config_default_gpu(void) {
    bank_config_t config = {
        .num_memory_banks = 32,       // Common for NVIDIA GPUs
        .bank_width_bytes = 4,        // 32-bit wide banks
        .bank_interleave_bytes = 4,   // Word-level interleaving
        .has_bank_conflicts = true,
        .l1_banks = 0,               // Not organized this way
        .shared_memory_banks = 32     // Shared memory banks
    };
    
    return config;
}

// Free bank conflicts
void free_bank_conflicts(bank_conflict_t *conflicts, int count) {
    if (!conflicts) return;
    
    for (int i = 0; i < count; i++) {
        if (conflicts[i].banks) {
            FREE_LOGGED(conflicts[i].banks);
        }
    }
    
    FREE_LOGGED(conflicts);
}
