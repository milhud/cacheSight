#include "bandwidth_benchmark.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>

// Use aligned allocation for better performance
void* allocate_aligned_buffer(size_t size, size_t alignment) {
    void *buffer = NULL;
    if (posix_memalign(&buffer, alignment, size) != 0) {
        LOG_ERROR("Failed to allocate aligned buffer of size %zu", size);
        return NULL;
    }
    
    // Touch all pages to ensure they're allocated
    memset(buffer, 0, size);
    
    LOG_DEBUG("Allocated aligned buffer: %zu bytes at %p (alignment: %zu)", size, buffer, alignment);
    return buffer;
}

void free_aligned_buffer(void *buffer) {
    if (buffer) {
        free(buffer);
        LOG_DEBUG("Freed aligned buffer at %p", buffer);
    }
}

// Flush cache by reading a large buffer
void flush_cache(void *buffer, size_t size) {
    volatile char *p = (volatile char *)buffer;
    size_t cache_flush_size = 32 * 1024 * 1024;  // 32MB to flush most caches
    
    // Allocate a temporary buffer to flush cache
    void *flush_buffer = allocate_aligned_buffer(cache_flush_size, 4096);
    if (flush_buffer) {
        volatile char *flush_p = (volatile char *)flush_buffer;
        for (size_t i = 0; i < cache_flush_size; i += 64) {
            flush_p[i] = i;
        }
        free_aligned_buffer(flush_buffer);
    }
    
    LOG_DEBUG("Cache flushed");
}

// Warm cache by accessing the buffer
void warm_cache(void *buffer, size_t size) {
    volatile char *p = (volatile char *)buffer;
    for (size_t i = 0; i < size; i += 64) {  // Cache line size
        p[i];
    }
    LOG_DEBUG("Cache warmed for buffer size %zu", size);
}

// Benchmark sequential read
double benchmark_sequential_read(void *buffer, size_t size, int iterations) {
    volatile uint64_t *p = (volatile uint64_t *)buffer;
    size_t num_elements = size / sizeof(uint64_t);
    
    LOG_DEBUG("Starting sequential read benchmark: %zu bytes, %d iterations", size, iterations);
    
    double start_time = get_timestamp();
    
    for (int iter = 0; iter < iterations; iter++) {
        uint64_t sum = 0;
        for (size_t i = 0; i < num_elements; i++) {
            sum += p[i];
        }
        // Prevent optimization
        if (sum == 0) {
            p[0] = 1;
        }
    }
    
    double end_time = get_timestamp();
    double elapsed = end_time - start_time;
    double bandwidth_gb = (size * iterations) / (elapsed * 1e9);
    
    LOG_DEBUG("Sequential read: %.2f GB/s", bandwidth_gb);
    return bandwidth_gb;
}

// Benchmark sequential write
double benchmark_sequential_write(void *buffer, size_t size, int iterations) {
    volatile uint64_t *p = (volatile uint64_t *)buffer;
    size_t num_elements = size / sizeof(uint64_t);
    
    LOG_DEBUG("Starting sequential write benchmark: %zu bytes, %d iterations", size, iterations);
    
    double start_time = get_timestamp();
    
    for (int iter = 0; iter < iterations; iter++) {
        for (size_t i = 0; i < num_elements; i++) {
            p[i] = i;
        }
    }
    
    double end_time = get_timestamp();
    double elapsed = end_time - start_time;
    double bandwidth_gb = (size * iterations) / (elapsed * 1e9);
    
    LOG_DEBUG("Sequential write: %.2f GB/s", bandwidth_gb);
    return bandwidth_gb;
}

// Benchmark random read
double benchmark_random_read(void *buffer, size_t size, int iterations) {
    volatile uint64_t *p = (volatile uint64_t *)buffer;
    size_t num_elements = size / sizeof(uint64_t);
    
    // Create random access pattern
    size_t *indices = MALLOC_LOGGED(num_elements * sizeof(size_t));
    if (!indices) return 0.0;
    
    // Initialize sequential then shuffle
    for (size_t i = 0; i < num_elements; i++) {
        indices[i] = i;
    }
    
    // Fisher-Yates shuffle
    for (size_t i = num_elements - 1; i > 0; i--) {
        size_t j = rand() % (i + 1);
        size_t temp = indices[i];
        indices[i] = indices[j];
        indices[j] = temp;
    }
    
    LOG_DEBUG("Starting random read benchmark: %zu bytes, %d iterations", size, iterations);
    
    double start_time = get_timestamp();
    
    for (int iter = 0; iter < iterations; iter++) {
        uint64_t sum = 0;
        for (size_t i = 0; i < num_elements; i++) {
            sum += p[indices[i]];
        }
        // Prevent optimization
        if (sum == 0) {
            p[0] = 1;
        }
    }
    
    double end_time = get_timestamp();
    double elapsed = end_time - start_time;
    double bandwidth_gb = (size * iterations) / (elapsed * 1e9);
    
    FREE_LOGGED(indices);
    
    LOG_DEBUG("Random read: %.2f GB/s", bandwidth_gb);
    return bandwidth_gb;
}

