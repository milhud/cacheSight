#include "sample_collector.h"
#include <stdlib.h>

// Hash table entry for hotspot lookup
typedef struct hotspot_entry {
    uint64_t key;                   // Hash key (instruction address or function)
    cache_hotspot_t hotspot;        // Hotspot data
    struct hotspot_entry *next;     // Next entry in chain
} hotspot_entry_t;

// Internal collector structure
struct sample_collector {
    collector_config_t config;
    cache_info_t cache_info;
    
    // Hash table for hotspot lookup
    hotspot_entry_t **hotspot_table;
    size_t table_size;
    size_t hotspot_count;
    
    // Raw samples buffer
    cache_miss_sample_t *all_samples;
    size_t all_samples_count;
    size_t all_samples_capacity;
    
    // Statistics
    collector_stats_t stats;
    
    pthread_mutex_t mutex;
};

// Hash function for addresses
static uint64_t hash_address(uint64_t addr, size_t table_size) {
    // Simple multiplicative hash
    return (addr * 2654435761ULL) % table_size;
}

// Create sample collector
sample_collector_t* sample_collector_create(const collector_config_t *config,
                                           const cache_info_t *cache_info) {
    if (!config || !cache_info) {
        LOG_ERROR("NULL parameters for sample_collector_create");
        return NULL;
    }
    
    sample_collector_t *collector = CALLOC_LOGGED(1, sizeof(sample_collector_t));
    if (!collector) {
        LOG_ERROR("Failed to allocate sample collector");
        return NULL;
    }
    
    collector->config = *config;
    collector->cache_info = *cache_info;
    pthread_mutex_init(&collector->mutex, NULL);
    
    // Initialize hash table
    collector->table_size = config->max_hotspots * 4;  // 4x for lower load factor
    collector->hotspot_table = CALLOC_LOGGED(collector->table_size, sizeof(hotspot_entry_t*));
    if (!collector->hotspot_table) {
        LOG_ERROR("Failed to allocate hotspot table");
        sample_collector_destroy(collector);
        return NULL;
    }
    
    // Initialize sample buffer
    collector->all_samples_capacity = 10000;
    collector->all_samples = CALLOC_LOGGED(collector->all_samples_capacity,
                                           sizeof(cache_miss_sample_t));
    if (!collector->all_samples) {
        LOG_ERROR("Failed to allocate sample buffer");
        sample_collector_destroy(collector);
        return NULL;
    }
    
    LOG_INFO("Created sample collector with table size %zu", collector->table_size);
    return collector;
}

// Destroy sample collector
void sample_collector_destroy(sample_collector_t *collector) {
    if (!collector) return;
    
    LOG_INFO("Destroying sample collector");
    
    // Free hash table entries
    if (collector->hotspot_table) {
        for (size_t i = 0; i < collector->table_size; i++) {
            hotspot_entry_t *entry = collector->hotspot_table[i];
            while (entry) {
                hotspot_entry_t *next = entry->next;
                if (entry->hotspot.samples) {
                    FREE_LOGGED(entry->hotspot.samples);
                }
                FREE_LOGGED(entry);
                entry = next;
            }
        }
        FREE_LOGGED(collector->hotspot_table);
    }
    
    if (collector->all_samples) {
        FREE_LOGGED(collector->all_samples);
    }
    
    pthread_mutex_destroy(&collector->mutex);
    FREE_LOGGED(collector);
}

// Add samples to collector
int sample_collector_add_samples(sample_collector_t *collector,
                                const cache_miss_sample_t *samples, int count) {
    if (!collector || !samples || count <= 0) {
        LOG_ERROR("Invalid parameters for sample_collector_add_samples");
        return -1;
    }
    
    LOG_INFO("Adding %d samples to collector", count);
    
    pthread_mutex_lock(&collector->mutex);
    
    // Grow buffer if needed
    while (collector->all_samples_count + count > collector->all_samples_capacity) {
        size_t new_capacity = collector->all_samples_capacity * 2;
        cache_miss_sample_t *new_buffer = realloc(collector->all_samples,
                                                  new_capacity * sizeof(cache_miss_sample_t));
        if (!new_buffer) {
            LOG_ERROR("Failed to grow sample buffer");
            pthread_mutex_unlock(&collector->mutex);
            return -1;
        }
        collector->all_samples = new_buffer;
        collector->all_samples_capacity = new_capacity;
        LOG_DEBUG("Grew sample buffer to %zu", new_capacity);
    }
    
    // Copy samples
    memcpy(&collector->all_samples[collector->all_samples_count],
           samples, count * sizeof(cache_miss_sample_t));
    collector->all_samples_count += count;
    collector->stats.total_samples_processed += count;
    
    pthread_mutex_unlock(&collector->mutex);
    
    LOG_INFO("Total samples in collector: %zu", collector->all_samples_count);
    return 0;
}

