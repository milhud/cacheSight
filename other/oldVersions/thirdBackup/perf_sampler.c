#include "perf_sampler.h"
#include <perfmon/pfmlib.h>
#include <perfmon/pfmlib_perf_event.h>
#include <sys/mman.h>
#include <poll.h>
#include <signal.h>
//#include <linux/perf_event.h>

#define MMAP_PAGES 256  // Number of pages for ring buffer

// Internal perf sampler structure
struct perf_sampler {
    perf_config_t config;
    int *perf_fds;              // File descriptors for perf events
    int num_fds;                // Number of file descriptors
    void **mmap_buffers;        // Memory mapped ring buffers
    size_t mmap_size;           // Size of each mmap buffer
    
    cache_miss_sample_t *samples;  // Collected samples
    int sample_count;           // Current number of samples
    int sample_capacity;        // Allocated capacity
    
    bool is_running;
    uint64_t start_time;
    uint64_t stop_time;
    
    pthread_t sampling_thread;
    pthread_mutex_t samples_mutex;
    bool stop_requested;
};

// Global initialization flag
static bool g_pfm_initialized = false;

// Initialize libpfm4
static int init_libpfm(void) {
    if (g_pfm_initialized) {
        return PFM_SUCCESS;
    }
    
    int ret = pfm_initialize();
    if (ret != PFM_SUCCESS) {
        LOG_ERROR("Failed to initialize libpfm4: %s", pfm_strerror(ret));
        return ret;
    }
    
    g_pfm_initialized = true;
    LOG_INFO("libpfm4 initialized successfully");
    return PFM_SUCCESS;
}

// Helper to parse perf sample data
static void parse_perf_sample(struct perf_event_header *header, 
                             cache_miss_sample_t *sample) {
    // This is a simplified parser - real implementation would handle
    // various sample formats based on perf_event_attr configuration
    
    struct {
        struct perf_event_header header;
        uint64_t ip;
        uint64_t addr;
        uint64_t time;
        uint32_t cpu;
        uint32_t res;
    } *data = (void *)header;
    
    if (header->type == PERF_RECORD_SAMPLE) {
        sample->instruction_addr = data->ip;
        sample->memory_addr = data->addr;
        sample->timestamp = data->time;
        sample->cpu_id = data->cpu;
        
        // These would need proper parsing from sample data
        sample->cache_level_missed = 1;  // Default to L1
        sample->is_write = false;
        sample->access_size = 8;
        sample->latency_cycles = 0;
        sample->tid = 0;
    }
}

// Sampling thread function
static void* sampling_thread_func(void *arg) {
    perf_sampler_t *sampler = (perf_sampler_t *)arg;
    
    LOG_INFO("Sampling thread started");
    
    struct pollfd *poll_fds = calloc(sampler->num_fds, sizeof(struct pollfd));
    if (!poll_fds) {
        LOG_ERROR("Failed to allocate poll fds");
        return NULL;
    }
    
    // Setup poll descriptors
    for (int i = 0; i < sampler->num_fds; i++) {
        poll_fds[i].fd = sampler->perf_fds[i];
        poll_fds[i].events = POLLIN;
    }
    
    while (!sampler->stop_requested) {
        int ret = poll(poll_fds, sampler->num_fds, 100);  // 100ms timeout
        
        if (ret < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR("Poll failed: %s", strerror(errno));
            break;
        }
        
        if (ret == 0) continue;  // Timeout
        
        // Process available data
        for (int i = 0; i < sampler->num_fds; i++) {
            if (!(poll_fds[i].revents & POLLIN)) continue;
            
            // Read from ring buffer
            struct perf_event_mmap_page *metadata = 
                (struct perf_event_mmap_page *)sampler->mmap_buffers[i];
            
            if (!metadata) continue;
            
            uint64_t head = metadata->data_head;
            uint64_t tail = metadata->data_tail;
            
            // Memory barrier
            __sync_synchronize();
            
            while (tail < head) {
                struct perf_event_header *header = 
                    (struct perf_event_header *)((char *)metadata + 
                    metadata->data_offset + (tail & (sampler->mmap_size - 1)));
                
                if (header->type == PERF_RECORD_SAMPLE) {
                    pthread_mutex_lock(&sampler->samples_mutex);
                    
                    if (sampler->sample_count < sampler->config.max_samples) {
                        parse_perf_sample(header, 
                                         &sampler->samples[sampler->sample_count]);
                        sampler->sample_count++;
                        
                        if (sampler->sample_count % 1000 == 0) {
                            LOG_DEBUG("Collected %d samples", sampler->sample_count);
                        }
                    }
                    
                    pthread_mutex_unlock(&sampler->samples_mutex);
                }
                
                tail += header->size;
            }
            
            // Update tail
            metadata->data_tail = tail;
            __sync_synchronize();
        }
        
        // Check if we've collected enough samples
        if (sampler->sample_count >= sampler->config.max_samples) {
            LOG_INFO("Maximum samples reached (%d)", sampler->config.max_samples);
            break;
        }
        
        // Check duration limit
        if (sampler->config.sampling_duration > 0) {
            double elapsed = (get_timestamp() - sampler->start_time);
            if (elapsed >= sampler->config.sampling_duration) {
                LOG_INFO("Sampling duration reached (%.2f seconds)", elapsed);
                break;
            }
        }
    }
    
    free(poll_fds);
    LOG_INFO("Sampling thread stopped");
    return NULL;
}

