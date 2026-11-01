# C++

The engine is C. A program built on it may be C++, linking the engine's CMake
targets. Such a program follows this guide with the deltas below. The gates that
check it are the engine's own, run from its checkout.

## Running the gates from another repository

Every gate checks the repository it is run from, not the one it ships in. A
repository holding the engine as a submodule runs them by path, for example
`python third_party/autana/scripts/gates/check_comment_length.py`.

| Gate | In another repository |
|---|---|
| Format, clang-tidy | `scripts/cpp/style_configs.py` writes the project's `.clang-format` and `.clang-tidy`, each with a generated-file banner; `format-file-list.sh` and `check-format.sh` from the submodule. |
| Build | Include `scripts/cpp/standards.cmake`: `autana_cpp_sanitize()` before the engine's subdirectory, `autana_cpp_checks()` after it. |
| Comments, headers, citations, constants | The same scripts. They read `.cc .cpp .cxx .hh .hpp .hxx` with `.c .h` (`c_comments.SOURCE_EXTENSIONS`). The name gives the language: raw strings, digit separators and `constexpr` constants are read in C++ files only, so a C++ header is `.hpp` and a `.h` is C. |
| Magic numbers, clones | The same scripts. Give `check_magic_numbers.py --include-root DIR` for each folder quoted includes resolve from. |
| Shell, exec bits, branch, ticket | `check-shell-scripts.sh`, `check-exec-bits.sh`, `check-branch-name.sh`, `check-pr-ticket.sh`. |
| Hooks | Set `core.hooksPath` to the submodule's `scripts/git-hooks`. The repository's own workflows mark what runs: `STAGED_TEXT_GATE` before a commit, `PUSH_TEST_GATE` before a push. |

## Language

| Rule | Why |
|---|---|
| C++20, no compiler extensions (`-std=c++20`). | One language on every compiler CI runs. |
| Every warning an error, with the flags `AUTANA_CPP_FLAGS` in `scripts/cpp/standards.cmake` lists. | The host tests' C flags, and their C++ counterparts. |
| Include the engine through one header, as a `SYSTEM` include directory. | The engine's C keeps its own warnings; the C++ flags judge only the C++. |
| Constants are `constexpr` (or `consteval` functions), sets of them `enum class`; maths constants come from `<numbers>`. Never `#define` a value. | Typed, scoped, and read by the magic-number gate like a C `#define`. |
| Every engine handle has one RAII owner; no `new` or `delete` outside it. | A C pointer with no owner is freed twice or never. |
| No exception crosses a call into or out of the engine. Turn an engine error code into the C++ side's own result at the wrapper. | C frames do not unwind. |
| Pass pointer and length as `std::span`; fill an engine struct with designated initializers. | The C call stays visible and its fields named. |
| Constrain templates with concepts, not SFINAE. | The constraint reads as what it requires. |

## Static analysis

clang-tidy runs at the formatter's LLVM major, with `WarningsAsErrors: '*'`. Its
checks are `CHECKS` in `scripts/cpp/style_configs.py`, a curated set of bug, performance
and modern-C++ checks, with `readability-function-cognitive-complexity` at
`FAIL_THRESHOLD` from `launcher/tools/quality/complexity_gate.py`.

A sanitizer build (`AUTANA_CPP_SANITIZE`) takes the host tests' flags from their own
probe, `host_sanitizer_flags()` in `launcher/tools/build/host_make.sh`: UBSan, and ASan
on Linux, or UBSan trapping on its first error where the toolchain has no runtime
(MinGW). UBSan reports and continues, so the tests fail on its `runtime error:` lines,
or run with `UBSAN_OPTIONS=halt_on_error=1`.
