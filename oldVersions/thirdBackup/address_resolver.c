#include "address_resolver.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <elf.h>
#include <link.h>
#include <dlfcn.h>
//#include <cxxabi.h>

// Symbol cache entry
typedef struct cache_entry {
    uint64_t address;
    symbol_info_t symbol;
    struct cache_entry *next;
} cache_entry_t;

// Internal resolver structure
struct address_resolver {
    pid_t pid;
    char binary_path[256];
    
    // Memory mappings
    memory_mapping_t *mappings;
    int mapping_count;
    
    // Symbol cache
    cache_entry_t **cache_table;
    size_t cache_size;
    size_t cache_entries;
    size_t max_cache_entries;
    
    // addr2line process for source resolution
    FILE *addr2line_pipe;
    pid_t addr2line_pid;
    
    pthread_mutex_t mutex;
};

// Hash function for cache
static uint64_t hash_address_cache(uint64_t addr, size_t table_size) {
    return (addr * 2654435761ULL) % table_size;
}

// Create address resolver
address_resolver_t* address_resolver_create(pid_t pid) {
    address_resolver_t *resolver = CALLOC_LOGGED(1, sizeof(address_resolver_t));
    if (!resolver) {
        LOG_ERROR("Failed to allocate address resolver");
        return NULL;
    }
    
    resolver->pid = pid;
    pthread_mutex_init(&resolver->mutex, NULL);
    
    // Initialize cache
    resolver->cache_size = 1024;
    resolver->max_cache_entries = 10000;
    resolver->cache_table = CALLOC_LOGGED(resolver->cache_size, sizeof(cache_entry_t*));
    if (!resolver->cache_table) {
        LOG_ERROR("Failed to allocate cache table");
        address_resolver_destroy(resolver);
        return NULL;
    }
    
    LOG_INFO("Created address resolver for PID %d", pid);
    return resolver;
}

// Destroy address resolver
void address_resolver_destroy(address_resolver_t *resolver) {
    if (!resolver) return;
    
    LOG_INFO("Destroying address resolver");
    
    // Close addr2line pipe
    if (resolver->addr2line_pipe) {
        pclose(resolver->addr2line_pipe);
    }
    
    // Free mappings
    if (resolver->mappings) {
        FREE_LOGGED(resolver->mappings);
    }
    
    // Free cache
    if (resolver->cache_table) {
        for (size_t i = 0; i < resolver->cache_size; i++) {
            cache_entry_t *entry = resolver->cache_table[i];
            while (entry) {
                cache_entry_t *next = entry->next;
                FREE_LOGGED(entry);
                entry = next;
            }
        }
        FREE_LOGGED(resolver->cache_table);
    }
    
    pthread_mutex_destroy(&resolver->mutex);
    FREE_LOGGED(resolver);
}

// Parse /proc/pid/maps
static int parse_proc_maps(address_resolver_t *resolver) {
    char maps_path[256];
    
    if (resolver->pid == 0 || resolver->pid == getpid()) {
        snprintf(maps_path, sizeof(maps_path), "/proc/self/maps");
    } else {
        snprintf(maps_path, sizeof(maps_path), "/proc/%d/maps", resolver->pid);
    }
    
    FILE *fp = fopen(maps_path, "r");
    if (!fp) {
        LOG_ERROR("Failed to open %s: %s", maps_path, strerror(errno));
        return -1;
    }
    
    // Count mappings
    char line[512];
    int count = 0;
    while (fgets(line, sizeof(line), fp)) {
        count++;
    }
    
    // Allocate mappings array
    resolver->mappings = CALLOC_LOGGED(count, sizeof(memory_mapping_t));
    if (!resolver->mappings) {
        fclose(fp);
        return -1;
    }
    
    // Parse mappings
    rewind(fp);
    int idx = 0;
    while (fgets(line, sizeof(line), fp) && idx < count) {
        memory_mapping_t *map = &resolver->mappings[idx];
        char perms[5];
        int ret = sscanf(line, "%lx-%lx %4s %lx %*x:%*x %*d %255s",
                        &map->start_addr, &map->end_addr, perms,
                        &map->file_offset, map->pathname);
        
        if (ret >= 4) {
            map->is_executable = (perms[2] == 'x');
            map->is_writable = (perms[1] == 'w');
            map->is_shared = (perms[3] == 's');
            
            if (ret < 5) {
                strcpy(map->pathname, "[anonymous]");
            }
            
            LOG_DEBUG("Mapping %d: 0x%lx-0x%lx %s %s",
                     idx, map->start_addr, map->end_addr, perms, map->pathname);
            idx++;
        }
    }
    
    resolver->mapping_count = idx;
    fclose(fp);
    
    LOG_INFO("Parsed %d memory mappings", resolver->mapping_count);
    return 0;
}

