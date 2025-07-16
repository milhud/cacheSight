#include "common.h"
#include <stdarg.h>
#include <sys/time.h>

// Global logger instance
logger_config_t g_logger = {
    .console_level = LOG_INFO,
    .file_level = LOG_DEBUG,
    .log_file_path = "",
    .log_file = NULL,
    .initialized = false
};

static const char* log_level_strings[] = {
    "DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL"
};

static const char* log_level_colors[] = {
    "\033[36m",  // Cyan for DEBUG
    "\033[32m",  // Green for INFO
    "\033[33m",  // Yellow for WARNING
    "\033[31m",  // Red for ERROR
    "\033[35m"   // Magenta for CRITICAL
};

void logger_init(const char *log_file_path, log_level_t console_level, log_level_t file_level) {
    if (g_logger.initialized) {
        LOG_WARNING("Logger already initialized, cleaning up first");
        logger_cleanup();
    }
    
    pthread_mutex_init(&g_logger.log_mutex, NULL);
    g_logger.console_level = console_level;
    g_logger.file_level = file_level;
    
    if (log_file_path && strlen(log_file_path) > 0) {
        strncpy(g_logger.log_file_path, log_file_path, sizeof(g_logger.log_file_path) - 1);
        g_logger.log_file = fopen(log_file_path, "a");
        if (!g_logger.log_file) {
            fprintf(stderr, "Failed to open log file %s: %s\n", log_file_path, strerror(errno));
        }
    }
    
    g_logger.initialized = true;
    LOG_INFO("Logger initialized - Console Level: %s, File Level: %s, Log File: %s",
             log_level_strings[console_level],
             log_level_strings[file_level],
             log_file_path ? log_file_path : "none");
}

void logger_cleanup(void) {
    if (g_logger.log_file) {
        LOG_INFO("Closing log file");
        fclose(g_logger.log_file);
        g_logger.log_file = NULL;
    }
    pthread_mutex_destroy(&g_logger.log_mutex);
    g_logger.initialized = false;
}

void log_message(log_level_t level, const char *file, int line, const char *func, const char *format, ...) {
    if (!g_logger.initialized) {
        // Emergency logging to stderr
        fprintf(stderr, "Logger not initialized! Message: ");
        va_list args;
        va_start(args, format);
        vfprintf(stderr, format, args);
        va_end(args);
        fprintf(stderr, "\n");
        return;
    }
    
    pthread_mutex_lock(&g_logger.log_mutex);
    
    // Get current time
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm *tm_info = localtime(&tv.tv_sec);
    char time_buffer[64];
    strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %H:%M:%S", tm_info);
    
    // Format the message
    char message[4096];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    
    // Extract just the filename (not the full path)
    const char *filename = strrchr(file, '/');
    if (filename) filename++;
    else filename = file;
    
    // Log to console if level is high enough
    if (level >= g_logger.console_level) {
        fprintf(stderr, "%s[%s.%03ld] [%s] [%s:%d:%s] %s\033[0m\n",
                log_level_colors[level],
                time_buffer, tv.tv_usec / 1000,
                log_level_strings[level],
                filename, line, func,
                message);
        fflush(stderr);
    }
    
    // Log to file if level is high enough
    if (g_logger.log_file && level >= g_logger.file_level) {
        fprintf(g_logger.log_file, "[%s.%03ld] [%s] [%s:%d:%s] %s\n",
                time_buffer, tv.tv_usec / 1000,
                log_level_strings[level],
                filename, line, func,
                message);
        fflush(g_logger.log_file);
    }
    
    pthread_mutex_unlock(&g_logger.log_mutex);
}

const char* access_pattern_to_string(access_pattern_t pattern) {
    switch (pattern) {
        case SEQUENTIAL: return "SEQUENTIAL";
        case STRIDED: return "STRIDED";
        case RANDOM: return "RANDOM";
        case GATHER_SCATTER: return "GATHER_SCATTER";
        case ACCESS_LOOP_CARRIED_DEP: return "ACCESS_LOOP_CARRIED_DEP";
        case NESTED_LOOP: return "NESTED_LOOP";
        case INDIRECT_ACCESS: return "INDIRECT_ACCESS";
        default: return "UNKNOWN";
    }
}

