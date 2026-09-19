#!/usr/bin/env python3
"""Boot a build.qemu image under Espressif's QEMU and report its self test.

run_qemu_tests.sh builds the image and calls this; it also runs alone against
a build directory that already exists:

    launcher/test/qemu_run.py launcher/build.qemu [--icount] [--timeout S]
                              [--log FILE] [--idf-path DIR]

It merges the build's binaries into one flash image, writes the default eFuse
block ESP-IDF's own `idf.py qemu` uses, starts qemu-system-xtensa -M esp32s3
with the console in a file, and stops it when SELFTEST_COMPLETE appears.

What a run can and cannot say. Pass and fail are real for anything that does
not read a clock. Wall-clock budgets mean nothing here, and QEMU does not
model the performance monitor, so those tests fail by construction. With
--icount virtual time advances one nanosecond per executed instruction, so a
"us per step" line times 1000 is instructions per step: exactly repeatable
for a step that runs on one core, and within about 1% when two cores share
it, because both cores' instructions are summed. It is never a time.

Several instances run at once; give each its own build directory copy or
--workdir, since the flash image is written beside the log.
"""

import argparse
import ast
import binascii
import glob
import json
import os
import re
import subprocess
import sys
import time

SENTINEL = "SELFTEST_COMPLETE"
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


def wait_for_sentinel(proc, log_path, timeout_s):
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            return False
        try:
            with open(log_path, "rb") as fh:
                if SENTINEL.encode() in fh.read():
                    return True
        except OSError:
            pass
        time.sleep(2.0)
    return False


def summarise(log_path):
    with open(log_path, "rb") as fh:
        text = ANSI.sub("", fh.read().decode("utf-8", errors="replace"))
    text = text.replace("\r", "")
    passed = len(re.findall(r":PASS$", text, flags=re.M))
    failed = re.findall(r"^\S*:\d+:(\w+):FAIL:? ?(.*)$", text, flags=re.M)
    for line in re.findall(r"device_tests: (.*us per step.*)$", text,
                           flags=re.M):
        print("  " + line)
    for name, why in failed:
        print("  FAIL %s: %s" % (name, why[:120]))
    sentinel = re.search(r"^%s .*$" % SENTINEL, text, flags=re.M)
    print("%d passed, %d failed; %s" %
          (passed, len(failed),
           sentinel.group(0) if sentinel else "NO %s - the run did not "
           "finish" % SENTINEL))
    return sentinel is not None


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("build_dir")
    parser.add_argument("--icount", action="store_true",
                        help="count instructions: -icount shift=0")
    parser.add_argument("--timeout", type=int, default=1800)
    parser.add_argument("--workdir", default=None,
                        help="where flash, eFuse and log go "
                             "(default: the build directory)")
    parser.add_argument("--log", default=None)
    parser.add_argument("--idf-path",
                        default=os.environ.get("IDF_PATH",
                                               r"C:\Espressif\esp-idf-v5.5"))
    args = parser.parse_args(argv)

    workdir = args.workdir or args.build_dir
    os.makedirs(workdir, exist_ok=True)
    flash = os.path.join(workdir, "qemu_flash.bin")
    efuse = os.path.join(workdir, "qemu_efuse.bin")
    log_path = args.log or os.path.join(workdir, "qemu_serial.log")

    python = find_one("~/.espressif/python_env/idf*_env/Scripts/python.exe",
                      "ESP-IDF Python") if os.name == "nt" else sys.executable
    qemu = find_one("~/.espressif/tools/qemu-xtensa/*/qemu/bin/"
                    "qemu-system-xtensa*", "qemu-system-xtensa")

    merge_flash(args.build_dir, flash, python)
    with open(efuse, "wb") as fh:
        fh.write(default_efuse(args.idf_path))
    if os.path.exists(log_path):
        os.remove(log_path)

    cmd = [qemu, "-M", "esp32s3", "-m", "32M",
           "-drive", "file=%s,if=mtd,format=raw" % flash,
           "-drive", "file=%s,if=none,format=raw,id=efuse" % efuse,
           "-global", "driver=nvram.esp32s3.efuse,property=drive,value=efuse",
           "-global", "driver=timer.esp32s3.timg,property=wdt_disable,"
                      "value=true",
           "-global", "driver=ssi_psram,property=is_octal,value=true",
           "-nographic", "-monitor", "none",
           "-serial", "file:%s" % log_path]
    if args.icount:
        cmd += ["-icount", "shift=0"]

    proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL,
                            stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    try:
        wait_for_sentinel(proc, log_path, args.timeout)
    finally:
        proc.kill()
        proc.wait()

    print("console: %s" % log_path)
    return 0 if summarise(log_path) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
