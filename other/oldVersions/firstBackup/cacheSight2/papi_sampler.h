#ifndef PAPI_SAMPLER_H
#define PAPI_SAMPLER_H

#include "common.h"
#include "perf_sampler.h"

// PAPI sampler state
typedef struct papi_sampler papi_sampler_t;

// PAPI-specific configuration
typedef struct {
    perf_config_t base_config;   // Inherit base configuration
    int overflow_threshold;      // Overflow threshold for sampling
    bool use_multiplexing;       // Enable counter multiplexing
    int num_events;              // Number of events to monitor
    char event_names[8][64];     // Event names (e.g., "PAPI_L1_DCM")
} papi_config_t;

// API functions
papi_sampler_t* papi_sampler_create(const papi_config_t *config);
void papi_sampler_destroy(papi_sampler_t *sampler);

int papi_sampler_start(papi_sampler_t *sampler);
int papi_sampler_stop(papi_sampler_t *sampler);
bool papi_sampler_is_running(const papi_sampler_t *sampler);

// Get samples (same format as perf_sampler)
int papi_sampler_get_samples(papi_sampler_t *sampler,
                            cache_miss_sample_t **samples, int *count);

// PAPI-specific functions
int papi_check_availability(void);
int papi_list_cache_events(char *buffer, size_t buffer_size);
papi_config_t papi_config_default(void);

// Statistics
int papi_sampler_get_stats(const papi_sampler_t *sampler, perf_stats_t *stats);

#endif // PAPI_SAMPLER_H
