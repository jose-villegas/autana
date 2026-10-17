"""Shell integration fixtures and YAML collection for pytest-embedded."""
import functools
import itertools
from pathlib import Path
import re
import shlex
import sys
import time

import pytest
import yaml

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "launcher/test"))
import qemu_run
from device_capture import BUILD_ID_RE, BUILD_ID_REQUEST, CRASH_LINE_RE, IMU_QUARTER_RE, RESET_LINE_RE

BOOT_TIMEOUT_S = 120
REPLY_TIMEOUT_S = 15
MILLISECONDS_PER_SECOND = 1000
FORBIDDEN = [re.compile(pattern.pattern.encode(), pattern.flags & ~re.UNICODE)
             for pattern in (CRASH_LINE_RE, RESET_LINE_RE)]
ERROR = rb"(?:\b\w+_ERR[^\r\n]*|ignoring line:[^\r\n]*)\r?\n"


@pytest.fixture
def app_path():
    return str(ROOT / "launcher")


@pytest.fixture
def qemu_prog_path(embedded_services):
    if "qemu" not in (embedded_services or "").split(","):
        return None
    binary = qemu_run.patched_qemu.installed(qemu_run.idf_path())
    if binary is None:
        pytest.fail("build the patched emulator: python launcher/test/qemu/patched_qemu.py build")
    return str(binary)


@pytest.fixture
def qemu_extra_args(embedded_services, test_case_tempdir):
    if "qemu" not in (embedded_services or "").split(","):
        return None
    efuse = Path(test_case_tempdir) / "efuse.bin"
    efuse.parent.mkdir(parents=True, exist_ok=True)
    efuse.write_bytes(qemu_run.efuse_with_mac(qemu_run.default_efuse(qemu_run.idf_path())))
    return shlex.join(qemu_run.qemu_machine_args(qemu_run.psram_mib(), str(efuse)))


@pytest.fixture
def console(dut):
    original_expect = dut.expect
    # QEMU starts and EspSerial explicitly resets; plain Serial only opens the port.
    boot = hasattr(dut, "qemu") or callable(getattr(getattr(dut, "serial", None), "hard_reset", None))
    def checked_expect(pattern, **kwargs):
        patterns = pattern if isinstance(pattern, list) else [pattern]
        match = original_expect([*patterns, *FORBIDDEN],
                                timeout=kwargs.pop("timeout", REPLY_TIMEOUT_S), **kwargs)
        for forbidden in FORBIDDEN:
            assert not forbidden.search(match[0]), f"firmware crash or reset: {match[0]!r}"
        return match

    dut.expect = checked_expect
    shell = Console(dut)
    if not boot:
        dut.write(BUILD_ID_REQUEST)
    expected = FORBIDDEN[1] if boot else BUILD_ID_RE.pattern.encode()
    match = original_expect([expected, *FORBIDDEN], timeout=BOOT_TIMEOUT_S)
    assert not FORBIDDEN[0].search(match[0]), f"firmware crash: {match[0]!r}"
    if FORBIDDEN[1].search(match[0]):
        dut.expect(qemu_run.LISTENING.encode(), timeout=BOOT_TIMEOUT_S)
        shell.exchange(BUILD_ID_REQUEST, BUILD_ID_RE.pattern)
    probe = shell.exchange("IMU quarter 0", IMU_QUARTER_RE.pattern)
    match = IMU_QUARTER_RE.fullmatch(probe)
    assert match and int(match[1]) == 0 and int(match[2]) > 0 and int(match[3]) > 0, probe
    shell.quarters = itertools.cycle(range(int(match[2])))
    next(shell.quarters)
    shell.sample_s = int(match[3]) / MILLISECONDS_PER_SECOND
    try:
        yield shell
        shell.exchange(BUILD_ID_REQUEST, BUILD_ID_RE.pattern)
    finally:
        dut.write("IMU release")


class Console:
    def __init__(self, dut):
        self.dut = dut
        self.sample_s = 0
        self.quarters = iter(())

    def exchange(self, command, expected, *, decline=False):
        self.dut.write(command)
        match = self.dut.expect([("(?:" + expected + r")\r?\n").encode(), ERROR])
        reply = match[0].decode().rstrip("\r\n")
        assert not re.search(ERROR.decode(), reply) or (decline and reply.endswith("_ERR not handled")), reply
        return reply

    def move(self, command):
        tag = command.split()[0].upper()
        reply = self.exchange(command, rf"{tag}(?:_OK[^\r\n]*|[^\r\n]*\r?\n{tag}_END)")
        parts = command.split()
        if len(parts) == 3 and parts[1] == "scene":
            rows = self.scenes(parts[0])
            assert [key for key, current in rows if current] == [parts[2]], (command, rows)
        self.tilt(next(self.quarters))
        return reply

    def tilt(self, quarter):
        started = time.monotonic()
        self.dut.write(f"IMU quarter {quarter}")
        self.exchange(BUILD_ID_REQUEST, BUILD_ID_RE.pattern)
        time.sleep(max(0, self.sample_s - (time.monotonic() - started)))

    def scenes(self, prefix):
        tag = prefix.upper()
        reply = self.exchange(prefix + " scenes", rf"{tag}(?!_END)[\s\S]*?{tag}_END|{tag}_ERR not handled", decline=True)
        if reply == tag + "_ERR not handled":
            return []
        rows = []
        lines = reply.splitlines()
        for line in lines[:-1]:
            match = re.fullmatch(rf"{re.escape(tag)} scene=(\S+) name=.+ current=([01])", line)
            assert match, f"malformed scenes reply: {line}"
            rows.append((match[1], match[2] == "1"))
        assert rows and len({key for key, _ in rows}) == len(rows), reply
        assert sum(current for _, current in rows) == 1, reply
        return rows


def run_recipe(console, steps):
    for step in steps:
        assert isinstance(step, dict) and "command" in step, step
        command = step["command"]
        assert isinstance(command, str) and command.strip() and "\n" not in command and "\r" not in command, step
        parts = command.lower().split()
        if parts[:2] == ["imu", "quarter"]:
            assert set(step) == {"command"} and len(parts) == 3 and parts[2].isdigit(), step
            console.tilt(int(parts[2]))
        else:
            assert set(step) == {"command", "expect"} and isinstance(step["expect"], str), step
            console.exchange(command, step["expect"])


class RecipeFile(pytest.File):
    def collect(self):
        steps = yaml.safe_load(self.path.read_text())
        assert isinstance(steps, list) and steps, "recipe must contain steps"
        yield RecipeItem.from_parent(self, name=self.path.stem, callobj=functools.partial(run_recipe, steps=steps))


class RecipeItem(pytest.Function):
    """Function is a pytest.Item with pytest's fixture setup and teardown."""

    def runtest(self):
        self.obj(console=self.funcargs["console"])


def pytest_configure(config):
    if any(Path(arg).resolve() == Path(__file__).parent for arg in config.args):
        config.args.extend(str(path) for path in sorted((ROOT / "launcher/main/apps").glob("*/tests/test_*.yaml"))
                           if str(path) not in config.args)


def pytest_ignore_collect(collection_path, config):
    if ROOT / "launcher/main/apps" in collection_path.parents and collection_path.is_file():
        return not (collection_path.suffix == ".yaml" and collection_path.name.startswith("test_"))


def pytest_collect_file(file_path, parent):
    if file_path.suffix == ".yaml" and file_path.name.startswith("test_"):
        return RecipeFile.from_parent(parent, path=file_path)


@pytest.fixture
def test_file_path(request):
    return str(request.node.path)
