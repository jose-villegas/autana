"""renderer: the renderer's sources, for Python that compiles it on a host; renderer.sh is the shell twin.
Every .c below the folders main/renderer_folders.txt names, so a new render file changes no caller."""
import pathlib

LAUNCHER = pathlib.Path(__file__).resolve().parents[2]


def sources(launcher=LAUNCHER):
    """Every renderer .c as a path, sorted by its launcher/main-relative name."""
    main = pathlib.Path(launcher) / "main"
    folders = [line for line in (main / "renderer_folders.txt").read_text(encoding="utf-8").splitlines()
               if line and not line.startswith("#")]
    return sorted((path for folder in folders for path in (main / folder).rglob("*.c")),
                  key=lambda path: path.relative_to(main).as_posix())
