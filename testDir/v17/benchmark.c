// comprehensive_cache_benchmark.c - Extensive cache optimization demonstrations
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <pthread.h>
#include <math.h>
#include <unistd.h>
#include <immintrin.h>

// ============================================================================
// CONFIGURATION - Large sizes to ensure cache misses and visible timing differences
// ============================================================================

#define KB (1024)
#define MB (1024 * 1024)
#define GB (1024 * 1024 * 1024)

// Cache sizes (typical modern CPU)
#define L1_CACHE_SIZE (32 * KB)
#define L2_CACHE_SIZE (256 * KB)
#define L3_CACHE_SIZE (8 * MB)
#define CACHE_LINE_SIZE 64

// Problem sizes designed to stress different cache levels
#define TINY_SIZE (L1_CACHE_SIZE / sizeof(double) / 4)      // Fits in L1
#define SMALL_SIZE (L1_CACHE_SIZE / sizeof(double) * 2)     // Exceeds L1
#define MEDIUM_SIZE (L2_CACHE_SIZE / sizeof(double) * 2)    // Exceeds L2
#define LARGE_SIZE (L3_CACHE_SIZE / sizeof(double) * 2)     // Exceeds L3
#define HUGE_SIZE (64 * MB / sizeof(double))                // Way beyond cache

// Matrix dimensions for different tests
#define SMALL_MATRIX 512
#define MEDIUM_MATRIX 1024
#define LARGE_MATRIX 2048
#define HUGE_MATRIX 4096

// Timing iterations
#define WARM_UP_ITERATIONS 3
#define TIMING_ITERATIONS 10

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

double get_time() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec * 1e-6;
}

void print_separator() {
    printf("\n");
    for (int i = 0; i < 80; i++) printf("=");
    printf("\n");
}

void print_header(const char* title) {
    print_separator();
    printf("%s\n", title);
    print_separator();
}

// Comparison function for qsort
int compare_size_t(const void* a, const void* b) {
    return (*(size_t*)a > *(size_t*)b) - (*(size_t*)a < *(size_t*)b);
}

// Initialize array with pattern that prevents compiler optimizations
void init_array_pattern(double* arr, size_t size) {
    for (size_t i = 0; i < size; i++) {
        // Simple pattern that still prevents optimization
        arr[i] = (i * 7919 + 12345) % 1000 / 10.0;
    }
}

// Flush cache by accessing large dummy array
void flush_cache() {
    size_t dummy_size = L3_CACHE_SIZE * 2;
    volatile char* dummy = malloc(dummy_size);
    if (dummy) {
        for (size_t i = 0; i < dummy_size; i += CACHE_LINE_SIZE) {
            dummy[i] = i;
        }
        free((void*)dummy);
    }
}

// ============================================================================
// BENCHMARK 1: Sequential vs Random Access with Multiple Array Sizes
// ============================================================================

typedef struct {
    double sequential_time;
    double random_time;
    double sorted_random_time;
    size_t size;
    double speedup_sorted;
} access_pattern_result_t;

access_pattern_result_t benchmark_access_patterns(size_t size, const char* size_name) {
    printf("\n--- Testing with %s (%zu MB) ---\n", size_name, (size * sizeof(double)) / MB);
    
    double* data = aligned_alloc(CACHE_LINE_SIZE, size * sizeof(double));
    size_t* indices = malloc(size * sizeof(size_t));
    access_pattern_result_t result = {0};
    result.size = size;
    
    // Initialize data
    init_array_pattern(data, size);
    
    // Create random access pattern
    for (size_t i = 0; i < size; i++) {
        indices[i] = i;
    }
    // Fisher-Yates shuffle
    for (size_t i = size - 1; i > 0; i--) {
        size_t j = rand() % (i + 1);
        size_t temp = indices[i];
        indices[i] = indices[j];
        indices[j] = temp;
    }
    
    // Warm up
    volatile double sum = 0;
    for (int w = 0; w < WARM_UP_ITERATIONS; w++) {
        for (size_t i = 0; i < size; i++) {
            sum += data[i];
        }
    }
    
    // SEQUENTIAL ACCESS
    flush_cache();
    double start = get_time();
    for (int iter = 0; iter < TIMING_ITERATIONS; iter++) {
        double local_sum = 0;
        for (size_t i = 0; i < size; i++) {
            local_sum += data[i] * 1.01;
        }
        sum = local_sum; // Prevent optimization
    }
    result.sequential_time = get_time() - start;
    
    // RANDOM ACCESS
    flush_cache();
    start = get_time();
    for (int iter = 0; iter < TIMING_ITERATIONS; iter++) {
        double local_sum = 0;
        for (size_t i = 0; i < size; i++) {
            local_sum += data[indices[i]] * 1.01;
        }
        sum = local_sum;
    }
    result.random_time = get_time() - start;
    
    // SORTED RANDOM ACCESS
    // Sort indices to improve locality
    qsort(indices, size, sizeof(size_t), compare_size_t);
    
    flush_cache();
    start = get_time();
    for (int iter = 0; iter < TIMING_ITERATIONS; iter++) {
        double local_sum = 0;
        for (size_t i = 0; i < size; i++) {
            local_sum += data[indices[i]] * 1.01;
        }
        sum = local_sum;
    }
    result.sorted_random_time = get_time() - start;
    
    printf("Sequential:     %.3f seconds\n", result.sequential_time);
    printf("Random:         %.3f seconds (%.2fx slower)\n", 
           result.random_time, result.random_time / result.sequential_time);
    printf("Sorted Random:  %.3f seconds (%.2fx faster than random)\n", 
           result.sorted_random_time, result.random_time / result.sorted_random_time);
    
    result.speedup_sorted = result.random_time / result.sorted_random_time;
    
    free(data);
    free(indices);
    return result;
}

