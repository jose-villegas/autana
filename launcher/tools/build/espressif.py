"""ESP-IDF checkout, paired tools and interpreter resolution.

Process IDF_PATH overrides the pin (pinned_tools_root ignores it); Windows installer IDF_PATH is a candidate.

    from espressif import idf_path, espressif_tools_root, idf_python
    checkout = idf_path()           # best effort, may not exist
    tools = espressif_tools_root()  # usable without a checkout
    python = idf_python()           # paired tools, or this interpreter

Build entry points use require_idf() to validate the selected installation.
Shell callers use this module's CLI; no persistent environment is changed.
"""
import json
import glob
import os
import re
import shutil
import sys
import subprocess
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(PROJECT_ROOT / "scripts/lib"))
import autana_config
from git_env import env_without_repository

VERSION = re.compile(r"idf(\d+)\.(\d+)_py(\d+)\.(\d+)_env")
ESP_CLANG = re.compile(r"esp-(\d+\.\d+\.\d+)")


def espressif_tools_root():
    """The tools installation paired with the selected ESP-IDF."""
    explicit = _override("IDF_TOOLS_PATH", os.environ, persistent_environment)
    return Path(explicit).resolve() if explicit else find_idf()[1]


def xtensa_tool(tool):
    """The ESP32-S3 toolchain's `tool` (objdump, nm, ...): from PATH, else
    from the tools installation. Exits when neither has it."""
    name = "xtensa-esp32s3-elf-" + tool
    found = shutil.which(name)
    if found:
        return found
    pattern = "tools/xtensa-esp-elf/*/xtensa-esp-elf/bin/" + name + "*"
    for candidate in sorted(espressif_tools_root().glob(pattern)):
        if candidate.is_file():
            return str(candidate)
    sys.exit("no %s on PATH or under the ESP-IDF tools" % name)


def idf_path(project=None):
    """The explicit checkout or the first installed checkout matching the pin."""
    return find_idf(project)[0]


IDF_PIN = "launcher/idf-pin.json"


def pinned_tools_root(project=None, **search):
    """The tools installation of the ESP-IDF checkout the pin selects, whatever
    IDF_PATH / IDF_TOOLS_PATH say, so a gate pinned to the CI image's tools
    still finds them while a session's environment points at another install."""
    base = search.pop("environ", None)
    env = {k: v for k, v in (os.environ if base is None else base).items()
           if k not in ("IDF_PATH", "IDF_TOOLS_PATH")}
    return find_idf(project, environ=env, **search)[1]


def idf_pin(root=Path(__file__).resolve().parents[3]):
    """launcher/idf-pin.json: the image CI builds in (`image`, its tag and
    digest), that image's ESP-IDF `commit`, and the LLVM release of its
    esp-clang (`esp_clang`), which also pins clang-format and clang-tidy."""
    return json.loads((root / IDF_PIN).read_text(encoding="utf-8"))


def idf_commit(idf):
    """The commit the ESP-IDF checkout at `idf` is at, or None when it is not one."""
    if not (Path(idf) / ".git").exists():
        return None
    try:
        result = subprocess.run(["git", "-C", str(idf), "rev-parse", "HEAD"], capture_output=True, text=True,
                                env=env_without_repository())
    except OSError:
        return None
    return result.stdout.strip() if result.returncode == 0 else None


def esp_clang(tools_json):
    """The LLVM release of the esp-clang ESP-IDF's tools.json recommends: esp-19.1.2_20250312 is 19.1.2."""
    tools = json.loads(Path(tools_json).read_text(encoding="utf-8"))["tools"]
    tool = next(t for t in tools if t["name"] == "esp-clang")
    name = next(v["name"] for v in tool["versions"] if v.get("status") == "recommended")
    return ESP_CLANG.match(name).group(1)


def idf_facts(idf):
    """What launcher/idf-pin.json records about the ESP-IDF at `idf`."""
    return {"commit": idf_commit(idf), "esp_clang": esp_clang(Path(idf) / "tools" / "tools.json")}


def idf_drift(found, pin=None):
    """One line per fact in `found` the pin records otherwise, naming both values;
    empty when they all match."""
    pin = idf_pin() if pin is None else pin
    return [f"{key} {found[key]} here, {pin.get(key) or '(none)'} pinned"
            for key in found if pin.get(key) != found[key]]


