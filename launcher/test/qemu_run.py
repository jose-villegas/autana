#!/usr/bin/env python3
"""Boot a build.qemu image under Espressif's QEMU and report its self test.

run_qemu_tests.sh builds the image and calls this; it also runs alone against
a build directory that already exists:

    launcher/test/qemu_run.py launcher/build.qemu [--icount] [--timeout S]
    launcher/test/qemu_run.py launcher/build.qemu.shell --suite run_gfx_suite
                              [--suite ...] [--touch down,184,224 --touch up,184,224]
                              [--screenshot shot.png]

It merges the build's binaries into one flash image, writes the default eFuse
block ESP-IDF's own `idf.py qemu` uses, and starts qemu-system-xtensa
-M esp32s3 with the console on a local TCP socket, which it logs to a file.

With no --suite or --screenshot the image is expected to run its suites by
itself (CONFIG_LAUNCHER_SELFTEST_AUTORUN) and the run ends at
SELFTEST_COMPLETE. With either, the image is expected to boot into the shell
instead: once the console listener is up, each --suite is sent as RUNSUITE
and waited out to its RUNSUITE_COMPLETE, each --touch is left on the screen
in turn, and then --screenshot sends SCREENSHOT and writes the frame as a
PNG (and its state as .json) the way `autana screenshot` does from a board:
turned to its default landscape view.

--do drives that same image as a user would, one ordered step at a time, and
runs after the --suite and --touch options and before --screenshot:

    --do "tap 95 187" --do "wait 2500" --do "screenshot sponza.png"
    --do "tilt 0 -4096 0" --do "swipe 222 1 222 200"
    --do "send open <app>" --do "send home"

A tap or swipe takes pixels of that screenshot, as `autana tap` does, and
goes in as TOUCH lines in the panel's own frame; a touch (and --touch) is
sent as it is, in panel pixels. A tilt goes in as an IMU line, and a send
writes its line to the console as typed, as `autana console` forwards one.
Leave --icount off for this: emulated time then runs far slower than the
host's, and how long a press lasts is counted in the emulated clock.

After every step the image must answer BUILDID, and any run fails on a crash
line (a panic, abort(), a failed assert) or a second boot banner: the same
verdict the board's captures get from scripts/lib/device_capture.py.

What a run can and cannot say. Pass and fail are real for anything that does
not read a clock. A ceiling pegged on the board is reported and not enforced
in a CONFIG_LAUNCHER_QEMU image, and tests of hardware QEMU lacks (the
performance monitor, a touch controller that physically answers) skip
themselves. With
--icount virtual time advances one nanosecond per executed instruction, so a
"us per step" line times 1000 is instructions per step: exactly repeatable
for a step that runs on one core, and within about 1% when two cores share
it, because both cores' instructions are summed. It is never a time.

Several instances run at once; give each its own build directory copy or
--workdir, since the flash image is written beside the log.
"""

import argparse
import ast
import base64
import binascii
import glob
import json
import os
import re
import socket
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "tools", "device"))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "tools", "build"))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "..", "scripts", "device"))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "..", "scripts", "lib"))
import device_profile  # noqa: E402  (path must be set up first)
from espressif import espressif_tools_root, idf_python  # noqa: E402  (path must be set up first)
from device_report import SUITE_COMPLETE_RE  # noqa: E402  (the board tool's own reading)
from device_capture import crash_signs  # noqa: E402  (the board tool's own crash verdict)
import screenshot as wire  # noqa: E402  (the board tool's own protocol)

QEMU_PASS_RE = re.compile(r":PASS$", re.M)
QEMU_IGNORE_RE = re.compile(r":IGNORE")
QEMU_FAIL_RE = re.compile(r"^\S*:\d+:(\w+):FAIL:? ?(.*)$", re.M)

SENTINEL = "SELFTEST_COMPLETE"
LISTENING = "listening for 'screenshot'"
ANSI = re.compile(r"\x1b\[[0-9;]*m")


def find_one(pattern, what):
    found = sorted(glob.glob(os.path.expanduser(pattern)))
    if not found:
        sys.exit("qemu_run: no %s found (%s) - install it with "
                 "`idf_tools.py install qemu-xtensa`" % (what, pattern))
    return found[-1]


def default_efuse(idf_path):
    """The esp32s3 eFuse image from ESP-IDF's own QEMU target table."""
    src_path = os.path.join(idf_path, "tools", "idf_py_actions", "qemu_ext.py")
    with open(src_path, encoding="utf-8") as fh:
        tree = ast.parse(fh.read())
    for node in ast.walk(tree):
        if not (isinstance(node, ast.Call)
                and getattr(node.func, "id", "") == "QemuTarget"
                and node.args
                and isinstance(node.args[0], ast.Constant)
                and node.args[0].value == "esp32s3"):
            continue
        for inner in ast.walk(node):
            if (isinstance(inner, ast.Call)
                    and getattr(inner.func, "attr", "") == "unhexlify"):
                return binascii.unhexlify(inner.args[0].value)
    sys.exit("qemu_run: no esp32s3 eFuse image in %s" % src_path)


