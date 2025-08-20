#ifndef BANK_CONFLICT_ANALYZER_H
#define BANK_CONFLICT_ANALYZER_H

#include "common.h"
#include "sample_collector.h"
#include "hardware_detector.h"

// Bank conflict information
typedef struct {
    int bank_id;                    // Memory bank ID
    uint64_t access_count;          // Total accesses to this bank
    double access_rate;             // Accesses per time unit
    int conflicting_threads[32];    // Threads accessing this bank
    int num_conflicting_threads;    // Number of threads
} bank_info_t;

// Bank conflict pattern
typedef struct {
    source_location_t location;     // Where conflict occurs
    int num_banks;                  // Number of banks involved
    bank_info_t *banks;            // Bank information
    double conflict_severity;       // 0-100, severity score
    uint64_t total_conflicts;       // Total conflicting accesses
    double performance_impact;      // Estimated performance loss (%)
    char pattern_description[256];  // Description of pattern
    bool is_gpu_related;           // GPU memory bank conflict
} bank_conflict_t;

// Bank configuration (architecture-specific)
typedef struct {
    int num_memory_banks;          // Number of memory banks
    int bank_width_bytes;          // Width of each bank
    int bank_interleave_bytes;     // Interleaving granularity
    bool has_bank_conflicts;       // Architecture has bank conflicts
    int l1_banks;                  // L1 cache banks (if applicable)
    int shared_memory_banks;       // GPU shared memory banks
} bank_config_t;

// API functions
int bank_conflict_analyzer_init(const bank_config_t *config);
void bank_conflict_analyzer_cleanup(void);

// Analysis functions
int analyze_bank_conflicts(const cache_miss_sample_t *samples, int sample_count,
                          bank_conflict_t **conflicts, int *conflict_count);

int analyze_bank_access_pattern(const cache_hotspot_t *hotspot,
                               bank_conflict_t *conflict);

// Bank calculation
int calculate_memory_bank(uint64_t address, const bank_config_t *config);
int calculate_cache_bank(uint64_t address, int cache_level, 
                        const cache_info_t *cache_info);

// Severity assessment
double calculate_bank_conflict_severity(const bank_conflict_t *conflict);
double estimate_bank_conflict_penalty(const bank_conflict_t *conflict,
                                    const cache_info_t *cache_info);

// Pattern detection
bool detect_strided_bank_conflict(const uint64_t *addresses, int count,
                                 int *stride, int *conflicting_banks);
bool detect_power_of_two_conflict(const uint64_t *addresses, int count);

// Mitigation
typedef struct {
    char technique[128];           // Mitigation technique name
    char description[512];         // Detailed description
    char code_example[1024];       // Example implementation
    double expected_improvement;   // Expected performance gain (%)
} bank_conflict_mitigation_t;

int suggest_bank_conflict_mitigation(const bank_conflict_t *conflict,
                                    bank_conflict_mitigation_t **mitigations,
                                    int *mitigation_count);

// Reporting
void print_bank_conflicts(const bank_conflict_t *conflicts, int count);
void print_bank_conflict_details(const bank_conflict_t *conflict);

// Configuration helpers
bank_config_t bank_config_default_cpu(void);
bank_config_t bank_config_default_gpu(void);
bank_config_t bank_config_for_architecture(const char *arch);

// Memory
void free_bank_conflicts(bank_conflict_t *conflicts, int count);

#endif // BANK_CONFLICT_ANALYZER_H
