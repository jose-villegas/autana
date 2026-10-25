"""Run the pull request's workflow gates on a developer's checkout.

List the commands before running them, choose firmware or QEMU tiers when
needed, and read each step's capture under --out. Workflow files own the
commands; OUT_OF_SCOPE owns the exclusions. Host-wide changes are reported
with their exact commands so the developer can prepare the machine.
"""
import argparse
from functools import partial
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

import yaml

from job_inputs import affected_changes, changes_from_name_status, git
from github_api import call

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'device'))
from device import git_bash

EXPRESSION = re.compile(r'\$\{\{\s*(.*?)\s*\}\}')
HOSTED = re.compile(r'^(ubuntu|windows|macos)-(latest|[\d.]+)(-.*)?$')
HOST_CHANGE = re.compile(
    r'(^|[;&|\n]\s*)(?:sudo\b|(?:python[\d.]* -m )?pip[\d.]* install\b|apt-get\b)'
    r'|curl\b.*\|\s*tar\b|idf_tools\.py.*\binstall\b|git config --global')
CHECKOUT_INSTALL = re.compile(r'\bnpm ci\b.*--prefix|\bgit submodule update\b|\bgit lfs pull\b|\bbake.py fetch\b')
TIERS = ('host', 'rel', 'dev', 'diag', 'qemu')
PROCESS_TIMEOUT_SECONDS = 60


def scope_text(item):
    return json.dumps({key: value for key, value in item.items() if key not in {'steps', 'workflow'}})


OUT_OF_SCOPE = (
    ('board / GPU', lambda item: 'runs-on' in item and not (
        isinstance(item['runs-on'], str) and HOSTED.fullmatch(item['runs-on']))),
    ('secret', lambda item: 'secrets' in item or bool(re.search(r'secrets\.(?!GITHUB_TOKEN\b)', scope_text(item)))),
    ("another branch's workflow", lambda item: 'uses' in item and
     ('workflow' in item or '.github/workflows/' in item['uses'])),
    ('reads the pull request', lambda item: 'github.event.pull_request' in
     str(item.get('if', '')) + str(item.get('env', {}))),
    ('CI wiring', lambda item: bool(item.get('outputs')) and all(
        'GITHUB_OUTPUT' in step['run'] or 'job_inputs.py affected' in step['run']
        for step in item.get('steps', []) if 'run' in step)),
)


def workflows(root):
    for path in sorted((root / '.github/workflows').glob('*.y*ml')):
        data = yaml.safe_load(path.read_text(encoding='utf-8'))
        yield dict(data, path=path, stem=path.stem)


def condition(text, ctx):
    """Evaluate the boolean expression subset used by workflow conditions."""
    text = str(text).strip().removeprefix('${{').removesuffix('}}').strip()
    tokens = re.findall(r"'[^']*'|\"[^\"]*\"|&&|\|\||!=|==|[!()]|[\w.-]+", text)
    position = 0

    def atom():
        nonlocal position
        token = tokens[position]
        position += 1
        if token == '!':
            return not atom()
        if token == '(':
            value = expression()
            if tokens[position] != ')':
                raise ValueError('unbalanced condition')
            position += 1
            return value
        if position < len(tokens) and tokens[position] == '(':
            position += 1
            if tokens[position] != ')':
                raise ValueError(f'unsupported function: {token}')
            position += 1
            return {'cancelled': False, 'always': True, 'success': ctx.get('success', True),
                    'failure': not ctx.get('success', True)}[token]
        if token.startswith(('"', "'")):
            return token[1:-1]
        if token in ('true', 'false'):
            return token == 'true'
        if token in ctx:
            return ctx[token]
        if token.startswith('steps.') and '.outputs.' in token:
            return ''
        if token.startswith('needs.'):
            return 'true' if '.outputs.' in token else 'success'
        raise ValueError(f'unmapped condition: {token}')

    def compare():
        nonlocal position
        value = atom()
        if position < len(tokens) and tokens[position] in ('==', '!='):
            operator = tokens[position]
            position += 1
            other = atom()
            value = value == other if operator == '==' else value != other
        return value

    def conjunction():
        nonlocal position
        value = compare()
        while position < len(tokens) and tokens[position] == '&&':
            position += 1
            other = compare()
            value = value and other
        return value

    def expression():
        nonlocal position
        value = conjunction()
        while position < len(tokens) and tokens[position] == '||':
            position += 1
            other = conjunction()
            value = value or other
        return value

    if not tokens:
        return True
    value = expression()
    if position != len(tokens):
        raise ValueError(f'unmapped condition: {text}')
    return value


