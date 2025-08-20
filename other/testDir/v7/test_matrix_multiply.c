#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define N 512

// Poor cache performance version
void matrix_multiply_bad(double A[N][N], double B[N][N], double C[N][N]) {
    // Bad loop order - poor spatial locality for B
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < N; j++) {
            C[i][j] = 0;
            for (int k = 0; k < N; k++) {
                C[i][j] += A[i][k] * B[k][j];  // B accessed column-wise
            }
        }
    }
}

// Better cache performance version (for comparison)
void matrix_multiply_better(double A[N][N], double B[N][N], double C[N][N]) {
    // Better loop order - ikj instead of ijk
    for (int i = 0; i < N; i++) {
        for (int k = 0; k < N; k++) {
            for (int j = 0; j < N; j++) {
                C[i][j] += A[i][k] * B[k][j];
            }
        }
    }
}

int main() {
    static double A[N][N], B[N][N], C[N][N];
    
    // Initialize matrices
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < N; j++) {
            A[i][j] = rand() / (double)RAND_MAX;
            B[i][j] = rand() / (double)RAND_MAX;
            C[i][j] = 0;
        }
    }
    
    printf("Starting matrix multiplication (%dx%d)...\n", N, N);
    
    clock_t start = clock();
    matrix_multiply_bad(A, B, C);
    clock_t end = clock();
    
    double time_spent = ((double)(end - start)) / CLOCKS_PER_SEC;
    printf("Time taken: %f seconds\n", time_spent);
    printf("Sample result: C[0][0] = %f\n", C[0][0]);
    
    return 0;
}
