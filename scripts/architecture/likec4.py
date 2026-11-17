"""Write model data (model.py's shape) as a standalone LikeC4 model; no Node
required.

  * modules carry test/tool and generated tags, plus any the caller's files
    carry (module_tags); a component carries the tags shared by the modules
    a default view shows, falling back to all its modules when every module
    is hidden. Default views and counts omit tests, tools and generated files;
  * arrows aggregate per-file include counts by module pair, with both ends'
    tags. Each view folds modules to the boxes it draws: the ancestor that
    is a direct child of the view's scope, or of the enclosing scope for an
    outside module. An arrow is hidden in that view when another directed
    path still joins its ends. Arrows are checked in sorted order, and each
    hidden arrow leaves the graph before the next check, so cycles never
    lose reachability. Every arrow stays in the model;
  * the drill container's components nest by folder; bands, when given, are
    boxes of their own holding their folders row by row, each with a scoped
    view. Every element view describes its scope.

Optional data keys: `tags` (tag -> colour or None, beyond the defaults),
`roles` ((marked, other): a component whose subtree holds a file flagged
`marked`, and its descendants, carry `marked`; the rest `other`),
`release_hidden` (tags a release-only view of the drill container also
hides), `bands`, `packages`, `packages_box`, `doc_links`, `links` and
`libraries`. A `deployment` object adds deployment nodes and views.
"""
import json
import pathlib

from model import identifier, summary

DEFAULT_HIDDEN = {'test', 'tool', 'generated'}
# How many names a relationship title lists before "+N".
TITLE_NAMES = 2


def quote(text):
    return "'" + (text or '').replace('\\', '\\\\').replace("'", "\\'").replace('\r', ' ').replace('\n', ' ') + "'"


def module_tags(files):
    variants = {f.get('variant') for f in files}
    tags = variants & {'dev', 'diag'} if len(variants) == 1 else set()
    tags |= {f['kind'] for f in files} & {'test', 'tool'}
    if any(f.get('iram') for f in files):
        tags.add('iram')
    if all(f.get('gen') for f in files):
        tags.add('generated')
    if any(f.get('variant') == 'release' and f.get('devcode') for f in files):
        tags.add('devcode')
    return tags


def redundant(pairs):
    """Edges whose ends remain reachable without that edge in this view."""
    adjacency = {}
    for a, b in pairs:
        adjacency.setdefault(a, set()).add(b)
    result = set()
    for a, b in sorted(pairs):
        seen, stack = {a}, list(adjacency[a] - {b})
        while stack:
            node = stack.pop()
            if node == b:
                result.add((a, b))
                adjacency[a].remove(b)
                break
            if node not in seen:
                seen.add(node)
                stack.extend(adjacency.get(node, ()))
    return result


def size_text(files):
    """"1 file · 7 lines": the count of files and their lines, each pluralised."""
    lines = sum(f['lines'] for f in files)
    return f"{len(files)} file{'' if len(files) == 1 else 's'} \u00b7 {lines} line{'' if lines == 1 else 's'}"


def shortlist(names):
    """The first TITLE_NAMES names, then how many more there are."""
    names = list(names)
    return ', '.join(names[:TITLE_NAMES] + ([f'+{len(names) - TITLE_NAMES}'] if len(names) > TITLE_NAMES else []))