// Add single sample
int sample_collector_add_sample(sample_collector_t *collector,
                               const cache_miss_sample_t *sample) {
    return sample_collector_add_samples(collector, sample, 1);
}

// Find or create hotspot entry
static hotspot_entry_t* find_or_create_hotspot(sample_collector_t *collector,
                                               uint64_t key,
                                               const source_location_t *location) {
    uint64_t hash = hash_address(key, collector->table_size);
    hotspot_entry_t *entry = collector->hotspot_table[hash];
    
    // Search for existing entry
    while (entry) {
        if (entry->key == key) {
            return entry;
        }
        entry = entry->next;
    }
    
    // Create new entry
    if (collector->hotspot_count >= collector->config.max_hotspots) {
        LOG_WARNING("Maximum hotspot count reached");
        return NULL;
    }
    
    entry = CALLOC_LOGGED(1, sizeof(hotspot_entry_t));
    if (!entry) {
        LOG_ERROR("Failed to allocate hotspot entry");
        return NULL;
    }
    
    entry->key = key;
    entry->hotspot.location = *location;
    entry->hotspot.sample_capacity = 100;
    entry->hotspot.samples = CALLOC_LOGGED(entry->hotspot.sample_capacity,
                                           sizeof(cache_miss_sample_t));
    if (!entry->hotspot.samples) {
        FREE_LOGGED(entry);
        return NULL;
    }
    
    // Add to hash table
    entry->next = collector->hotspot_table[hash];
    collector->hotspot_table[hash] = entry;
    collector->hotspot_count++;
    
    LOG_DEBUG("Created new hotspot for key 0x%lx at %s:%d",
              key, location->file, location->line);
    
    return entry;
}

// Process samples and create hotspots
int sample_collector_process(sample_collector_t *collector) {
    if (!collector) {
        LOG_ERROR("NULL collector in sample_collector_process");
        return -1;
    }
    
    LOG_INFO("Processing %zu samples into hotspots", collector->all_samples_count);
    
    pthread_mutex_lock(&collector->mutex);
    
    // Process each sample
    for (size_t i = 0; i < collector->all_samples_count; i++) {
        cache_miss_sample_t *sample = &collector->all_samples[i];
        
        // Determine key based on aggregation mode
        uint64_t key;
        if (collector->config.aggregate_by_function) {
            // Aggregate by function (simplified - would need symbol table)
            key = sample->instruction_addr & ~0xFFFULL;  // Align to 4KB
        } else {
            // Aggregate by instruction
            key = sample->instruction_addr;
        }
        
        // Find or create hotspot
        hotspot_entry_t *entry = find_or_create_hotspot(collector, key, 
                                                        &sample->source_loc);
        if (!entry) continue;
        
        cache_hotspot_t *hotspot = &entry->hotspot;
        
        // Update hotspot statistics
        hotspot->total_misses++;
        hotspot->total_accesses++;  // Simplified - assumes miss = access
        
        // Update address range
        if (hotspot->sample_count == 0) {
            hotspot->address_range_start = sample->memory_addr;
            hotspot->address_range_end = sample->memory_addr;
        } else {
            if (sample->memory_addr < hotspot->address_range_start) {
                hotspot->address_range_start = sample->memory_addr;
            }
            if (sample->memory_addr > hotspot->address_range_end) {
                hotspot->address_range_end = sample->memory_addr;
            }
        }
        
        // Update cache level statistics
        if (sample->cache_level_missed >= 1 && sample->cache_level_missed <= 4) {
            hotspot->cache_levels_affected[sample->cache_level_missed - 1]++;
        }
        
        // Add sample to hotspot
        if (hotspot->sample_count < hotspot->sample_capacity) {
            // Grow if needed
            if (hotspot->sample_count == hotspot->sample_capacity) {
                size_t new_capacity = hotspot->sample_capacity * 2;
                cache_miss_sample_t *new_samples = realloc(hotspot->samples,
                    new_capacity * sizeof(cache_miss_sample_t));
                if (new_samples) {
                    hotspot->samples = new_samples;
                    hotspot->sample_capacity = new_capacity;
                }
            }
            
            if (hotspot->sample_count < hotspot->sample_capacity) {
                hotspot->samples[hotspot->sample_count++] = *sample;
            }
        }
        
        // Update latency
        hotspot->avg_latency_cycles = 
            (hotspot->avg_latency_cycles * (hotspot->sample_count - 1) + 
             sample->latency_cycles) / hotspot->sample_count;
    }
    
    // Calculate miss rates and detect patterns
    sample_collector_analyze_patterns(collector);
    
    // Detect false sharing if enabled
    if (collector->config.detect_false_sharing) {
        sample_collector_detect_false_sharing(collector);
    }
    
    pthread_mutex_unlock(&collector->mutex);
    
    LOG_INFO("Created %zu hotspots from samples", collector->hotspot_count);
    return 0;
}