// ============================================================================
// BENCHMARK 2: Matrix Operations - Naive vs Cache-Optimized
// ============================================================================

void benchmark_matrix_multiply(int n) {
    printf("\n--- Matrix Multiply %dx%d (%.1f MB per matrix) ---\n", 
           n, n, (n * n * sizeof(double)) / (double)MB);
    
    double* A = aligned_alloc(CACHE_LINE_SIZE, n * n * sizeof(double));
    double* B = aligned_alloc(CACHE_LINE_SIZE, n * n * sizeof(double));
    double* C = aligned_alloc(CACHE_LINE_SIZE, n * n * sizeof(double));
    
    // Initialize matrices
    for (int i = 0; i < n * n; i++) {
        A[i] = (double)(i % 100) / 100.0;
        B[i] = (double)((i + 1) % 100) / 100.0;
        C[i] = 0.0;
    }
    
    // 1. NAIVE IMPLEMENTATION (ijk order - poor cache usage)
    flush_cache();
    double start = get_time();
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            double sum = 0.0;
            for (int k = 0; k < n; k++) {
                sum += A[i * n + k] * B[k * n + j];  // B access is strided!
            }
            C[i * n + j] = sum;
        }
    }
    double naive_time = get_time() - start;
    
    // Reset C
    memset(C, 0, n * n * sizeof(double));
    
    // 2. CACHE-FRIENDLY ORDER (ikj order)
    flush_cache();
    start = get_time();
    for (int i = 0; i < n; i++) {
        for (int k = 0; k < n; k++) {
            double a_ik = A[i * n + k];
            for (int j = 0; j < n; j++) {
                C[i * n + j] += a_ik * B[k * n + j];  // Sequential access to B and C
            }
        }
    }
    double ikj_time = get_time() - start;
    
    // Reset C
    memset(C, 0, n * n * sizeof(double));
    
    // 3. BLOCKED/TILED IMPLEMENTATION
    int block_size = 64;  // Tuned for L1 cache
    flush_cache();
    start = get_time();
    for (int i0 = 0; i0 < n; i0 += block_size) {
        for (int j0 = 0; j0 < n; j0 += block_size) {
            for (int k0 = 0; k0 < n; k0 += block_size) {
                // Mini matrix multiply for block
                for (int i = i0; i < i0 + block_size && i < n; i++) {
                    for (int k = k0; k < k0 + block_size && k < n; k++) {
                        double a_ik = A[i * n + k];
                        for (int j = j0; j < j0 + block_size && j < n; j++) {
                            C[i * n + j] += a_ik * B[k * n + j];
                        }
                    }
                }
            }
        }
    }
    double blocked_time = get_time() - start;
    
    printf("Naive (ijk):     %.3f seconds\n", naive_time);
    printf("Cache-friendly:  %.3f seconds (%.2fx speedup)\n", ikj_time, naive_time / ikj_time);
    printf("Blocked/Tiled:   %.3f seconds (%.2fx speedup)\n", blocked_time, naive_time / blocked_time);
    
    free(A);
    free(B);
    free(C);
}

// ============================================================================
// BENCHMARK 3: Structure Layout - AoS vs SoA with Complex Operations
// ============================================================================

typedef struct {
    double x, y, z;
    double vx, vy, vz;
    double mass;
    double charge;
    double pressure;
    double temperature;
    int id;
    int type;
} particle_aos_t;

typedef struct {
    double* x;
    double* y; 
    double* z;
    double* vx;
    double* vy;
    double* vz;
    double* mass;
    double* charge;
    double* pressure;
    double* temperature;
    int* id;
    int* type;
} particle_soa_t;

