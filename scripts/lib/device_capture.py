"""Unity result lines shared by device capture tools."""
import re

# Device results allow an empty file field and include ignored tests.
RESULT_RE = re.compile(r"^(?P<file>\S*?):\d+:(?P<name>\w+):(?P<status>PASS|FAIL|IGNORE)(?::\s*(?P<message>.*))?$")
