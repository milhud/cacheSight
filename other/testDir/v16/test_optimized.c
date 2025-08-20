// test_all_cache_patterns_optimized.c
// This version implements the cache optimization recommendations
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <math.h>
#include <immintrin.h>  // For SIMD intrinsics
#include <xmmintrin.h>  // For prefetch

#define ARRAY_SIZE 4096
#define MATRIX_SIZE 1024
#define CACHE_LINE 64
#define L1_SIZE (32 * 1024)
#define L2_SIZE (512 * 1024)
#define TILE_SIZE 32  // For loop tiling
#define BLOCK_SIZE 64  // For cache blocking

// Helper macro for min
#define min(a,b) ((a) < (b) ? (a) : (b))

// 1. SEQUENTIAL access pattern - OPTIMIZED with vectorization and prefetching
void pattern_sequential_optimized() {
    // Ensure alignment for vectorization
    static double __attribute__((aligned(32))) data[ARRAY_SIZE];
    double sum = 0;
    
    // Initialize data
    for (int i = 0; i < ARRAY_SIZE; i++) {
        data[i] = i * 0.5;
    }
    
    #ifdef __AVX__
    // Vectorized version using AVX
    __m256d vsum = _mm256_setzero_pd();
    
    // Process 4 doubles at a time with prefetching
    for (int i = 0; i < ARRAY_SIZE; i += 4) {
        // Prefetch future data
        if (i + 16 < ARRAY_SIZE) {
            _mm_prefetch(&data[i + 16], _MM_HINT_T0);
        }
        
        __m256d vdata = _mm256_load_pd(&data[i]);
        vsum = _mm256_add_pd(vsum, vdata);
    }
    
    // Horizontal sum
    double temp[4];
    _mm256_store_pd(temp, vsum);
    sum = temp[0] + temp[1] + temp[2] + temp[3];
    #else
    // Fallback to standard loop with prefetching
    for (int i = 0; i < ARRAY_SIZE; i++) {
        if (i + 4 < ARRAY_SIZE) {
            __builtin_prefetch(&data[i + 4], 0, 3);
        }
        sum += data[i];
    }
    #endif
}

// 2. STRIDED access pattern - OPTIMIZED with loop tiling
void pattern_strided_optimized() {
    static double __attribute__((aligned(32))) matrix[MATRIX_SIZE][MATRIX_SIZE];
    double sum = 0;
    
    // Initialize matrix
    for (int i = 0; i < MATRIX_SIZE; i++) {
        for (int j = 0; j < MATRIX_SIZE; j++) {
            matrix[i][j] = i * j * 0.01;
        }
    }
    
    // Loop tiling to improve cache reuse
    for (int ii = 0; ii < MATRIX_SIZE; ii += TILE_SIZE) {
        for (int jj = 0; jj < MATRIX_SIZE; jj += TILE_SIZE) {
            // Process one tile
            for (int i = ii; i < min(ii + TILE_SIZE, MATRIX_SIZE); i++) {
                // Vectorized inner loop with reduced stride
                #pragma omp simd reduction(+:sum)
                for (int j = jj; j < min(jj + TILE_SIZE, MATRIX_SIZE); j++) {
                    sum += matrix[i][j];
                }
            }
        }
    }
}

// Comparison function for qsort (needs to be outside function)
static int compare_int(const void *a, const void *b) {
    return (*(int*)a - *(int*)b);
}

// 3. RANDOM access pattern - OPTIMIZED with sorting and blocking
void pattern_random_optimized() {
    static int __attribute__((aligned(32))) data[ARRAY_SIZE];
    static int indices[ARRAY_SIZE];
    int sum = 0;
    
    // Initialize data and indices
    for (int i = 0; i < ARRAY_SIZE; i++) {
        data[i] = i;
        indices[i] = rand() % ARRAY_SIZE;
    }
    
    // Sort indices for better locality
    int sorted_indices[ARRAY_SIZE];
    memcpy(sorted_indices, indices, ARRAY_SIZE * sizeof(int));
    qsort(sorted_indices, ARRAY_SIZE, sizeof(int), compare_int);
    
    // Process in cache-sized blocks
    for (int block = 0; block < ARRAY_SIZE; block += BLOCK_SIZE) {
        int block_end = min(block + BLOCK_SIZE, ARRAY_SIZE);
        
        // Prefetch block
        for (int i = block; i < block_end; i++) {
            __builtin_prefetch(&data[sorted_indices[i]], 0, 3);
        }
        
        // Process block
        for (int i = block; i < block_end; i++) {
            sum += data[sorted_indices[i]];
        }
    }
}