def pr_jobs(root):
    for workflow in workflows(root):
        if 'pull_request' not in workflow.get('on', workflow.get(True, {})):
            continue
        for name, job in workflow['jobs'].items():
            expression = job.get('if', '')
            # Input decisions and job outcomes are evaluated when running, not discovering.
            try:
                included = condition(expression, {'github.event_name': 'pull_request',
                    'github.ref': 'refs/pull/local/merge'})
            except (ValueError, KeyError, IndexError):
                included = True
            if included:
                yield dict(job, key=f'{workflow["stem"]}:{name}', workflow=workflow,
                           env={**workflow.get('env', {}), **job.get('env', {})})


def expand(text, ctx):
    def replacement(match):
        expression = match[1].strip()
        if expression in ctx:
            return str(ctx[expression])
        fallback = re.fullmatch(r"([\w.]+)\s*\|\|\s*'([^']*)'", expression)
        if fallback and fallback[1] in ctx:
            return str(ctx[fallback[1]] or fallback[2])
        return match[0]
    return EXPRESSION.sub(replacement, str(text))


def shell_segments(command):
    quote = ''
    depth = 0
    escaped = False
    start = 0
    index = 0
    while index < len(command):
        char = command[index]
        if escaped:
            escaped = False
        elif char == "\\" and quote != "'":
            escaped = True
        elif quote:
            if char == quote:
                quote = ''
        elif char in "\"'":
            quote = char
        elif char in '({':
            depth += 1
        elif char in ')}':
            depth -= 1
        elif depth == 0 and (char == '\n' or command[index:index + 2] == '&&'):
            separator = '\n' if char == '\n' else '&&'
            yield command[start:index], separator
            index += len(separator)
            start = index
            continue
        index += 1
    yield command[start:], ''


def split_run(command):
    """Keep shell state while omitting top-level host-wide changes."""
    dropped = []
    parts = []
    remaining = []
    for part, separator in shell_segments(command):
        if HOST_CHANGE.search(part):
            dropped.append(part.strip())
            part = part[:len(part) - len(part.lstrip())] + ':' + part[len(part.rstrip()):]
        elif part.strip() not in ('', ':'):
            remaining.append(part.strip())
        parts.append(part + separator)
    classification = ('skip' if not remaining else 'setup' if all(
        CHECKOUT_INSTALL.search(part) for part in remaining) else 'gate')
    return ''.join(parts), dropped, classification


def submodules(root, recursive):
    command = 'git submodule update --init' + (' --recursive' if recursive == 'recursive' else '')
    if not root or not (root / '.gitmodules').exists():
        return command
    common = Path(os.fsdecode(git(root, 'rev-parse', '--git-common-dir')).strip())
    common = (root / common).resolve()
    configured = os.fsdecode(git(root, 'config', '--file', '.gitmodules',
                                  '--get-regexp', r'^submodule\..*\.path$'))
    commands = []
    for line in configured.splitlines():
        key, path = line.split(None, 1)
        name = key.removeprefix('submodule.').removesuffix('.path')
        reference = common / 'modules' / name
        options = (' --reference ' + shlex.quote(reference.as_posix()) + ' --dissociate'
                   if (reference / 'objects').is_dir() else '')
        commands.append(command + options + ' -- ' + shlex.quote(path))
    return ' && '.join(commands) or command



def bind_condition(text, ctx):
    text = str(text).strip().removeprefix('${{').removesuffix('}}').strip()
    return re.sub(r"'[^']*'|\"[^\"]*\"|\binputs\.[\w-]+",
                  lambda match: repr(str(ctx[match[0]])) if match[0] in ctx else match[0], text)


