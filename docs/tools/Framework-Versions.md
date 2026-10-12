# Framework versions

Every framework the firmware and its tools build with is pinned to one exact
version, written in one file. CI installs exactly that version and never
resolves "latest" at build time. Bump pull requests then move each pin to the
newest release, so the pins do not fall behind.

## Where each version lives

| Framework | Its one owner | A bump pull request comes from | What the bump regenerates, or which check names it |
|---|---|---|---|
| ESP-IDF (CI image, commit, esp-clang release) | [`launcher/idf-pin.json`](../../launcher/idf-pin.json) | ESP-IDF refresh, weekly | the refresh writes the pin, the component lock and the IRAM budgets; any other check that fails names its own command |
| ESP-IDF components | [`launcher/dependencies.lock`](../../launcher/dependencies.lock), within the ranges in each `idf_component.yml` | ESP-IDF refresh, weekly | the refresh re-solves it; a build that rewrites it fails CI with the diff |
| clang-format, clang-tidy | the `esp_clang` release in `launcher/idf-pin.json` | moves with ESP-IDF | a new major reformats the tree: that bump is a migration |
| QEMU and its patches | ESP-IDF's `tools/tools.json` ([`patched_qemu.py`](../../launcher/test/qemu/patched_qemu.py)) | moves with ESP-IDF | a patch that no longer applies stops the QEMU build, naming the file |
| Python | [`.python-version`](../../.python-version) | Renovate | CI proves it; a new minor waits for the bake stack's wheels |
| Python packages | `requirements*.txt` beside the tools that use them | Renovate, one PR for all | a bake package moves bake keys: the Bakes workflow bakes, then `bake.py lock --from-run N` |
| PyTorch and nvdiffrast for the GPU fit | [`requirements-gpu.txt`](../../launcher/tools/r3d/requirements-gpu.txt) | Renovate, from the dashboard | the GPU runner's environment is rebuilt by hand ([r3d README](../../launcher/tools/r3d/README.md)) |
| Node | [`.node-version`](../../.node-version) | Renovate | CI proves it |
| Node packages | `package.json` + `package-lock.json` in [`scripts/gates/`](../../scripts/gates/package.json) and [`scripts/gates/mermaid/`](../../scripts/gates/mermaid/package.json) | Renovate, one PR for all | CI proves it |
| microui, Waveshare BSP | the submodule commit, and the commit each vendored copy was taken from in [`third_party/vendored.json`](../../third_party/vendored.json) | Renovate, monthly | `scripts/deps/vendored.py check` fails until `vendored.py update` merges the upstream change into the copy |
| meshoptimizer, ufbx | the submodule commit | Renovate, monthly | the commit is in every mesh bake key: the Bakes workflow bakes, then `bake.py lock --from-run N` |
| GitHub Actions | the commit SHA in each `uses:` line | Renovate | CI proves it |
| CI runner image | `runs-on: ubuntu-24.04` | Renovate | CI proves it; GitHub updates the image's own packages (gcc, apt's llvm) in place |

A major release is a migration, not a routine bump: Renovate lists it on the
repository's Dependency Dashboard issue and opens it only when someone ticks
it there, and a newer ESP-IDF release line (`release-v6.1` for `release-v5.5`)
is a warning in the refresh run. Each bump pull request names the standing
ticket for routine bumps; a migration gets its own.

## Matching your machine

`python scripts/deps/versions.py` lists every pin beside what your machine
has; `--drift` lists only the differences. A local ESP-IDF build, clang-format,
clang-tidy and the mermaid check also print a note naming both versions when
they run on another one, and carry on; CI runs the pinned one. To match:

| Framework | Command |
|---|---|
| ESP-IDF | in your ESP-IDF checkout, `git fetch`, `git checkout <commit from idf-pin.json>`, `git submodule update --init --recursive`, then `install.sh esp32s3` (`install.ps1` on Windows) |
| clang-format, clang-tidy | come with that ESP-IDF's esp-clang (`python tools/idf_tools.py install esp-clang`), or `pip install clang-format==<release>`, where `python scripts/lib/pinned_tool.py clang-format --pinned` prints the release |
| Python | install the version in `.python-version`, then `python -m pip install -r <requirements file>` for the tools you run |
| Node | install the version in `.node-version`, then `npm ci --prefix scripts/gates` and `npm ci --prefix scripts/gates/mermaid` |
| submodules | `git submodule update --init` |
| components | nothing: a build uses `launcher/dependencies.lock` as committed |

## How a bump arrives

Renovate reads [`.github/renovate.json5`](../../.github/renovate.json5), which
the Renovate config workflow validates, and opens its pull requests on Monday
mornings, submodules on the first of the month. Its branches and pull requests
pass the branch-name and ticket checks like any other.

The ESP-IDF refresh ([`idf-refresh.yml`](../../.github/workflows/idf-refresh.yml))
does what Renovate cannot, because only a run inside ESP-IDF can regenerate what
derives from it. It pulls the newest image of the pin's `tag`, has
`scripts/deps/idf_pin.py write` read the commit and esp-clang release from
inside it, runs `idf.py update-dependencies`, builds the three variants and
rewrites their IRAM budgets. A pull request opened by a workflow starts no
checks of its own: close and reopen it to run CI.