// 4. GATHER_SCATTER pattern - OPTIMIZED with blocking
void pattern_gather_scatter_optimized() {
    static double __attribute__((aligned(32))) src[ARRAY_SIZE];
    static double __attribute__((aligned(32))) dst[ARRAY_SIZE];
    static int gather_indices[ARRAY_SIZE];
    
    // Initialize
    for (int i = 0; i < ARRAY_SIZE; i++) {
        src[i] = i * 1.5;
        gather_indices[i] = (i * 17) % ARRAY_SIZE;
    }
    
    // Process in cache-sized blocks with prefetching
    for (int block = 0; block < ARRAY_SIZE; block += BLOCK_SIZE) {
        int block_end = min(block + BLOCK_SIZE, ARRAY_SIZE);
        
        // First pass: prefetch
        for (int i = block; i < block_end; i++) {
            __builtin_prefetch(&src[gather_indices[i]], 0, 1);
        }
        
        // Second pass: process
        for (int i = block; i < block_end; i++) {
            dst[i] = src[gather_indices[i]];
        }
    }
}

// 5. LOOP_CARRIED_DEP pattern - OPTIMIZED with unrolling
void pattern_loop_carried_dependency_optimized() {
    static double __attribute__((aligned(32))) data[ARRAY_SIZE];
    data[0] = 1.0;
    
    // Unroll to expose instruction-level parallelism
    int i;
    for (i = 1; i < ARRAY_SIZE - 3; i += 4) {
        data[i] = data[i-1] * 1.1 + i;
        data[i+1] = data[i] * 1.1 + (i+1);
        data[i+2] = data[i+1] * 1.1 + (i+2);
        data[i+3] = data[i+2] * 1.1 + (i+3);
    }
    
    // Handle remainder
    for (; i < ARRAY_SIZE; i++) {
        data[i] = data[i-1] * 1.1 + i;
    }
}

// 6. NESTED_LOOP pattern - OPTIMIZED with loop interchange
void pattern_nested_loop_optimized() {
    static double __attribute__((aligned(32))) matrix[MATRIX_SIZE][MATRIX_SIZE];
    double sum = 0;
    
    // Initialize matrix
    for (int i = 0; i < MATRIX_SIZE; i++) {
        for (int j = 0; j < MATRIX_SIZE; j++) {
            matrix[i][j] = i + j;
        }
    }
    
    // Optimized row-major access with tiling
    for (int ii = 0; ii < MATRIX_SIZE; ii += TILE_SIZE) {
        for (int jj = 0; jj < MATRIX_SIZE; jj += TILE_SIZE) {
            // Process tile with proper loop order
            for (int i = ii; i < min(ii + TILE_SIZE, MATRIX_SIZE); i++) {
                // Vectorized inner loop
                #pragma omp simd reduction(+:sum)
                for (int j = jj; j < min(jj + TILE_SIZE, MATRIX_SIZE); j++) {
                    sum += matrix[i][j];  // Sequential in memory
                }
            }
        }
    }
}

// 7. INDIRECT_ACCESS pattern - OPTIMIZED with cache blocking
void pattern_indirect_access_optimized() {
    static double *pointers[ARRAY_SIZE];
    static double __attribute__((aligned(32))) data[ARRAY_SIZE];
    double sum = 0;
    
    // Initialize
    for (int i = 0; i < ARRAY_SIZE; i++) {
        data[i] = i * 2.0;
        pointers[i] = &data[rand() % ARRAY_SIZE];
    }
    
    // Process in cache-sized blocks
    for (int block = 0; block < ARRAY_SIZE; block += BLOCK_SIZE) {
        int block_end = min(block + BLOCK_SIZE, ARRAY_SIZE);
        
        // First pass: prefetch
        for (int i = block; i < block_end; i++) {
            __builtin_prefetch(pointers[i], 0, 3);
        }
        
        // Second pass: process
        for (int i = block; i < block_end; i++) {
            sum += *pointers[i];
        }
    }
}

// 8. THRASHING pattern - OPTIMIZED with tiling and non-temporal hints
void antipattern_thrashing_optimized() {
    // Use smaller working set that fits in cache
    static double __attribute__((aligned(32))) huge_array[4 * 1024 * 1024 / sizeof(double)];
    double sum = 0;
    
    // Process in L2 cache-sized chunks
    size_t chunk_size = L2_SIZE / sizeof(double);
    size_t array_size = sizeof(huge_array) / sizeof(double);
    
    for (int iter = 0; iter < 10; iter++) {
        // Process array in chunks that fit in L2 cache
        for (size_t chunk = 0; chunk < array_size; chunk += chunk_size) {
            size_t chunk_end = min(chunk + chunk_size, array_size);
            
            #ifdef __AVX__
            // Use non-temporal stores for streaming
            for (size_t i = chunk; i < chunk_end; i += 8) {
                __m256d vdata = _mm256_load_pd(&huge_array[i]);
                __m256d vsum = _mm256_set1_pd(sum);
                vsum = _mm256_add_pd(vsum, vdata);
                
                // Store result
                double temp[4];
                _mm256_store_pd(temp, vsum);
                sum += temp[0] + temp[1] + temp[2] + temp[3];
            }
            #else
            // Fallback without AVX
            for (size_t i = chunk; i < chunk_end; i++) {
                sum += huge_array[i];
            }
            #endif
        }
    }
    #ifdef __AVX__
    _mm_sfence();  // Ensure completion
    #endif
}