// Benchmark random write
double benchmark_random_write(void *buffer, size_t size, int iterations) {
    volatile uint64_t *p = (volatile uint64_t *)buffer;
    size_t num_elements = size / sizeof(uint64_t);
    
    // Create random access pattern
    size_t *indices = MALLOC_LOGGED(num_elements * sizeof(size_t));
    if (!indices) return 0.0;
    
    for (size_t i = 0; i < num_elements; i++) {
        indices[i] = i;
    }
    
    // Fisher-Yates shuffle
    for (size_t i = num_elements - 1; i > 0; i--) {
        size_t j = rand() % (i + 1);
        size_t temp = indices[i];
        indices[i] = indices[j];
        indices[j] = temp;
    }
    
    LOG_DEBUG("Starting random write benchmark: %zu bytes, %d iterations", size, iterations);
    
    double start_time = get_timestamp();
    
    for (int iter = 0; iter < iterations; iter++) {
        for (size_t i = 0; i < num_elements; i++) {
            p[indices[i]] = i;
        }
    }
    
    double end_time = get_timestamp();
    double elapsed = end_time - start_time;
    double bandwidth_gb = (size * iterations) / (elapsed * 1e9);
    
    FREE_LOGGED(indices);
    
    LOG_DEBUG("Random write: %.2f GB/s", bandwidth_gb);
    return bandwidth_gb;
}

// Benchmark memory copy
double benchmark_memory_copy(void *src, void *dst, size_t size, int iterations) {
    LOG_DEBUG("Starting memory copy benchmark: %zu bytes, %d iterations", size, iterations);
    
    double start_time = get_timestamp();
    
    for (int iter = 0; iter < iterations; iter++) {
        memcpy(dst, src, size);
    }
    
    double end_time = get_timestamp();
    double elapsed = end_time - start_time;
    double bandwidth_gb = (size * 2 * iterations) / (elapsed * 1e9);  // *2 for read+write
    
    LOG_DEBUG("Memory copy: %.2f GB/s", bandwidth_gb);
    return bandwidth_gb;
}

// Measure access latency using pointer chasing
double measure_access_latency(void *buffer, size_t size, size_t stride) {
    struct pointer_chase {
        struct pointer_chase *next;
        char padding[56];  // Make it cache line sized
    };
    
    struct pointer_chase *nodes = (struct pointer_chase *)buffer;
    size_t num_nodes = size / sizeof(struct pointer_chase);
    
    if (num_nodes < 2) return 0.0;
    
    // Create pointer chase pattern
    for (size_t i = 0; i < num_nodes - 1; i++) {
        size_t next = (i + stride) % num_nodes;
        nodes[i].next = &nodes[next];
    }
    nodes[num_nodes - 1].next = &nodes[0];
    
    // Warm up
    volatile struct pointer_chase *p = &nodes[0];
    for (int i = 0; i < 1000; i++) {
        p = p->next;
    }
    
    // Measure
    int chase_count = 1000000;
    double start_time = get_timestamp();
    
    p = &nodes[0];
    for (int i = 0; i < chase_count; i++) {
        p = p->next;
    }
    
    double end_time = get_timestamp();
    double elapsed = end_time - start_time;
    double latency_ns = (elapsed * 1e9) / chase_count;
    
    LOG_DEBUG("Access latency for size %zu: %.2f ns", size, latency_ns);
    return latency_ns;
}

