#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

#define ARRAY_SIZE 1024
#define MATRIX_SIZE 512
#define CACHE_LINE_SIZE 64
#define NUM_THREADS 4

// Example 1: Thrashing - Working set exceeds cache
void test_thrashing() {
    printf("Testing cache thrashing...\n");
    
    // Large matrix that exceeds L1/L2 cache
    static double matrix[MATRIX_SIZE][MATRIX_SIZE];
    double sum = 0;
    
    // Poor access pattern - column-wise traversal
    for (int j = 0; j < MATRIX_SIZE; j++) {
        for (int i = 0; i < MATRIX_SIZE; i++) {
            sum += matrix[i][j];  // Column-wise = poor spatial locality
        }
    }
    
    printf("Thrashing sum: %f\n", sum);
}

// Example 2: False Sharing - Multiple threads accessing same cache line
typedef struct {
    int counter;
    // No padding - causes false sharing!
} thread_data_bad_t;

typedef struct {
    int counter;
    char padding[CACHE_LINE_SIZE - sizeof(int)];  // Fixed version
} thread_data_good_t;

thread_data_bad_t shared_data_bad[NUM_THREADS];
thread_data_good_t shared_data_good[NUM_THREADS];

void* false_sharing_worker(void* arg) {
    int thread_id = *(int*)arg;
    
    // This causes false sharing
    for (int i = 0; i < 10000000; i++) {
        shared_data_bad[thread_id].counter++;
    }
    
    return NULL;
}

void test_false_sharing() {
    printf("Testing false sharing...\n");
    pthread_t threads[NUM_THREADS];
    int thread_ids[NUM_THREADS];
    
    clock_t start = clock();
    
    // Create threads that will fight over cache lines
    for (int i = 0; i < NUM_THREADS; i++) {
        thread_ids[i] = i;
        pthread_create(&threads[i], NULL, false_sharing_worker, &thread_ids[i]);
    }
    
    // Wait for threads
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }
    
    clock_t end = clock();
    double time_spent = ((double)(end - start)) / CLOCKS_PER_SEC;
    printf("False sharing time: %f seconds\n", time_spent);
}

// Example 3: Irregular Gather/Scatter
void test_gather_scatter() {
    printf("Testing irregular gather/scatter...\n");
    
    static int data[ARRAY_SIZE];
    static int indices[ARRAY_SIZE];
    int sum = 0;
    
    // Initialize with random indices
    for (int i = 0; i < ARRAY_SIZE; i++) {
        data[i] = i;
        indices[i] = rand() % ARRAY_SIZE;  // Random access pattern
    }
    
    // Irregular gather operation
    for (int i = 0; i < ARRAY_SIZE; i++) {
        sum += data[indices[i]];  // Indirect, random access
    }
    
    printf("Gather/scatter sum: %d\n", sum);
}

// Example 4: Loop-carried dependency
void test_loop_carried_dependency() {
    printf("Testing loop-carried dependency...\n");
    
    static double fibonacci[ARRAY_SIZE];
    fibonacci[0] = 0;
    fibonacci[1] = 1;
    
    // Each iteration depends on previous ones
    for (int i = 2; i < ARRAY_SIZE; i++) {
        fibonacci[i] = fibonacci[i-1] + fibonacci[i-2];  // Dependency!
    }
    
    printf("Fibonacci[100] = %f\n", fibonacci[100]);
}

// Example 5: Streaming access that evicts useful data
void test_streaming_eviction() {
    printf("Testing streaming eviction...\n");
    
    static double useful_data[1024];  // Want to keep this in cache
    static double stream_data[1024 * 1024];  // Large streaming data
    
    // Initialize useful data
    for (int i = 0; i < 1024; i++) {
        useful_data[i] = i * 2.0;
    }
    
    double sum = 0;
    
    // Repeatedly access useful data with streaming interference
    for (int iter = 0; iter < 10; iter++) {
        // Access useful data
        for (int i = 0; i < 1024; i++) {
            sum += useful_data[i];
        }
        
        // Stream through large data (evicts useful_data)
        for (int i = 0; i < 1024 * 1024; i++) {
            stream_data[i] = i;
        }
    }
    
    printf("Streaming sum: %f\n", sum);
}

// Example 6: Bank conflicts (power-of-2 stride)
void test_bank_conflicts() {
    printf("Testing bank conflicts...\n");
    
    #define BANK_SIZE 1024
    static float matrix[BANK_SIZE][BANK_SIZE];  // Power-of-2 size
    float sum = 0;
    
    // Strided access that hits same banks repeatedly
    for (int i = 0; i < BANK_SIZE; i++) {
        for (int j = 0; j < BANK_SIZE; j += 16) {  // Stride of 16
            sum += matrix[i][j];
        }
    }
    
    printf("Bank conflict sum: %f\n", sum);
}

// Example 7: Structure with poor layout (AoS problem)
typedef struct {
    double x;
    double y;
    double z;
    double vx;
    double vy;
    double vz;
    double mass;
    int id;
} particle_aos_t;

void test_aos_inefficiency() {
    printf("Testing AoS inefficiency...\n");
    
    static particle_aos_t particles[ARRAY_SIZE];
    double total_x = 0;
    
    // Only accessing x coordinate - wastes cache space
    for (int i = 0; i < ARRAY_SIZE; i++) {
        total_x += particles[i].x;  // Loads entire struct, uses one field
    }
    
    printf("Total X: %f\n", total_x);
}

// Example 8: Hotspot reuse pattern
void test_hotspot_reuse() {
    printf("Testing hotspot reuse...\n");
    
    static int hotspot_data[16];  // Small, frequently accessed
    long sum = 0;
    
    // Repeatedly hammer the same small memory region
    for (long i = 0; i < 100000000; i++) {
        sum += hotspot_data[i % 16];  // Always hits same 16 elements
    }
    
    printf("Hotspot sum: %ld\n", sum);
}

// Main test driver
int main() {
    printf("=== Cache Pattern Test Suite ===\n");
    printf("This program demonstrates various cache anti-patterns\n");
    printf("Run with cache_optimizer to detect these issues\n\n");
    
    // Initialize random seed
    srand(time(NULL));
    
    // Run all tests
    test_thrashing();
    printf("\n");
    
    test_false_sharing();
    printf("\n");
    
    test_gather_scatter();
    printf("\n");
    
    test_loop_carried_dependency();
    printf("\n");
    
    test_streaming_eviction();
    printf("\n");
    
    test_bank_conflicts();
    printf("\n");
    
    test_aos_inefficiency();
    printf("\n");
    
    test_hotspot_reuse();
    printf("\n");
    
    printf("=== All tests completed ===\n");
    return 0;
}
