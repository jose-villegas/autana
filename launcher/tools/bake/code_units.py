"""A stage's code as the top-level units its entry functions reach, not its whole files.

A unit is a top-level function, a class header (its decorators, bases and body without methods), a method, or
a module-level statement. The walk starts at the entry functions and at what runs on import with an effect
the walk cannot see the reader of: a statement that binds no name (not a docstring or `pass`), an assignment
to a subscript or attribute, and a decorator, assignment, default argument or class body statement that calls
the stage's own code. A decorator counts unless it is Python's own (a builtin, or the standard library's, such
as dataclass or functools.cache). From there it follows:

- a name, global or imported anywhere in the unit, to its definitions, through every import of it;
- `module.name` through module aliases however many hops deep, and a module used as a value to all of it;
- any other attribute, and any string that could be one, to every method and class attribute of that name;
- a class to its header, every method (another package may call any) and its subclasses; a method to its
  class.

What the walk cannot place counts more, never less, and is reported so a test can fail on it: a name no
definition or import holds, or a name a stage module does not define, counts that whole module. A computed
getattr counts every method and class attribute; sys.modules, globals(), locals(), eval,
exec and import_module count every unit of the stage."""
import ast
import builtins
import dataclasses
import pathlib
import symtable
import sys

DEFS = (ast.FunctionDef, ast.AsyncFunctionDef)
ASSIGNS = (ast.Assign, ast.AnnAssign, ast.AugAssign)
# Calls that reach code by a name the walk cannot read: any unit of the stage may run.
OPAQUE_CALLS = {"globals", "locals", "eval", "exec", "__import__", "import_module"}
# Calls that read or write an attribute by a name the call computes.
ATTRIBUTE_CALLS = {"getattr", "setattr", "hasattr", "delattr", "vars"}
MODULE_NAMES = {"__file__", "__name__", "__doc__", "__spec__", "__builtins__", "__path__", "__package__"}
BUILTINS = set(dir(builtins)) | MODULE_NAMES


@dataclasses.dataclass(eq=False)
class Unit:
    path: pathlib.Path
    node: ast.AST
    names: set                  # what it defines at module level (a method: its own name)
    owner: str = None           # a method's class
    always: bool = False        # runs on import with an effect whether or not anything names it
    attributes: set = dataclasses.field(default_factory=set)  # a class header: its class-level names

    @property
    def lines(self):
        """(first, last) line of the unit, and the spans inside it that are other units (a class's methods)."""
        first = min([self.node.lineno] + [d.lineno for d in getattr(self.node, "decorator_list", [])])
        skip = [] if not isinstance(self.node, ast.ClassDef) else [
            (min([m.lineno] + [d.lineno for d in m.decorator_list]), m.end_lineno)
            for m in self.node.body if isinstance(m, DEFS)]
        return (first, self.node.end_lineno), skip


def class_effects(node):
    """Whether a class body does something at import the walk cannot see the reader of: a statement that binds no
    name or writes into an object, or one in a nested class (Walk counts the decorators)."""
    return any(
        False if isinstance(item, DEFS) else class_effects(item) if isinstance(item, ast.ClassDef)
        else not stored(item) or stores_into_objects(item) if isinstance(item, ASSIGNS) else not inert(item)
        for item in node.body)


def inert(node):
    """A statement that does nothing: `pass`, or a constant on its own (a docstring, `...`)."""
    return isinstance(node, ast.Pass) or isinstance(node, ast.Expr) and isinstance(node.value, ast.Constant)


def decorators(node):
    """The decorators a def or class runs at import: its own, its methods' and its nested classes'."""
    yield from node.decorator_list
    for item in node.body if isinstance(node, ast.ClassDef) else []:
        if isinstance(item, (*DEFS, ast.ClassDef)):
            yield from decorators(item)


def import_time(node):
    """The expressions a class body evaluates at import: its statements, its methods' decorators and default
    arguments, and the same for nested classes."""
    for item in node.body:
        if isinstance(item, DEFS):
            yield from item.decorator_list + item.args.defaults + [d for d in item.args.kw_defaults if d is not None]
        elif isinstance(item, ast.ClassDef):
            yield from item.decorator_list + item.bases
            yield from import_time(item)
        else:
            yield item


def stored(node):
    return {sub.id for sub in ast.walk(node) if isinstance(sub, ast.Name) and isinstance(sub.ctx, ast.Store)} | {
        sub.name for sub in ast.walk(node) if isinstance(sub, (*DEFS, ast.ClassDef))}


def stores_into_objects(node):
    """Whether an assignment writes a subscript or attribute: state that whatever reads the object sees."""
    targets = node.targets if isinstance(node, ast.Assign) else [node.target]
    return any(isinstance(sub, (ast.Subscript, ast.Attribute)) for target in targets for sub in ast.walk(target))


