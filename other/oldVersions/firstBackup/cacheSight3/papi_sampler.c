#include "papi_sampler.h"
#include <papi.h>
#include <pthread.h>

// Internal PAPI sampler structure
struct papi_sampler {
    papi_config_t config;
    int event_set;               // PAPI event set
    int *event_codes;            // PAPI event codes
    long long *values;           // Counter values
    
    cache_miss_sample_t *samples;  // Collected samples
    int sample_count;            // Current number of samples
    int sample_capacity;         // Allocated capacity
    
    bool is_running;
    uint64_t start_time;
    uint64_t stop_time;
    
    pthread_t sampling_thread;
    pthread_mutex_t samples_mutex;
    bool stop_requested;
};

// Global PAPI initialization flag
static bool g_papi_initialized = false;
static pthread_mutex_t g_papi_init_mutex = PTHREAD_MUTEX_INITIALIZER;

// Overflow handler for PAPI
static void papi_overflow_handler(int event_set, void *address, 
                                 long long overflow_vector, void *context) {
    papi_sampler_t *sampler = (papi_sampler_t *)context;
    
    if (!sampler || sampler->sample_count >= sampler->config.base_config.max_samples) {
        return;
    }
    
    pthread_mutex_lock(&sampler->samples_mutex);
    
    if (sampler->sample_count < sampler->config.base_config.max_samples) {
        cache_miss_sample_t *sample = &sampler->samples[sampler->sample_count];
        
        // Fill sample data
        sample->instruction_addr = (uint64_t)address;
        sample->memory_addr = 0;  // PAPI doesn't provide data address easily
        sample->timestamp = get_timestamp() * 1e9;  // Convert to nanoseconds
        sample->cpu_id = sched_getcpu();
        sample->tid = gettid();
        
        // Determine which cache level based on overflow vector
        // This is simplified - real implementation would map events properly
        if (overflow_vector & 0x1) {
            sample->cache_level_missed = 1;  // L1
        } else if (overflow_vector & 0x2) {
            sample->cache_level_missed = 2;  // L2
        } else {
            sample->cache_level_missed = 3;  // L3
        }
        
        sample->is_write = false;  // Would need to track separately
        sample->access_size = 8;   // Default
        sample->latency_cycles = 0; // Not directly available
        
        sampler->sample_count++;
        
        if (sampler->sample_count % 1000 == 0) {
            LOG_DEBUG("PAPI collected %d samples", sampler->sample_count);
        }
    }
    
    pthread_mutex_unlock(&sampler->samples_mutex);
}

// Initialize PAPI library
static int init_papi(void) {
    pthread_mutex_lock(&g_papi_init_mutex);
    
    if (g_papi_initialized) {
        pthread_mutex_unlock(&g_papi_init_mutex);
        return PAPI_OK;
    }
    
    int version = PAPI_library_init(PAPI_VER_CURRENT);
    if (version != PAPI_VER_CURRENT) {
        LOG_ERROR("PAPI library version mismatch: expected %d, got %d",
                  PAPI_VER_CURRENT, version);
        pthread_mutex_unlock(&g_papi_init_mutex);
        return PAPI_EVERS;
    }
    
    // Initialize thread support
    int ret = PAPI_thread_init(pthread_self);
    if (ret != PAPI_OK) {
        LOG_ERROR("Failed to initialize PAPI thread support: %s",
                  PAPI_strerror(ret));
        pthread_mutex_unlock(&g_papi_init_mutex);
        return ret;
    }
    
    g_papi_initialized = true;
    LOG_INFO("PAPI library initialized successfully");
    
    pthread_mutex_unlock(&g_papi_init_mutex);
    return PAPI_OK;
}