def composite_steps(items, root, ctx=None, parents=()):
    ctx = ctx or {}
    for step in items:
        step = dict(step)
        action = step.get('uses', '')
        if action.startswith('./.github/actions/'):
            if action in parents:
                raise ValueError('recursive composite action: ' + action)
            folder = root / action
            definition = next(folder.glob('action.y*ml'))
            data = yaml.safe_load(definition.read_text(encoding='utf-8'))
            inputs = {name: value.get('default', '') for name, value in data.get('inputs', {}).items()}
            inputs.update(step.get('with', {}))
            nested = {**ctx, **{'inputs.' + name: expand(value, ctx) for name, value in inputs.items()}}
            children = composite_steps(data['runs']['steps'], root, nested, (*parents, action))
            for child in children:
                parent_condition = bind_condition(step.get('if', ''), ctx)
                child_condition = child.get('if', '').strip().removeprefix('${{').removesuffix('}}').strip()
                child['if'] = ' && '.join(f'({value})' for value in (parent_condition, child_condition) if value)
                child['env'] = {**{key: expand(value, ctx) for key, value in step.get('env', {}).items()},
                                **child.get('env', {})}
                yield child
        else:
            for key in ('run', 'if', 'working-directory'):
                if key in step:
                    step[key] = bind_condition(step[key], ctx) if key == 'if' else expand(step[key], ctx)
            for key in ('env', 'with'):
                if key in step:
                    step[key] = {name: expand(value, ctx) for name, value in step[key].items()}
            yield step


def steps(job, root=None):
    default = {**job['workflow'].get('defaults', {}).get('run', {}),
               **job.get('defaults', {}).get('run', {})}
    expanded = list(composite_steps(job.get('steps', []), root,
                                   {'github.job': job['key'].split(':', 1)[1]}))
    commands = '\n'.join(step.get('run', '') + str(step.get('env', {})) for step in expanded)
    tiers = set(re.findall(r'\bautana build (rel|dev|diag)\b', commands))
    if 'patched_qemu.py' in commands:
        tiers = {'qemu'}
    tier = ','.join(value for value in TIERS if value in tiers) or 'host'
    for index, step in enumerate(expanded):
        label = step.get('name', step.get('id', step.get('uses', step.get('run', '').split('\n')[0])))
        row = dict(kind='gate', tier=tier, job=job['key'], label=label, dir=step.get(
            'working-directory', default.get('working-directory', '.')), command='', reason='',
            env={**job.get('env', {}), **step.get('env', {})},
            condition=step.get('if', ''), job_condition=job.get('if', ''),
            id=step.get('id', ''), index=index,
            shell=step.get('shell', default.get('shell', '')), dropped=[],
            idf='container' in job)
        excluded = next((reason for reason, predicate in OUT_OF_SCOPE if predicate(step)), None)
        if excluded:
            yield dict(row, kind='skip', reason=excluded)
            continue
        action = step.get('uses', '').split('@')[0]
        if 'run' in step:
            script, dropped, classification = split_run(step['run'])
            for name, value in row['env'].items():
                cleaned, installs, _ = split_run(str(value))
                row['env'][name] = cleaned
                dropped.extend(installs)
            yield dict(row, command=script, dropped=dropped, kind=classification,
                       reason='host-wide change' if classification == 'skip' else '')
        elif action == 'actions/checkout':
            checkout = step.get('with', {})
            commands = []
            if checkout.get('submodules'):
                commands.append(submodules(root, checkout['submodules']))
            if checkout.get('lfs'):
                commands.append('git lfs pull')
            for command in commands:
                yield dict(row, kind='setup', command=command)
        elif not action.startswith('actions/'):
            yield dict(row, kind='warn', reason='unknown uses: ' + action)


def context(root, base):
    def revision(*args):
        if not (root / '.git').exists():
            return ''
        try:
            return os.fsdecode(git(root, *args)).strip()
        except subprocess.CalledProcessError:
            return ''
    base = base.removeprefix('refs/remotes/').split('/', 1)[-1]
    return {'github.base_ref': base.removeprefix('origin/'),
            'github.head_ref': revision('branch', '--show-current'),
            'github.sha': revision('rev-parse', 'HEAD'), 'github.event_name': 'pull_request',
            'github.token': os.environ.get('GH_TOKEN', ''),
            'secrets.GITHUB_TOKEN': os.environ.get('GH_TOKEN', ''),
            'runner.temp': '${{ runner.temp }}'}


