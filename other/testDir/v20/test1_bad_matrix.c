// test1_bad_matrix.c - Should trigger loop reordering recommendation
#include <stdio.h>
#include <stdlib.h>

#define N 512

void bad_matrix_multiply(double A[N][N], double B[N][N], double C[N][N]) {
    // BAD: ijk order causes strided access to B
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < N; j++) {
            double sum = 0.0;
            for (int k = 0; k < N; k++) {
                sum += A[i][k] * B[k][j];  // B[k][j] is strided!
            }
            C[i][j] = sum;
        }
    }
}

int main() {
    static double A[N][N], B[N][N], C[N][N];
    
    // Initialize
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < N; j++) {
            A[i][j] = i + j;
            B[i][j] = i - j;
        }
    }
    
    bad_matrix_multiply(A, B, C);
    
    printf("Result: %f\n", C[N/2][N/2]);
    return 0;
}