// Sampling thread for PAPI
static void* papi_sampling_thread(void *arg) {
    papi_sampler_t *sampler = (papi_sampler_t *)arg;
    
    LOG_INFO("PAPI sampling thread started");
    
    // Register thread with PAPI
    if (PAPI_register_thread() != PAPI_OK) {
        LOG_ERROR("Failed to register thread with PAPI");
        return NULL;
    }
    
    // Attach to event set
    if (PAPI_attach(sampler->event_set, 0) != PAPI_OK) {
        LOG_ERROR("Failed to attach PAPI event set");
        return NULL;
    }
    
    while (!sampler->stop_requested) {
        // Read counters periodically
        if (PAPI_read(sampler->event_set, sampler->values) == PAPI_OK) {
            // Log counter values periodically
            if (sampler->sample_count % 10000 == 0) {
                for (int i = 0; i < sampler->config.num_events; i++) {
                    LOG_DEBUG("Event %s: %lld", 
                             sampler->config.event_names[i], 
                             sampler->values[i]);
                }
            }
        }
        
        // Check if we've collected enough samples
        if (sampler->sample_count >= sampler->config.base_config.max_samples) {
            LOG_INFO("Maximum samples reached (%d)", 
                    sampler->config.base_config.max_samples);
            break;
        }
        
        // Check duration limit
        if (sampler->config.base_config.sampling_duration > 0) {
            double elapsed = get_timestamp() - sampler->start_time;
            if (elapsed >= sampler->config.base_config.sampling_duration) {
                LOG_INFO("Sampling duration reached (%.2f seconds)", elapsed);
                break;
            }
        }
        
        // Sleep briefly
        usleep(10000);  // 10ms
    }
    
    // Detach from event set
    PAPI_detach(sampler->event_set);
    
    // Unregister thread
    PAPI_unregister_thread();
    
    LOG_INFO("PAPI sampling thread stopped");
    return NULL;
}

// Create PAPI sampler
papi_sampler_t* papi_sampler_create(const papi_config_t *config) {
    if (!config) {
        LOG_ERROR("NULL config provided to papi_sampler_create");
        return NULL;
    }
    
    if (init_papi() != PAPI_OK) {
        return NULL;
    }
    
    papi_sampler_t *sampler = CALLOC_LOGGED(1, sizeof(papi_sampler_t));
    if (!sampler) {
        LOG_ERROR("Failed to allocate PAPI sampler");
        return NULL;
    }
    
    sampler->config = *config;
    pthread_mutex_init(&sampler->samples_mutex, NULL);
    
    // Allocate arrays
    sampler->event_codes = CALLOC_LOGGED(config->num_events, sizeof(int));
    sampler->values = CALLOC_LOGGED(config->num_events, sizeof(long long));
    
    if (!sampler->event_codes || !sampler->values) {
        LOG_ERROR("Failed to allocate PAPI arrays");
        papi_sampler_destroy(sampler);
        return NULL;
    }
    
    // Convert event names to codes
    for (int i = 0; i < config->num_events; i++) {
        int ret = PAPI_event_name_to_code((char*)config->event_names[i], 
                                          &sampler->event_codes[i]);
        if (ret != PAPI_OK) {
            LOG_ERROR("Failed to convert event name %s: %s",
                     config->event_names[i], PAPI_strerror(ret));
            papi_sampler_destroy(sampler);
            return NULL;
        }
        LOG_DEBUG("Event %s mapped to code 0x%x", 
                 config->event_names[i], sampler->event_codes[i]);
    }
    
    // Create event set
    sampler->event_set = PAPI_NULL;
    if (PAPI_create_eventset(&sampler->event_set) != PAPI_OK) {
        LOG_ERROR("Failed to create PAPI event set");
        papi_sampler_destroy(sampler);
        return NULL;
    }
    
    // Enable multiplexing if requested
    if (config->use_multiplexing) {
        if (PAPI_set_multiplex(sampler->event_set) != PAPI_OK) {
            LOG_WARNING("Failed to enable multiplexing");
        }
    }
    
    // Add events to event set
    for (int i = 0; i < config->num_events; i++) {
        int ret = PAPI_add_event(sampler->event_set, sampler->event_codes[i]);
        if (ret != PAPI_OK) {
            LOG_ERROR("Failed to add event %s: %s",
                     config->event_names[i], PAPI_strerror(ret));
            // Continue with other events
        }
    }
    
    // Setup overflow handling for first event
    if (config->overflow_threshold > 0 && config->num_events > 0) {
        int ret = PAPI_overflow(sampler->event_set, sampler->event_codes[0],
                               config->overflow_threshold, 0,
                               papi_overflow_handler, sampler);
        if (ret != PAPI_OK) {
            LOG_WARNING("Failed to setup overflow handling: %s", PAPI_strerror(ret));
        }
    }
    
    // Allocate sample buffer
    sampler->sample_capacity = config->base_config.max_samples;
    sampler->samples = CALLOC_LOGGED(sampler->sample_capacity, sizeof(cache_miss_sample_t));
    if (!sampler->samples) {
        LOG_ERROR("Failed to allocate sample buffer");
        papi_sampler_destroy(sampler);
        return NULL;
    }
    
    LOG_INFO("Created PAPI sampler with %d events", config->num_events);
    return sampler;
}