def rows(root, ctx=None):
    ctx = ctx or context(root, 'origin/main')
    for job in pr_jobs(root):
        excluded = next((reason for reason, predicate in OUT_OF_SCOPE if predicate(job)), None)
        if excluded:
            yield dict(kind='skip', tier='host', job=job['key'], label=job.get('name', job['key']),
                       dir='.', command='', reason=excluded)
            continue
        validation = dict(ctx)
        for step in composite_steps(job.get('steps', []), root):
            if step.get('id'):
                validation[f'steps.{step["id"]}.outcome'] = 'success'
        try:
            condition(job.get('if', ''), validation)
        except (ValueError, KeyError, IndexError) as error:
            yield dict(kind='warn', tier='host', job=job['key'], label=job['key'],
                       dir='.', command='', reason=str(error))
            continue
        for row in steps(job, root):
            if row['kind'] == 'skip':
                yield row
                continue
            try:
                applies = condition(row['condition'], validation)
                if not applies and not re.search(r'\b(?:steps|needs)\.', row['condition']):
                    yield dict(row, kind='skip', reason='condition excludes this pull request')
                    continue
            except (ValueError, KeyError, IndexError) as error:
                row.update(kind='warn', reason=str(error))
            row['command'] = expand(row['command'], ctx)
            row['env'] = {key: expand(value, ctx) for key, value in row['env'].items()}
            pending = row['command'] + str(row['env'])
            # Step outputs are resolved after their producer runs in the same job.
            pending = pending.replace('${{ runner.temp }}', '')
            pending = re.sub(r'\$\{\{\s*steps\.([\w-]+)\.outputs\.[\w-]+\s*\}\}',
                lambda match: '' if f'steps.{match[1]}.outcome' in validation else match[0], pending)
            if row['kind'] in ('gate', 'setup') and EXPRESSION.search(pending):
                row.update(kind='warn', reason='unmapped expression: ' + pending)
            yield row


def affected_jobs(root, base):
    token = os.environ.get('GH_TOKEN')
    if not token:
        try:
            token = subprocess.check_output(['gh', 'auth', 'token'], text=True,
                stdin=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=PROCESS_TIMEOUT_SECONDS).strip()
        except (OSError, subprocess.SubprocessError):
            token = ''
    if not token:
        return {job['key']: 'running: no GitHub token for input manifests'
                for job in pr_jobs(root)}
    try:
        origin = os.fsdecode(git(root, 'remote', 'get-url', 'origin')).strip()
        repository = re.search(r'github.com[:/](.+?)(?:\.git)?$', origin)
        if not repository:
            raise ValueError('origin is not a GitHub repository')
        request = partial(call, token=token, repository=repository[1])
        merge_base = os.fsdecode(git(root, 'merge-base', base, 'HEAD')).strip()
        changes = changes_from_name_status(os.fsdecode(git(root, 'diff', '--name-status', '-z', '-M', merge_base)))
        changes.extend({'filename': name, 'status': 'added'} for name in
                       os.fsdecode(git(root, 'ls-files', '-o', '--exclude-standard', '-z')).split('\0') if name)
        base_files = os.fsdecode(git(root, 'ls-tree', '-r', '--name-only', '-z', base)).split('\0')
        decisions = {}
        for workflow in workflows(root):
            jobs = set()
            for job in workflow['jobs'].values():
                for step in job.get('steps', []):
                    if 'job_inputs.py affected' in step.get('run', ''):
                        jobs.update(re.findall(r'--job\s+([\w-]+)', step['run']))
            if jobs:
                def decision(job, value, reason):
                    decisions[f'{workflow["stem"]}:{job}'] = (
                        'not affected' if value == 'false' else
                        'affected: ' + reason.removeprefix('matches: ') if reason.startswith('matches: ')
                        else 'running: ' + reason)

                affected_changes(workflow['stem'], sorted(jobs), changes,
                                 context(root, base)['github.base_ref'], base_files, reporter=decision, request=request)
        return decisions
    except Exception as error:
        return {job['key']: f'running: input manifests unavailable ({error})'
                for job in pr_jobs(root)}