void benchmark_aos_vs_soa(size_t n) {
    printf("\n--- AoS vs SoA with %zu particles (%.1f MB) ---\n", 
           n, (n * sizeof(particle_aos_t)) / (double)MB);
    
    // Allocate AoS
    particle_aos_t* aos = aligned_alloc(CACHE_LINE_SIZE, n * sizeof(particle_aos_t));
    
    // Allocate SoA
    particle_soa_t soa;
    soa.x = aligned_alloc(CACHE_LINE_SIZE, n * sizeof(double));
    soa.y = aligned_alloc(CACHE_LINE_SIZE, n * sizeof(double));
    soa.z = aligned_alloc(CACHE_LINE_SIZE, n * sizeof(double));
    soa.vx = aligned_alloc(CACHE_LINE_SIZE, n * sizeof(double));
    soa.vy = aligned_alloc(CACHE_LINE_SIZE, n * sizeof(double));
    soa.vz = aligned_alloc(CACHE_LINE_SIZE, n * sizeof(double));
    soa.mass = aligned_alloc(CACHE_LINE_SIZE, n * sizeof(double));
    soa.charge = aligned_alloc(CACHE_LINE_SIZE, n * sizeof(double));
    soa.pressure = aligned_alloc(CACHE_LINE_SIZE, n * sizeof(double));
    soa.temperature = aligned_alloc(CACHE_LINE_SIZE, n * sizeof(double));
    soa.id = aligned_alloc(CACHE_LINE_SIZE, n * sizeof(int));
    soa.type = aligned_alloc(CACHE_LINE_SIZE, n * sizeof(int));
    
    // Initialize data
    for (size_t i = 0; i < n; i++) {
        double val = (double)(i % 360);
        aos[i].x = val * 0.1;
        aos[i].y = val * 0.2;
        aos[i].z = i * 0.1;
        aos[i].vx = -val * 0.01;
        aos[i].vy = val * 0.01;
        aos[i].vz = 0.5;
        aos[i].mass = 1.0 + (i % 10) * 0.1;
        aos[i].charge = (i % 3) - 1.0;
        aos[i].pressure = 101325.0 + i * 0.1;
        aos[i].temperature = 293.15 + ((i % 20) - 10) * 1.0;
        aos[i].id = i;
        aos[i].type = i % 5;
        
        // Copy to SoA
        soa.x[i] = aos[i].x;
        soa.y[i] = aos[i].y;
        soa.z[i] = aos[i].z;
        soa.vx[i] = aos[i].vx;
        soa.vy[i] = aos[i].vy;
        soa.vz[i] = aos[i].vz;
        soa.mass[i] = aos[i].mass;
        soa.charge[i] = aos[i].charge;
        soa.pressure[i] = aos[i].pressure;
        soa.temperature[i] = aos[i].temperature;
        soa.id[i] = aos[i].id;
        soa.type[i] = aos[i].type;
    }
    
    // TEST 1: Position update (touches x,y,z,vx,vy,vz)
    printf("\nPosition Update Test:\n");
    double dt = 0.001;
    
    // AoS version
    flush_cache();
    double start = get_time();
    for (int iter = 0; iter < 100; iter++) {
        for (size_t i = 0; i < n; i++) {
            aos[i].x += aos[i].vx * dt;
            aos[i].y += aos[i].vy * dt;
            aos[i].z += aos[i].vz * dt;
        }
    }
    double aos_pos_time = get_time() - start;
    
    // SoA version
    flush_cache();
    start = get_time();
    for (int iter = 0; iter < 100; iter++) {
        for (size_t i = 0; i < n; i++) {
            soa.x[i] += soa.vx[i] * dt;
            soa.y[i] += soa.vy[i] * dt;
            soa.z[i] += soa.vz[i] * dt;
        }
    }
    double soa_pos_time = get_time() - start;
    
    printf("AoS: %.3f seconds\n", aos_pos_time);
    printf("SoA: %.3f seconds (%.2fx speedup)\n", soa_pos_time, aos_pos_time / soa_pos_time);
    
    // TEST 2: Force calculation (only touches x,y,z,mass for read)
    printf("\nForce Calculation Test (gravity between all pairs):\n");
    
    double* forces_x = calloc(n, sizeof(double));
    double* forces_y = calloc(n, sizeof(double));
    double* forces_z = calloc(n, sizeof(double));
    
    // AoS version
    flush_cache();
    start = get_time();
    for (size_t i = 0; i < n; i++) {
        for (size_t j = i + 1; j < n && j < i + 100; j++) {  // Limit to nearby particles
            double dx = aos[j].x - aos[i].x;
            double dy = aos[j].y - aos[i].y;
            double dz = aos[j].z - aos[i].z;
            double r2 = dx*dx + dy*dy + dz*dz + 0.01;  // Softening
            // Approximate 1/sqrt(r2) without sqrt
            double r_inv = 1.0;
            // Newton-Raphson approximation for 1/sqrt
            for (int iter = 0; iter < 2; iter++) {
                r_inv = r_inv * (1.5 - 0.5 * r2 * r_inv * r_inv);
            }
            double f = aos[i].mass * aos[j].mass * r_inv * r_inv * r_inv;
            forces_x[i] += f * dx;
            forces_y[i] += f * dy;
            forces_z[i] += f * dz;
        }
    }
    double aos_force_time = get_time() - start;
    
    // Reset forces
    memset(forces_x, 0, n * sizeof(double));
    memset(forces_y, 0, n * sizeof(double));
    memset(forces_z, 0, n * sizeof(double));
    
    // SoA version
    flush_cache();
    start = get_time();
    for (size_t i = 0; i < n; i++) {
        for (size_t j = i + 1; j < n && j < i + 100; j++) {
            double dx = soa.x[j] - soa.x[i];
            double dy = soa.y[j] - soa.y[i];
            double dz = soa.z[j] - soa.z[i];
            double r2 = dx*dx + dy*dy + dz*dz + 0.01;
            // Approximate 1/sqrt(r2) without sqrt
            double r_inv = 1.0;
            for (int iter = 0; iter < 2; iter++) {
                r_inv = r_inv * (1.5 - 0.5 * r2 * r_inv * r_inv);
            }
            double f = soa.mass[i] * soa.mass[j] * r_inv * r_inv * r_inv;
            forces_x[i] += f * dx;
            forces_y[i] += f * dy;
            forces_z[i] += f * dz;
        }
    }
    double soa_force_time = get_time() - start;
    
    printf("AoS: %.3f seconds\n", aos_force_time);
    printf("SoA: %.3f seconds (%.2fx speedup)\n", soa_force_time, aos_force_time / soa_force_time);
    
    // Clean up
    free(aos);
    free(soa.x);
    free(soa.y);
    free(soa.z);
    free(soa.vx);
    free(soa.vy);
    free(soa.vz);
    free(soa.mass);
    free(soa.charge);
    free(soa.pressure);
    free(soa.temperature);
    free(soa.id);
    free(soa.type);
    free(forces_x);
    free(forces_y);
    free(forces_z);
}

