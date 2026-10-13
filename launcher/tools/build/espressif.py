"""ESP-IDF checkout, paired tools and interpreter resolution.

Explicit process overrides win. Otherwise pinned projects select an installed
checkout at the pinned commit, ignoring Windows installer environment defaults.
Shell callers use this module's CLI; no persistent environment is changed.
"""
import json
import glob
import os
import re
import sys
from functools import lru_cache
from pathlib import Path

VERSION = re.compile(r"idf(\d+)\.(\d+)_py(\d+)\.(\d+)_env")
ESP_CLANG = re.compile(r"esp-(\d+\.\d+\.\d+)")


def espressif_tools_root():
    """The tools installation paired with the selected ESP-IDF."""
    return resolved_idf()[1]


def idf_path(project=None):
    """The explicit checkout or the first installed checkout matching the pin."""
    return resolved_idf(str(project) if project else None)[0]


IDF_PIN = "launcher/idf-pin.json"


def idf_pin(root=Path(__file__).resolve().parents[3]):
    """launcher/idf-pin.json: the image CI builds in (`image`, its tag and
    digest), that image's ESP-IDF `commit`, and the LLVM release of its
    esp-clang (`esp_clang`), which also pins clang-format and clang-tidy."""
    return json.loads((root / IDF_PIN).read_text(encoding="utf-8"))


def idf_commit(idf):
    """The commit the ESP-IDF checkout at `idf` is at, or None when it is not one."""
    try:
        gitdir = Path(idf) / ".git"
        if gitdir.is_file():
            gitdir = Path(idf) / gitdir.read_text().strip().removeprefix("gitdir: ")
        head = (gitdir / "HEAD").read_text().strip()
        if not head.startswith("ref: "):
            return head
        ref = head.removeprefix("ref: ")
        common = gitdir
        if (gitdir / "commondir").is_file():
            common = gitdir / (gitdir / "commondir").read_text().strip()
        for directory in (gitdir, common):
            if (directory / ref).is_file():
                return (directory / ref).read_text().strip()
        for line in (common / "packed-refs").read_text().splitlines():
            if line.endswith(" " + ref):
                return line.split()[0]
    except OSError:
        pass
    return None


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


def idf_python():
    """`IDF_PYTHON_ENV_PATH` (set once ESP-IDF's export script is sourced)
    when paired with the selected install, then a matching checkout version
    under espressif_tools_root(), then this interpreter."""
    if os.name != "nt":
        return sys.executable
    checkout, tools = resolved_idf()
    project = idf_project()
    pinned_project = (project / IDF_PIN).is_file()
    if pinned_project:
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
    return found[-1] if found else sys.executable


PROJECT_ROOT = Path(__file__).resolve().parents[3]
REGISTRY_ENV = r"SYSTEM\CurrentControlSet\Control\Session Manager\Environment"


def persistent_environment(key):
    """Windows installer defaults, with user settings taking precedence."""
    if os.name != "nt":
        return None
    import winreg
    for hive, location in ((winreg.HKEY_CURRENT_USER, "Environment"),
                           (winreg.HKEY_LOCAL_MACHINE, REGISTRY_ENV)):
        try:
            with winreg.OpenKey(hive, location) as registry:
                return winreg.QueryValueEx(registry, key)[0]
        except OSError:
            continue
    return None


def checkout_version(idf):
    text = (Path(idf) / "tools/cmake/version.cmake").read_text()
    return tuple(re.search(r"set\(IDF_VERSION_" + part + r"\s+(\d+)\)", text).group(1)
                 for part in ("MAJOR", "MINOR"))


def install_commands(pin, home=None, checkout=None):
    tag = pin["image"].split(":", 1)[1].split("@", 1)[0].removeprefix("release-")
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


