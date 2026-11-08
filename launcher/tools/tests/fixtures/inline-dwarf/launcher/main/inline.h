/* inline: Compiler inputs for the inline DWARF placement fixtures. */
static inline int
foo(int x) {
    if (x > 7) {
        asm volatile("" : "+r"(x));
        return x * x;
    }
    asm volatile("" : "+r"(x));
    return x + 3;
}

static inline int
bar(int x) {
    return x * 3;
}