const char* miss_type_to_string(miss_type_t type) {
    switch (type) {
        case MISS_COMPULSORY: return "COMPULSORY";
        case MISS_CAPACITY: return "CAPACITY";
        case MISS_CONFLICT: return "CONFLICT";
        case MISS_COHERENCE: return "COHERENCE";
        case MISS_PREFETCH_FAILED: return "PREFETCH_FAILED";
        default: return "UNKNOWN";
    }
}

const char* cache_antipattern_to_string(cache_antipattern_t pattern) {
    switch (pattern) {
        case HOTSPOT_REUSE: return "HOTSPOT_REUSE";
        case THRASHING: return "THRASHING";
        case FALSE_SHARING: return "FALSE_SHARING";
        case IRREGULAR_GATHER_SCATTER: return "IRREGULAR_GATHER_SCATTER";
        case UNCOALESCED_ACCESS: return "UNCOALESCED_ACCESS";
        case CACHE_LOOP_CARRIED_DEP: return "CACHE_LOOP_CARRIED_DEP";
        case INSTRUCTION_OVERFLOW: return "INSTRUCTION_OVERFLOW";
        case DEAD_STORES: return "DEAD_STORES";
        case HIGH_ASSOCIATIVITY_PRESSURE: return "HIGH_ASSOCIATIVITY_PRESSURE";
        case STREAMING_EVICTION: return "STREAMING_EVICTION";
        case STACK_OVERFLOW: return "STACK_OVERFLOW";
        case BANK_CONFLICTS: return "BANK_CONFLICTS";
        default: return "UNKNOWN";
    }
}

const char* optimization_type_to_string(optimization_type_t type) {
    switch (type) {
        case OPT_LOOP_TILING: return "LOOP_TILING";
        case OPT_DATA_LAYOUT_CHANGE: return "DATA_LAYOUT_CHANGE";
        case OPT_PREFETCH_HINTS: return "PREFETCH_HINTS";
        case OPT_MEMORY_ALIGNMENT: return "MEMORY_ALIGNMENT";
        case OPT_MEMORY_POOLING: return "MEMORY_POOLING";
        case OPT_ACCESS_REORDER: return "ACCESS_REORDER";
        case OPT_LOOP_UNROLL: return "LOOP_UNROLL";
        case OPT_CACHE_BLOCKING: return "CACHE_BLOCKING";
        case OPT_NUMA_BINDING: return "NUMA_BINDING";
        default: return "UNKNOWN";
    }
}

double get_timestamp(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1000000.0;
}

char* format_bytes(size_t bytes, char *buffer, size_t buffer_size) {
    const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit_index = 0;
    double size = (double)bytes;
    
    while (size >= 1024.0 && unit_index < 4) {
        size /= 1024.0;
        unit_index++;
    }
    
    snprintf(buffer, buffer_size, "%.2f %s", size, units[unit_index]);
    return buffer;
}

void* malloc_logged(size_t size, const char *file, int line, const char *func) {
    void *ptr = malloc(size);
    if (ptr) {
        char size_str[32];
        format_bytes(size, size_str, sizeof(size_str));
        LOG_DEBUG("Memory allocated: %s at %p [%s:%d:%s]", size_str, ptr, file, line, func);
    } else {
        LOG_ERROR("Memory allocation failed for %zu bytes [%s:%d:%s]", size, file, line, func);
    }
    return ptr;
}

void* calloc_logged(size_t nmemb, size_t size, const char *file, int line, const char *func) {
    void *ptr = calloc(nmemb, size);
    size_t total_size = nmemb * size;
    if (ptr) {
        char size_str[32];
        format_bytes(total_size, size_str, sizeof(size_str));
        LOG_DEBUG("Memory allocated (calloc): %s at %p [%s:%d:%s]", size_str, ptr, file, line, func);
    } else {
        LOG_ERROR("Memory allocation (calloc) failed for %zu bytes [%s:%d:%s]", total_size, file, line, func);
    }
    return ptr;
}

void free_logged(void *ptr, const char *file, int line, const char *func) {
    if (ptr) {
        LOG_DEBUG("Memory freed: %p [%s:%d:%s]", ptr, file, line, func);
        free(ptr);
    }
}