// 9. FALSE_SHARING pattern - OPTIMIZED with padding
typedef struct {
    int counter;
    char padding[CACHE_LINE - sizeof(int)];  // Pad to cache line size
} __attribute__((aligned(CACHE_LINE))) shared_counter_optimized_t;

shared_counter_optimized_t counters_optimized[8];

void* false_sharing_thread_optimized(void* arg) {
    int id = *(int*)arg;
    // Each thread works on its own cache line
    for (int i = 0; i < 10000000; i++) {
        counters_optimized[id].counter++;
    }
    return NULL;
}

void antipattern_false_sharing_optimized() {
    pthread_t threads[4];
    int ids[4] = {0, 1, 2, 3};
    
    // Initialize counters
    for (int i = 0; i < 4; i++) {
        counters_optimized[i].counter = 0;
    }
    
    for (int i = 0; i < 4; i++) {
        pthread_create(&threads[i], NULL, false_sharing_thread_optimized, &ids[i]);
    }
    for (int i = 0; i < 4; i++) {
        pthread_join(threads[i], NULL);
    }
}

// 10. STREAMING_EVICTION pattern - OPTIMIZED with non-temporal hints
void antipattern_streaming_eviction_optimized() {
    static double __attribute__((aligned(32))) useful[1024];
    static double __attribute__((aligned(32))) stream[1024 * 1024];
    double sum = 0;
    
    // Initialize useful data
    for (int i = 0; i < 1024; i++) {
        useful[i] = i * 2.0;
    }
    
    for (int iter = 0; iter < 100; iter++) {
        // Keep useful data in cache with prefetching
        for (int i = 0; i < 1024; i += 4) {
            if (i + 16 < 1024) {
                _mm_prefetch(&useful[i + 16], _MM_HINT_T0);
            }
            sum += useful[i] + useful[i+1] + useful[i+2] + useful[i+3];
        }
        
        #ifdef __AVX__
        // Use non-temporal stores for streaming data
        for (int i = 0; i < 1024 * 1024; i += 4) {
            __m256d vdata = _mm256_set_pd(i+3, i+2, i+1, i);
            _mm256_stream_pd(&stream[i], vdata);  // Bypass cache
        }
        _mm_sfence();
        #else
        // Fallback without AVX
        for (int i = 0; i < 1024 * 1024; i++) {
            stream[i] = i;
        }
        #endif
    }
}

// 11. Structure of Arrays (SoA) - OPTIMIZED layout
// Instead of Array of Structures (AoS)
typedef struct {
    double *x, *y, *z;
    double *vx, *vy, *vz;
    double *mass;
    int *id;
    size_t count;
} particle_array_t;

void pattern_soa_optimized() {
    const int N = 4096;
    particle_array_t particles;
    
    // Allocate SoA with proper alignment
    particles.x = aligned_alloc(32, N * sizeof(double));
    particles.y = aligned_alloc(32, N * sizeof(double));
    particles.z = aligned_alloc(32, N * sizeof(double));
    particles.vx = aligned_alloc(32, N * sizeof(double));
    particles.vy = aligned_alloc(32, N * sizeof(double));
    particles.vz = aligned_alloc(32, N * sizeof(double));
    particles.mass = aligned_alloc(32, N * sizeof(double));
    particles.id = aligned_alloc(32, N * sizeof(int));
    particles.count = N;
    
    // Initialize
    for (int i = 0; i < N; i++) {
        particles.x[i] = i * 0.1;
        particles.y[i] = i * 0.2;
        particles.z[i] = i * 0.3;
        particles.vx[i] = 1.0;
        particles.vy[i] = 2.0;
        particles.vz[i] = 3.0;
        particles.mass[i] = 1.0;
        particles.id[i] = i;
    }
    
    double sum_x = 0;
    
    #ifdef __AVX__
    // Vectorized computation on x coordinates
    __m256d vsum = _mm256_setzero_pd();
    
    for (int i = 0; i < N; i += 4) {
        __m256d vx = _mm256_load_pd(&particles.x[i]);
        vsum = _mm256_add_pd(vsum, vx);
    }
    
    // Horizontal sum
    double temp[4];
    _mm256_store_pd(temp, vsum);
    sum_x = temp[0] + temp[1] + temp[2] + temp[3];
    #else
    // Fallback without AVX
    for (int i = 0; i < N; i++) {
        sum_x += particles.x[i];
    }
    #endif
    
    // Cleanup
    free(particles.x);
    free(particles.y);
    free(particles.z);
    free(particles.vx);
    free(particles.vy);
    free(particles.vz);
    free(particles.mass);
    free(particles.id);
}

