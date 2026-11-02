#include <stdio.h>
#include <stdlib.h>
#define panicf(...) do { fprintf(stderr, "panic: " __VA_ARGS__); fputc('\n', stderr); abort(); } while (0)