// Analyze access patterns in hotspots
int sample_collector_analyze_patterns(sample_collector_t *collector) {
    if (!collector) return -1;
    
    LOG_DEBUG("Analyzing access patterns in hotspots");
    
    // Iterate through all hotspots
    for (size_t i = 0; i < collector->table_size; i++) {
        hotspot_entry_t *entry = collector->hotspot_table[i];
        while (entry) {
            cache_hotspot_t *hotspot = &entry->hotspot;
            
            if (hotspot->sample_count < collector->config.min_samples_per_hotspot) {
                entry = entry->next;
                continue;
            }
            
            // Calculate miss rate
            if (hotspot->total_accesses > 0) {
                hotspot->miss_rate = (double)hotspot->total_misses / 
                                    hotspot->total_accesses;
            }
            
            // Analyze access pattern from samples
            if (hotspot->sample_count >= 2) {
                // Sort samples by memory address
                qsort(hotspot->samples, hotspot->sample_count,
                      sizeof(cache_miss_sample_t),
                      [](const void *a, const void *b) {
                          const cache_miss_sample_t *sa = (const cache_miss_sample_t *)a;
                          const cache_miss_sample_t *sb = (const cache_miss_sample_t *)b;
                          if (sa->memory_addr < sb->memory_addr) return -1;
                          if (sa->memory_addr > sb->memory_addr) return 1;
                          return 0;
                      });
                
                // Detect stride pattern
                uint64_t total_stride = 0;
                int stride_count = 0;
                
                for (size_t j = 1; j < hotspot->sample_count; j++) {
                    uint64_t stride = hotspot->samples[j].memory_addr - 
                                     hotspot->samples[j-1].memory_addr;
                    if (stride > 0 && stride < 4096) {  // Reasonable stride
                        total_stride += stride;
                        stride_count++;
                    }
                }
                
                if (stride_count > hotspot->sample_count / 2) {
                    uint64_t avg_stride = total_stride / stride_count;
                    if (avg_stride == 1) {
                        hotspot->dominant_pattern = SEQUENTIAL;
                    } else if (avg_stride <= 64) {
                        hotspot->dominant_pattern = STRIDED;
                    } else {
                        hotspot->dominant_pattern = RANDOM;
                    }
                } else {
                    hotspot->dominant_pattern = RANDOM;
                }
            }
            
            LOG_DEBUG("Hotspot at %s:%d - pattern: %s, miss_rate: %.2f%%",
                      hotspot->location.file, hotspot->location.line,
                      access_pattern_to_string(hotspot->dominant_pattern),
                      hotspot->miss_rate * 100);
            
            entry = entry->next;
        }
    }
    
    return 0;
}

