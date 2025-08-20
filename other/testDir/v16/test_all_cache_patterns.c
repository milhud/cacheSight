// test_all_cache_patterns.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <math.h>

#define ARRAY_SIZE 4096
#define MATRIX_SIZE 1024
#define CACHE_LINE 64
#define L1_SIZE (32 * 1024)
#define L2_SIZE (512 * 1024)

// 1. SEQUENTIAL access pattern
void pattern_sequential() {
    static double data[ARRAY_SIZE];
    double sum = 0;
    
    // Perfect sequential access
    for (int i = 0; i < ARRAY_SIZE; i++) {
        sum += data[i];  // Line 22: Sequential access
    }
}

// 2. STRIDED access pattern  
void pattern_strided() {
    static double matrix[MATRIX_SIZE][MATRIX_SIZE];
    double sum = 0;
    
    // Stride of 8 elements
    for (int i = 0; i < MATRIX_SIZE; i++) {
        for (int j = 0; j < MATRIX_SIZE; j += 8) {
            sum += matrix[i][j];  // Line 33: Strided access
        }
    }
}

// 3. RANDOM access pattern
void pattern_random() {
    static int data[ARRAY_SIZE];
    static int indices[ARRAY_SIZE];
    int sum = 0;
    
    // Initialize random indices
    for (int i = 0; i < ARRAY_SIZE; i++) {
        indices[i] = rand() % ARRAY_SIZE;
    }
    
    // Random access via indices
    for (int i = 0; i < ARRAY_SIZE; i++) {
        sum += data[indices[i]];  // Line 50: Random access
    }
}

// 4. GATHER_SCATTER pattern
void pattern_gather_scatter() {
    static double src[ARRAY_SIZE];
    static double dst[ARRAY_SIZE];
    static int gather_indices[ARRAY_SIZE];
    
    // Initialize scattered indices
    for (int i = 0; i < ARRAY_SIZE; i++) {
        gather_indices[i] = (i * 17) % ARRAY_SIZE;  // Scattered pattern
    }
    
    // Gather operation
    for (int i = 0; i < ARRAY_SIZE; i++) {
        dst[i] = src[gather_indices[i]];  // Line 67: Gather/scatter
    }
}

// 5. ACCESS_LOOP_CARRIED_DEP pattern
void pattern_loop_carried_dependency() {
    static double data[ARRAY_SIZE];
    data[0] = 1.0;
    
    // Each iteration depends on previous
    for (int i = 1; i < ARRAY_SIZE; i++) {
        data[i] = data[i-1] * 1.1 + i;  // Line 77: Loop-carried dependency
    }
}

// 6. NESTED_LOOP pattern (poor loop ordering)
void pattern_nested_loop_poor() {
    static double matrix[MATRIX_SIZE][MATRIX_SIZE];
    double sum = 0;
    
    // Column-major in row-major layout (cache unfriendly)
    for (int j = 0; j < MATRIX_SIZE; j++) {
        for (int i = 0; i < MATRIX_SIZE; i++) {
            sum += matrix[i][j];  // Line 89: Poor nested loop order
        }
    }
}

// 7. INDIRECT_ACCESS pattern
void pattern_indirect_access() {
    static double *pointers[ARRAY_SIZE];
    static double data[ARRAY_SIZE];
    double sum = 0;
    
    // Initialize pointers
    for (int i = 0; i < ARRAY_SIZE; i++) {
        pointers[i] = &data[rand() % ARRAY_SIZE];
    }
    
    // Indirect access through pointers
    for (int i = 0; i < ARRAY_SIZE; i++) {
        sum += *pointers[i];  // Line 106: Indirect pointer access
    }
}

// Cache anti-patterns that classifier should detect:

// 8. THRASHING pattern
void antipattern_thrashing() {
    // Working set size > cache size
    static double huge_array[4 * 1024 * 1024 / sizeof(double)];  // 32MB
    double sum = 0;
    
    // Access pattern that thrashes the cache
    for (int iter = 0; iter < 10; iter++) {
        for (int i = 0; i < sizeof(huge_array)/sizeof(double); i += 64) {
            sum += huge_array[i];  // Line 120: Cache thrashing
        }
    }
}