def _version_key(path):
    """(idf major, idf minor, py major, py minor), so idf5.10 sorts after
    idf5.9 and py3.14 after py3.9; a plain string sort gets both backwards
    (`"5.10" < "5.9"`)."""
    m = VERSION.search(path)
    return tuple(int(g) for g in m.groups()) if m else (0, 0, 0, 0)


def idf_python(*, project=None, installation=None):
    """`IDF_PYTHON_ENV_PATH` (set once ESP-IDF's export script is sourced)
    when paired with the selected install, then a matching checkout version
    under the selected tools, then this interpreter."""
    # POSIX exports put ESP-IDF's Python on PATH before launching this process.
    if os.name != "nt":
        return sys.executable
    checkout, tools = installation if installation is not None else find_idf(project)
    project = Path(project or idf_project())
    pinned_project = (project / IDF_PIN).is_file()
    if pinned_project and (checkout / "tools/cmake/version.cmake").is_file():
        major, minor = checkout_version(checkout)
        env_pattern = f"idf{major}.{minor}_*_env"
    else:
        env_pattern = "idf*_env"
    env = os.environ.get("IDF_PYTHON_ENV_PATH")
    if env:
        candidate = Path(env) / "Scripts/python.exe"
        if candidate.is_file() and (not pinned_project or
                                   (Path(env).match(env_pattern) and
                                    candidate.resolve().is_relative_to(tools.resolve()))):
            return str(candidate)
    pattern = str(tools / "python_env" / env_pattern / "Scripts" / "python.exe")
    found = sorted(glob.glob(pattern), key=_version_key)
    if found:
        return found[-1]
    return sys.executable


REGISTRY_ENV = r"SYSTEM\CurrentControlSet\Control\Session Manager\Environment"


def persistent_environment(key, locations=None):
    """Windows installer defaults, with user settings taking precedence."""
    if os.name != "nt":
        return None
    import winreg
    for hive, location in (locations if locations is not None else
                           ((winreg.HKEY_CURRENT_USER, "Environment"),
                            (winreg.HKEY_LOCAL_MACHINE, REGISTRY_ENV))):
        try:
            with winreg.OpenKey(hive, location) as registry:
                return winreg.QueryValueEx(registry, key)[0]
        except OSError:
            continue
    return None


def checkout_version(idf):
    """The checkout's major and minor version for pairing its Python environment."""
    text = (Path(idf) / "tools/cmake/version.cmake").read_text()
    return tuple(re.search(r"set\(IDF_VERSION_" + part + r"\s+(\d+)\)", text).group(1)
                 for part in ("MAJOR", "MINOR"))


def idf_image_tag(image):
    """The tag of an image reference: espressif/idf:release-v5.5@sha256:... is
    release-v5.5, and a registry port (host:5000/espressif/idf:...) is not a tag."""
    return image.split("@")[0].rpartition(":")[2]


def install_commands(pin, home=None, checkout=None):
    """Commands to install a missing pinned checkout, or just its missing tools."""
    tag = idf_image_tag(pin["image"]).removeprefix("release-")
    folder = Path("C:/Espressif") / tag if os.name == "nt" else (home or Path.home()) / "esp" / tag
    existing = checkout is not None
    checkout = Path(checkout) if existing else folder / "esp-idf"
    if existing:
        folder = checkout.parent
    installer = (f"powershell -NoProfile -Command '& {{ $env:IDF_TOOLS_PATH=\"{folder / 'tools'}\"; .\\install.ps1 esp32s3 }}'"
                 if os.name == "nt" else f"IDF_TOOLS_PATH='{folder / 'tools'}' ./install.sh esp32s3")
    if existing:
        return f"cd '{checkout}'\n{installer}"
    return (f"git clone https://github.com/espressif/esp-idf.git '{checkout}'\n"
            f"cd '{checkout}'\ngit checkout {pin['commit']}\n"
            f"git submodule update --init --recursive\n{installer}")


def _override(key, env, persistent):
    value = env.get(key)
    default = persistent(key) if os.name == "nt" else None
    if value and default and os.path.normcase(os.path.abspath(value)) == os.path.normcase(os.path.abspath(default)):
        return None
    return value