// Detect false sharing
int sample_collector_detect_false_sharing(sample_collector_t *collector) {
    if (!collector) return -1;
    
    LOG_DEBUG("Detecting false sharing in hotspots");
    
    int cache_line_size = collector->cache_info.levels[0].line_size;
    int false_sharing_count = 0;
    
    // Check each hotspot for false sharing indicators
    for (size_t i = 0; i < collector->table_size; i++) {
        hotspot_entry_t *entry = collector->hotspot_table[i];
        while (entry) {
            cache_hotspot_t *hotspot = &entry->hotspot;
            
            // False sharing indicators:
            // 1. Multiple CPUs accessing same cache line
            // 2. High miss rate with small address range
            // 3. Write accesses from different CPUs
            
            if (hotspot->sample_count < 10) {
                entry = entry->next;
                continue;
            }
            
            // Check CPU diversity
            int cpu_mask = 0;
            int write_cpu_mask = 0;
            
            for (size_t j = 0; j < hotspot->sample_count; j++) {
                cpu_mask |= (1 << hotspot->samples[j].cpu_id);
                if (hotspot->samples[j].is_write) {
                    write_cpu_mask |= (1 << hotspot->samples[j].cpu_id);
                }
            }
            
            // Count unique CPUs
            int cpu_count = __builtin_popcount(cpu_mask);
            int write_cpu_count = __builtin_popcount(write_cpu_mask);
            
            // Check if address range fits in few cache lines
            uint64_t range = hotspot->address_range_end - hotspot->address_range_start;
            int cache_lines = (range / cache_line_size) + 1;
            
            // Detect false sharing
            if (cpu_count >= 2 && cache_lines <= 2 && hotspot->miss_rate > 0.3) {
                hotspot->is_false_sharing = true;
                false_sharing_count++;
                
                LOG_WARNING("Potential false sharing detected at %s:%d - "
                           "%d CPUs, %d cache lines, %.1f%% miss rate",
                           hotspot->location.file, hotspot->location.line,
                           cpu_count, cache_lines, hotspot->miss_rate * 100);
            }
            
            entry = entry->next;
        }
    }
    
    LOG_INFO("Detected %d potential false sharing hotspots", false_sharing_count);
    collector->stats.cache_line_conflicts = false_sharing_count;
    
    return false_sharing_count;
}

// Get hotspots
int sample_collector_get_hotspots(sample_collector_t *collector,
                                 cache_hotspot_t **hotspots, int *count) {
    if (!collector || !hotspots || !count) {
        LOG_ERROR("Invalid parameters for sample_collector_get_hotspots");
        return -1;
    }
    
    pthread_mutex_lock(&collector->mutex);
    
    // Count significant hotspots
    int hotspot_count = 0;
    for (size_t i = 0; i < collector->table_size; i++) {
        hotspot_entry_t *entry = collector->hotspot_table[i];
        while (entry) {
            if (entry->hotspot.sample_count >= collector->config.min_samples_per_hotspot &&
                entry->hotspot.miss_rate >= collector->config.hotspot_threshold) {
                hotspot_count++;
            }
            entry = entry->next;
        }
    }
    
    if (hotspot_count == 0) {
        *hotspots = NULL;
        *count = 0;
        pthread_mutex_unlock(&collector->mutex);
        return 0;
    }
    
    // Allocate array
    *hotspots = CALLOC_LOGGED(hotspot_count, sizeof(cache_hotspot_t));
    if (!*hotspots) {
        LOG_ERROR("Failed to allocate hotspot array");
        pthread_mutex_unlock(&collector->mutex);
        return -1;
    }
    
    // Copy hotspots
    int idx = 0;
    for (size_t i = 0; i < collector->table_size; i++) {
        hotspot_entry_t *entry = collector->hotspot_table[i];
        while (entry && idx < hotspot_count) {
            if (entry->hotspot.sample_count >= collector->config.min_samples_per_hotspot &&
                entry->hotspot.miss_rate >= collector->config.hotspot_threshold) {
                
                // Deep copy hotspot
                (*hotspots)[idx] = entry->hotspot;
                
                // Copy samples
                if (entry->hotspot.sample_count > 0) {
                    (*hotspots)[idx].samples = MALLOC_LOGGED(
                        entry->hotspot.sample_count * sizeof(cache_miss_sample_t));
                    if ((*hotspots)[idx].samples) {
                        memcpy((*hotspots)[idx].samples, entry->hotspot.samples,
                               entry->hotspot.sample_count * sizeof(cache_miss_sample_t));
                    }
                }
                
                idx++;
            }
            entry = entry->next;
        }
    }
    
    *count = idx;
    
    // Sort by total misses
    qsort(*hotspots, *count, sizeof(cache_hotspot_t), compare_hotspots_by_misses);
    
    pthread_mutex_unlock(&collector->mutex);
    
    LOG_INFO("Retrieved %d significant hotspots", *count);
    return 0;
}