// 9. FALSE_SHARING pattern
typedef struct {
    int counter;
    // No padding - false sharing!
} shared_counter_t;

shared_counter_t counters[8];  // Multiple threads will access

void* false_sharing_thread(void* arg) {
    int id = *(int*)arg;
    for (int i = 0; i < 10000000; i++) {
        counters[id].counter++;  // Line 135: False sharing
    }
    return NULL;
}

void antipattern_false_sharing() {
    pthread_t threads[4];
    int ids[4] = {0, 1, 2, 3};
    
    for (int i = 0; i < 4; i++) {
        pthread_create(&threads[i], NULL, false_sharing_thread, &ids[i]);
    }
    for (int i = 0; i < 4; i++) {
        pthread_join(threads[i], NULL);
    }
}

// 10. STREAMING_EVICTION pattern
void antipattern_streaming_eviction() {
    static double useful[1024];    // Want to keep in cache
    static double stream[1024 * 1024];  // Large streaming data
    double sum = 0;
    
    // Initialize useful data
    for (int i = 0; i < 1024; i++) {
        useful[i] = i * 2.0;
    }
    
    // Pattern that evicts useful data
    for (int iter = 0; iter < 100; iter++) {
        // Access useful data
        for (int i = 0; i < 1024; i++) {
            sum += useful[i];  // Line 167: Want to keep in cache
        }
        
        // Stream through large array (evicts useful data)
        for (int i = 0; i < 1024 * 1024; i++) {
            stream[i] = i;  // Line 172: Streaming eviction
        }
    }
}

// Structure patterns for data layout analysis

// 11. Array of Structures (AoS) - inefficient
typedef struct {
    double x, y, z;
    double vx, vy, vz;  
    double mass;
    int id;
} particle_t;

void pattern_aos_inefficient() {
    static particle_t particles[4096];
    double sum_x = 0;
    
    // Only accessing x field - wastes cache bandwidth
    for (int i = 0; i < 4096; i++) {
        sum_x += particles[i].x;  // Line 192: AoS inefficiency
    }
}

// 12. Unaligned access pattern
void pattern_unaligned() {
    static char buffer[ARRAY_SIZE * 8 + 1];
    double sum = 0;
    
    // Misaligned double access
    double* unaligned = (double*)(buffer + 1);  // Off by 1 byte!
    
    for (int i = 0; i < ARRAY_SIZE; i++) {
        sum += unaligned[i];  // Line 205: Unaligned access
    }
}

int main() {
    printf("=== Comprehensive Cache Pattern Test ===\n");
    printf("Testing all access patterns and anti-patterns\n\n");
    
    srand(time(NULL));
    
    // Test each pattern
    printf("Testing SEQUENTIAL pattern...\n");
    pattern_sequential();
    
    printf("Testing STRIDED pattern...\n");
    pattern_strided();
    
    printf("Testing RANDOM pattern...\n");
    pattern_random();
    
    printf("Testing GATHER_SCATTER pattern...\n");
    pattern_gather_scatter();
    
    printf("Testing LOOP_CARRIED_DEP pattern...\n");
    pattern_loop_carried_dependency();
    
    printf("Testing NESTED_LOOP pattern...\n");
    pattern_nested_loop_poor();
    
    printf("Testing INDIRECT_ACCESS pattern...\n");
    pattern_indirect_access();
    
    printf("Testing THRASHING antipattern...\n");
    antipattern_thrashing();
    
    printf("Testing FALSE_SHARING antipattern...\n");
    antipattern_false_sharing();
    
    printf("Testing STREAMING_EVICTION antipattern...\n");
    antipattern_streaming_eviction();
    
    printf("Testing AoS inefficiency...\n");
    pattern_aos_inefficient();
    
    printf("Testing UNALIGNED access...\n");
    pattern_unaligned();
    
    printf("\n=== All patterns tested ===\n");
    return 0;
}
