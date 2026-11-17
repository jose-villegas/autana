"""The files GitHub gives a CI step: its summary page and its outputs. Outside a CI step there are none, and
nothing is written."""
import os


def append(variable, text):
    if os.environ.get(variable):
        with open(os.environ[variable], "a", encoding="utf-8") as file:
            file.write(text)


def summarize(text):
    """Adds `text` to the step's summary page as a paragraph of its own."""
    append("GITHUB_STEP_SUMMARY", text + "\n\n")


def output(values):
    """Sets the step's outputs from {name: value}; a bool reads as true or false, as workflow expressions do."""
    append("GITHUB_OUTPUT", "".join(f"{name}={str(value).lower() if isinstance(value, bool) else value}\n"
                                    for name, value in values.items()))
