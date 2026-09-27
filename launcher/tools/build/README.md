# Build

| File | Purpose |
|---|---|
| [build.sh](build.sh) | Builds a selected firmware variant: what `autana build` runs, and the build half of `autana flash`, which then writes a snapshot of the image with `scripts/device/flash_image.sh`. Build with `autana build [rel\|dev\|diag]`. |
| [build_diag_check.sh](build_diag_check.sh) | Builds diagnostics with `autana build diag` and checks the complexity ratchet. |
| [idf.sh](idf.sh) | Runs ESP-IDF commands from POSIX shells. |
| [idf_shim.bat](idf_shim.bat) | Starts ESP-IDF commands from Git Bash on Windows. |
| [idf_variant.sh](idf_variant.sh) | Selects ESP-IDF configuration for a build variant. |
| [espressif.py](espressif.py) | Finds the local ESP-IDF Python and tool installation. |
| [espressif.sh](espressif.sh) | Finds the local Espressif tools from shell scripts. |
| [find_cc.sh](find_cc.sh) | Finds a host C compiler. |
| [check_release_symbols.sh](check_release_symbols.sh) | Checks that release firmware excludes development symbols. |
