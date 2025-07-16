#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

// Logging levels
typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO = 1,
    LOG_WARNING = 2,
    LOG_ERROR = 3,
    LOG_CRITICAL = 4
} log_level_t;

// Global logging configuration
typedef struct {
    log_level_t console_level;
    log_level_t file_level;
    char log_file_path[512];
    FILE *log_file;
    pthread_mutex_t log_mutex;
    bool initialized;
} logger_config_t;

extern logger_config_t g_logger;

// Source location tracking
typedef struct {
    char file[256];
    int line;
    int column;
    char function[128];
} source_location_t;

// Access pattern types; appended access to LOOP_CARRIED_DEP
typedef enum {
    SEQUENTIAL,
    STRIDED,
    RANDOM,
    GATHER_SCATTER,
    ACCESS_LOOP_CARRIED_DEP,
    NESTED_LOOP,
    INDIRECT_ACCESS
} access_pattern_t;

// Miss types
typedef enum {
    MISS_COMPULSORY,
    MISS_CAPACITY,
    MISS_CONFLICT,
    MISS_COHERENCE,
    MISS_PREFETCH_FAILED
} miss_type_t;

// Cache anti-patterns; appended cache to LOOP_CARRIED_DEP
typedef enum {
    HOTSPOT_REUSE,
    THRASHING,
    FALSE_SHARING,
    IRREGULAR_GATHER_SCATTER,
    UNCOALESCED_ACCESS,
    CACHE_LOOP_CARRIED_DEP,
    INSTRUCTION_OVERFLOW,
    DEAD_STORES,
    HIGH_ASSOCIATIVITY_PRESSURE,
    STREAMING_EVICTION,
    STACK_OVERFLOW,
    BANK_CONFLICTS
} cache_antipattern_t;

// Optimization types
typedef enum {
    OPT_LOOP_TILING,
    OPT_DATA_LAYOUT_CHANGE,
    OPT_PREFETCH_HINTS,
    OPT_MEMORY_ALIGNMENT,
    OPT_MEMORY_POOLING,
    OPT_ACCESS_REORDER,
    OPT_LOOP_UNROLL,
    OPT_CACHE_BLOCKING,
    OPT_NUMA_BINDING,
    OPT_LOOP_VECTORIZE
} optimization_type_t;

// Logging functions
void logger_init(const char *log_file_path, log_level_t console_level, log_level_t file_level);
void logger_cleanup(void);
void log_message(log_level_t level, const char *file, int line, const char *func, const char *format, ...);

// Convenience macros
#define LOG_DEBUG(...) log_message(LOG_DEBUG, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define LOG_INFO(...) log_message(LOG_INFO, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define LOG_WARNING(...) log_message(LOG_WARNING, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define LOG_ERROR(...) log_message(LOG_ERROR, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define LOG_CRITICAL(...) log_message(LOG_CRITICAL, __FILE__, __LINE__, __func__, __VA_ARGS__)

// Utility functions
const char* access_pattern_to_string(access_pattern_t pattern);
const char* miss_type_to_string(miss_type_t type);
const char* cache_antipattern_to_string(cache_antipattern_t pattern);
const char* optimization_type_to_string(optimization_type_t type);
double get_timestamp(void);
char* format_bytes(size_t bytes, char *buffer, size_t buffer_size);

// Memory allocation with logging
#define MALLOC_LOGGED(size) malloc_logged(size, __FILE__, __LINE__, __func__)
#define CALLOC_LOGGED(nmemb, size) calloc_logged(nmemb, size, __FILE__, __LINE__, __func__)
#define FREE_LOGGED(ptr) free_logged(ptr, __FILE__, __LINE__, __func__)

void* malloc_logged(size_t size, const char *file, int line, const char *func);
void* calloc_logged(size_t nmemb, size_t size, const char *file, int line, const char *func);
void free_logged(void *ptr, const char *file, int line, const char *func);

// Make sure these functions are declared here:
void log_message(log_level_t level, const char *file, int line, 
                const char *func, const char *fmt, ...);
const char* access_pattern_to_string(access_pattern_t pattern);

#ifdef __cplusplus
}
#endif

#endif // COMMON_H