# A locally administered address. The image file starts at EFUSE_RD_WR_DIS
# (0x2C), so EFUSE_RD_MAC_SPI_SYS_0/1 (0x44, 0x48) sit at 0x18: the low four
# bytes of the MAC first, then the high two, each little-endian.
QEMU_MAC = bytes.fromhex("02005145 4d55".replace(" ", ""))
MAC_FILE_OFFSET = 0x44 - 0x2C


def efuse_with_mac(image):
    image = bytearray(image)
    image[MAC_FILE_OFFSET:MAC_FILE_OFFSET + 4] = QEMU_MAC[2:][::-1]
    image[MAC_FILE_OFFSET + 4:MAC_FILE_OFFSET + 6] = QEMU_MAC[:2][::-1]
    return bytes(image)


def free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def merge_flash(build_dir, out_path, python):
    with open(os.path.join(build_dir, "flasher_args.json")) as fh:
        args = json.load(fh)
    settings = args["flash_settings"]
    cmd = [python, "-m", "esptool", "--chip", "esp32s3", "merge_bin",
           "-o", out_path, "--fill-flash-size", settings["flash_size"],
           "--flash_mode", settings["flash_mode"],
           "--flash_size", settings["flash_size"],
           "--flash_freq", settings["flash_freq"]]
    for offset, name in sorted(args["flash_files"].items(),
                               key=lambda kv: int(kv[0], 16)):
        cmd += [offset, os.path.join(build_dir, name)]
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)


class Console:
    """QEMU's serial port over TCP: every byte is logged, lines are matched."""

    def __init__(self, proc, port, log_path, deadline):
        self.proc = proc
        self.deadline = deadline
        self.log = open(log_path, "wb")
        self.pending = b""
        self.sock = None
        while self.sock is None:
            try:
                self.sock = socket.create_connection(("127.0.0.1", port), 1.0)
            except OSError:
                if proc.poll() is not None or time.monotonic() > deadline:
                    raise
                time.sleep(0.2)
        self.sock.settimeout(1.0)

    def close(self):
        self.log.close()
        self.sock.close()

    def send(self, line):
        self.sock.sendall(line.encode() + b"\n")

    def lines(self, seconds=None):
        """Complete lines as they arrive, until QEMU exits or time runs out:
        the run's, or `seconds` from now when that comes first."""
        deadline = self.deadline
        if seconds is not None:
            deadline = min(deadline, time.monotonic() + seconds)
        while time.monotonic() < deadline and self.proc.poll() is None:
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout:
                continue
            if not chunk:
                return
            self.log.write(chunk)
            self.log.flush()
            self.pending += chunk
            while b"\n" in self.pending:
                raw, self.pending = self.pending.split(b"\n", 1)
                text = raw.decode("utf-8", errors="replace").rstrip("\r")
                yield ANSI.sub("", text)

    def wait_for(self, needle):
        for line in self.lines():
            if needle in line:
                return True
        return False


def take_screenshot(console, out_path):
    console.send(wire.TRIGGER.decode().strip())
    total, chunks, state = None, [], None
    for line in console.lines():
        if line.startswith(wire.REFUSED_PREFIX):
            print("screenshot refused: %s" % line[len(wire.REFUSED_PREFIX):])
            return False
        begin = wire.BEGIN_RE.match(line)
        if begin:
            total = int(begin.group(1))
        elif total is None:
            continue
        elif line.startswith(wire.DATA_PREFIX):
            chunks.append(line[len(wire.DATA_PREFIX):])
        elif line.startswith(wire.STATE_PREFIX):
            state = line[len(wire.STATE_PREFIX):]
        elif line == wire.END_LINE:
            bmp = base64.b64decode("".join(chunks))
            if len(bmp) != total:
                print("screenshot: decoded %d bytes, device announced %d"
                      % (len(bmp), total))
                return False
            png = wire.turn_png(wire.bmp_bytes_to_png(bmp), wire.DEFAULT_TURN_QUARTER)
            png_path, _ = wire.write_capture(out_path, png, state, wire.DEFAULT_TURN_QUARTER)
            print("screenshot: %s" % png_path)
            return True
    print("screenshot: the capture never finished")
    return False


# A sample is a LEVEL the image's polling task picks up on its own schedule,
# so each one is left in place long enough to be seen and acted on. There is
# no acknowledgement to wait for instead: see main/console/console_inject.c's
# own top comment for why it sets no flag.
TOUCH_SETTLE_S = 0.6