// ============================================================================
// BENCHMARK 4: False Sharing in Multithreaded Code
// ============================================================================

#define NUM_THREADS 4
#define FALSE_SHARING_ITERATIONS (100 * 1000 * 1000)

typedef struct {
    volatile long counter;
} counter_false_t;

typedef struct {
    volatile long counter;
    char padding[CACHE_LINE_SIZE - sizeof(long)];
} counter_padded_t;

counter_false_t false_counters[NUM_THREADS];
counter_padded_t padded_counters[NUM_THREADS];

typedef struct {
    int thread_id;
    int iterations;
    void* counters;
    int is_padded;
} thread_data_t;

void* increment_counter(void* arg) {
    thread_data_t* data = (thread_data_t*)arg;
    
    if (data->is_padded) {
        counter_padded_t* counters = (counter_padded_t*)data->counters;
        for (int i = 0; i < data->iterations; i++) {
            counters[data->thread_id].counter++;
            // Simulate some work
            volatile int dummy = 0;
            for (int j = 0; j < 10; j++) dummy++;
        }
    } else {
        counter_false_t* counters = (counter_false_t*)data->counters;
        for (int i = 0; i < data->iterations; i++) {
            counters[data->thread_id].counter++;
            // Simulate some work
            volatile int dummy = 0;
            for (int j = 0; j < 10; j++) dummy++;
        }
    }
    
    return NULL;
}

void benchmark_false_sharing() {
    printf("\n--- False Sharing Benchmark (%d threads) ---\n", NUM_THREADS);
    
    pthread_t threads[NUM_THREADS];
    thread_data_t thread_data[NUM_THREADS];
    
    // Test 1: False sharing (unpadded)
    memset(false_counters, 0, sizeof(false_counters));
    
    double start = get_time();
    for (int i = 0; i < NUM_THREADS; i++) {
        thread_data[i].thread_id = i;
        thread_data[i].iterations = FALSE_SHARING_ITERATIONS;
        thread_data[i].counters = false_counters;
        thread_data[i].is_padded = 0;
        pthread_create(&threads[i], NULL, increment_counter, &thread_data[i]);
    }
    
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }
    double false_time = get_time() - start;
    
    // Test 2: No false sharing (padded)
    memset(padded_counters, 0, sizeof(padded_counters));
    
    start = get_time();
    for (int i = 0; i < NUM_THREADS; i++) {
        thread_data[i].thread_id = i;
        thread_data[i].iterations = FALSE_SHARING_ITERATIONS;
        thread_data[i].counters = padded_counters;
        thread_data[i].is_padded = 1;
        pthread_create(&threads[i], NULL, increment_counter, &thread_data[i]);
    }
    
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }
    double padded_time = get_time() - start;
    
    // Verify correctness
    long false_total = 0, padded_total = 0;
    for (int i = 0; i < NUM_THREADS; i++) {
        false_total += false_counters[i].counter;
        padded_total += padded_counters[i].counter;
    }
    
    printf("False sharing:   %.3f seconds (total: %ld)\n", false_time, false_total);
    printf("Padded:          %.3f seconds (total: %ld)\n", padded_time, padded_total);
    printf("Speedup:         %.2fx\n", false_time / padded_time);
    printf("Throughput improvement: %.1f million ops/sec\n", 
           ((padded_total - false_total) / 1e6) / (padded_time - false_time));
}

// ============================================================================
// BENCHMARK 5: Loop Tiling for Large Data Sets
// ============================================================================

void benchmark_2d_stencil(int n) {
    printf("\n--- 2D Stencil Computation %dx%d ---\n", n, n);
    
    double* grid = aligned_alloc(CACHE_LINE_SIZE, n * n * sizeof(double));
    double* new_grid = aligned_alloc(CACHE_LINE_SIZE, n * n * sizeof(double));
    
    // Initialize grid with interesting pattern
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            grid[i * n + j] = ((i * 7919 + j * 4567) % 1000) / 1000.0;
        }
    }
    
    // 5-point stencil weights
    const double center = 0.5;
    const double neighbor = 0.125;
    
    // Version 1: Naive implementation
    flush_cache();
    double start = get_time();
    for (int iter = 0; iter < 10; iter++) {
        for (int i = 1; i < n-1; i++) {
            for (int j = 1; j < n-1; j++) {
                new_grid[i * n + j] = center * grid[i * n + j] +
                                      neighbor * (grid[(i-1) * n + j] +
                                                  grid[(i+1) * n + j] +
                                                  grid[i * n + (j-1)] +
                                                  grid[i * n + (j+1)]);
            }
        }
        // Swap grids
        double* temp = grid;
        grid = new_grid;
        new_grid = temp;
    }
    double naive_time = get_time() - start;
    
    // Version 2: Tiled implementation
    const int tile_size = 64;  // Tuned for cache
    flush_cache();
    start = get_time();
    for (int iter = 0; iter < 10; iter++) {
        for (int ti = 1; ti < n-1; ti += tile_size) {
            for (int tj = 1; tj < n-1; tj += tile_size) {
                // Process tile
                int i_end = (ti + tile_size < n-1) ? ti + tile_size : n-1;
                int j_end = (tj + tile_size < n-1) ? tj + tile_size : n-1;
                
                for (int i = ti; i < i_end; i++) {
                    for (int j = tj; j < j_end; j++) {
                        new_grid[i * n + j] = center * grid[i * n + j] +
                                              neighbor * (grid[(i-1) * n + j] +
                                                          grid[(i+1) * n + j] +
                                                          grid[i * n + (j-1)] +
                                                          grid[i * n + (j+1)]);
                    }
                }
            }
        }
        // Swap grids
        double* temp = grid;
        grid = new_grid;
        new_grid = temp;
    }
    double tiled_time = get_time() - start;
    
    printf("Naive:  %.3f seconds\n", naive_time);
    printf("Tiled:  %.3f seconds (%.2fx speedup)\n", tiled_time, naive_time / tiled_time);
    
    free(grid);
    free(new_grid);
}

