"""Load console fixtures and recipe collection for engine and app paths."""
pytest_plugins = ["console_pytest", "board_pytest"]


def pytest_addoption(parser):
    parser.addoption("--tour-pairs", action="store_true",
                     help="walk every ordered pair of an app's scenes, not one cycle through them")