def run_suite(console, name):
    console.send("RUNSUITE %s" % name)
    for line in console.lines():
        complete = SUITE_COMPLETE_RE.search(line)
        if complete is None or complete.group("name") != name:
            continue
        if complete.group("found") != "1":
            print("suite %s: not registered in this image" % name)
            return False
        if int(complete.group("unmatched") or 0):
            print("suite %s: selected no test" % name)
            return False
        return True
    print("suite %s: never completed" % name)
    return False


# A swipe is this many samples, each left in place long enough to be polled.
SWIPE_POINTS = 12
SWIPE_POINT_S = 0.08


def touch(console, state, x, y):
    console.send("TOUCH %s %d %d" % (state, x, y))
    return True


def touch_tap(console, x, y):
    x, y = wire.panel_point(x, y, wire.DEFAULT_TURN_QUARTER)
    touch(console, "down", x, y)
    time.sleep(TOUCH_SETTLE_S)
    touch(console, "up", x, y)
    time.sleep(TOUCH_SETTLE_S)
    return True


def touch_swipe(console, x0, y0, x1, y1):
    x0, y0 = wire.panel_point(x0, y0, wire.DEFAULT_TURN_QUARTER)
    x1, y1 = wire.panel_point(x1, y1, wire.DEFAULT_TURN_QUARTER)
    for i in range(SWIPE_POINTS + 1):
        touch(console, "down", x0 + (x1 - x0) * i // SWIPE_POINTS,
              y0 + (y1 - y0) * i // SWIPE_POINTS)
        time.sleep(SWIPE_POINT_S)
    touch(console, "up", x1, y1)
    time.sleep(TOUCH_SETTLE_S)
    return True


def run_action(console, action):
    words = action.split()
    verb, args = words[0], words[1:]
    try:
        if verb == "suite" and len(args) == 1:
            return run_suite(console, args[0])
        if verb == "screenshot" and len(args) == 1:
            return take_screenshot(console, args[0])
        if verb == "touch" and len(args) == 3 and args[0] in ("down", "up"):
            touch(console, args[0], int(args[1]), int(args[2]))
            time.sleep(TOUCH_SETTLE_S)
            return True
        if verb == "tap" and len(args) == 2:
            return touch_tap(console, *map(int, args))
        if verb == "swipe" and len(args) == 4:
            return touch_swipe(console, *map(int, args))
        if verb == "tilt" and len(args) == 3:
            console.send("IMU %d %d %d" % tuple(map(int, args)))
            time.sleep(TOUCH_SETTLE_S)
            return True
        if verb == "send" and args:
            console.send(action.split(None, 1)[1])
            time.sleep(TOUCH_SETTLE_S)
            return True
        if verb == "wait" and len(args) == 1:
            time.sleep(int(args[0]) / 1000.0)
            return True
    except ValueError as error:
        print("not an action: %r (%s)" % (action, error))
        return False
    print("not an action: %r" % action)
    return False


# After each step the image is asked for its build id: the console task
# answering proves the firmware is still up. A reboot answers too, once the
# new boot's console is listening, and is caught by its second boot banner.
# Generous, because without --icount a heavy frame can starve the console.
HEARTBEAT = "BUILDID"
HEARTBEAT_REPLY = "BUILD_ID="
HEARTBEAT_S = 15


def heartbeat(console):
    console.send(HEARTBEAT)
    for line in console.lines(HEARTBEAT_S):
        if HEARTBEAT_REPLY in line:
            return True
    return False


def drive_shell(console, actions):
    if not console.wait_for(LISTENING):
        print("the console listener never came up - is this an image that "
              "boots into the shell (no sdkconfig.defaults.diag_autorun)?")
        return False
    time.sleep(0.3)
    ok = True
    for action in actions:
        ok = run_action(console, action) and ok
        if not heartbeat(console):
            print("no heartbeat within %d s after %r - the firmware stopped "
                  "answering; the rest of the steps are skipped" % (HEARTBEAT_S, action))
            return False
    return ok


# A run starts from QEMU's own power-on: one boot, and no other.
BOOTS = 1


def summarise(log_path):
    with open(log_path, "rb") as fh:
        text = ANSI.sub("", fh.read().decode("utf-8", errors="replace"))
    text = text.replace("\r", "")
    passed = len(QEMU_PASS_RE.findall(text))
    ignored = len(QEMU_IGNORE_RE.findall(text))
    failed = QEMU_FAIL_RE.findall(text)
    for line in re.findall(r"device_tests: (.*us per step.*)$", text,
                           flags=re.M):
        print("  " + line)
    for name, why in failed:
        print("  FAIL %s: %s" % (name, why[:120]))
    crashes = crash_signs(text, BOOTS)
    for sign in crashes:
        print("  " + sign)
    sentinel = re.search(r"^%s .*$" % SENTINEL, text, flags=re.M)
    print("%d passed, %d failed, %d skipped%s" %
          (passed, len(failed), ignored,
           "; " + sentinel.group(0) if sentinel else ""))
    return sentinel is not None, len(failed) + len(crashes)


def verdict(log_path, finished, actions):
    autorun_ended, failures = summarise(log_path)
    if not actions and not autorun_ended:
        print("NO %s - the run did not finish" % SENTINEL)
    return 0 if finished and not failures else 1


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("build_dir")
    parser.add_argument("--icount", action="store_true",
                        help="count instructions: -icount shift=0")
    parser.add_argument("--timeout", type=int, default=1800)
    parser.add_argument("--suite", action="append", default=[],
                        help="run this registered suite by name (repeatable); "
                             "needs an image that boots into the shell")
    parser.add_argument("--touch", action="append", default=[],
                        metavar="down|up,X,Y",
                        help="leave this touch sample in place, in panel "
                             "coordinates (repeatable, in order); needs a "
                             "CONFIG_LAUNCHER_QEMU image, where no controller "
                             "answers and the injected sample is all the polling "
                             "task has to read")
    parser.add_argument("--do", action="append", default=[], metavar="ACTION",
                        help="one step, in order (repeatable): 'suite NAME', "
                             "'tap X Y', 'swipe X0 Y0 X1 Y1', 'touch down|up "
                             "X Y', 'tilt AX AY AZ', 'send LINE', 'wait MS', "
                             "'screenshot PNG'. A tap or swipe takes pixels of the "
                             "screenshot, a touch the panel's own; tilt is raw "
                             "accelerometer counts, 4096 to the g; send writes "
                             "LINE to the console as typed, e.g. 'send home'")
    parser.add_argument("--screenshot", default=None, metavar="PNG",
                        help="capture the screen once the suites and touches "
                             "are done")
    parser.add_argument("--workdir", default=None,
                        help="where flash, eFuse and log go "
                             "(default: the build directory)")
    parser.add_argument("--log", default=None)
    parser.add_argument("--idf-path", default=os.environ.get("IDF_PATH"),
                        required=not os.environ.get("IDF_PATH"),
                        help="the ESP-IDF checkout (default: $IDF_PATH)")
    args = parser.parse_args(argv)

    actions = ["suite %s" % name for name in args.suite]
    actions += ["touch " + spec.replace(",", " ") for spec in args.touch]
    actions += args.do
    if args.screenshot:
        actions.append("screenshot %s" % args.screenshot)

    workdir = args.workdir or args.build_dir
    os.makedirs(workdir, exist_ok=True)
    flash = os.path.join(workdir, "qemu_flash.bin")
    efuse = os.path.join(workdir, "qemu_efuse.bin")
    log_path = args.log or os.path.join(workdir, "qemu_serial.log")

    python = idf_python()
    qemu = find_one(str(espressif_tools_root() / "tools" / "qemu-xtensa" / "*" / "qemu" / "bin" /
                        "qemu-system-xtensa*"), "qemu-system-xtensa")

    merge_flash(args.build_dir, flash, python)
    with open(efuse, "wb") as fh:
        fh.write(efuse_with_mac(default_efuse(args.idf_path)))
    if os.path.exists(log_path):
        os.remove(log_path)

    # The board's own PSRAM size: QEMU would happily offer more, and a test
    # that only fits in the surplus would pass here and fail on the board.
    profile = device_profile.load(None, None)
    psram_mib = device_profile.require(profile, "DP_PSRAM_BYTES", int) >> 20

    port = free_port()
    cmd = [qemu, "-M", "esp32s3", "-m", "%dM" % psram_mib,
           "-drive", "file=%s,if=mtd,format=raw" % flash,
           "-drive", "file=%s,if=none,format=raw,id=efuse" % efuse,
           "-global", "driver=nvram.esp32s3.efuse,property=drive,value=efuse",
           "-global", "driver=timer.esp32s3.timg,property=wdt_disable,"
                      "value=true",
           "-global", "driver=ssi_psram,property=is_octal,value=true",
           "-nographic", "-monitor", "none",
           "-serial", "tcp:127.0.0.1:%d,server=on,wait=on" % port]
    if args.icount:
        cmd += ["-icount", "shift=0"]

    proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL,
                            stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    finished = False
    try:
        console = Console(proc, port, log_path,
                          time.monotonic() + args.timeout)
        try:
            if actions:
                finished = drive_shell(console, actions)
            else:
                finished = console.wait_for(SENTINEL)
        finally:
            console.close()
    finally:
        proc.kill()
        proc.wait()

    print("console: %s" % log_path)
    return verdict(log_path, finished, actions)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