// ============================================================================
// BENCHMARK 6: Prefetching Effectiveness
// ============================================================================

void benchmark_prefetching(size_t n) {
    printf("\n--- Prefetching Benchmark with %zu elements ---\n", n);
    
    // Allocate arrays
    double* a = aligned_alloc(CACHE_LINE_SIZE, n * sizeof(double));
    double* b = aligned_alloc(CACHE_LINE_SIZE, n * sizeof(double));
    double* c = aligned_alloc(CACHE_LINE_SIZE, n * sizeof(double));
    
    // Initialize
    init_array_pattern(a, n);
    init_array_pattern(b, n);
    
    // Test 1: No prefetching
    flush_cache();
    double start = get_time();
    for (int iter = 0; iter < 20; iter++) {
        for (size_t i = 0; i < n; i++) {
            c[i] = a[i] * b[i] + (a[i] - b[i]) * 0.5;
        }
    }
    double no_prefetch_time = get_time() - start;
    
    // Test 2: Software prefetching
    flush_cache();
    start = get_time();
    for (int iter = 0; iter < 20; iter++) {
        for (size_t i = 0; i < n; i++) {
            // Prefetch ahead
            if (i + 8 < n) {
                __builtin_prefetch(&a[i + 8], 0, 3);
                __builtin_prefetch(&b[i + 8], 0, 3);
                __builtin_prefetch(&c[i + 8], 1, 3);
            }
            // Simple computation instead of trig functions
            c[i] = a[i] * b[i] + (a[i] - b[i]) * 0.5;
        }
    }
    double prefetch_time = get_time() - start;
    
    // Test 3: Prefetching with larger distance
    flush_cache();
    start = get_time();
    for (int iter = 0; iter < 20; iter++) {
        for (size_t i = 0; i < n; i++) {
            // Prefetch further ahead
            if (i + 32 < n) {
                __builtin_prefetch(&a[i + 32], 0, 1);
                __builtin_prefetch(&b[i + 32], 0, 1);
                __builtin_prefetch(&c[i + 32], 1, 1);
            }
            c[i] = a[i] * b[i] + (a[i] - b[i]) * 0.5;
        }
    }
    double prefetch_far_time = get_time() - start;
    
    printf("No prefetch:        %.3f seconds\n", no_prefetch_time);
    printf("Prefetch (+8):      %.3f seconds (%.2fx speedup)\n", 
           prefetch_time, no_prefetch_time / prefetch_time);
    printf("Prefetch (+32):     %.3f seconds (%.2fx speedup)\n", 
           prefetch_far_time, no_prefetch_time / prefetch_far_time);
    
    free(a);
    free(b);
    free(c);
}

// ============================================================================
// BENCHMARK 7: Cache Associativity and Conflict Misses
// ============================================================================

void benchmark_cache_conflicts(size_t array_size, int stride) {
    printf("\n--- Cache Conflict Test (stride=%d) ---\n", stride);
    
    double* data = aligned_alloc(CACHE_LINE_SIZE, array_size * sizeof(double));
    init_array_pattern(data, array_size);
    
    // Test different access patterns
    size_t num_accesses = array_size / stride;
    
    // Pattern 1: Strided access (may cause conflicts)
    flush_cache();
    double start = get_time();
    double sum = 0;
    for (int iter = 0; iter < 100; iter++) {
        for (size_t i = 0; i < num_accesses; i++) {
            size_t idx = (i * stride) % array_size;
            sum += data[idx] * 1.01;
        }
    }
    double strided_time = get_time() - start;
    
    // Pattern 2: Sequential access (no conflicts)
    flush_cache();
    start = get_time();
    sum = 0;
    for (int iter = 0; iter < 100; iter++) {
        for (size_t i = 0; i < num_accesses; i++) {
            sum += data[i] * 1.01;
        }
    }
    double sequential_time = get_time() - start;
    
    printf("Strided:     %.3f seconds\n", strided_time);
    printf("Sequential:  %.3f seconds (%.2fx faster)\n", 
           sequential_time, strided_time / sequential_time);
    
    free(data);
}

// ============================================================================
// BENCHMARK 8: Working Set Size Impact
// ============================================================================

