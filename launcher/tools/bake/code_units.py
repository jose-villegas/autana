"""A stage's code as the top-level units its entry functions reach, not its whole files.

A unit is a top-level function, a class header (its decorators, bases and body without methods), a method, or
a module-level statement. The walk starts at the entry functions and at every statement that runs on import,
and follows:

- a global name, to its definition in the module or through a `from` import;
- `module.name`, through module aliases however many hops deep;
- any other attribute, and any string that could be one, to every method and class attribute of that name;
- a class to its header and dunder methods, and a method to its class.

What the walk cannot follow counts more, never less: a name it cannot place counts its whole module (and is
reported, so a test can fail on it); a getattr or setattr with a computed name counts every method and class
attribute; globals(), locals(), vars(), eval, exec and import_module count every unit of the stage."""
import ast
import builtins
import dataclasses
import pathlib
import symtable

DEFS = (ast.FunctionDef, ast.AsyncFunctionDef)
# Calls that reach code by a name the walk cannot read: any unit of the stage may run.
OPAQUE_CALLS = {"globals", "locals", "eval", "exec", "__import__", "import_module"}
# Calls that read or write an attribute by a computed name.
ATTRIBUTE_CALLS = {"getattr", "setattr", "hasattr", "delattr"}
MODULE_NAMES = {"__file__", "__name__", "__doc__", "__spec__", "__builtins__", "__path__", "__package__"}
BUILTINS = set(dir(builtins)) | MODULE_NAMES


@dataclasses.dataclass(eq=False)
class Unit:
    path: pathlib.Path
    node: ast.AST
    names: set                  # what it defines at module level (a method: its own name)
    owner: str = None           # a method's class
    always: bool = False        # runs on import whether or not anything names it
    attributes: set = dataclasses.field(default_factory=set)  # a class header: its class-level names

    @property
    def lines(self):
        """(first, last) line of the unit, and the spans inside it that are other units (a class's methods)."""
        first = min([self.node.lineno] + [d.lineno for d in getattr(self.node, "decorator_list", [])])
        skip = [] if not isinstance(self.node, ast.ClassDef) else [
            (min([m.lineno] + [d.lineno for d in m.decorator_list]), m.end_lineno)
            for m in self.node.body if isinstance(m, DEFS)]
        return (first, self.node.end_lineno), skip


def stored(node):
    return {sub.id for sub in ast.walk(node) if isinstance(sub, ast.Name) and isinstance(sub.ctx, ast.Store)} | {
        sub.name for sub in ast.walk(node) if isinstance(sub, (*DEFS, ast.ClassDef))}


def is_main_guard(node):
    test = ast.unparse(node.test) if isinstance(node, ast.If) else ""
    return "__name__" in test and "__main__" in test


