#include "primality_test.h"
#include <stdint.h>
#include <math.h>

// Simple primality test function will return 1 if the number is prime, 0 otherwise.
// TODO: Upgrade to Miller-Rabin primality test 
int is_prime(uint64_t n){
    if (n <= 1) return 0;
    if (n <= 3) return 1;
    if (n % 2 == 0 || n % 3 == 0) return 0;
    
    for (uint64_t i = 5; i * i <= n; i = i + 6){
        if (n % i == 0 || n % (i + 2) == 0){
            return 0;
        }
    }
    return 1;
}
