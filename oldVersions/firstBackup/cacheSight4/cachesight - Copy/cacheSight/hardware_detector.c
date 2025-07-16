#include "hardware_detector.h"
#include <dirent.h>
#include <sys/sysinfo.h>
#include <ctype.h>
#include <sys/utsname.h> // Required for struct utsname and uname()

static bool g_initialized = false;

// Helper function to read a single value from a file
static long read_long_from_file(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        LOG_DEBUG("Failed to open %s: %s", path, strerror(errno));
        return -1;
    }
    
    long value = -1;
    if (fscanf(fp, "%ld", &value) != 1) {
        LOG_DEBUG("Failed to read value from %s", path);
        value = -1;
    }
    
    fclose(fp);
    return value;
}

// Helper function to read a string from a file
static int read_string_from_file(const char *path, char *buffer, size_t buffer_size) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        LOG_DEBUG("Failed to open %s: %s", path, strerror(errno));
        return -1;
    }
    
    if (fgets(buffer, buffer_size, fp) == NULL) {
        LOG_DEBUG("Failed to read string from %s", path);
        fclose(fp);
        return -1;
    }
    
    // Remove trailing newline
    size_t len = strlen(buffer);
    if (len > 0 && buffer[len-1] == '\n') {
        buffer[len-1] = '\0';
    }
    
    fclose(fp);
    return 0;
}

// Parse cache information from /sys/devices/system/cpu/cpu*/cache/
static int parse_cache_from_sys(cache_info_t *info) {
    LOG_INFO("Parsing cache information from /sys/devices/system/cpu/");
    
    int cache_count = 0;
    char path[512];
    
    // We'll check cpu0's cache structure (usually representative)
    for (int index = 0; index < 8; index++) {
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu0/cache/index%d/size", index);
        
        // Check if this cache index exists
        if (access(path, F_OK) != 0) {
            LOG_DEBUG("Cache index %d does not exist", index);
            break;
        }
        
        cache_level_t *cache = &info->levels[cache_count];
        
        // Read cache level
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu0/cache/index%d/level", index);
        long level = read_long_from_file(path);
        if (level > 0) {
            cache->level = (int)level;
            LOG_DEBUG("Cache index %d is level %d", index, cache->level);
        }
        
        // Read cache size
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu0/cache/index%d/size", index);
        FILE *fp = fopen(path, "r");
        if (fp) {
            char size_str[32];
            if (fgets(size_str, sizeof(size_str), fp) != NULL) {
                // Parse size (e.g., "32K", "256K", "8192K")
                int size_val;
                char unit;
                if (sscanf(size_str, "%d%c", &size_val, &unit) == 2) {
                    if (unit == 'K') {
                        cache->size = size_val * 1024;
                    } else if (unit == 'M') {
                        cache->size = size_val * 1024 * 1024;
                    }
                    LOG_DEBUG("Cache size: %zu bytes", cache->size);
                }
            }
            fclose(fp);
        }
        
        // Read cache line size
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu0/cache/index%d/coherency_line_size", index);
        long line_size = read_long_from_file(path);
        if (line_size > 0) {
            cache->line_size = (size_t)line_size;
            LOG_DEBUG("Cache line size: %zu bytes", cache->line_size);
        }
        
        // Read associativity
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu0/cache/index%d/ways_of_associativity", index);
        long ways = read_long_from_file(path);
        if (ways > 0) {
            cache->associativity = (int)ways;
            LOG_DEBUG("Cache associativity: %d-way", cache->associativity);
        }
        
        // Read number of sets
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu0/cache/index%d/number_of_sets", index);
        long sets = read_long_from_file(path);
        if (sets > 0) {
            cache->sets = (int)sets;
            LOG_DEBUG("Cache sets: %d", cache->sets);
        }
        
        // Read cache type
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu0/cache/index%d/type", index);
        read_string_from_file(path, cache->type, sizeof(cache->type));
        LOG_DEBUG("Cache type: %s", cache->type);
        
        // Read shared CPU list to determine if shared
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu0/cache/index%d/shared_cpu_list", index);
        char cpu_list[256];
        if (read_string_from_file(path, cpu_list, sizeof(cpu_list)) == 0) {
            // Count commas and ranges to estimate sharing
            int cpu_count = 1;
            for (char *p = cpu_list; *p; p++) {
                if (*p == ',' || *p == '-') cpu_count++;
            }
            cache->shared = (cpu_count > 1);
            cache->sharing_cpu_count = cpu_count;
            LOG_DEBUG("Cache shared across %d CPUs", cpu_count);
        }
        
        // Estimate latency based on level (rough approximation)
        switch (cache->level) {
            case 1: cache->latency_cycles = 4; break;
            case 2: cache->latency_cycles = 12; break;
            case 3: cache->latency_cycles = 40; break;
            default: cache->latency_cycles = 100; break;
        }
        
        // Determine if inclusive (heuristic: L3 is often inclusive on Intel)
        cache->inclusive = (cache->level == 3);
        
        cache_count++;
        LOG_INFO("Detected L%d cache: %zu KB, %zu B line, %d-way associative",
                 cache->level, cache->size / 1024, cache->line_size, cache->associativity);
    }
    
    info->num_levels = cache_count;
    LOG_INFO("Total cache levels detected: %d", cache_count);
    return 0;
}