// Initialize from process
int address_resolver_init_process(address_resolver_t *resolver) {
    if (!resolver) {
        LOG_ERROR("NULL resolver in address_resolver_init_process");
        return -1;
    }
    
    LOG_INFO("Initializing address resolver from process");
    
    pthread_mutex_lock(&resolver->mutex);
    
    // Parse memory mappings
    if (parse_proc_maps(resolver) != 0) {
        pthread_mutex_unlock(&resolver->mutex);
        return -1;
    }
    
    // Find main executable
    for (int i = 0; i < resolver->mapping_count; i++) {
        if (resolver->mappings[i].is_executable &&
            resolver->mappings[i].file_offset == 0 &&
            resolver->mappings[i].pathname[0] == '/') {
            strncpy(resolver->binary_path, resolver->mappings[i].pathname,
                    sizeof(resolver->binary_path) - 1);
            LOG_INFO("Found main executable: %s", resolver->binary_path);
            break;
        }
    }
    
    pthread_mutex_unlock(&resolver->mutex);
    
    return 0;
}

// Initialize from binary
int address_resolver_init_binary(address_resolver_t *resolver, const char *binary_path) {
    if (!resolver || !binary_path) {
        LOG_ERROR("Invalid parameters for address_resolver_init_binary");
        return -1;
    }
    
    LOG_INFO("Initializing address resolver from binary: %s", binary_path);
    
    pthread_mutex_lock(&resolver->mutex);
    
    strncpy(resolver->binary_path, binary_path, sizeof(resolver->binary_path) - 1);
    
    // For standalone binary analysis, create minimal mapping
    if (resolver->mapping_count == 0) {
        resolver->mappings = CALLOC_LOGGED(1, sizeof(memory_mapping_t));
        if (resolver->mappings) {
            resolver->mappings[0].start_addr = 0x400000;  // Typical load address
            resolver->mappings[0].end_addr = 0x800000;
            resolver->mappings[0].file_offset = 0;
            strncpy(resolver->mappings[0].pathname, binary_path,
                    sizeof(resolver->mappings[0].pathname) - 1);
            resolver->mappings[0].is_executable = true;
            resolver->mapping_count = 1;
        }
    }
    
    pthread_mutex_unlock(&resolver->mutex);
    
    return 0;
}

// Start addr2line process
static int start_addr2line(address_resolver_t *resolver) {
    if (resolver->addr2line_pipe) {
        return 0;  // Already started
    }
    
    if (strlen(resolver->binary_path) == 0) {
        LOG_ERROR("No binary path set for addr2line");
        return -1;
    }
    
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "addr2line -e %s -f -C", resolver->binary_path);
    
    resolver->addr2line_pipe = popen(cmd, "r+");
    if (!resolver->addr2line_pipe) {
        LOG_ERROR("Failed to start addr2line: %s", strerror(errno));
        return -1;
    }
    
    // Set to line buffered
    setlinebuf(resolver->addr2line_pipe);
    
    LOG_DEBUG("Started addr2line process");
    return 0;
}