class Module:
    def __init__(self, path, find_module):
        self.path = path
        source = path.read_text(encoding="utf-8")
        self.tree = ast.parse(source, str(path))
        self.table = symtable.symtable(source, str(path), "exec")
        self.imports, self.stars = {}, []
        for node in ast.walk(self.tree):
            if isinstance(node, ast.Import):
                for alias in node.names:
                    # `import a.b` binds `a`, and `a.b` is an attribute of it.
                    name = alias.name if alias.asname else alias.name.split(".")[0]
                    self.imports[alias.asname or name] = ("module", find_module(name, 0))
            elif isinstance(node, ast.ImportFrom):
                base = find_module(node.module or "", node.level)
                for alias in node.names:
                    if alias.name == "*":
                        self.stars.append(base)
                        continue
                    submodule = find_module(((node.module + ".") if node.module else "") + alias.name, node.level)
                    self.imports[alias.asname or alias.name] = (
                        ("module", submodule) if submodule is not None else ("name", base, alias.name))
        self.units = list(self.split())

    def split(self):
        for node in self.tree.body:
            if isinstance(node, (ast.Import, ast.ImportFrom)) or is_main_guard(node):
                continue
            if isinstance(node, DEFS):
                yield Unit(self.path, node, {node.name})
            elif isinstance(node, ast.ClassDef):
                body = [item for item in node.body if not isinstance(item, DEFS)]
                yield Unit(self.path, node, {node.name}, attributes=set().union(set(), *map(stored, body)))
                yield from (Unit(self.path, item, {item.name}, owner=node.name)
                            for item in node.body if isinstance(item, DEFS))
            elif isinstance(node, (ast.Assign, ast.AnnAssign, ast.AugAssign)):
                yield Unit(self.path, node, stored(node))
            else:
                yield Unit(self.path, node, stored(node), always=True)

    def scope(self, unit):
        """The symbol table of a def or class unit."""
        tables = self.table.get_children()
        if unit.owner is not None:
            tables = [child for table in tables if table.get_name() == unit.owner and table.get_type() == "class"
                      for child in table.get_children()]
        return next(table for table in tables
                    if table.get_name() == unit.node.name and table.get_lineno() == unit.node.lineno)

    def global_names(self, unit):
        """The global names a unit reads: from its symbol table and every scope nested in it (a class header
        leaves out its methods, units of their own), plus what its decorators, defaults and annotations read
        in the enclosing scope."""
        node = unit.node
        if not isinstance(node, (*DEFS, ast.ClassDef)):
            return {sub.id for sub in ast.walk(node) if isinstance(sub, ast.Name)}
        methods = {item.lineno for item in node.body if isinstance(item, DEFS)} if isinstance(node, ast.ClassDef) else set()
        names, pending = set(), [self.scope(unit)]
        while pending:
            table = pending.pop()
            names |= {symbol.get_name() for symbol in table.get_symbols() if symbol.is_global()}
            pending += [child for child in table.get_children() if child.get_lineno() not in methods]
        outer = list(node.decorator_list)
        if isinstance(node, ast.ClassDef):
            outer += node.bases + node.keywords
        else:
            arguments = node.args.posonlyargs + node.args.args + node.args.kwonlyargs
            outer += node.args.defaults + [default for default in node.args.kw_defaults if default is not None]
            outer += [a.annotation for a in arguments if a.annotation is not None]
            outer += [node.returns] if node.returns is not None else []
        return names | {sub.id for part in outer for sub in ast.walk(part) if isinstance(sub, ast.Name)}

    def parts(self, unit):
        """The nodes of a unit's own code: a class header without its methods."""
        if isinstance(unit.node, ast.ClassDef):
            return unit.node.decorator_list + unit.node.bases + unit.node.keywords + [
                item for item in unit.node.body if not isinstance(item, DEFS)]
        return [unit.node]


