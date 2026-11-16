#include "inline.h"

__attribute__((noinline)) int
caller(int x) {
    return foo(x);
}

__attribute__((noinline)) int
other(int x) {
    return bar(x);
}