// Resolve single address
int address_resolver_resolve(address_resolver_t *resolver,
                           uint64_t address, symbol_info_t *symbol) {
    if (!resolver || !symbol) {
        LOG_ERROR("Invalid parameters for address_resolver_resolve");
        return -1;
    }
    
    pthread_mutex_lock(&resolver->mutex);
    
    // Check cache first
    uint64_t hash = hash_address_cache(address, resolver->cache_size);
    cache_entry_t *entry = resolver->cache_table[hash];
    
    while (entry) {
        if (entry->address == address) {
            *symbol = entry->symbol;
            pthread_mutex_unlock(&resolver->mutex);
            LOG_DEBUG("Cache hit for address 0x%lx", address);
            return 0;
        }
        entry = entry->next;
    }
    
    // Cache miss - resolve address
    memset(symbol, 0, sizeof(symbol_info_t));
    symbol->address = address;
    
    // Find mapping
    const memory_mapping_t *mapping = NULL;
    for (int i = 0; i < resolver->mapping_count; i++) {
        if (address >= resolver->mappings[i].start_addr &&
            address < resolver->mappings[i].end_addr) {
            mapping = &resolver->mappings[i];
            break;
        }
    }
    
    if (!mapping) {
        LOG_DEBUG("No mapping found for address 0x%lx", address);
        pthread_mutex_unlock(&resolver->mutex);
        return -1;
    }
    
    // Start addr2line if needed
    if (start_addr2line(resolver) != 0) {
        pthread_mutex_unlock(&resolver->mutex);
        return -1;
    }
    
    // Query addr2line
    fprintf(resolver->addr2line_pipe, "0x%lx\n", address);
    fflush(resolver->addr2line_pipe);
    
    // Read function name
    char line[1024];
    if (fgets(line, sizeof(line), resolver->addr2line_pipe)) {
        line[strcspn(line, "\n")] = '\0';
        strncpy(symbol->name, line, sizeof(symbol->name) - 1);
        
        // Check if it's a valid symbol
        if (strcmp(line, "??") != 0) {
            symbol->is_function = true;
            
            // Copy to demangled name (addr2line with -C already demangles)
            strncpy(symbol->demangled_name, line, sizeof(symbol->demangled_name) - 1);
        }
    }
    
    // Read source location
    if (fgets(line, sizeof(line), resolver->addr2line_pipe)) {
        line[strcspn(line, "\n")] = '\0';
        
        // Parse filename:line format
        char *colon = strrchr(line, ':');
        if (colon && strcmp(line, "??:0") != 0) {
            *colon = '\0';
            strncpy(symbol->location.file, line, sizeof(symbol->location.file) - 1);
            symbol->location.line = atoi(colon + 1);
            
            // Extract just filename (not full path)
            const char *filename = strrchr(symbol->location.file, '/');
            if (filename) {
                filename++;
            } else {
                filename = symbol->location.file;
            }
            
            // Copy function name to location
            strncpy(symbol->location.function, symbol->name,
                    sizeof(symbol->location.function) - 1);
        }
    }
    
    // Add to cache
    if (resolver->cache_entries < resolver->max_cache_entries) {
        entry = MALLOC_LOGGED(sizeof(cache_entry_t));
        if (entry) {
            entry->address = address;
            entry->symbol = *symbol;
            entry->next = resolver->cache_table[hash];
            resolver->cache_table[hash] = entry;
            resolver->cache_entries++;
            
            LOG_DEBUG("Cached symbol for 0x%lx: %s at %s:%d",
                     address, symbol->name, symbol->location.file,
                     symbol->location.line);
        }
    }
    
    pthread_mutex_unlock(&resolver->mutex);
    
    return 0;
}

// Resolve batch of addresses
int address_resolver_resolve_batch(address_resolver_t *resolver,
                                 const uint64_t *addresses, int count,
                                 symbol_info_t *symbols) {
    if (!resolver || !addresses || !symbols || count <= 0) {
        LOG_ERROR("Invalid parameters for address_resolver_resolve_batch");
        return -1;
    }
    
    LOG_INFO("Resolving batch of %d addresses", count);
    
    int resolved = 0;
    for (int i = 0; i < count; i++) {
        if (address_resolver_resolve(resolver, addresses[i], &symbols[i]) == 0) {
            resolved++;
        }
    }
    
    LOG_INFO("Resolved %d of %d addresses", resolved, count);
    return resolved;
}

// Get source location
int address_resolver_get_source_location(address_resolver_t *resolver,
                                       uint64_t address, source_location_t *location) {
    if (!resolver || !location) {
        LOG_ERROR("Invalid parameters for address_resolver_get_source_location");
        return -1;
    }
    
    symbol_info_t symbol;
    if (address_resolver_resolve(resolver, address, &symbol) == 0) {
        *location = symbol.location;
        return 0;
    }
    
    return -1;
}

// Get line info
int address_resolver_get_line_info(address_resolver_t *resolver,
                                 uint64_t address, char *filename,
                                 size_t filename_size, int *line, int *column) {
    if (!resolver || !filename || !line) {
        LOG_ERROR("Invalid parameters for address_resolver_get_line_info");
        return -1;
    }
    
    source_location_t location;
    if (address_resolver_get_source_location(resolver, address, &location) == 0) {
        strncpy(filename, location.file, filename_size - 1);
        filename[filename_size - 1] = '\0';
        *line = location.line;
        if (column) {
            *column = location.column;
        }
        return 0;
    }
    
    return -1;
}