class Walk:
    def __init__(self, paths, find_module):
        """`find_module(path, name, level)` is the file `import name` at relative `level` loads from the
        module at `path`, or None when it is not code of the stage's tools."""
        self.modules = {path: Module(path, lambda name, level, p=path: find_module(p, name, level))
                        for path in paths}
        self.top, self.loose, self.dunders = {}, {}, {}
        for module in self.modules.values():
            for unit in module.units:
                if unit.owner is None:
                    for name in unit.names:
                        self.top.setdefault((unit.path, name), []).append(unit)
                    for name in unit.attributes:
                        self.loose.setdefault(name, []).append(unit)
                elif unit.node.name.startswith("__") and unit.node.name.endswith("__"):
                    self.dunders.setdefault((unit.path, unit.owner), []).append(unit)
                else:
                    self.loose.setdefault(unit.node.name, []).append(unit)
        self.unresolved = []
        # An assignment that calls the stage's own code at import may change state other code reads, so it
        # counts whether or not its name is read.
        for module in self.modules.values():
            for unit in module.units:
                if isinstance(unit.node, (ast.Assign, ast.AnnAssign, ast.AugAssign)):
                    unit.always = any(isinstance(sub, ast.Call) and self.calls_stage(unit.path, sub.func)
                                      for sub in ast.walk(unit.node))

    def named(self, path, name, seen=()):
        """The units `name` stands for in the module at `path`, following `from` imports and star imports."""
        if path not in self.modules or (path, name) in seen:
            return []
        seen += ((path, name),)
        if (path, name) in self.top:
            return self.top[(path, name)]
        module = self.modules[path]
        entry = module.imports.get(name)
        if entry is not None:
            return self.named(entry[1], entry[2], seen) if entry[0] == "name" else []
        return [unit for star in module.stars for unit in self.named(star, name, seen)]

    def placed(self, path, name):
        module = self.modules[path]
        return (path, name) in self.top or name in module.imports or name in BUILTINS or bool(module.stars)

    def module_of(self, path, node):
        """The stage module an expression names (`alias`, `alias.sub`, ...), or None."""
        if isinstance(node, ast.Name):
            entry = self.modules[path].imports.get(node.id)
            return entry[1] if entry is not None and entry[0] == "module" else None
        if not isinstance(node, ast.Attribute):
            return None
        base = self.module_of(path, node.value)
        if base not in self.modules:
            return None
        entry = self.modules[base].imports.get(node.attr)
        if entry is not None and entry[0] == "module":
            return entry[1]
        if base.name == "__init__.py":
            for candidate in (base.parent / f"{node.attr}.py", base.parent / node.attr / "__init__.py"):
                if candidate.resolve() in self.modules:
                    return candidate.resolve()
        return None

    def calls_stage(self, path, func):
        """Whether a callee may be the stage's own code: a name defined or imported from the stage, a name it
        cannot place, or an attribute that is not of another package's module."""
        if isinstance(func, ast.Name):
            return bool(self.named(path, func.id)) or not self.placed(path, func.id)
        if isinstance(func, ast.Attribute):
            base = self.module_of(path, func.value)
            if base is not None:
                return bool(self.named(base, func.attr))
            if isinstance(func.value, ast.Name) and self.modules[path].imports.get(func.value.id, ("",))[0] == "module":
                return False
            return bool(self.loose.get(func.attr))
        return True

    def builtin_call(self, path, func):
        """The builtin a call reaches by its bare name (not shadowed in the module), or `import_module`
        through any object; None for any other call."""
        if isinstance(func, ast.Attribute):
            return func.attr if func.attr in ("import_module", "__import__") else None
        if isinstance(func, ast.Name) and (path, func.id) not in self.top and func.id not in self.modules[path].imports:
            return func.id
        return None

    def everything(self):
        return [unit for module in self.modules.values() for unit in module.units]

    def reached(self, unit):
        """The units one unit leads to."""
        module = self.modules[unit.path]
        found = []
        for name in module.global_names(unit):
            if not self.placed(unit.path, name):
                self.unresolved.append((unit.path, unit.node.lineno, name))
                found += module.units
            found += self.named(unit.path, name)
        for part in module.parts(unit):
            for sub in ast.walk(part):
                if isinstance(sub, ast.Attribute):
                    base = self.module_of(unit.path, sub.value)
                    found += self.named(base, sub.attr) if base is not None else self.loose.get(sub.attr, [])
                elif isinstance(sub, ast.Constant) and isinstance(sub.value, str) and sub.value.isidentifier():
                    found += self.loose.get(sub.value, []) + self.named(unit.path, sub.value)
                elif isinstance(sub, ast.Call):
                    callee = self.builtin_call(unit.path, sub.func)
                    if callee in OPAQUE_CALLS or (callee == "vars" and not sub.args):
                        found += self.everything()
                    elif callee in ATTRIBUTE_CALLS and not (len(sub.args) > 1 and isinstance(sub.args[1], ast.Constant)):
                        found += [loose for units in self.loose.values() for loose in units]
        if isinstance(unit.node, ast.ClassDef):
            found += self.dunders.get((unit.path, unit.node.name), [])
        if unit.owner is not None:
            found += [owner for owner in self.top.get((unit.path, unit.owner), []) if isinstance(owner.node, ast.ClassDef)]
        return found

    def units(self, entries):
        """Every unit `entries` ((path, function) pairs) and the import-time statements reach, in file order."""
        missing = [f"{path.name}:{name}" for path, name in entries if (path, name) not in self.top]
        if missing:
            raise LookupError("no such entry function: " + ", ".join(missing))
        pending = [unit for path, name in entries for unit in self.top[(path, name)]]
        pending += [unit for unit in self.everything() if unit.always]
        kept = set()
        while pending:
            unit = pending.pop()
            if id(unit) not in kept:
                kept.add(id(unit))
                pending += self.reached(unit)
        return [unit for unit in self.everything() if id(unit) in kept]