// Create perf sampler
perf_sampler_t* perf_sampler_create(const perf_config_t *config) {
    if (!config) {
        LOG_ERROR("NULL config provided to perf_sampler_create");
        return NULL;
    }
    
    if (init_libpfm() != PFM_SUCCESS) {
        return NULL;
    }
    
    perf_sampler_t *sampler = CALLOC_LOGGED(1, sizeof(perf_sampler_t));
    if (!sampler) {
        LOG_ERROR("Failed to allocate perf sampler");
        return NULL;
    }
    
    sampler->config = *config;
    pthread_mutex_init(&sampler->samples_mutex, NULL);
    
    // Allocate sample buffer
    sampler->sample_capacity = config->max_samples;
    sampler->samples = CALLOC_LOGGED(sampler->sample_capacity, sizeof(cache_miss_sample_t));
    if (!sampler->samples) {
        LOG_ERROR("Failed to allocate sample buffer");
        FREE_LOGGED(sampler);
        return NULL;
    }
    
    LOG_INFO("Created perf sampler with capacity for %d samples", config->max_samples);
    
    // Setup perf events
    int num_cpus = sampler->config.sample_all_cpus ? sysconf(_SC_NPROCESSORS_ONLN) : 1;
    sampler->num_fds = num_cpus;
    sampler->perf_fds = CALLOC_LOGGED(num_cpus, sizeof(int));
    sampler->mmap_buffers = CALLOC_LOGGED(num_cpus, sizeof(void*));
    
    if (!sampler->perf_fds || !sampler->mmap_buffers) {
        LOG_ERROR("Failed to allocate perf fd arrays");
        perf_sampler_destroy(sampler);
        return NULL;
    }
    
    // Initialize all fds to -1
    for (int i = 0; i < num_cpus; i++) {
        sampler->perf_fds[i] = -1;
    }
    
    // Setup perf_event_attr
    struct perf_event_attr attr = {
        .type = PERF_TYPE_HW_CACHE,
        .size = sizeof(struct perf_event_attr),
        .config = PERF_COUNT_HW_CACHE_L1D | 
                 (PERF_COUNT_HW_CACHE_OP_READ << 8) |
                 (PERF_COUNT_HW_CACHE_RESULT_MISS << 16),
        .sample_period = config->sample_period,
        .sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_ADDR | 
                      PERF_SAMPLE_TIME | PERF_SAMPLE_CPU,
        .disabled = 1,
        .exclude_kernel = !config->include_kernel,
        .exclude_hv = 1,
        .mmap = 1,
        .comm = 1,
        .task = 1,
        .precise_ip = 2,  // Request precise IP
    };
    
    // Create perf events
    for (int cpu = 0; cpu < num_cpus; cpu++) {
        int target_cpu = sampler->config.sample_all_cpus ? cpu : -1;
        
        sampler->perf_fds[cpu] = perf_event_open(&attr, -1, target_cpu, -1, 0);
        if (sampler->perf_fds[cpu] < 0) {
            LOG_ERROR("Failed to create perf event for CPU %d: %s", 
                     cpu, strerror(errno));
            // Continue with other CPUs
            continue;
        }
        
        // Setup mmap buffer
        sampler->mmap_size = (MMAP_PAGES + 1) * getpagesize();
        sampler->mmap_buffers[cpu] = mmap(NULL, sampler->mmap_size,
                                         PROT_READ | PROT_WRITE, MAP_SHARED,
                                         sampler->perf_fds[cpu], 0);
        
        if (sampler->mmap_buffers[cpu] == MAP_FAILED) {
            LOG_ERROR("Failed to mmap perf buffer for CPU %d: %s",
                     cpu, strerror(errno));
            close(sampler->perf_fds[cpu]);
            sampler->perf_fds[cpu] = -1;
            sampler->mmap_buffers[cpu] = NULL;
            continue;
        }
        
        LOG_DEBUG("Setup perf event for CPU %d, fd=%d", cpu, sampler->perf_fds[cpu]);
    }
    
    // Check if we got at least one working event
    bool has_event = false;
    for (int i = 0; i < num_cpus; i++) {
        if (sampler->perf_fds[i] >= 0) {
            has_event = true;
            break;
        }
    }
    
    if (!has_event) {
        LOG_ERROR("Failed to create any perf events");
        perf_sampler_destroy(sampler);
        return NULL;
    }
    
    LOG_INFO("Perf sampler created successfully");
    return sampler;
}