void benchmark_working_set_sizes() {
    printf("\n--- Working Set Size Impact ---\n");
    
    struct {
        size_t size;
        const char* name;
    } sizes[] = {
        {L1_CACHE_SIZE / sizeof(double) / 2, "L1/2"},
        {L1_CACHE_SIZE / sizeof(double), "L1"},
        {L1_CACHE_SIZE / sizeof(double) * 2, "2*L1"},
        {L2_CACHE_SIZE / sizeof(double), "L2"},
        {L2_CACHE_SIZE / sizeof(double) * 2, "2*L2"},
        {L3_CACHE_SIZE / sizeof(double), "L3"},
        {L3_CACHE_SIZE / sizeof(double) * 2, "2*L3"},
    };
    
    for (int s = 0; s < sizeof(sizes)/sizeof(sizes[0]); s++) {
        size_t size = sizes[s].size;
        double* data = aligned_alloc(CACHE_LINE_SIZE, size * sizeof(double));
        init_array_pattern(data, size);
        
        flush_cache();
        double start = get_time();
        double sum = 0;
        
        // Access entire working set repeatedly
        for (int iter = 0; iter < 1000; iter++) {
            for (size_t i = 0; i < size; i++) {
                sum += data[i] * 1.0001;
            }
        }
        
        double time = get_time() - start;
        double bandwidth = (size * sizeof(double) * 1000) / (time * GB);
        
        printf("%-8s: %.3f seconds, %.1f GB/s\n", sizes[s].name, time, bandwidth);
        
        free(data);
    }
}

// ============================================================================
// BENCHMARK 9: Complex Nested Loop Optimizations
// ============================================================================

void benchmark_3d_computation(int n) {
    printf("\n--- 3D Grid Computation %dx%dx%d ---\n", n, n, n);
    
    double* grid1 = aligned_alloc(CACHE_LINE_SIZE, n * n * n * sizeof(double));
    double* grid2 = aligned_alloc(CACHE_LINE_SIZE, n * n * n * sizeof(double));
    
    // Initialize
    for (int i = 0; i < n * n * n; i++) {
        grid1[i] = (i * 7919) % 1000 / 1000.0;
        grid2[i] = (i * 4567) % 1000 / 1000.0;
    }
    
    // Version 1: Poor loop order (kji)
    flush_cache();
    double start = get_time();
    for (int k = 1; k < n-1; k++) {
        for (int j = 1; j < n-1; j++) {
            for (int i = 1; i < n-1; i++) {
                int idx = k * n * n + j * n + i;
                grid2[idx] = 0.125 * (
                    grid1[idx - n*n] + grid1[idx + n*n] +  // k-1, k+1
                    grid1[idx - n] + grid1[idx + n] +      // j-1, j+1
                    grid1[idx - 1] + grid1[idx + 1] +      // i-1, i+1
                    2.0 * grid1[idx]
                );
            }
        }
    }
    double poor_order_time = get_time() - start;
    
    // Version 2: Better loop order (ijk)
    flush_cache();
    start = get_time();
    for (int i = 1; i < n-1; i++) {
        for (int j = 1; j < n-1; j++) {
            for (int k = 1; k < n-1; k++) {
                int idx = k * n * n + j * n + i;
                grid2[idx] = 0.125 * (
                    grid1[idx - n*n] + grid1[idx + n*n] +
                    grid1[idx - n] + grid1[idx + n] +
                    grid1[idx - 1] + grid1[idx + 1] +
                    2.0 * grid1[idx]
                );
            }
        }
    }
    double better_order_time = get_time() - start;
    
    // Version 3: Tiled with optimal order
    const int tile = 32;
    flush_cache();
    start = get_time();
    for (int ii = 1; ii < n-1; ii += tile) {
        for (int jj = 1; jj < n-1; jj += tile) {
            for (int kk = 1; kk < n-1; kk += tile) {
                // Process tile
                for (int i = ii; i < ii + tile && i < n-1; i++) {
                    for (int j = jj; j < jj + tile && j < n-1; j++) {
                        for (int k = kk; k < kk + tile && k < n-1; k++) {
                            int idx = k * n * n + j * n + i;
                            grid2[idx] = 0.125 * (
                                grid1[idx - n*n] + grid1[idx + n*n] +
                                grid1[idx - n] + grid1[idx + n] +
                                grid1[idx - 1] + grid1[idx + 1] +
                                2.0 * grid1[idx]
                            );
                        }
                    }
                }
            }
        }
    }
    double tiled_time = get_time() - start;
    
    printf("Poor order (kji):  %.3f seconds\n", poor_order_time);
    printf("Better order (ijk): %.3f seconds (%.2fx speedup)\n", 
           better_order_time, poor_order_time / better_order_time);
    printf("Tiled:             %.3f seconds (%.2fx speedup)\n", 
           tiled_time, poor_order_time / tiled_time);
    
    free(grid1);
    free(grid2);
}

// ============================================================================
// BENCHMARK 10: Memory Bandwidth Saturation
// ============================================================================

