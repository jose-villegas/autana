"""The literal a Python module assigns to a name, read without importing it."""
import ast


def module_constant(path, name):
    """The literal `path` assigns to `name` at top level; ValueError when it assigns none."""
    for node in ast.parse(path.read_text(encoding="utf-8")).body:
        if isinstance(node, ast.Assign) and any(
                isinstance(target, ast.Name) and target.id == name for target in node.targets):
            return ast.literal_eval(node.value)
    raise ValueError(f"{path}: missing {name}")