// Destroy perf sampler
void perf_sampler_destroy(perf_sampler_t *sampler) {
    if (!sampler) return;
    
    LOG_INFO("Destroying perf sampler");
    
    // Stop sampling if running
    if (sampler->is_running) {
        perf_sampler_stop(sampler);
    }
    
    // Clean up perf events
    if (sampler->perf_fds) {
        for (int i = 0; i < sampler->num_fds; i++) {
            if (sampler->mmap_buffers && sampler->mmap_buffers[i]) {
                munmap(sampler->mmap_buffers[i], sampler->mmap_size);
            }
            if (sampler->perf_fds[i] >= 0) {
                close(sampler->perf_fds[i]);
            }
        }
        FREE_LOGGED(sampler->perf_fds);
    }
    
    if (sampler->mmap_buffers) {
        FREE_LOGGED(sampler->mmap_buffers);
    }
    
    if (sampler->samples) {
        FREE_LOGGED(sampler->samples);
    }
    
    pthread_mutex_destroy(&sampler->samples_mutex);
    FREE_LOGGED(sampler);
}

// Start sampling
int perf_sampler_start(perf_sampler_t *sampler) {
    if (!sampler) {
        LOG_ERROR("NULL sampler in perf_sampler_start");
        return -1;
    }
    
    if (sampler->is_running) {
        LOG_WARNING("Sampler already running");
        return 0;
    }
    
    LOG_INFO("Starting perf sampling");
    
    // Reset state
    sampler->sample_count = 0;
    sampler->stop_requested = false;
    sampler->start_time = get_timestamp();
    
    // Enable all perf events
    for (int i = 0; i < sampler->num_fds; i++) {
        if (sampler->perf_fds[i] >= 0) {
            if (ioctl(sampler->perf_fds[i], PERF_EVENT_IOC_ENABLE, 0) < 0) {
                LOG_ERROR("Failed to enable perf event %d: %s", i, strerror(errno));
            }
        }
    }
    
    // Start sampling thread
    if (pthread_create(&sampler->sampling_thread, NULL, 
                      sampling_thread_func, sampler) != 0) {
        LOG_ERROR("Failed to create sampling thread: %s", strerror(errno));
        
        // Disable events
        for (int i = 0; i < sampler->num_fds; i++) {
            if (sampler->perf_fds[i] >= 0) {
                ioctl(sampler->perf_fds[i], PERF_EVENT_IOC_DISABLE, 0);
            }
        }
        return -1;
    }
    
    sampler->is_running = true;
    LOG_INFO("Perf sampling started successfully");
    return 0;
}

// Stop sampling
int perf_sampler_stop(perf_sampler_t *sampler) {
    if (!sampler) {
        LOG_ERROR("NULL sampler in perf_sampler_stop");
        return -1;
    }
    
    if (!sampler->is_running) {
        LOG_WARNING("Sampler not running");
        return 0;
    }
    
    LOG_INFO("Stopping perf sampling");
    
    // Signal thread to stop
    sampler->stop_requested = true;
    
    // Wait for thread to finish
    pthread_join(sampler->sampling_thread, NULL);
    
    // Disable all perf events
    for (int i = 0; i < sampler->num_fds; i++) {
        if (sampler->perf_fds[i] >= 0) {
            ioctl(sampler->perf_fds[i], PERF_EVENT_IOC_DISABLE, 0);
        }
    }
    
    sampler->stop_time = get_timestamp();
    sampler->is_running = false;
    
    LOG_INFO("Perf sampling stopped. Collected %d samples in %.2f seconds",
             sampler->sample_count, sampler->stop_time - sampler->start_time);
    
    return 0;
}

// Check if sampler is running
bool perf_sampler_is_running(const perf_sampler_t *sampler) {
    return sampler ? sampler->is_running : false;
}