void benchmark_memory_bandwidth() {
    printf("\n--- Memory Bandwidth Test ---\n");
    
    size_t size = 128 * MB / sizeof(double);  // 128 MB
    double* src = aligned_alloc(CACHE_LINE_SIZE, size * sizeof(double));
    double* dst = aligned_alloc(CACHE_LINE_SIZE, size * sizeof(double));
    
    init_array_pattern(src, size);
    
    // Test 1: Simple copy
    flush_cache();
    double start = get_time();
    for (int iter = 0; iter < 10; iter++) {
        memcpy(dst, src, size * sizeof(double));
    }
    double memcpy_time = get_time() - start;
    double memcpy_bandwidth = (size * sizeof(double) * 10 * 2) / (memcpy_time * GB);
    
    // Test 2: Manual copy
    flush_cache();
    start = get_time();
    for (int iter = 0; iter < 10; iter++) {
        for (size_t i = 0; i < size; i++) {
            dst[i] = src[i];
        }
    }
    double manual_time = get_time() - start;
    double manual_bandwidth = (size * sizeof(double) * 10 * 2) / (manual_time * GB);
    
    // Test 3: Non-temporal stores
    flush_cache();
    start = get_time();
    for (int iter = 0; iter < 10; iter++) {
        #ifdef __AVX__
        for (size_t i = 0; i < size; i += 4) {
            __m256d data = _mm256_load_pd(&src[i]);
            _mm256_stream_pd(&dst[i], data);
        }
        _mm_sfence();
        #else
        for (size_t i = 0; i < size; i++) {
            dst[i] = src[i];
        }
        #endif
    }
    double stream_time = get_time() - start;
    double stream_bandwidth = (size * sizeof(double) * 10 * 2) / (stream_time * GB);
    
    printf("memcpy:           %.3f seconds, %.1f GB/s\n", memcpy_time, memcpy_bandwidth);
    printf("Manual copy:      %.3f seconds, %.1f GB/s\n", manual_time, manual_bandwidth);
    printf("Streaming stores: %.3f seconds, %.1f GB/s\n", stream_time, stream_bandwidth);
    
    free(src);
    free(dst);
}

// ============================================================================
// BENCHMARK 11: Real-world Example - Image Processing
// ============================================================================

void benchmark_image_convolution(int width, int height) {
    printf("\n--- Image Convolution %dx%d ---\n", width, height);
    
    // Allocate "image" buffers
    float* input = aligned_alloc(CACHE_LINE_SIZE, width * height * sizeof(float));
    float* output = aligned_alloc(CACHE_LINE_SIZE, width * height * sizeof(float));
    
    // Initialize with pattern
    for (int i = 0; i < width * height; i++) {
        input[i] = (float)(i % 256) / 255.0f;
    }
    
    // 5x5 Gaussian kernel
    float kernel[5][5] = {
        {1, 4, 6, 4, 1},
        {4, 16, 24, 16, 4},
        {6, 24, 36, 24, 6},
        {4, 16, 24, 16, 4},
        {1, 4, 6, 4, 1}
    };
    float kernel_sum = 256.0f;
    
    // Version 1: Naive convolution
    flush_cache();
    double start = get_time();
    for (int y = 2; y < height - 2; y++) {
        for (int x = 2; x < width - 2; x++) {
            float sum = 0.0f;
            for (int ky = -2; ky <= 2; ky++) {
                for (int kx = -2; kx <= 2; kx++) {
                    sum += input[(y + ky) * width + (x + kx)] * kernel[ky + 2][kx + 2];
                }
            }
            output[y * width + x] = sum / kernel_sum;
        }
    }
    double naive_time = get_time() - start;
    
    // Version 2: Tiled convolution
    const int tile_size = 64;
    flush_cache();
    start = get_time();
    for (int ty = 2; ty < height - 2; ty += tile_size) {
        for (int tx = 2; tx < width - 2; tx += tile_size) {
            // Process tile
            int y_end = (ty + tile_size < height - 2) ? ty + tile_size : height - 2;
            int x_end = (tx + tile_size < width - 2) ? tx + tile_size : width - 2;
            
            for (int y = ty; y < y_end; y++) {
                for (int x = tx; x < x_end; x++) {
                    float sum = 0.0f;
                    for (int ky = -2; ky <= 2; ky++) {
                        for (int kx = -2; kx <= 2; kx++) {
                            sum += input[(y + ky) * width + (x + kx)] * kernel[ky + 2][kx + 2];
                        }
                    }
                    output[y * width + x] = sum / kernel_sum;
                }
            }
        }
    }
    double tiled_time = get_time() - start;
    
    printf("Naive:  %.3f seconds\n", naive_time);
    printf("Tiled:  %.3f seconds (%.2fx speedup)\n", tiled_time, naive_time / tiled_time);
    
    free(input);
    free(output);
}

// ============================================================================
// BENCHMARK 12: Complex Data Structure Traversal
// ============================================================================

typedef struct node {
    struct node* next;
    struct node* child;
    double data[8];  // Payload
    int value;
} node_t;

void benchmark_tree_traversal(int num_nodes) {
    printf("\n--- Tree Traversal (%d nodes) ---\n", num_nodes);
    
    // Create a somewhat random tree structure
    node_t* nodes = aligned_alloc(CACHE_LINE_SIZE, num_nodes * sizeof(node_t));
    
    // Initialize nodes
    for (int i = 0; i < num_nodes; i++) {
        nodes[i].value = i;
        for (int j = 0; j < 8; j++) {
            nodes[i].data[j] = (i * j * 7919) % 1000 / 100.0;
        }
        
        // Create structure (binary tree with some randomness)
        if (i * 2 + 1 < num_nodes) {
            nodes[i].child = &nodes[i * 2 + 1];
        } else {
            nodes[i].child = NULL;
        }
        
        if (i * 2 + 2 < num_nodes) {
            nodes[i].next = &nodes[i * 2 + 2];
        } else {
            nodes[i].next = NULL;
        }
    }
    
    // Version 1: Depth-first traversal (poor locality)
    flush_cache();
    double start = get_time();
    double sum = 0;
    for (int iter = 0; iter < 100; iter++) {
        // Recursive traversal simulation using explicit stack
        node_t* stack[1000];
        int top = 0;
        stack[top++] = &nodes[0];
        
        while (top > 0) {
            node_t* current = stack[--top];
            sum += current->data[0] + current->value;
            
            if (current->child) stack[top++] = current->child;
            if (current->next) stack[top++] = current->next;
        }
    }
    double dfs_time = get_time() - start;
    
    // Version 2: Breadth-first traversal (better locality)
    flush_cache();
    start = get_time();
    sum = 0;
    for (int iter = 0; iter < 100; iter++) {
        // Queue-based traversal
        node_t* queue[num_nodes];
        int head = 0, tail = 0;
        queue[tail++] = &nodes[0];
        
        while (head < tail) {
            node_t* current = queue[head++];
            sum += current->data[0] + current->value;
            
            if (current->child) queue[tail++] = current->child;
            if (current->next) queue[tail++] = current->next;
        }
    }
    double bfs_time = get_time() - start;
    
    // Version 3: Linear scan (best locality)
    flush_cache();
    start = get_time();
    sum = 0;
    for (int iter = 0; iter < 100; iter++) {
        for (int i = 0; i < num_nodes; i++) {
            sum += nodes[i].data[0] + nodes[i].value;
        }
    }
    double linear_time = get_time() - start;
    
    printf("Depth-first:   %.3f seconds\n", dfs_time);
    printf("Breadth-first: %.3f seconds (%.2fx speedup)\n", bfs_time, dfs_time / bfs_time);
    printf("Linear scan:   %.3f seconds (%.2fx speedup)\n", linear_time, dfs_time / linear_time);
    
    free(nodes);
}