def resolve_idf(project=PROJECT_ROOT, *, environ=None, roots=None,
                persistent=persistent_environment, home=None):
    """Resolve a project's checkout and tools; injectable search roots support isolated installs."""
    sys.path.insert(0, str(PROJECT_ROOT / "scripts/lib"))
    import autana_config
    project = Path(project)
    env = os.environ if environ is None else environ
    home = Path.home() if home is None else Path(home)
    if not (project / IDF_PIN).is_file():
        return (Path(env.get("IDF_PATH") or home / "esp/esp-idf"),
                Path(env.get("IDF_TOOLS_PATH") or home / ".espressif"))
    pin = idf_pin(project)
    config = autana_config.load(project)

    def explicit(key):
        value = env.get(key)
        default = persistent(key) if os.name == "nt" else None
        return value if value and (not default or os.path.normcase(os.path.abspath(value)) !=
                                  os.path.normcase(os.path.abspath(default))) else None

    selected = explicit("IDF_PATH")
    if not selected:
        candidates = [autana_config.path_value(value, project) for value in config.get("idf_installs", [])]
        default = persistent("IDF_PATH") if os.name == "nt" else None
        if default:
            candidates.append(Path(default))
        if roots is None:
            roots = [home / "esp"]
            if os.name == "nt":
                roots.insert(0, Path("C:/Espressif"))
        for root in roots:
            candidates.extend(sorted(Path(root).glob("*")))
        for candidate in candidates:
            for checkout in (candidate, candidate / "esp-idf"):
                if idf_commit(checkout) == pin["commit"]:
                    selected = checkout
                    break
            if selected:
                break
    if not selected:
        raise ValueError("No installed ESP-IDF matches the pin. Run:\n" + install_commands(pin, home))
    selected = Path(selected).resolve()
    tools = explicit("IDF_TOOLS_PATH")
    if not tools:
        major, minor = checkout_version(selected)
        for candidate in (selected.parent / "tools", home / ".espressif"):
            if any(path.is_dir() for path in (candidate / "python_env").glob(f"idf{major}.{minor}_*")):
                tools = candidate
                break
    if not tools:
        raise ValueError(f"No tools installed for ESP-IDF at {selected}. Run:\n" + install_commands(pin, home, selected))
    return selected, Path(tools).resolve()


@lru_cache(maxsize=None)
def _resolved_idf(project, checkout, tools, home):
    return resolve_idf(project, environ={"IDF_PATH": checkout, "IDF_TOOLS_PATH": tools}, home=Path(home))


def idf_project():
    sys.path.insert(0, str(PROJECT_ROOT / "scripts/lib"))
    import autana_config
    project = autana_config.project_dir()
    return project if os.environ.get(autana_config.PROJECT_ENV) or (project / "launcher/CMakeLists.txt").is_file() else PROJECT_ROOT


def resolved_idf(project=None):
    return _resolved_idf(str(project or idf_project()),
                         os.environ.get("IDF_PATH"), os.environ.get("IDF_TOOLS_PATH"), str(Path.home()))


def install_note(project=None):
    project = Path(project or idf_project())
    checkout, tools = resolved_idf(str(project))
    commit = idf_commit(checkout)
    pinned = (project / IDF_PIN).is_file() and commit == idf_pin(project)["commit"]
    return f"ESP-IDF {checkout} ({(commit or 'unknown')[:8]}, {'pinned' if pinned else 'override'}), tools {tools}"


def main():
    import shlex
    try:
        project = Path(sys.argv[2]) if len(sys.argv) > 2 else idf_project()
        checkout, tools = resolved_idf(str(project))
        verb = sys.argv[1]
        if verb == "env":
            print("IDF_PATH=" + shlex.quote(str(checkout)))
            print("IDF_TOOLS_PATH=" + shlex.quote(str(tools)))
        elif verb == "tools":
            print(tools)
        elif verb == "note":
            print(install_note(project))
            if (project / IDF_PIN).is_file():
                for drift in idf_drift({"commit": idf_commit(checkout)}, idf_pin(project)):
                    print(f"note: ESP-IDF {drift}; CI builds on the pin (docs/tools/Framework-Versions.md)")
        else:
            raise ValueError(f"unknown ESP-IDF query: {verb}")
    except (ValueError, OSError) as error:
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