def shell_command(row, command, root, script=None):
    template = row.get('shell', '') or 'bash -e {0}'
    template = {'bash': 'bash --noprofile --norc -eo pipefail {0}',
                'sh': 'sh -e {0}'}.get(template, template)
    shell = shlex.split(template)
    if shell[0] == 'bash':
        shell[0] = git_bash()
    if row.get('idf'):
        entrypoint = shlex.quote((root / 'scripts/ci/in_idf_image.sh').as_posix())
        inner = script.with_suffix('.idf.sh')
        inner.write_text(command, encoding='utf-8', newline='\n')
        invocation = shlex.join(part.replace('{0}', inner.as_posix()) for part in shell)
        command = f'sh {entrypoint} {shlex.quote(row["dir"])} {shlex.quote(invocation)}'
    script.write_text(command, encoding='utf-8', newline='\n')
    return [part.replace('{0}', script.as_posix()) for part in shell]


def run(root, out, base='origin/main', tiers=('host',)):
    try:
        reference = os.fsdecode(git(root, 'rev-parse', '--symbolic-full-name', '--verify', base)).strip()
    except subprocess.CalledProcessError:
        reference = ''
    if not reference.startswith('refs/remotes/'):
        raise ValueError('--base takes a branch such as origin/main')
    out.mkdir(parents=True, exist_ok=True)
    ctx = context(root, reference)
    decisions = affected_jobs(root, base)
    counts = dict(ok=0, failed=0, skipped=0, not_run=0)

    def verdict(status, detail):
        counts[status] += 1
        if status == 'failed' and not detail.startswith(f'FAIL {log}:'):
            log.write_text(detail, encoding='utf-8')
            detail = f'FAIL {log}: ' + detail.removeprefix('FAIL ')
        print(detail.replace('\r', r'\r').replace('\n', r'\n'), flush=True)

    selected = set(tiers) | {'host'}
    job_state = {}
    outcomes = {}
    broken_steps = set()
    skipped_installs = {}
    announced = set()
    for number, row in enumerate(rows(root, ctx)):
        key, tier, label = row['job'], row['tier'], row['label']
        prefix = f'{key}: {label}'
        log = out / f'{number:03d}-{key.replace(":", "-")}.log'
        decision = decisions.get(key, 'running: job has no recorded-input selector')
        if key not in announced:
            print(f'{key}: {decision}', flush=True)
            announced.add(key)
        dropped = row.get('dropped', [])
        suffix = ' - dropped: ' + '; '.join(dropped) if dropped else ''
        for install in dropped:
            skipped_installs.setdefault(key, []).append((label, install))
        workflow, name = key.split(':', 1)
        dependency = f'{workflow}:needs.{name}.result'
        if row['kind'] == 'warn':
            verdict('failed', f'FAIL {prefix}: {row["reason"]}' + suffix)
            continue
        if not set(tier.split(',')) <= selected:
            verdict('not_run', f'NOT RUN {tier}: {prefix}' + suffix)
            outcomes[dependency] = 'skipped'
            continue
        if decision == 'not affected':
            verdict('skipped', f'not affected: {prefix}' + suffix)
            outcomes[dependency] = 'skipped'
            continue
        if row['kind'] == 'skip':
            verdict('skipped', f'skip {row["reason"]}: {prefix}' + suffix)
            continue
        if key not in job_state:
            temporary = Path(tempfile.mkdtemp(prefix=key.replace(':', '-') + '-', dir=out))
            env = dict(os.environ, RUNNER_TEMP=temporary.as_posix(),
                       GITHUB_WORKSPACE=root.as_posix(), GITHUB_BASE_REF=ctx['github.base_ref'],
                       GITHUB_STEP_SUMMARY=(temporary / 'summary').as_posix(),
                       GITHUB_PATH=(temporary / 'path').as_posix())
            job_state[key] = (env, dict(ctx, success=True, **{'runner.temp': temporary.as_posix()}))
        env, state = job_state[key]
        state.update({name.split(':', 1)[1]: value for name, value in outcomes.items()
                      if name.startswith(workflow + ':')})
        expression = row['condition']
        try:
            allowed = (condition(row['job_condition'], state)
                       and (key, row['index']) not in broken_steps
                       and condition(expression, state)
                       and (state['success'] or any(marker in expression for marker in
                            ('always()', 'cancelled()', 'failure()'))))
        except (ValueError, KeyError, IndexError) as error:
            verdict('failed', f'FAIL {prefix}: {error}' + suffix)
            continue
        if not allowed:
            verdict('skipped', f'skip condition: {prefix}' + suffix)
            if row['id']:
                state[f'steps.{row["id"]}.outcome'] = 'skipped'
            continue
        command = expand(row['command'], state)
        step_env = dict(env, **{name: expand(value, state) for name, value in row['env'].items()})
        if EXPRESSION.search(command + str(step_env)):
            verdict('failed', f'FAIL {prefix}: unresolved expression' + suffix)
            continue
        path_file = Path(env['RUNNER_TEMP']) / f'path-{number}'
        step_env['GITHUB_PATH'] = path_file.as_posix()
        output = Path(env['RUNNER_TEMP']) / f'output-{number}'
        step_env['GITHUB_OUTPUT'] = output.as_posix()
        try:
            with log.open('w', encoding='utf-8') as capture:
                result = subprocess.run(shell_command(row, command, root,
                    Path(env['RUNNER_TEMP']) / f'step-{number}.sh'), cwd=root / row['dir'],
                                        env=step_env, stdin=subprocess.DEVNULL, stdout=capture, stderr=subprocess.STDOUT)
            passed = result.returncode == 0
        except OSError as error:
            log.write_text(str(error), encoding='utf-8')
            passed = False
        if path_file.exists():
            additions = path_file.read_text(encoding='utf-8', errors='replace').splitlines()
            env['PATH'] = os.pathsep.join([*reversed(additions), env.get('PATH', '')])
        if not passed and re.search(r'No module named|command not found|not recognized',
                                    log.read_text(encoding='utf-8', errors='replace')):
            hints = [f'{install_label}: {install}'
                     for install_label, install in skipped_installs.get(key, [])]
            if hints:
                suffix += ' - Host-wide change skipped (' + '; '.join(hints) + ')'
        verdict('ok' if passed else 'failed',
                f'{"ok" if passed else "FAIL"} {log}: {prefix}' + suffix)
        if row['id']:
            state[f'steps.{row["id"]}.outcome'] = 'success' if passed else 'failure'
            if output.exists():
                for line in output.read_text(encoding='utf-8', errors='replace').splitlines():
                    if '=' in line:
                        name, value = line.split('=', 1)
                        state[f'steps.{row["id"]}.outputs.{name}'] = value
        outcomes[dependency] = 'success' if passed and state['success'] else 'failure'
        if not passed:
            state['success'] = False
            broken_steps.add((key, row['index']))
    print(f'{counts["ok"]} ok, {counts["failed"]} failed, '
          f'{counts["skipped"]} skipped, {counts["not_run"]} not run', flush=True)
    return int(counts['failed'] != 0)