// Parse CPU information from /proc/cpuinfo
static int parse_cpu_info(cache_info_t *info) {
    LOG_INFO("Parsing CPU information from /proc/cpuinfo");
    
    FILE *fp = fopen("/proc/cpuinfo", "r");
    if (!fp) {
        LOG_ERROR("Failed to open /proc/cpuinfo: %s", strerror(errno));
        return -1;
    }
    
    char line[1024];
    int physical_cores = 0;
    int logical_cores = 0;
    bool found_model = false;
    
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "processor", 9) == 0) {
            logical_cores++;
        } else if (strncmp(line, "model name", 10) == 0 && !found_model) {
            char *colon = strchr(line, ':');
            if (colon) {
                strncpy(info->cpu_model, colon + 2, sizeof(info->cpu_model) - 1);
                // Remove newline
                char *newline = strchr(info->cpu_model, '\n');
                if (newline) *newline = '\0';
                found_model = true;
                LOG_DEBUG("CPU Model: %s", info->cpu_model);
            }
        } else if (strncmp(line, "cpu family", 10) == 0) {
            sscanf(line, "cpu family : %d", &info->cpu_family);
        } else if (strncmp(line, "model", 5) == 0 && strncmp(line, "model name", 10) != 0) {
            sscanf(line, "model : %d", &info->cpu_model_num);
        } else if (strncmp(line, "cpu MHz", 7) == 0) {
            double mhz;
            if (sscanf(line, "cpu MHz : %lf", &mhz) == 1) {
                info->cpu_frequency_ghz = mhz / 1000.0;
                LOG_DEBUG("CPU Frequency: %.2f GHz", info->cpu_frequency_ghz);
            }
        } else if (strncmp(line, "cpu cores", 9) == 0) {
            sscanf(line, "cpu cores : %d", &physical_cores);
        }
    }
    
    fclose(fp);
    
    info->num_threads = logical_cores;
    info->num_cores = physical_cores > 0 ? physical_cores : logical_cores;
    
    LOG_INFO("Detected %d physical cores, %d logical cores", info->num_cores, info->num_threads);
    return 0;
}

// Initialize hardware detector
int hardware_detector_init(void) {
    if (g_initialized) {
        LOG_WARNING("Hardware detector already initialized");
        return 0;
    }
    
    LOG_INFO("Initializing hardware detector");
    g_initialized = true;
    return 0;
}

// Cleanup hardware detector
void hardware_detector_cleanup(void) {
    if (!g_initialized) {
        return;
    }
    
    LOG_INFO("Cleaning up hardware detector");
    g_initialized = false;
}

// Main detection function
int detect_cache_hierarchy(cache_info_t *info) {
    if (!g_initialized) {
        LOG_ERROR("Hardware detector not initialized");
        return -1;
    }
    
    LOG_INFO("Starting cache hierarchy detection");
    memset(info, 0, sizeof(cache_info_t));
    
    // Get architecture
    struct utsname uts;
    if (uname(&uts) == 0) {
        strncpy(info->arch, uts.machine, sizeof(info->arch) - 1);
        LOG_INFO("Architecture: %s", info->arch);
    }
    
    // Parse cache hierarchy
    if (parse_cache_from_sys(info) != 0) {
        LOG_ERROR("Failed to parse cache hierarchy");
        return -1;
    }
    
    // Parse CPU information
    if (parse_cpu_info(info) != 0) {
        LOG_ERROR("Failed to parse CPU information");
        return -1;
    }
    
    // Get memory information
    struct sysinfo si;
    if (sysinfo(&si) == 0) {
        info->total_memory = si.totalram * si.mem_unit;
        char mem_str[32];
        format_bytes(info->total_memory, mem_str, sizeof(mem_str));
        LOG_INFO("Total memory: %s", mem_str);
    }
    
    // Get page size
    info->page_size = getpagesize();
    LOG_INFO("Page size: %d bytes", info->page_size);
    
    // Get NUMA nodes
    info->numa_nodes = get_numa_node_count();
    LOG_INFO("NUMA nodes: %d", info->numa_nodes);
    
    // Estimate memory bandwidth (simplified - would need actual benchmark)
    // Rough estimate based on architecture
    if (strcmp(info->arch, "x86_64") == 0) {
        info->memory_bandwidth_gbps = 25; // Typical DDR4 bandwidth
    } else {
        info->memory_bandwidth_gbps = 20; // Conservative estimate
    }
    LOG_INFO("Estimated memory bandwidth: %zu GB/s", info->memory_bandwidth_gbps);
    
    LOG_INFO("Cache hierarchy detection complete");
    return 0;
}