// Destroy PAPI sampler
void papi_sampler_destroy(papi_sampler_t *sampler) {
    if (!sampler) return;
    
    LOG_INFO("Destroying PAPI sampler");
    
    // Stop sampling if running
    if (sampler->is_running) {
        papi_sampler_stop(sampler);
    }
    
    // Cleanup PAPI resources
    if (sampler->event_set != PAPI_NULL) {
        PAPI_cleanup_eventset(sampler->event_set);
        PAPI_destroy_eventset(&sampler->event_set);
    }
    
    // Free arrays
    if (sampler->event_codes) {
        FREE_LOGGED(sampler->event_codes);
    }
    if (sampler->values) {
        FREE_LOGGED(sampler->values);
    }
    if (sampler->samples) {
        FREE_LOGGED(sampler->samples);
    }
    
    pthread_mutex_destroy(&sampler->samples_mutex);
    FREE_LOGGED(sampler);
}

// Start PAPI sampling
int papi_sampler_start(papi_sampler_t *sampler) {
    if (!sampler) {
        LOG_ERROR("NULL sampler in papi_sampler_start");
        return -1;
    }
    
    if (sampler->is_running) {
        LOG_WARNING("PAPI sampler already running");
        return 0;
    }
    
    LOG_INFO("Starting PAPI sampling");
    
    // Reset state
    sampler->sample_count = 0;
    sampler->stop_requested = false;
    sampler->start_time = get_timestamp();
    
    // Reset counters
    if (PAPI_reset(sampler->event_set) != PAPI_OK) {
        LOG_ERROR("Failed to reset PAPI counters");
        return -1;
    }
    
    // Start counters
    if (PAPI_start(sampler->event_set) != PAPI_OK) {
        LOG_ERROR("Failed to start PAPI counters");
        return -1;
    }
    
    // Start sampling thread
    if (pthread_create(&sampler->sampling_thread, NULL,
                      papi_sampling_thread, sampler) != 0) {
        LOG_ERROR("Failed to create PAPI sampling thread");
        PAPI_stop(sampler->event_set, sampler->values);
        return -1;
    }
    
    sampler->is_running = true;
    LOG_INFO("PAPI sampling started successfully");
    return 0;
}

// Stop PAPI sampling
int papi_sampler_stop(papi_sampler_t *sampler) {
    if (!sampler) {
        LOG_ERROR("NULL sampler in papi_sampler_stop");
        return -1;
    }
    
    if (!sampler->is_running) {
        LOG_WARNING("PAPI sampler not running");
        return 0;
    }
    
    LOG_INFO("Stopping PAPI sampling");
    
    // Signal thread to stop
    sampler->stop_requested = true;
    
    // Wait for thread to finish
    pthread_join(sampler->sampling_thread, NULL);
    
    // Stop counters
    if (PAPI_stop(sampler->event_set, sampler->values) != PAPI_OK) {
        LOG_ERROR("Failed to stop PAPI counters");
    }
    
    sampler->stop_time = get_timestamp();
    sampler->is_running = false;
    
    LOG_INFO("PAPI sampling stopped. Collected %d samples in %.2f seconds",
             sampler->sample_count, sampler->stop_time - sampler->start_time);
    
    // Log final counter values
    for (int i = 0; i < sampler->config.num_events; i++) {
        LOG_INFO("Final %s: %lld", 
                 sampler->config.event_names[i], 
                 sampler->values[i]);
    }
    
    return 0;
}

// Check if sampler is running
bool papi_sampler_is_running(const papi_sampler_t *sampler) {
    return sampler ? sampler->is_running : false;
}

// Get collected samples
int papi_sampler_get_samples(papi_sampler_t *sampler,
                            cache_miss_sample_t **samples, int *count) {
    if (!sampler || !samples || !count) {
        LOG_ERROR("Invalid parameters for papi_sampler_get_samples");
        return -1;
    }
    
    pthread_mutex_lock(&sampler->samples_mutex);
    
    *count = sampler->sample_count;
    if (sampler->sample_count > 0) {
        *samples = MALLOC_LOGGED(sampler->sample_count * sizeof(cache_miss_sample_t));
        if (*samples) {
            memcpy(*samples, sampler->samples,
                   sampler->sample_count * sizeof(cache_miss_sample_t));
        } else {
            *count = 0;
            pthread_mutex_unlock(&sampler->samples_mutex);
            return -1;
        }
    } else {
        *samples = NULL;
    }
    
    pthread_mutex_unlock(&sampler->samples_mutex);
    
    LOG_INFO("Retrieved %d PAPI samples", *count);
    return 0;
}

