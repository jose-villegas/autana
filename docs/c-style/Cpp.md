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
| Format | `.clang-format` is a copy whose generated-file banner names its source; `format-file-list.sh` and `check-format.sh` from the submodule. |
| Comments, headers, citations, constants | The same scripts. They read `.cc .cpp .cxx .hh .hpp .hxx` with `.c .h` (`c_comments.SOURCE_EXTENSIONS`), raw strings and digit separators included. |
| Magic numbers, clones | The same scripts. Give `check_magic_numbers.py --include-root DIR` for each folder quoted includes resolve from. |
| Shell, exec bits, branch, ticket | `check-shell-scripts.sh`, `check-exec-bits.sh`, `check-branch-name.sh`, `check-pr-ticket.sh`. |
| Hooks | Set `core.hooksPath` to the submodule's `scripts/git-hooks`. The repository's own workflows mark what runs: `STAGED_TEXT_GATE` before a commit, `PUSH_TEST_GATE` before a push. |

## Language

| Rule | Why |
|---|---|
| C++20, no compiler extensions (`-std=c++20`). | One language on every compiler CI runs. |
| `-Wall -Wextra -Wpedantic -Werror -Werror=vla -ffp-contract=off`, plus `-Wshadow -Wnon-virtual-dtor -Wold-style-cast -Woverloaded-virtual -Wcast-align -Wnull-dereference -Wdouble-promotion -Wimplicit-fallthrough -Wextra-semi -Wdeprecated -Wzero-as-null-pointer-constant -Wsuggest-override`. | The host tests' C flags, and their C++ counterparts. `-ffp-contract=off` keeps float results equal to the engine's. |
| Include the engine through one header, as a `SYSTEM` include directory. | The engine's C keeps its own warnings; the C++ flags judge only the C++. |
| Constants are `constexpr` (or `consteval` functions), sets of them `enum class`; maths constants come from `<numbers>`. Never `#define` a value. | Typed, scoped, and read by the magic-number gate like a C `#define`. |
| Every engine handle has one RAII owner; no `new` or `delete` outside it. | A C pointer with no owner is freed twice or never. |
| No exception crosses a call into or out of the engine. Turn an engine error code into the C++ side's own result at the wrapper. | C frames do not unwind. |
| Pass pointer and length as `std::span`; fill an engine struct with designated initializers. | The C call stays visible and its fields named. |
| Constrain templates with concepts, not SFINAE. | The constraint reads as what it requires. |

## Static analysis

clang-tidy runs at the formatter's LLVM major, with `WarningsAsErrors: '*'`:

- `bugprone-*`, `performance-*`;
- `modernize-use-designated-initializers`, `modernize-use-ranges`, `modernize-use-std-numbers`, `modernize-use-starts-ends-with`, `modernize-use-integer-sign-comparison`, `modernize-use-constraints`;
- `cppcoreguidelines-owning-memory`, `cppcoreguidelines-pro-type-member-init`, `cppcoreguidelines-special-member-functions`, `cppcoreguidelines-avoid-capturing-lambda-coroutines`;
- `misc-const-correctness`, `readability-use-std-min-max`;
- `readability-function-cognitive-complexity` at `FAIL_THRESHOLD` from `launcher/tools/quality/complexity_gate.py`.

A sanitizer build runs the tests with AddressSanitizer (on Linux) and
UndefinedBehaviorSanitizer. Where the toolchain has no UBSan runtime (MinGW), it uses
`-fsanitize=undefined -fsanitize-undefined-trap-on-error`, the fallback the host
tests take (`launcher/tools/build/host_make.sh`).