def find_idf(project=None, *, environ=None, roots=None,
             persistent=persistent_environment, home=None):
    """Best-effort checkout and tools paths; with no install found it
    falls back to ~/esp/esp-idf and ~/.espressif.

    Explicit process IDF_PATH wins; otherwise all candidates are searched for
    the pin. Tools-only callers can supply IDF_TOOLS_PATH without a checkout.
    """
    project = Path(project or idf_project())
    env = os.environ if environ is None else environ
    home = Path.home() if home is None else Path(home)
    selected = _override("IDF_PATH", env, persistent)
    default = persistent("IDF_PATH") if os.name == "nt" else None
    pin = idf_pin(project) if (project / IDF_PIN).is_file() else None
    if not selected and pin:
        candidates = [autana_config.path_value(value, project)
                      for value in autana_config.load(project).get("idf_installs", [])]
        if default:
            candidates.append(Path(default))
        if roots is None:
            roots = [home / "esp"]
            if os.name == "nt":
                roots = [Path("C:/Espressif"), Path("C:/Espressif/frameworks"), *roots]
        for root in roots:
            candidates.extend(sorted(Path(root).glob("*")))
        for candidate in candidates:
            for checkout in (candidate, candidate / "esp-idf"):
                if idf_commit(checkout) == pin["commit"]:
                    selected = checkout
                    break
            if selected:
                break
    checkout = Path(selected or env.get("IDF_PATH") or home / "esp/esp-idf")
    tools = _override("IDF_TOOLS_PATH", env, persistent)
    if not tools and (checkout / "tools/cmake/version.cmake").is_file():
        major, minor = checkout_version(checkout)
        candidates = [checkout.parent / "tools"]
        default_tools = persistent("IDF_TOOLS_PATH") if os.name == "nt" else None
        if default_tools:
            candidates.append(Path(default_tools))
        candidates.append(home / ".espressif")
        for candidate in candidates:
            if any(path.is_dir() for path in (candidate / "python_env").glob(f"idf{major}.{minor}_*_env")):
                tools = candidate
                break
    return checkout.resolve(), Path(tools or env.get("IDF_TOOLS_PATH") or home / ".espressif").resolve()


def require_idf(project=None, **search):
    """Require a usable checkout and matching tools, with actionable install commands."""
    project = Path(project or idf_project())
    checkout, tools = find_idf(project, **search)
    pin = idf_pin(project) if (project / IDF_PIN).is_file() else None
    override = _override("IDF_PATH", search.get("environ", os.environ),
                         search.get("persistent", persistent_environment))
    if pin and (not (checkout / "tools/cmake/version.cmake").is_file() or
                (not override and idf_commit(checkout) != pin["commit"])):
        commands = install_commands(pin, search.get("home"))
        raise ValueError("No installed ESP-IDF matches the pin. Run:\n" + commands)
    if pin:
        major, minor = checkout_version(checkout)
        environments = (tools / "python_env").glob(f"idf{major}.{minor}_*_env")
        if not any((path / "Scripts/python.exe").is_file() if os.name == "nt" else path.is_dir()
                   for path in environments):
            raise ValueError(f"No tools installed for ESP-IDF at {checkout}. Run:\n" +
                             install_commands(pin, search.get("home"), checkout))
    return checkout, tools


def idf_project():
    """The command's selected project, or this repository for standalone tooling."""
    project = autana_config.project_dir()
    return project if (project / "launcher/CMakeLists.txt").is_file() else PROJECT_ROOT


def idf_note_lines(project=None):
    """The validated installation line and any off-pin note for CLI entry points."""
    project = Path(project or idf_project())
    checkout, tools = require_idf(project)
    commit = idf_commit(checkout)
    pin = idf_pin(project) if (project / IDF_PIN).is_file() else None
    pinned = pin is not None and commit == pin["commit"]
    lines = [f"ESP-IDF {checkout} ({(commit or 'unknown')[:8]}, {'pinned' if pinned else 'override'}), tools {tools}"]
    if pin:
        for drift in idf_drift({"commit": commit}, pin):
            lines.append(f"note: ESP-IDF {drift}; CI builds on the pin (docs/tools/Framework-Versions.md)")
    return lines


def main():
    import shlex
    try:
        project = Path(sys.argv[2]) if len(sys.argv) > 2 else idf_project()
        verb = sys.argv[1]
        if verb == "env":
            checkout, tools = require_idf(project)
            python = idf_python(project=project, installation=(checkout, tools))
            print("IDF_PATH=" + shlex.quote(str(checkout)))
            print("IDF_TOOLS_PATH=" + shlex.quote(str(tools)))
            print("IDF_PYTHON=" + shlex.quote(python))
        elif verb == "tools":
            print(espressif_tools_root())
        elif verb == "note":
            print("\n".join(idf_note_lines(project)))
        else:
            raise ValueError(f"unknown ESP-IDF query: {verb}")
    except (ValueError, OSError) as error:
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
