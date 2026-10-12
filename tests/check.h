// CHECK(cond) for the data-free unit tests: prints the failing line and counts it in `fails`.
#pragma once
#include <cstdio>

static int fails = 0;
#define CHECK(c)                                        \
    do {                                                \
        if (!(c)) {                                     \
            printf("FAIL line %d: %s\n", __LINE__, #c); \
            fails++;                                    \
        }                                               \
    } while (0)