def emit(data, deployment=None):
    """The model text. `deployment`, when given, has `kinds` (deployment node
    kinds for the specification) and `emit(out, base, containers)`, which
    appends a deployment block and returns the view lines to add."""
    components = data['components']
    packages = data.get('packages', [])
    containers = [n for n in data['containers']['nodes'] if n['type'] == 'container']
    containers += [dict(p, type='container', name=p['id'], package=True) for p in packages]
    system = next(n for n in data['context']['nodes'] if n['type'] == 'system')
    drill = next(c for c in containers if c.get('drill'))
    box = data.get('packages_box')
    members = set(box['members']) if box else set()
    containers.sort(key=lambda c: c['id'] in members)
    where = {c['id']: system['id'] + '.' + (box['id'] + '.' if c['id'] in members else '') + c['id'] for c in containers}
    base = where[drill['id']]
    paths, modules, file_module = {}, {}, {}
    by_id = {c['id']: c for c in components}
    bands = data.get('bands', [])
    band_paths = {b['id']: base + '.' + identifier('b_', b['id']) for b in bands}
    children = {}
    for c in sorted(components, key=lambda c: c['id']):
        owner = c.get('package', drill['id'])
        parent = c.get('parent', '')
        children.setdefault((owner, parent), []).append(c)
        above = paths[parent] if parent in by_id else band_paths[c['band']] if 'band' in c else where[owner]
        paths[c['id']] = above if c.get('package') == c['id'] else above + '.' + identifier('c_', c['id'].rsplit('/', 1)[-1])
    subtree = {c['id']: {c['id']} for c in components}
    for c in components:
        up = c.get('parent', '')
        while up in by_id:
            subtree[up].add(c['id'])
            up = by_id[up].get('parent', '')
    for f in data['files']:
        stem = f.get('module', f['path'].rsplit('.', 1)[0])
        modules.setdefault((f['comp'], stem), []).append(f)
    roles = data.get('roles')
    marked = set().union(*(ids for ids in subtree.values()
                           if roles and any(f.get(roles[0]) and f['comp'] in ids for f in data['files'])))
    tags, module_names = {}, {}
    for (cid, stem), files in modules.items():
        folder = next(c['folder'] for c in components if c['id'] == cid)
        name = stem[len(folder) + 1:] if stem.startswith(folder + '/') else stem.rsplit('/', 1)[-1]
        full = paths[cid] + '.' + identifier('m_', name)
        module_names[full] = name
        tags[full] = module_tags(files) | ({roles[0] if cid in marked else roles[1]} if roles else set())
        for f in files:
            file_module[f['path']] = full
    view_tags = {c['id']: identifier('indirect_c_', c['id']) for c in components}
    band_tags = {b['id']: identifier('indirect_b_', b['id']) for b in bands}
    box_path = system['id'] + '.' + box['id'] if box else None
    box_tags = {box['id']: identifier('indirect_p_', box['id'])} if box else {}
    release_hidden = tuple(data.get('release_hidden', ()))
    colours = data.get('tags', {})
    all_tags = DEFAULT_HIDDEN | {'indirect'} | set(colours) | ({'indirect_release'} if release_hidden else set())
    out = ['specification {']
    styles = {'person': 'shape person', 'external': 'color gray', 'module': 'color slate; sizing fixed', 'component': 'sizing fixed', 'band': 'color indigo'}
    for kind in ('person', 'system', 'external', 'container', 'band', 'component', 'module'):
        out.append(f'  element {kind} {{ notation {quote(kind.capitalize())}' + (f' style {{ {styles[kind]} }}' if kind in styles else '') + ' }')
    out.extend(["  relationship includes { notation 'Source header include'; color slate; line solid }",
                "  relationship links { notation 'CMake component requirement'; color blue; line dashed; multiple true }"])
    for kind in (deployment.kinds if deployment else ()):
        out.append(f'  deploymentNode {kind} {{ notation {quote(kind.capitalize())} }}')
    out.extend('  tag ' + t + (' { color ' + colours[t] + ' }' if colours.get(t) else '')
               for t in sorted(all_tags | set(view_tags.values()) | set(band_tags.values()) | set(box_tags.values())))
    out.extend(['}', '', 'model {'])
    source = data['source']

    def element(eid, kind, name, desc, src, indent, tech='', marks=(), metadata=(), folder=None, doc=None):
        pad = ' ' * indent
        out.append(f'{pad}{eid} = {kind} {quote(name)} {{')
        if marks:
            out.append(pad + '  ' + ' '.join('#' + t for t in sorted(marks)))
        if tech:
            out.append(f'{pad}  technology {quote(tech)}')
        short = summary(desc)
        if short != desc:
            out.append(f'{pad}  summary {quote(short)}')
        out.append(f'{pad}  description {quote(desc)}')
        if source['repo']:
            url = source['repo'] + '/blob/' + source['commit'] + '/'
            out.append(f"{pad}  link {url}{src or system['src']} {quote('Source: ' + pathlib.PurePosixPath(src or system['src']).name)}")
            if folder:
                out.append(f"{pad}  link {source['repo']}/tree/{source['commit']}/{folder} 'Source folder'")
            if doc:
                out.append(f"{pad}  link {url}{doc['path']}#{doc['anchor']} {quote('Doc: ' + doc['heading'])}")
        if metadata:
            out.append(pad + '  metadata {')
            out.extend(f'{pad}    {key} {quote(value)}' for key, value in metadata)
            out.append(pad + '  }')

    def close(indent):
        out.append(' ' * indent + '}')

    descriptions = {system['id']: system['desc']}
    descriptions.update({paths[c['id']]: c['desc'] for c in components})
    descriptions.update({band_paths[b['id']]: b['desc'] for b in bands})
    descriptions.update({where[c['id']]: c['desc'] for c in containers})
    if box:
        descriptions[system['id'] + '.' + box['id']] = box['desc']
    fqns = {}
    for n in data['context']['nodes']:
        if n['type'] in ('person', 'ext'):
            element(n['id'], 'external' if n['type'] == 'ext' else 'person', n['name'],
                    n['desc'], n.get('src'), 2, n.get('tech', ''))
            close(2)
            fqns[n['id']] = n['id']
    element(system['id'], 'system', system['name'], system['desc'], system['src'], 2)
    opened = False
    for container in containers:
        fqns[container['id']] = where[container['id']]
        owner = container['id']
        at = 6 if owner in members else 4
        if at == 6 and not opened:
            opened = True
            element(box['id'], 'container', box['name'], box['desc'], box['src'], 4)
        element(container['id'], 'container', container['name'], container['desc'], container['src'], at, container.get('tech', ''),
                folder=container.get('folder') if container.get('package') else None,
                doc=data.get('doc_links', {}).get(owner))

        def component(c, indent):
            owned = [files for (cid, _), files in sorted(modules.items()) if cid == c['id']]
            marks = [tags[file_module[fs[0]['path']]] for (cid, _), fs in modules.items() if cid in subtree[c['id']]]
            shown = [t for t in marks if not t & DEFAULT_HIDDEN]
            common = set.intersection(*(shown or marks)) if marks else set()
            visible = [f for fs in (files for (cid, _), files in modules.items() if cid in subtree[c['id']])
                       for f in fs if not module_tags([f]) & DEFAULT_HIDDEN]
            tech = size_text(visible)
            wrapper = c.get('package') == c['id']
            if not wrapper:
                element(identifier('c_', c['id'].rsplit('/', 1)[-1]), 'component', c['id'],
                        c['desc'], c['src'], indent, tech, common, folder=c['folder'], doc=data.get('doc_links', {}).get(c['id']))
            else:
                indent -= 2
            for files in owned:
                full = file_module[files[0]['path']]
                vendor = {}
                for f in files:
                    for name, count, src in f['ext']:
                        key = name + (f" ({source['repo']}/blob/{source['commit']}/{src})" if src and source['repo'] else '')
                        vendor[key] = vendor.get(key, 0) + count
                metadata = [('vendor_includes', '; '.join(f'{name}: {n}' for name, n in sorted(vendor.items())))] if vendor else []
                element(full.rsplit('.', 1)[1], 'module', module_names[full],
                        next((f.get('desc', f['summary']) for f in files if f['summary']), ''), files[0]['path'], indent + 2,
                        ', '.join(f['path'].rsplit('/', 1)[-1] for f in files), tags[full], metadata, doc=data.get('doc_links', {}).get(files[0].get('module', files[0]['path'].rsplit('.', 1)[0])))
                close(indent + 2)
            for child in children.get((owner, c['id']), ()):
                component(child, indent + 2)
            if not wrapper:
                close(indent)

        top = children.get((owner, ''), ())
        for c in top:
            if 'band' not in c:
                component(c, at + 2)
        for b in bands if owner == drill['id'] else ():
            held = sorted((c for c in top if c.get('band') == b['id']), key=lambda c: (c['row'], c['id']))
            sizes = [f for c in held for (cid, _), files in modules.items() if cid in subtree[c['id']]
                     for f in files if not module_tags([f]) & DEFAULT_HIDDEN]
            element(identifier('b_', b['id']), 'band', b['id'], b['desc'], b['src'], at + 2,
                    size_text(sizes))
            for c in held:
                component(c, at + 4)
            close(at + 2)
        close(at)
    if opened:
        close(4)
    close(2)
    for a, b, title in data['containers']['edges']:
        out.append(f'  {fqns[a]} -> {fqns[b]} {quote(title)}')
    counts, relationship_tags, headers = {}, {}, {}
    for f in data['files']:
        a = file_module[f['path']]
        for target, count in f['inc']:
            b = file_module.get(target)
            if b and a != b:
                pair = (a, b)
                counts[pair] = counts.get(pair, 0) + count
                named = headers.setdefault(pair, {})
                name = target.rsplit('/', 1)[-1]
                named[name] = named.get(name, 0) + count
                relationship_tags[pair] = tags[a] | tags[b]

    def reduce(shown, scope, tag):
        def fold(module):
            parts, depth = module.split('.'), 0
            for part in scope.split('.'):
                if parts[depth] != part:
                    break
                depth += 1
            return '.'.join(parts[:depth + 1])
        drawn = {}
        for a, b in shown:
            if a.startswith(scope + '.') or b.startswith(scope + '.'):
                pair = fold(a), fold(b)
                if pair[0] != pair[1]:
                    drawn.setdefault(pair, []).append((a, b))
        for pair in redundant(drawn):
            for edge in drawn[pair]:
                relationship_tags[edge].add(tag)

    shown = [p for p in counts if not relationship_tags[p] & DEFAULT_HIDDEN]
    reduce(shown, base, 'indirect')
    if release_hidden:
        reduce([p for p in shown if not relationship_tags[p] & set(release_hidden)], base, 'indirect_release')
    for cid, scope in paths.items():
        reduce(shown, scope, view_tags[cid])
    for bid, scope in band_paths.items():
        reduce(shown, scope, band_tags[bid])
    if box:
        reduce(shown, box_path, box_tags[box['id']])
    for (a, b), count in sorted(counts.items()):
        marks = ''.join(' #' + t for t in sorted(relationship_tags[a, b]))
        ranked = sorted(headers[a, b].items(), key=lambda item: (-item[1], item[0]))
        listed = '; '.join(f'{name}: {n}' for name, n in ranked)
        out.append(f"  {a} -[includes]-> {b} {{{marks} metadata {{ includes '{count}' headers {quote(listed)} }} }}")
    targets = dict(fqns)
    targets.update(data.get('libraries', {}))
    for c in components:
        targets['code:' + c['folder']] = paths[c['id']]
    for container in containers:
        if container.get('folder'):
            targets['code:' + container['folder']] = fqns[container['id']]
    for edge in data.get('links', []):
        a, b = targets[edge['from']], targets[edge['to']]
        mark = ' #' + edge['tag'] if edge.get('tag') else ''
        out.append(f"  {a} -[links]-> {b} {quote('requires ' + shortlist(edge['components']))} {{{mark} metadata {{ components {quote(', '.join(edge['components']))} }} }}")
    out.append('}')
    deployment_views = deployment.emit(out, base, containers) if deployment else []
    out.extend(['', 'views {'])

    def view(name, scope, title, hidden=(), indirect=None):
        out.append(f'  view {name}' + (f' of {scope}' if scope else '') + ' {')
        out.extend([f'    title {quote(title)}', f'    description {quote(descriptions.get(scope, system["desc"]))}', '    include *'])
        if hidden:
            expression = ' or '.join('tag is #' + t for t in hidden)
            out.append('    exclude * where ' + expression)
            out.append('    exclude * -> * where ' + expression + ' or tag is #' + indirect)
        close(2)

    view('index', '', 'Context')
    view('containers', system['id'], 'Containers')
    hidden = tuple(sorted(DEFAULT_HIDDEN))
    view(drill['id'], base, drill['name'] + ': components', hidden, 'indirect')
    if release_hidden:
        view(drill['id'] + '_release', base, drill['name'] + ': components, release image only',
             release_hidden + hidden, 'indirect_release')
    view(drill['id'] + '_all', base, drill['name'] + ': components, with tests and tools')
    if box:
        view(box['id'], box_path, box['name'], hidden, box_tags[box['id']])
    for b in bands:
        view(identifier('b_', b['id']), band_paths[b['id']], b['id'], hidden, band_tags[b['id']])
    for c in components:
        view(identifier('c_', c['id']), paths[c['id']], c['id'], hidden, view_tags[c['id']])
    out.extend(deployment_views)
    out.append('}')
    return '\n'.join(out) + '\n'


def write(out, data, text):
    """Write the model folder site.py builds from: <system id>.c4 and the
    LikeC4 project config titled for the system. Returns the model's path."""
    out = pathlib.Path(out)
    out.mkdir(parents=True, exist_ok=True)
    system = next(n for n in data['context']['nodes'] if n['type'] == 'system')
    path = out / (system['id'] + '.c4')
    path.write_text(text, encoding='utf-8')
    title = system['name'] + ' architecture'
    (out / 'likec4.config.json').write_text(json.dumps(dict(name=title, title=title, styles=dict(defaults=dict(view=dict(routing='ortho'))))) + '\n', encoding='utf-8')
    return path
