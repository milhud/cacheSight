// minimal_test.c - Should generate MAX 2 recommendations
#include <stdio.h>
#define N 100

void simple_loop() {
    double a[N], b[N];
    for (int i = 0; i < N; i++) {  // Should suggest vectorization
        a[i] = b[i] * 2.0;
    }
}

int main() {
    simple_loop();
    return 0;
}
