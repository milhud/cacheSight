// cache_benchmark.c - Properly benchmark cache optimizations
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <pthread.h>
#include <math.h>

// Larger sizes to actually stress the cache
#define SMALL_ARRAY 4096
#define LARGE_ARRAY (16 * 1024 * 1024 / sizeof(double))  // 16MB of doubles
#define MATRIX_DIM 2048  // 32MB matrix

// Timing function
double get_time() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec * 1e-6;
}

// Compare implementations with timing
void benchmark_sequential() {
    printf("\n=== Sequential Access Benchmark ===\n");
    
    double *data = malloc(LARGE_ARRAY * sizeof(double));
    
    // Initialize
    for (size_t i = 0; i < LARGE_ARRAY; i++) {
        data[i] = i * 0.5;
    }
    
    // Unoptimized version
    double start = get_time();
    double sum1 = 0;
    for (int iter = 0; iter < 10; iter++) {
        for (size_t i = 0; i < LARGE_ARRAY; i++) {
            sum1 += data[i];
        }
    }
    double unopt_time = get_time() - start;
    
    // Optimized with prefetching
    start = get_time();
    double sum2 = 0;
    for (int iter = 0; iter < 10; iter++) {
        for (size_t i = 0; i < LARGE_ARRAY; i++) {
            if (i + 8 < LARGE_ARRAY) {
                __builtin_prefetch(&data[i + 8], 0, 3);
            }
            sum2 += data[i];
        }
    }
    double opt_time = get_time() - start;
    
    printf("Unoptimized: %.3f seconds (sum=%.0f)\n", unopt_time, sum1);
    printf("Optimized:   %.3f seconds (sum=%.0f)\n", opt_time, sum2);
    printf("Speedup:     %.2fx\n", unopt_time / opt_time);
    
    free(data);
}

void benchmark_matrix_transpose() {
    printf("\n=== Matrix Transpose Benchmark ===\n");
    
    double (*matrix)[MATRIX_DIM] = malloc(sizeof(double[MATRIX_DIM][MATRIX_DIM]));
    double (*result)[MATRIX_DIM] = malloc(sizeof(double[MATRIX_DIM][MATRIX_DIM]));
    
    // Initialize
    for (int i = 0; i < MATRIX_DIM; i++) {
        for (int j = 0; j < MATRIX_DIM; j++) {
            matrix[i][j] = i * MATRIX_DIM + j;
        }
    }
    
    // Naive transpose (poor cache usage)
    double start = get_time();
    for (int i = 0; i < MATRIX_DIM; i++) {
        for (int j = 0; j < MATRIX_DIM; j++) {
            result[j][i] = matrix[i][j];  // Writes are strided!
        }
    }
    double naive_time = get_time() - start;
    
    // Tiled transpose (cache-friendly)
    #define TILE 32
    start = get_time();
    for (int i = 0; i < MATRIX_DIM; i += TILE) {
        for (int j = 0; j < MATRIX_DIM; j += TILE) {
            // Transpose tile
            for (int ti = i; ti < i + TILE && ti < MATRIX_DIM; ti++) {
                for (int tj = j; tj < j + TILE && tj < MATRIX_DIM; tj++) {
                    result[tj][ti] = matrix[ti][tj];
                }
            }
        }
    }
    double tiled_time = get_time() - start;
    
    printf("Naive:  %.3f seconds\n", naive_time);
    printf("Tiled:  %.3f seconds\n", tiled_time);
    printf("Speedup: %.2fx\n", naive_time / tiled_time);
    
    free(matrix);
    free(result);
}

void benchmark_random_access() {
    printf("\n=== Random vs Sorted Access ===\n");
    
    size_t size = 1024 * 1024;  // 1M elements
    int *data = malloc(size * sizeof(int));
    int *indices = malloc(size * sizeof(int));
    
    // Initialize
    for (size_t i = 0; i < size; i++) {
        data[i] = i;
        indices[i] = rand() % size;
    }
    
    // Random access
    double start = get_time();
    long long sum1 = 0;
    for (int iter = 0; iter < 100; iter++) {
        for (size_t i = 0; i < size; i++) {
            sum1 += data[indices[i]];
        }
    }
    double random_time = get_time() - start;
    
    // Sort indices
    int compare_int(const void *a, const void *b) {
        return *(int*)a - *(int*)b;
    }
    qsort(indices, size, sizeof(int), compare_int);
    
    // Sorted access
    start = get_time();
    long long sum2 = 0;
    for (int iter = 0; iter < 100; iter++) {
        for (size_t i = 0; i < size; i++) {
            sum2 += data[indices[i]];
        }
    }
    double sorted_time = get_time() - start;
    
    printf("Random:  %.3f seconds (sum=%lld)\n", random_time, sum1);
    printf("Sorted:  %.3f seconds (sum=%lld)\n", sorted_time, sum2);
    printf("Speedup: %.2fx\n", random_time / sorted_time);
    
    free(data);
    free(indices);
}

