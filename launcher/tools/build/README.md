# Build

| File | Purpose |
|---|---|
| [build_flash.sh](build_flash.sh) | Builds a selected firmware variant and handles locked flashing. |
| [build_flash_dev.sh](build_flash_dev.sh) | Runs the development build and flash wrapper. |
| [build_flash_diag.sh](build_flash_diag.sh) | Runs the diagnostics build and flash wrapper. |
| [build_diag_check.sh](build_diag_check.sh) | Builds diagnostics and checks the complexity ratchet. |
| [idf.sh](idf.sh) | Runs ESP-IDF commands from POSIX shells. |
| [idf_shim.bat](idf_shim.bat) | Starts ESP-IDF commands from Git Bash on Windows. |
| [idf_variant.sh](idf_variant.sh) | Selects ESP-IDF configuration for a build variant. |
| [espressif.py](espressif.py) | Finds the local ESP-IDF Python and tool installation. |
| [espressif.sh](espressif.sh) | Finds the local Espressif tools from shell scripts. |
| [find_cc.sh](find_cc.sh) | Finds a host C compiler. |
| [check_release_symbols.sh](check_release_symbols.sh) | Checks that release firmware excludes development symbols. |
