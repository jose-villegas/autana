"""Stand-ins for build.sh and flash_image.sh as device.run_to_end() meets
them: the build leaves an ESP-IDF-shaped image in its variant's build
directory, the write notes in its log which snapshot it was handed."""

from pathlib import Path

import device

FLASH_ARGS = ("--flash_mode dio --flash_freq 80m --flash_size 16MB\n"
              "0x0 bootloader/bootloader.bin\n"
              "0x10000 launcher.bin\n"
              "0x8000 partition_table/partition-table.bin\n")


def image_files(build_id, app="app"):
    return [("flash_args", FLASH_ARGS), ("build_id.txt", build_id + "\n"),
            ("bootloader/bootloader.bin", "boot"), ("launcher.bin", app),
            ("partition_table/partition-table.bin", "parts")]


def write_image(build_dir, build_id, app=b"app"):
    for relative, content in image_files(build_id, app.decode()):
        path = Path(build_dir) / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content.encode())


def build_source(build_dir, build_id="abc"):
    """A python build.sh that leaves the image write_image() does."""
    return ("import pathlib\n"
            f"for relative, content in {image_files(build_id)!r}:\n"
            f"    path = pathlib.Path({str(build_dir)!r}) / relative\n"
            "    path.parent.mkdir(parents=True, exist_ok=True)\n"
            "    path.write_bytes(content.encode())\n")


def variant_of(command):
    for flag, variant in (("--dev", "dev"), ("--diag", "diag")):
        if flag in command:
            return variant
    return "release"


def is_build(command):
    return Path(command[1]).name == "build.sh"


def scripts(build_id="abc", build=None, write=None, calls=None):
    """A run_to_end() side effect. `build` and `write`, when given, replace
    the default behaviour of that half; `calls` collects (command, lost)."""
    def run(command, lost=None, timeout=None, **options):
        if calls is not None:
            calls.append((command, lost))
        if is_build(command):
            if build is not None:
                return build(command, **options)
            build_dir = Path(options["cwd"]) / "launcher" / device.BUILD_DIRS[variant_of(command)]
            write_image(build_dir, build_id)
            options["stdout"].write(b"built\n")
            return None
        if write is not None:
            return write(command, lost=lost, **options)
        options["stdout"].write(b"wrote " + (Path(command[-1]) / "build_id.txt").read_bytes())
        return None
    return run


def worktree(root):
    """A worktree with both scripts present, as device.Built requires."""
    root = Path(root)
    for script in (device.BUILD_SCRIPT, device.FLASH_SCRIPT):
        (root / script).parent.mkdir(parents=True, exist_ok=True)
        (root / script).write_text("")
    return root