// Get memory mappings
int address_resolver_get_mappings(address_resolver_t *resolver,
                                memory_mapping_t **mappings, int *count) {
    if (!resolver || !mappings || !count) {
        LOG_ERROR("Invalid parameters for address_resolver_get_mappings");
        return -1;
    }
    
    pthread_mutex_lock(&resolver->mutex);
    
    if (resolver->mapping_count > 0) {
        *mappings = MALLOC_LOGGED(resolver->mapping_count * sizeof(memory_mapping_t));
        if (*mappings) {
            memcpy(*mappings, resolver->mappings,
                   resolver->mapping_count * sizeof(memory_mapping_t));
            *count = resolver->mapping_count;
        } else {
            *count = 0;
            pthread_mutex_unlock(&resolver->mutex);
            return -1;
        }
    } else {
        *mappings = NULL;
        *count = 0;
    }
    
    pthread_mutex_unlock(&resolver->mutex);
    
    return 0;
}

// Free mappings
void address_resolver_free_mappings(memory_mapping_t *mappings) {
    if (mappings) {
        FREE_LOGGED(mappings);
    }
}

// Find mapping for address
const memory_mapping_t* address_resolver_find_mapping(address_resolver_t *resolver,
                                                    uint64_t address) {
    if (!resolver) return NULL;
    
    pthread_mutex_lock(&resolver->mutex);
    
    for (int i = 0; i < resolver->mapping_count; i++) {
        if (address >= resolver->mappings[i].start_addr &&
            address < resolver->mappings[i].end_addr) {
            pthread_mutex_unlock(&resolver->mutex);
            return &resolver->mappings[i];
        }
    }
    
    pthread_mutex_unlock(&resolver->mutex);
    return NULL;
}

// Clear cache
void address_resolver_clear_cache(address_resolver_t *resolver) {
    if (!resolver) return;
    
    LOG_INFO("Clearing address resolver cache");
    
    pthread_mutex_lock(&resolver->mutex);
    
    for (size_t i = 0; i < resolver->cache_size; i++) {
        cache_entry_t *entry = resolver->cache_table[i];
        while (entry) {
            cache_entry_t *next = entry->next;
            FREE_LOGGED(entry);
            entry = next;
        }
        resolver->cache_table[i] = NULL;
    }
    
    resolver->cache_entries = 0;
    
    pthread_mutex_unlock(&resolver->mutex);
}

// Set cache size
int address_resolver_set_cache_size(address_resolver_t *resolver, size_t max_entries) {
    if (!resolver) return -1;
    
    pthread_mutex_lock(&resolver->mutex);
    resolver->max_cache_entries = max_entries;
    pthread_mutex_unlock(&resolver->mutex);
    
    LOG_INFO("Set cache size to %zu entries", max_entries);
    return 0;
}

// Demangle C++ names
const char* address_resolver_demangle(const char *mangled_name,
                                    char *buffer, size_t buffer_size) {
    if (!mangled_name || !buffer || buffer_size == 0) {
        return mangled_name;
    }
    
    // Since we can't use C++ demangling in pure C, just copy the name
    strncpy(buffer, mangled_name, buffer_size - 1);
    buffer[buffer_size - 1] = '\0';
    
    // Check if it looks like a C++ mangled name and add a hint
    if (strncmp(mangled_name, "_Z", 2) == 0) {
        // It's probably a C++ mangled name
        size_t current_len = strlen(buffer);
        const char *suffix = " <mangled>";
        size_t suffix_len = strlen(suffix);
        
        // Add suffix if there's room
        if (current_len + suffix_len < buffer_size - 1) {
            strcat(buffer, suffix);
        }
    }
    
    return buffer;
}

// Print symbol info
void address_resolver_print_symbol(const symbol_info_t *symbol) {
    if (!symbol) return;
    
    printf("Symbol at 0x%lx:\n", symbol->address);
    printf("  Name: %s\n", symbol->name);
    if (strlen(symbol->demangled_name) > 0 &&
        strcmp(symbol->name, symbol->demangled_name) != 0) {
        printf("  Demangled: %s\n", symbol->demangled_name);
    }
    if (strlen(symbol->location.file) > 0) {
        printf("  Location: %s:%d\n", symbol->location.file, symbol->location.line);
    }
    printf("  Type: %s%s\n",
           symbol->is_function ? "function" : "data",
           symbol->is_inlined ? " (inlined)" : "");
}

// Print memory mapping
void address_resolver_print_mapping(const memory_mapping_t *mapping) {
    if (!mapping) return;
    
    printf("0x%016lx-0x%016lx %c%c%c %08lx %s\n",
           mapping->start_addr, mapping->end_addr,
           mapping->is_writable ? 'w' : '-',
           mapping->is_executable ? 'x' : '-',
           mapping->is_shared ? 's' : 'p',
           mapping->file_offset,
           mapping->pathname);
}