// False sharing benchmark
typedef struct {
    int counter;
} no_pad_t;

typedef struct {
    int counter;
    char padding[60];  // Pad to 64 bytes
} padded_t;

no_pad_t no_pad_counters[8];
padded_t padded_counters[8];
#define ITERATIONS 100000000

void* false_sharing_unopt(void* arg) {
    int id = *(int*)arg;
    for (int i = 0; i < ITERATIONS; i++) {
        no_pad_counters[id].counter++;
    }
    return NULL;
}

void* false_sharing_opt(void* arg) {
    int id = *(int*)arg;
    for (int i = 0; i < ITERATIONS; i++) {
        padded_counters[id].counter++;
    }
    return NULL;
}

void benchmark_false_sharing() {
    printf("\n=== False Sharing Benchmark ===\n");
    
    pthread_t threads[4];
    int ids[4] = {0, 1, 2, 3};
    
    // Without padding
    double start = get_time();
    for (int i = 0; i < 4; i++) {
        pthread_create(&threads[i], NULL, false_sharing_unopt, &ids[i]);
    }
    for (int i = 0; i < 4; i++) {
        pthread_join(threads[i], NULL);
    }
    double unopt_time = get_time() - start;
    
    // With padding
    start = get_time();
    for (int i = 0; i < 4; i++) {
        pthread_create(&threads[i], NULL, false_sharing_opt, &ids[i]);
    }
    for (int i = 0; i < 4; i++) {
        pthread_join(threads[i], NULL);
    }
    double opt_time = get_time() - start;
    
    printf("Without padding: %.3f seconds\n", unopt_time);
    printf("With padding:    %.3f seconds\n", opt_time);
    printf("Speedup:         %.2fx\n", unopt_time / opt_time);
}

// AoS vs SoA benchmark
typedef struct {
    double x, y, z;
    double vx, vy, vz;
} particle_aos_t;

void benchmark_aos_vs_soa() {
    printf("\n=== AoS vs SoA Benchmark ===\n");
    
    size_t n = 1024 * 1024;
    
    // AoS version
    particle_aos_t *aos = malloc(n * sizeof(particle_aos_t));
    for (size_t i = 0; i < n; i++) {
        aos[i].x = i * 0.1;
        aos[i].vx = 1.0;
    }
    
    double start = get_time();
    for (int iter = 0; iter < 100; iter++) {
        for (size_t i = 0; i < n; i++) {
            aos[i].x += aos[i].vx * 0.01;  // Only touching x and vx
        }
    }
    double aos_time = get_time() - start;
    
    // SoA version
    double *x = malloc(n * sizeof(double));
    double *vx = malloc(n * sizeof(double));
    for (size_t i = 0; i < n; i++) {
        x[i] = i * 0.1;
        vx[i] = 1.0;
    }
    
    start = get_time();
    for (int iter = 0; iter < 100; iter++) {
        for (size_t i = 0; i < n; i++) {
            x[i] += vx[i] * 0.01;
        }
    }
    double soa_time = get_time() - start;
    
    printf("AoS: %.3f seconds\n", aos_time);
    printf("SoA: %.3f seconds\n", soa_time);
    printf("Speedup: %.2fx\n", aos_time / soa_time);
    
    free(aos);
    free(x);
    free(vx);
}

int main() {
    printf("=== Cache Optimization Benchmarks ===\n");
    printf("Demonstrating when optimizations actually help\n");
    
    benchmark_sequential();
    benchmark_matrix_transpose();
    benchmark_random_access();
    benchmark_false_sharing();
    benchmark_aos_vs_soa();
    
    printf("\n=== Key Insights ===\n");
    printf("1. Optimizations need large datasets to show benefits\n");
    printf("2. Modern CPUs already handle many patterns well\n");
    printf("3. Some 'optimizations' add overhead for small data\n");
    printf("4. Always measure before optimizing!\n");
    
    return 0;
}