// Print cache information
void print_cache_info(const cache_info_t *info) {
    printf("\n=== System Information ===\n");
    printf("Architecture: %s\n", info->arch);
    printf("CPU Model: %s\n", info->cpu_model);
    printf("CPU Family: %d, Model: %d\n", info->cpu_family, info->cpu_model_num);
    printf("CPU Frequency: %.2f GHz\n", info->cpu_frequency_ghz);
    printf("Cores: %d physical, %d logical\n", info->num_cores, info->num_threads);
    printf("NUMA Nodes: %d\n", info->numa_nodes);
    printf("Page Size: %d bytes\n", info->page_size);
    
    char mem_str[32];
    format_bytes(info->total_memory, mem_str, sizeof(mem_str));
    printf("Total Memory: %s\n", mem_str);
    printf("Memory Bandwidth: ~%zu GB/s\n", info->memory_bandwidth_gbps);
    
    printf("\n=== Cache Hierarchy ===\n");
    for (int i = 0; i < info->num_levels; i++) {
        const cache_level_t *cache = &info->levels[i];
        char size_str[32];
        format_bytes(cache->size, size_str, sizeof(size_str));
        
        printf("L%d %s Cache:\n", cache->level, cache->type);
        printf("  Size: %s\n", size_str);
        printf("  Line Size: %zu bytes\n", cache->line_size);
        printf("  Associativity: %d-way\n", cache->associativity);
        printf("  Sets: %d\n", cache->sets);
        printf("  Latency: ~%d cycles\n", cache->latency_cycles);
        printf("  Shared: %s", cache->shared ? "Yes" : "No");
        if (cache->shared) {
            printf(" (across %d CPUs)", cache->sharing_cpu_count);
        }
        printf("\n");
        printf("  Inclusive: %s\n", cache->inclusive ? "Yes" : "No");
        printf("\n");
    }
}

// Save cache information to file
int save_cache_info_to_file(const cache_info_t *info, const char *filename) {
    LOG_INFO("Saving cache information to %s", filename);
    
    FILE *fp = fopen(filename, "w");
    if (!fp) {
        LOG_ERROR("Failed to open %s for writing: %s", filename, strerror(errno));
        return -1;
    }
    
    fprintf(fp, "# Cache Hierarchy Information\n");
    fprintf(fp, "# Generated by Cache Optimizer Tool\n\n");
    
    fprintf(fp, "[SYSTEM]\n");
    fprintf(fp, "arch=%s\n", info->arch);
    fprintf(fp, "cpu_model=%s\n", info->cpu_model);
    fprintf(fp, "cpu_family=%d\n", info->cpu_family);
    fprintf(fp, "cpu_model_num=%d\n", info->cpu_model_num);
    fprintf(fp, "cpu_frequency_ghz=%.2f\n", info->cpu_frequency_ghz);
    fprintf(fp, "num_cores=%d\n", info->num_cores);
    fprintf(fp, "num_threads=%d\n", info->num_threads);
    fprintf(fp, "numa_nodes=%d\n", info->numa_nodes);
    fprintf(fp, "page_size=%d\n", info->page_size);
    fprintf(fp, "total_memory=%zu\n", info->total_memory);
    fprintf(fp, "memory_bandwidth_gbps=%zu\n", info->memory_bandwidth_gbps);
    fprintf(fp, "num_cache_levels=%d\n\n", info->num_levels);
    
    for (int i = 0; i < info->num_levels; i++) {
        const cache_level_t *cache = &info->levels[i];
        fprintf(fp, "[CACHE_L%d]\n", cache->level);
        fprintf(fp, "size=%zu\n", cache->size);
        fprintf(fp, "line_size=%zu\n", cache->line_size);
        fprintf(fp, "associativity=%d\n", cache->associativity);
        fprintf(fp, "sets=%d\n", cache->sets);
        fprintf(fp, "type=%s\n", cache->type);
        fprintf(fp, "latency_cycles=%d\n", cache->latency_cycles);
        fprintf(fp, "shared=%d\n", cache->shared ? 1 : 0);
        fprintf(fp, "sharing_cpu_count=%d\n", cache->sharing_cpu_count);
        fprintf(fp, "inclusive=%d\n\n", cache->inclusive ? 1 : 0);
    }
    
    fclose(fp);
    LOG_INFO("Cache information saved successfully");
    return 0;
}

// Helper functions
int get_cpu_count(void) {
    return sysconf(_SC_NPROCESSORS_ONLN);
}

int get_numa_node_count(void) {
    int count = 0;
    DIR *dir = opendir("/sys/devices/system/node");
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (strncmp(entry->d_name, "node", 4) == 0 && isdigit(entry->d_name[4])) {
                count++;
            }
        }
        closedir(dir);
    }
    return count > 0 ? count : 1;
}

size_t get_total_memory(void) {
    struct sysinfo si;
    if (sysinfo(&si) == 0) {
        return si.totalram * si.mem_unit;
    }
    return 0;
}

int get_page_size(void) {
    return getpagesize();
}

const char* get_architecture(void) {
    static char arch[32];
    struct utsname uts;
    if (uname(&uts) == 0) {
        strncpy(arch, uts.machine, sizeof(arch) - 1);
        return arch;
    }
    return "unknown";
}