// Free hotspots
void sample_collector_free_hotspots(cache_hotspot_t *hotspots, int count) {
    if (!hotspots) return;
    
    for (int i = 0; i < count; i++) {
        if (hotspots[i].samples) {
            FREE_LOGGED(hotspots[i].samples);
        }
    }
    
    FREE_LOGGED(hotspots);
}

// Get statistics
int sample_collector_get_stats(const sample_collector_t *collector,
                              collector_stats_t *stats) {
    if (!collector || !stats) {
        LOG_ERROR("Invalid parameters for sample_collector_get_stats");
        return -1;
    }
    
    pthread_mutex_lock((pthread_mutex_t*)&collector->mutex);
    
    *stats = collector->stats;
    stats->hotspot_count = collector->hotspot_count;
    
    if (collector->hotspot_count > 0) {
        stats->avg_samples_per_hotspot = 
            (double)collector->all_samples_count / collector->hotspot_count;
    }
    
    // Count unique addresses and instructions
    // This is simplified - real implementation would use hash sets
    stats->total_unique_addresses = collector->all_samples_count / 10;
    stats->total_unique_instructions = collector->hotspot_count;
    
    pthread_mutex_unlock((pthread_mutex_t*)&collector->mutex);
    
    return 0;
}

// Print hotspots
void sample_collector_print_hotspots(const cache_hotspot_t *hotspots, int count) {
    printf("\n=== Top Cache Hotspots ===\n");
    
    for (int i = 0; i < count && i < 20; i++) {
        const cache_hotspot_t *h = &hotspots[i];
        
        printf("\n[%d] %s:%d in %s()\n", i + 1,
               h->location.file, h->location.line, h->location.function);
        printf("    Total misses: %lu (%.1f%% miss rate)\n",
               h->total_misses, h->miss_rate * 100);
        printf("    Avg latency: %.1f cycles\n", h->avg_latency_cycles);
        printf("    Pattern: %s\n", access_pattern_to_string(h->dominant_pattern));
        
        char range_str[64];
        format_bytes(h->address_range_end - h->address_range_start,
                    range_str, sizeof(range_str));
        printf("    Address range: 0x%lx - 0x%lx (%s)\n",
               h->address_range_start, h->address_range_end, range_str);
        
        printf("    Cache misses: L1=%d, L2=%d, L3=%d, LLC=%d\n",
               h->cache_levels_affected[0], h->cache_levels_affected[1],
               h->cache_levels_affected[2], h->cache_levels_affected[3]);
        
        if (h->is_false_sharing) {
            printf("    *** POTENTIAL FALSE SHARING DETECTED ***\n");
        }
    }
}

// Get default configuration
collector_config_t collector_config_default(void) {
    collector_config_t config = {
        .min_samples_per_hotspot = 10,
        .hotspot_threshold = 0.01,      // 1% miss rate
        .aggregate_by_function = false,
        .detect_false_sharing = true,
        .max_hotspots = 1000
    };
    
    return config;
}

// Comparison functions for sorting
int compare_hotspots_by_misses(const void *a, const void *b) {
    const cache_hotspot_t *ha = (const cache_hotspot_t *)a;
    const cache_hotspot_t *hb = (const cache_hotspot_t *)b;
    
    if (ha->total_misses > hb->total_misses) return -1;
    if (ha->total_misses < hb->total_misses) return 1;
    return 0;
}

int compare_hotspots_by_latency(const void *a, const void *b) {
    const cache_hotspot_t *ha = (const cache_hotspot_t *)a;
    const cache_hotspot_t *hb = (const cache_hotspot_t *)b;
    
    if (ha->avg_latency_cycles > hb->avg_latency_cycles) return -1;
    if (ha->avg_latency_cycles < hb->avg_latency_cycles) return 1;
    return 0;
}