def is_main_guard(node):
    test = ast.unparse(node.test) if isinstance(node, ast.If) else ""
    return "__name__" in test and "__main__" in test


def names_read(nodes):
    return {sub.id for node in nodes for sub in ast.walk(node) if isinstance(sub, ast.Name)}


class Module:
    def __init__(self, path, find_module):
        self.path = path
        source = path.read_text(encoding="utf-8")
        self.tree = ast.parse(source, str(path))
        self.table = symtable.symtable(source, str(path), "exec")
        # name -> every target an import anywhere in the module binds it to: ("module", file or None, dotted)
        # or ("name", file of the module or None, name).
        # name -> the top-level package of every import of it (None for a relative import).
        self.imports, self.stars, self.packages = {}, [], {}
        for node in ast.walk(self.tree):
            if isinstance(node, ast.Import):
                for alias in node.names:
                    # `import a.b` binds `a`, and `a.b` is an attribute of it.
                    dotted = alias.name if alias.asname else alias.name.split(".")[0]
                    self.bind(alias.asname or dotted, ("module", find_module(dotted, 0), dotted))
                    self.packages.setdefault(alias.asname or dotted, set()).add(alias.name.split(".")[0])
            elif isinstance(node, ast.ImportFrom):
                base = find_module(node.module or "", node.level)
                for alias in node.names:
                    package = node.module.split(".")[0] if node.module and not node.level else None
                    self.packages.setdefault(alias.asname or alias.name, set()).add(package)
                    if (node.module, alias.name, node.level) == ("sys", "modules", 0):
                        self.bind(alias.asname or alias.name, ("module", None, "sys.modules"))
                        continue
                    if alias.name == "*":
                        self.stars.append(base)
                        continue
                    dotted = ((node.module + ".") if node.module else "") + alias.name
                    submodule = find_module(dotted, node.level)
                    self.bind(alias.asname or alias.name, ("module", submodule, dotted) if submodule is not None
                              else ("name", base, alias.name))
        self.units = list(self.split())

    def bind(self, name, target):
        self.imports.setdefault(name, []).append(target)

    def split(self):
        for node in self.tree.body:
            if isinstance(node, (ast.Import, ast.ImportFrom)) or is_main_guard(node):
                continue
            if isinstance(node, DEFS):
                yield Unit(self.path, node, {node.name})
            elif isinstance(node, ast.ClassDef):
                body = [item for item in node.body if not isinstance(item, DEFS)]
                yield Unit(self.path, node, {node.name}, attributes=set().union(set(), *map(stored, body)),
                           always=class_effects(node))
                yield from (Unit(self.path, item, {item.name}, owner=node.name)
                            for item in node.body if isinstance(item, DEFS))
            elif isinstance(node, ASSIGNS):
                names = stored(node)
                yield Unit(self.path, node, names, always=not names or stores_into_objects(node))
            else:
                yield Unit(self.path, node, stored(node), always=not inert(node))

    def scope(self, unit):
        """The symbol table of a def or class unit."""
        tables = self.table.get_children()
        if unit.owner is not None:
            tables = [child for table in tables if table.get_name() == unit.owner and table.get_type() == "class"
                      for child in table.get_children()]
        return next(table for table in tables
                    if table.get_name() == unit.node.name and table.get_lineno() == unit.node.lineno)

    def global_names(self, unit):
        """The module-level names a unit reads: the globals of its symbol table and every scope nested in it
        (a class header leaves out its methods, units of their own); the names imports inside it bind; and
        what its decorators, defaults and annotations read in the enclosing scope."""
        node = unit.node
        if not isinstance(node, (*DEFS, ast.ClassDef)):
            return names_read([node])
        methods = {item.lineno for item in node.body if isinstance(item, DEFS)} if isinstance(node, ast.ClassDef) else set()
        names, pending = set(), [self.scope(unit)]
        while pending:
            table = pending.pop()
            names |= {symbol.get_name() for symbol in table.get_symbols() if symbol.is_global()}
            pending += [child for child in table.get_children() if child.get_lineno() not in methods]
        names |= {name for name in names_read(self.parts(unit)) if name in self.imports}
        outer = list(node.decorator_list)
        if isinstance(node, ast.ClassDef):
            outer += node.bases + node.keywords
        else:
            arguments = node.args.posonlyargs + node.args.args + node.args.kwonlyargs
            outer += node.args.defaults + [default for default in node.args.kw_defaults if default is not None]
            outer += [a.annotation for a in arguments if a.annotation is not None]
            outer += [node.returns] if node.returns is not None else []
        return names | names_read(outer)

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
        self.find = find_module
        self.modules = {path: Module(path, lambda name, level, p=path: find_module(p, name, level))
                        for path in paths}
        self.top, self.by_name, self.loose, self.methods, self.subclasses = {}, {}, {}, {}, {}
        for unit in self.everything():
            if unit.owner is None:
                for name in unit.names:
                    self.top.setdefault((unit.path, name), []).append(unit)
                    self.by_name.setdefault(name, []).append(unit)
                for name in unit.attributes:
                    self.loose.setdefault(name, []).append(unit)
                for base in unit.node.bases if isinstance(unit.node, ast.ClassDef) else []:
                    name = base.attr if isinstance(base, ast.Attribute) else getattr(base, "id", None)
                    self.subclasses.setdefault(name, []).append(unit)
            else:
                self.methods.setdefault((unit.path, unit.owner), []).append(unit)
                self.loose.setdefault(unit.node.name, []).append(unit)
        self.unresolved = []
        # What runs at import and calls the stage's own code may change state other code reads, so it counts
        # whether or not its name is read: an assignment, a default argument, a class body statement.
        for unit in self.everything():
            if unit.owner is None and any(self.calls_stage(unit.path, call.func) for call in self.import_calls(unit)):
                unit.always = True
            # A decorator runs at import and may register what it wraps, unless it is Python's own.
            if isinstance(unit.node, (*DEFS, ast.ClassDef)) and unit.owner is None and any(
                    not self.from_python(unit.path, d) for d in decorators(unit.node)):
                unit.always = True

    def from_python(self, path, decorator):
        """Whether a decorator is Python's own: rooted at a builtin or at names imported only from the standard
        library, with no call into the stage inside it (`dataclass`, `functools.cache`, `property`)."""
        root = decorator.func if isinstance(decorator, ast.Call) else decorator
        while isinstance(root, ast.Attribute):
            root = root.value
        module = self.modules[path]
        if not isinstance(root, ast.Name) or (path, root.id) in self.top or module.stars:
            return False
        packages = module.packages.get(root.id)
        python = root.id in BUILTINS if packages is None else packages <= sys.stdlib_module_names
        inner = [call for call in ast.walk(decorator) if isinstance(call, ast.Call) and call is not decorator]
        return python and not any(self.calls_stage(path, call.func) for call in inner)

    @staticmethod
    def import_calls(unit):
        node = unit.node
        if isinstance(node, ASSIGNS):
            evaluated = [node]
        elif isinstance(node, DEFS):
            evaluated = node.args.defaults + [default for default in node.args.kw_defaults if default is not None]
        elif isinstance(node, ast.ClassDef):
            evaluated = list(import_time(node))
        else:
            evaluated = []
        return [sub for part in evaluated for sub in ast.walk(part) if isinstance(sub, ast.Call)]

    def everything(self):
        return [unit for module in self.modules.values() for unit in module.units]

    def targets(self, path, name):
        return self.modules[path].imports.get(name, [])

    def named(self, path, name, seen=(), strict=True):
        """The units `name` stands for in the stage module at `path`, through every import of it and star
        imports. With `strict`, a name the module does not hold counts all of it and is reported."""
        if path not in self.modules or (path, name) in seen:
            return []
        seen += ((path, name),)
        found = list(self.top.get((path, name), []))
        for kind, target, rest in self.targets(path, name):
            found += self.named(target, rest, seen, strict) if kind == "name" else []
        found += [unit for star in self.modules[path].stars for unit in self.named(star, name, seen, False)]
        if not found and strict and not self.targets(path, name) and name not in BUILTINS:
            self.unresolved.append((path, 0, name))
            found = self.modules[path].units
        return found

    def placed(self, path, name):
        module = self.modules[path]
        return (path, name) in self.top or name in module.imports or name in BUILTINS or bool(module.stars)

    def modules_of(self, path, node):
        """Every stage module an expression may name (`alias`, `alias.sub`, ...), through every import."""
        if isinstance(node, ast.Name):
            return [target for kind, target, _ in self.targets(path, node.id) if kind == "module" and target in self.modules]
        if not isinstance(node, ast.Attribute):
            return []
        found = []
        for base in self.modules_of(path, node.value):
            found += [target for kind, target, _ in self.targets(base, node.attr)
                      if kind == "module" and target in self.modules]
            if base.name == "__init__.py":
                found += [c.resolve() for c in (base.parent / f"{node.attr}.py", base.parent / node.attr / "__init__.py")
                          if c.resolve() in self.modules]
        for dotted in self.dotted(path, node):  # a package without an __init__.py (`import ns.sub`)
            file = self.find(path, dotted, 0)
            found += [file] if file in self.modules else []
        return found

    def dotted(self, path, node):
        """The dotted module names an `alias.sub` chain spells from its imports."""
        if isinstance(node, ast.Name):
            return [rest for kind, _, rest in self.targets(path, node.id) if kind == "module"]
        bases = self.dotted(path, node.value) if isinstance(node, ast.Attribute) else []
        return [f"{base}.{node.attr}" for base in bases]

    def calls_stage(self, path, func):
        """Whether a callee may be the stage's own code: a name defined or imported from the stage, a name it
        cannot place, or an attribute that is not of another package's module."""
        if isinstance(func, ast.Name):
            return bool(self.named(path, func.id, strict=False)) or not self.placed(path, func.id)
        if isinstance(func, ast.Attribute):
            modules = self.modules_of(path, func.value)
            if modules:
                return any(self.named(module, func.attr, strict=False) for module in modules)
            return not self.dotted(path, func.value) and bool(self.attribute(func.attr))
        return True

    def attribute(self, name):
        """What `x.name` may reach on an object the walk cannot place: methods and class attributes of that
        name (a module object reached any other way than its import is opaque, see `reached`)."""
        return self.loose.get(name, [])

    def builtin_call(self, path, func):
        """The builtin a call reaches by its bare name (not shadowed in the module), or `import_module`
        through any object; None for any other call."""
        if isinstance(func, ast.Attribute):
            return func.attr if func.attr in ("import_module", "__import__") else None
        if isinstance(func, ast.Name) and (path, func.id) not in self.top and func.id not in self.modules[path].imports:
            return func.id
        return None

    def attribute_call(self, call, callee):
        """What getattr, setattr, hasattr, delattr or vars reaches on an object (a module passed to one counts
        whole as a value): the named attribute when the name is a string, else every method, class attribute
        and top-level unit; vars of an object reads only its data."""
        name = call.args[1].value if len(call.args) > 1 and isinstance(call.args[1], ast.Constant) else None
        if callee == "vars":
            return [] if call.args else self.everything()
        if isinstance(name, str):
            return self.attribute(name)
        return [unit for units in self.loose.values() for unit in units]

    def sys_modules(self, path, node, parents):
        """What a read of sys.modules reaches: the module a constant key names (none when it is another
        package's), nothing for a membership test, and every unit for any other use (a computed key)."""
        parent = parents.get(id(node))
        if isinstance(parent, ast.Compare) and all(isinstance(op, (ast.In, ast.NotIn)) for op in parent.ops):
            return []
        key = parent.slice if isinstance(parent, ast.Subscript) else None
        if isinstance(parent, ast.Attribute) and parent.attr == "get":
            call = parents.get(id(parent))
            key = call.args[0] if isinstance(call, ast.Call) and call.args else None
        if isinstance(key, ast.Constant) and isinstance(key.value, str):
            file = self.find(path, key.value, 0)
            return self.modules[file].units if file in self.modules else []
        return self.everything()

    def reached(self, unit):
        """The units one unit leads to."""
        module = self.modules[unit.path]
        found = []
        for name in module.global_names(unit):
            if not self.placed(unit.path, name):
                self.unresolved.append((unit.path, unit.node.lineno, name))
                found += module.units
            else:
                found += self.named(unit.path, name)
        for part in module.parts(unit):
            bases = {id(sub.value) for sub in ast.walk(part) if isinstance(sub, ast.Attribute)}
            parents = {id(child): node for node in ast.walk(part) for child in ast.iter_child_nodes(node)}
            for sub in ast.walk(part):
                if isinstance(sub, (ast.Name, ast.Attribute)) and "sys.modules" in self.dotted(unit.path, sub):
                    found += self.sys_modules(unit.path, sub, parents)
                if isinstance(sub, ast.Attribute):
                    if id(sub) in bases and self.modules_of(unit.path, sub):
                        continue  # `pkg.sub` in `pkg.sub.f`: the outer attribute names what is read
                    modules = self.modules_of(unit.path, sub.value)
                    found += [] if modules else self.attribute(sub.attr)
                    for base in modules:
                        found += self.modules[base].units if sub.attr == "__dict__" else self.named(base, sub.attr)
                elif isinstance(sub, ast.Name) and id(sub) not in bases:
                    for value in self.modules_of(unit.path, sub):  # a module passed around as a value
                        found += self.modules[value].units
                elif isinstance(sub, ast.Constant) and isinstance(sub.value, str) and sub.value.isidentifier():
                    found += self.loose.get(sub.value, []) + self.named(unit.path, sub.value, strict=False)
                elif isinstance(sub, ast.Call):
                    callee = self.builtin_call(unit.path, sub.func)
                    if callee in OPAQUE_CALLS:
                        found += self.everything()
                    elif callee in ATTRIBUTE_CALLS:
                        found += self.attribute_call(sub, callee)
        if isinstance(unit.node, ast.ClassDef):
            # Every method (another package may call any of them) and every subclass (`__init_subclass__`).
            found += self.methods.get((unit.path, unit.node.name), []) + self.subclasses.get(unit.node.name, [])
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