// 12. Aligned access pattern - OPTIMIZED
void pattern_aligned_optimized() {
    // Properly aligned allocation
    double *aligned_data = aligned_alloc(32, ARRAY_SIZE * sizeof(double));
    double sum = 0;
    
    // Initialize
    for (int i = 0; i < ARRAY_SIZE; i++) {
        aligned_data[i] = i * 1.5;
    }
    
    #ifdef __AVX__
    // Vectorized processing with aligned loads
    __m256d vsum = _mm256_setzero_pd();
    
    for (int i = 0; i < ARRAY_SIZE; i += 4) {
        __m256d vdata = _mm256_load_pd(&aligned_data[i]);  // Aligned load
        vsum = _mm256_add_pd(vsum, vdata);
    }
    
    // Horizontal sum
    double temp[4];
    _mm256_store_pd(temp, vsum);
    sum = temp[0] + temp[1] + temp[2] + temp[3];
    #else
    // Fallback without AVX
    for (int i = 0; i < ARRAY_SIZE; i++) {
        sum += aligned_data[i];
    }
    #endif
    
    free(aligned_data);
}

// Matrix multiplication with all optimizations
void matrix_multiply_optimized() {
    #define MAT_SIZE 512  // Use macro for static array size
    static double __attribute__((aligned(32))) A[MAT_SIZE][MAT_SIZE];
    static double __attribute__((aligned(32))) B[MAT_SIZE][MAT_SIZE];
    static double __attribute__((aligned(32))) C[MAT_SIZE][MAT_SIZE];
    const int N = MAT_SIZE;
    
    // Initialize matrices
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < N; j++) {
            A[i][j] = i + j;
            B[i][j] = i - j;
            C[i][j] = 0.0;
        }
    }
    
    // Tiled matrix multiplication with all optimizations
    for (int ii = 0; ii < N; ii += TILE_SIZE) {
        for (int jj = 0; jj < N; jj += TILE_SIZE) {
            for (int kk = 0; kk < N; kk += TILE_SIZE) {
                // Process tile
                for (int i = ii; i < min(ii + TILE_SIZE, N); i++) {
                    for (int k = kk; k < min(kk + TILE_SIZE, N); k++) {
                        double a_ik = A[i][k];
                        
                        // Vectorized inner loop
                        #pragma omp simd
                        for (int j = jj; j < min(jj + TILE_SIZE, N); j++) {
                            C[i][j] += a_ik * B[k][j];
                        }
                    }
                }
            }
        }
    }
}

int main() {
    printf("=== Optimized Cache Pattern Test ===\n");
    printf("Testing all optimized patterns\n\n");
    
    srand(time(NULL));
    
    // Test optimized patterns
    printf("Testing SEQUENTIAL pattern (vectorized + prefetch)...\n");
    pattern_sequential_optimized();
    
    printf("Testing STRIDED pattern (tiled)...\n");
    pattern_strided_optimized();
    
    printf("Testing RANDOM pattern (sorted + blocked)...\n");
    pattern_random_optimized();
    
    printf("Testing GATHER_SCATTER pattern (blocked + prefetch)...\n");
    pattern_gather_scatter_optimized();
    
    printf("Testing LOOP_CARRIED_DEP pattern (unrolled)...\n");
    pattern_loop_carried_dependency_optimized();
    
    printf("Testing NESTED_LOOP pattern (interchanged + tiled)...\n");
    pattern_nested_loop_optimized();
    
    printf("Testing INDIRECT_ACCESS pattern (blocked)...\n");
    pattern_indirect_access_optimized();
    
    printf("Testing THRASHING antipattern (tiled + non-temporal)...\n");
    antipattern_thrashing_optimized();
    
    printf("Testing FALSE_SHARING antipattern (padded)...\n");
    antipattern_false_sharing_optimized();
    
    printf("Testing STREAMING_EVICTION antipattern (non-temporal)...\n");
    antipattern_streaming_eviction_optimized();
    
    printf("Testing SoA optimization...\n");
    pattern_soa_optimized();
    
    printf("Testing ALIGNED access...\n");
    pattern_aligned_optimized();
    
    printf("Testing optimized matrix multiplication...\n");
    matrix_multiply_optimized();
    
    printf("\n=== All optimized patterns tested ===\n");
    printf("Compile with: gcc -O3 -march=native -fopenmp test_all_cache_patterns_optimized.c -lpthread -lm\n");
    printf("For AVX support: gcc -O3 -march=native -mavx2 -mfma -fopenmp test_all_cache_patterns_optimized.c -lpthread -lm\n");
    
    return 0;
}