def main(argv=None):
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(encoding='utf-8')
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('list', 'run'))
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--base', default='origin/main')
    parser.add_argument('--out', type=Path)
    parser.add_argument('--tier', default='host', help='comma-separated tiers: ' + ','.join(TIERS))
    parser.epilog = 'list columns: kind | tier | workflow:job | label | directory | command or reason'
    args = parser.parse_args(argv)
    root = args.root.resolve()
    ctx = context(root, args.base)
    if args.action == 'list':
        for row in rows(root, ctx):
            print(' | '.join((row['kind'], row['tier'], row['job'], row['label'],
                              row['dir'], row['reason'] or row['command'].replace('\n', r'\n'))) +
                  (' | env: ' + ' '.join(f'{key}={shlex.quote(str(value))}' for key, value in row.get('env', {}).items()
                    if 'TOKEN' not in key) if row.get('env') else '') +
                  (' | dropped: ' + '; '.join(row['dropped']) if row.get('dropped') else ''),
                  flush=True)
        return 0
    if args.out is None:
        parser.error('run requires --out DIR')
    tiers = args.tier.split(',')
    if set(tiers) - set(TIERS):
        parser.error('unknown tier')
    try:
        return run(root, args.out.resolve(), args.base, tiers)
    except ValueError as error:
        parser.error(str(error))


if __name__ == '__main__':
    sys.exit(main())