// Check PAPI availability
int papi_check_availability(void) {
    if (init_papi() != PAPI_OK) {
        return -1;
    }
    
    // Check if we have any hardware counters
    int num_counters = PAPI_num_hwctrs();
    if (num_counters <= 0) {
        LOG_ERROR("No hardware counters available");
        return -1;
    }
    
    LOG_INFO("PAPI available with %d hardware counters", num_counters);
    
    // Check for specific cache events
    int event_code;
    if (PAPI_event_name_to_code("PAPI_L1_DCM", &event_code) == PAPI_OK) {
        LOG_INFO("L1 data cache miss event available");
    }
    if (PAPI_event_name_to_code("PAPI_L2_DCM", &event_code) == PAPI_OK) {
        LOG_INFO("L2 data cache miss event available");
    }
    if (PAPI_event_name_to_code("PAPI_L3_TCM", &event_code) == PAPI_OK) {
        LOG_INFO("L3 total cache miss event available");
    }
    
    return 0;
}

// List available cache events
int papi_list_cache_events(char *buffer, size_t buffer_size) {
    if (!buffer || buffer_size == 0) {
        LOG_ERROR("Invalid parameters for papi_list_cache_events");
        return -1;
    }
    
    if (init_papi() != PAPI_OK) {
        return -1;
    }
    
    buffer[0] = '\0';
    size_t offset = 0;
    
    // Common cache events to check
    const char *cache_events[] = {
        "PAPI_L1_DCM", "PAPI_L1_ICM", "PAPI_L1_TCM",
        "PAPI_L2_DCM", "PAPI_L2_ICM", "PAPI_L2_TCM",
        "PAPI_L3_DCM", "PAPI_L3_ICM", "PAPI_L3_TCM",
        "PAPI_TLB_DM", "PAPI_TLB_IM",
        NULL
    };
    
    for (int i = 0; cache_events[i] != NULL; i++) {
        int event_code;
        if (PAPI_event_name_to_code((char*)cache_events[i], &event_code) == PAPI_OK) {
            offset += snprintf(buffer + offset, buffer_size - offset,
                              "%s\n", cache_events[i]);
        }
    }
    
    return 0;
}

// Get default PAPI configuration
papi_config_t papi_config_default(void) {
    papi_config_t config = {
        .base_config = perf_config_default(),
        .overflow_threshold = 100000,  // Overflow every 100K events
        .use_multiplexing = false,
        .num_events = 3,
        .event_names = {
            "PAPI_L1_DCM",  // L1 data cache misses
            "PAPI_L2_DCM",  // L2 data cache misses
            "PAPI_L3_TCM"   // L3 total cache misses
        }
    };
    
    LOG_DEBUG("Created default PAPI configuration");
    return config;
}

// Get statistics
int papi_sampler_get_stats(const papi_sampler_t *sampler, perf_stats_t *stats) {
    if (!sampler || !stats) {
        LOG_ERROR("Invalid parameters for papi_sampler_get_stats");
        return -1;
    }
    
    memset(stats, 0, sizeof(perf_stats_t));
    
    pthread_mutex_lock((pthread_mutex_t*)&sampler->samples_mutex);
    
    stats->total_samples = sampler->sample_count;
    
    // Count samples by cache level
    for (int i = 0; i < sampler->sample_count; i++) {
        switch (sampler->samples[i].cache_level_missed) {
            case 1: stats->l1_misses++; break;
            case 2: stats->l2_misses++; break;
            case 3: stats->l3_misses++; break;
            default: stats->llc_misses++; break;
        }
    }
    
    if (sampler->is_running) {
        stats->sampling_duration_ns = (get_timestamp() - sampler->start_time) * 1e9;
    } else if (sampler->stop_time > sampler->start_time) {
        stats->sampling_duration_ns = (sampler->stop_time - sampler->start_time) * 1e9;
    }
    
    pthread_mutex_unlock((pthread_mutex_t*)&sampler->samples_mutex);
    
    return 0;
}
