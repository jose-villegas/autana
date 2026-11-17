# Inline DWARF fixtures

The ELF files contain `foo` with a CU-relative range list and `bar` with
`low_pc`/`high_pc`. Their header file indices exercise DWARF 4 and 5 line tables.

Regenerate from this folder with Ubuntu GCC 13.3.0 and GNU ld 2.42:

```sh
for v in 4 5; do gcc -O2 -g -gdwarf-$v -fdebug-prefix-map="$PWD"=. -nostdlib -no-pie -Wl,-e,caller,--build-id=none,-z,max-page-size=0x100 launcher/main/foo.c -o inline-$v.elf; done
```

The tests copy these files and rename `.debug_info` for the missing-debug case.
No compiler or binary utilities run during tests.