// Measure memory bandwidth
int measure_memory_bandwidth(const cache_info_t *cache_info, bandwidth_results_t *results) {
    LOG_INFO("Starting memory bandwidth measurements");
    
    memset(results, 0, sizeof(bandwidth_results_t));
    
    // Use a buffer size larger than L3 cache to measure main memory
    size_t buffer_size = cache_info->levels[cache_info->num_levels - 1].size * 4;
    if (buffer_size < 64 * 1024 * 1024) {
        buffer_size = 64 * 1024 * 1024;  // At least 64MB
    }
    
    LOG_INFO("Using buffer size: %zu MB", buffer_size / (1024 * 1024));
    
    void *buffer1 = allocate_aligned_buffer(buffer_size, 4096);
    void *buffer2 = allocate_aligned_buffer(buffer_size, 4096);
    
    if (!buffer1 || !buffer2) {
        LOG_ERROR("Failed to allocate benchmark buffers");
        free_aligned_buffer(buffer1);
        free_aligned_buffer(buffer2);
        return -1;
    }
    
    // Initialize buffers
    memset(buffer1, 0x5A, buffer_size);
    memset(buffer2, 0xA5, buffer_size);
    
    int iterations = 10;
    int warmup = 2;
    
    // Warmup runs
    LOG_INFO("Running warmup iterations");
    for (int i = 0; i < warmup; i++) {
        benchmark_sequential_read(buffer1, buffer_size, 1);
        benchmark_sequential_write(buffer1, buffer_size, 1);
    }
    
    // Sequential read
    LOG_INFO("Measuring sequential read bandwidth");
    flush_cache(buffer1, buffer_size);
    results->sequential_read_gbps = benchmark_sequential_read(buffer1, buffer_size, iterations);
    
    // Sequential write
    LOG_INFO("Measuring sequential write bandwidth");
    flush_cache(buffer1, buffer_size);
    results->sequential_write_gbps = benchmark_sequential_write(buffer1, buffer_size, iterations);
    
    // Random read
    LOG_INFO("Measuring random read bandwidth");
    flush_cache(buffer1, buffer_size);
    results->random_read_gbps = benchmark_random_read(buffer1, buffer_size, iterations / 2);
    
    // Random write
    LOG_INFO("Measuring random write bandwidth");
    flush_cache(buffer1, buffer_size);
    results->random_write_gbps = benchmark_random_write(buffer1, buffer_size, iterations / 2);
    
    // Memory copy
    LOG_INFO("Measuring memory copy bandwidth");
    flush_cache(buffer1, buffer_size);
    flush_cache(buffer2, buffer_size);
    results->copy_bandwidth_gbps = benchmark_memory_copy(buffer1, buffer2, buffer_size, iterations);
    
    free_aligned_buffer(buffer1);
    free_aligned_buffer(buffer2);
    
    LOG_INFO("Memory bandwidth measurements complete");
    return 0;
}

// Measure cache latency at different levels
int measure_cache_latency(const cache_info_t *cache_info, bandwidth_results_t *results) {
    LOG_INFO("Starting cache latency measurements");
    
    // Test different buffer sizes to hit different cache levels
    size_t test_sizes[] = {
        4 * 1024,           // 4KB - should fit in L1
        32 * 1024,          // 32KB - typical L1 size
        256 * 1024,         // 256KB - typical L2 size
        2 * 1024 * 1024,    // 2MB - between L2 and L3
        8 * 1024 * 1024,    // 8MB - typical L3 size
        32 * 1024 * 1024,   // 32MB - larger than L3
        128 * 1024 * 1024,  // 128MB - definitely main memory
        0                   // Sentinel
    };
    
    for (int i = 0; test_sizes[i] > 0; i++) {
        size_t size = test_sizes[i];
        void *buffer = allocate_aligned_buffer(size, 4096);
        if (!buffer) {
            LOG_ERROR("Failed to allocate buffer for latency test");
            continue;
        }
        
        LOG_INFO("Testing latency for buffer size %zu KB", size / 1024);
        
        // Initialize buffer
        memset(buffer, 0, size);
        
        // Measure latency
        double latency = measure_access_latency(buffer, size, 1);
        
        // Determine which cache level this likely corresponds to
        int level = -1;
        for (int j = 0; j < cache_info->num_levels; j++) {
            if (size <= cache_info->levels[j].size) {
                level = j;
                break;
            }
        }
        
        if (level >= 0 && level < 8) {
            results->latency_ns[level] = latency;
            LOG_INFO("L%d cache latency: %.2f ns", level + 1, latency);
        } else {
            // Main memory
            LOG_INFO("Main memory latency: %.2f ns", latency);
        }
        
        free_aligned_buffer(buffer);
    }
    
    LOG_INFO("Cache latency measurements complete");
    return 0;
}

// Print bandwidth results
void print_bandwidth_results(const bandwidth_results_t *results) {
    printf("\n=== Memory Bandwidth Results ===\n");
    printf("Sequential Read:  %.2f GB/s\n", results->sequential_read_gbps);
    printf("Sequential Write: %.2f GB/s\n", results->sequential_write_gbps);
    printf("Random Read:      %.2f GB/s\n", results->random_read_gbps);
    printf("Random Write:     %.2f GB/s\n", results->random_write_gbps);
    printf("Memory Copy:      %.2f GB/s\n", results->copy_bandwidth_gbps);
    
    printf("\n=== Cache Latency Results ===\n");
    for (int i = 0; i < 8 && results->latency_ns[i] > 0; i++) {
        printf("Level %d: %.2f ns\n", i + 1, results->latency_ns[i]);
    }
}