// Get collected samples
int perf_sampler_get_samples(perf_sampler_t *sampler, 
                            cache_miss_sample_t **samples, int *count) {
    if (!sampler || !samples || !count) {
        LOG_ERROR("Invalid parameters for perf_sampler_get_samples");
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
    
    LOG_INFO("Retrieved %d samples", *count);
    return 0;
}

// Free samples
void perf_sampler_free_samples(cache_miss_sample_t *samples) {
    if (samples) {
        FREE_LOGGED(samples);
    }
}

// Get default configuration
perf_config_t perf_config_default(void) {
    perf_config_t config = {
        .sample_period = 10000,      // Sample every 10K events
        .max_samples = 100000,       // Max 100K samples
        .sample_all_cpus = false,    // Current CPU only
        .include_kernel = false,     // User space only
        .cache_levels_mask = 0x7,    // L1, L2, L3
        .sampling_duration = 0       // No time limit
    };
    
    LOG_DEBUG("Created default perf configuration");
    return config;
}

// Check permissions
int perf_check_permissions(void) {
    // Check if we can read perf_event_paranoid
    FILE *fp = fopen("/proc/sys/kernel/perf_event_paranoid", "r");
    if (!fp) {
        LOG_ERROR("Cannot read perf_event_paranoid");
        return -1;
    }
    
    int paranoid_level;
    if (fscanf(fp, "%d", &paranoid_level) != 1) {
        fclose(fp);
        LOG_ERROR("Failed to parse perf_event_paranoid");
        return -1;
    }
    fclose(fp);
    
    LOG_INFO("perf_event_paranoid level: %d", paranoid_level);
    
    if (paranoid_level > 1) {
        LOG_WARNING("perf_event_paranoid=%d may restrict profiling. "
                   "Consider: sudo sysctl kernel.perf_event_paranoid=1",
                   paranoid_level);
        return 1;  // Warning but not error
    }
    
    return 0;
}

// Get error string
const char* perf_get_error_string(int error_code) {
    switch (error_code) {
        case EACCES:
        case EPERM:
            return "Permission denied. Check perf_event_paranoid setting.";
        case ENOENT:
            return "Event not supported by kernel/hardware.";
        case ENOSYS:
            return "Perf events not supported by kernel.";
        case ENODEV:
            return "No hardware support for requested event.";
        case EOPNOTSUPP:
            return "Operation not supported.";
        case EINVAL:
            return "Invalid parameters.";
        case EMFILE:
            return "Too many open files.";
        case EBUSY:
            return "Performance monitoring unit is busy.";
        default:
            return strerror(error_code);
    }
}

// Get statistics
int perf_sampler_get_stats(const perf_sampler_t *sampler, perf_stats_t *stats) {
    if (!sampler || !stats) {
        LOG_ERROR("Invalid parameters for perf_sampler_get_stats");
        return -1;
    }
    
    memset(stats, 0, sizeof(perf_stats_t));
    
    pthread_mutex_lock((pthread_mutex_t*)&sampler->samples_mutex);
    
    stats->total_samples = sampler->sample_count;
    
    // Count samples by cache level
    uint64_t total_latency = 0;
    for (int i = 0; i < sampler->sample_count; i++) {
        switch (sampler->samples[i].cache_level_missed) {
            case 1: stats->l1_misses++; break;
            case 2: stats->l2_misses++; break;
            case 3: stats->l3_misses++; break;
            default: stats->llc_misses++; break;
        }
        total_latency += sampler->samples[i].latency_cycles;
    }
    
    if (stats->total_samples > 0) {
        stats->avg_latency = (double)total_latency / stats->total_samples;
    }
    
    if (sampler->is_running) {
        stats->sampling_duration_ns = (get_timestamp() - sampler->start_time) * 1e9;
    } else if (sampler->stop_time > sampler->start_time) {
        stats->sampling_duration_ns = (sampler->stop_time - sampler->start_time) * 1e9;
    }
    
    pthread_mutex_unlock((pthread_mutex_t*)&sampler->samples_mutex);
    
    return 0;
}

// Print statistics
void perf_print_stats(const perf_stats_t *stats) {
    printf("\n=== Perf Sampling Statistics ===\n");
    printf("Total samples: %lu\n", stats->total_samples);
    printf("Sampling duration: %.3f seconds\n", stats->sampling_duration_ns / 1e9);
    
    if (stats->total_samples > 0) {
        printf("\nCache miss distribution:\n");
        printf("  L1 misses: %lu (%.1f%%)\n", 
               stats->l1_misses, 
               stats->l1_misses * 100.0 / stats->total_samples);
        printf("  L2 misses: %lu (%.1f%%)\n",
               stats->l2_misses,
               stats->l2_misses * 100.0 / stats->total_samples);
        printf("  L3 misses: %lu (%.1f%%)\n",
               stats->l3_misses,
               stats->l3_misses * 100.0 / stats->total_samples);
        printf("  LLC misses: %lu (%.1f%%)\n",
               stats->llc_misses,
               stats->llc_misses * 100.0 / stats->total_samples);
        
        printf("\nAverage latency: %.1f cycles\n", stats->avg_latency);
        
        double sample_rate = stats->total_samples / (stats->sampling_duration_ns / 1e9);
        printf("Sample rate: %.0f samples/second\n", sample_rate);
    }
}
