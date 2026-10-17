# Build environment

Which ESP-IDF `autana build` uses, and where its build folders live.

## Which ESP-IDF

`launcher/tools/build/espressif.py` picks the checkout, names it and its
paired tools on every build, and prints a drift note when it is off-pin.

| Order | Checkout selection |
|---|---|
| Process `IDF_PATH` | Any commit. On Windows the installer's stored value does not count as an override; on Linux a profile sourcing `export.sh` does. |
| `idf_installs` | Checkouts or their parent folders, in list order, matching `launcher/idf-pin.json`. |
| Installer `IDF_PATH` (Windows) | A candidate matching the pin, HKCU before HKLM. |
| Known folders | Under `C:\Espressif` (including `frameworks/`) and `~/esp`, matching the pin. |

With no match, a build stops and prints the clone, checkout, submodule and
install commands. Tools come from `IDF_TOOLS_PATH`, else the checkout's
sibling `tools/`, the installer's tools root, then `~/.espressif`, and need a
Python environment for the checkout's major.minor.

autana never changes environment variables or the registry. To override for
one command:

```sh
IDF_PATH="$HOME/esp/custom-idf" autana build dev
```

```powershell
powershell -NoProfile -Command { $env:IDF_PATH = 'C:\Espressif\my-idf'; autana build dev }
```

## Where builds live

Windows builds use short paths to stay under object-path limits:

```text
launcher\build.dev -> %LOCALAPPDATA%\autana\b\<hash>\build.dev
                                             <hash>\owner.txt
```

`owner.txt` names the launcher directory the hash comes from. The
`build_root` setting picks another root. Linux builds in-tree unless it is set,
then the in-tree names become symlinks (junctions on Windows).

The first build replaces an in-tree build folder with a link to the external
one, and builds delete folders of launcher directories that no longer exist.
A folder configured for another ESP-IDF is emptied, except `build.log` and the
build lock; so is an unstamped non-empty one, once.