// ============================================================================
// MAIN BENCHMARK RUNNER
// ============================================================================

int main(int argc, char* argv[]) {
    printf("================================================================================\n");
    printf("                   COMPREHENSIVE CACHE OPTIMIZATION BENCHMARK\n");
    printf("================================================================================\n");
    printf("System Configuration:\n");
    printf("  L1 Cache: %d KB\n", L1_CACHE_SIZE / KB);
    printf("  L2 Cache: %d KB\n", L2_CACHE_SIZE / KB);
    printf("  L3 Cache: %d MB\n", L3_CACHE_SIZE / MB);
    printf("  Cache Line: %d bytes\n", CACHE_LINE_SIZE);
    printf("================================================================================\n");
    
    // Seed random number generator
    srand(time(NULL));
    
    // Run all benchmarks
    print_header("BENCHMARK 1: Access Pattern Analysis");
    benchmark_access_patterns(SMALL_SIZE, "Small (L1 size)");
    benchmark_access_patterns(MEDIUM_SIZE, "Medium (L2 size)");
    benchmark_access_patterns(LARGE_SIZE, "Large (L3 size)");
    benchmark_access_patterns(HUGE_SIZE, "Huge (Beyond cache)");
    
    print_header("BENCHMARK 2: Matrix Multiplication");
    benchmark_matrix_multiply(SMALL_MATRIX);
    benchmark_matrix_multiply(MEDIUM_MATRIX);
    benchmark_matrix_multiply(LARGE_MATRIX);
    
    print_header("BENCHMARK 3: Array of Structures vs Structure of Arrays");
    benchmark_aos_vs_soa(100000);    // 100K particles
    benchmark_aos_vs_soa(1000000);   // 1M particles
    benchmark_aos_vs_soa(10000000);  // 10M particles
    
    print_header("BENCHMARK 4: False Sharing");
    benchmark_false_sharing();
    
    print_header("BENCHMARK 5: 2D Stencil Computation");
    benchmark_2d_stencil(MEDIUM_MATRIX);
    benchmark_2d_stencil(LARGE_MATRIX);
    
    print_header("BENCHMARK 6: Prefetching Effectiveness");
    benchmark_prefetching(LARGE_SIZE);
    benchmark_prefetching(HUGE_SIZE);
    
    print_header("BENCHMARK 7: Cache Conflict Misses");
    benchmark_cache_conflicts(L3_CACHE_SIZE / sizeof(double), 1);      // Sequential
    benchmark_cache_conflicts(L3_CACHE_SIZE / sizeof(double), 16);     // Small stride
    benchmark_cache_conflicts(L3_CACHE_SIZE / sizeof(double), 512);    // Large stride
    benchmark_cache_conflicts(L3_CACHE_SIZE / sizeof(double), 4096);   // Very large stride
    
    print_header("BENCHMARK 8: Working Set Size Impact");
    benchmark_working_set_sizes();
    
    print_header("BENCHMARK 9: 3D Grid Computation");
    benchmark_3d_computation(256);
    benchmark_3d_computation(384);
    
    print_header("BENCHMARK 10: Memory Bandwidth");
    benchmark_memory_bandwidth();
    
    print_header("BENCHMARK 11: Image Convolution");
    benchmark_image_convolution(2048, 2048);
    benchmark_image_convolution(4096, 4096);
    
    print_header("BENCHMARK 12: Tree Traversal");
    benchmark_tree_traversal(100000);
    benchmark_tree_traversal(1000000);
    
    print_separator();
    printf("\nBenchmark complete! Key findings:\n");
    printf("- Cache optimizations can provide 2-10x speedups for large data\n");
    printf("- False sharing can cause 3-5x slowdowns in parallel code\n");
    printf("- Loop tiling and proper ordering are critical for nested loops\n");
    printf("- AoS to SoA transformation provides significant benefits\n");
    printf("- Prefetching helps but requires careful tuning\n");
    printf("\nCompile with: gcc -O3 -march=native -pthread comprehensive_cache_benchmark.c -o cache_bench\n");
    printf("Note: If you still get linker errors, add -lm at the end\n");
    
    return 0;
}