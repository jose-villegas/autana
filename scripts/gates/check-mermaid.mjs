#!/usr/bin/env node
// Validates every ```mermaid block in the repo's tracked Markdown by rendering it with
// mermaid-cli. GitHub is the only render target and it fails silently, showing a parse
// error where the diagram should be - this is the only thing that catches that.
//
//   node scripts/gates/check-mermaid.mjs                 # every git-tracked *.md
//   node scripts/gates/check-mermaid.mjs path/to/file.md # one file or directory
//   node scripts/gates/check-mermaid.mjs --verbose       # also list the blocks that passed
//   node scripts/gates/check-mermaid.mjs --probe         # exit 0 when an mmdc runs, else 1
//   node scripts/gates/check-mermaid.mjs --stdin <label> # one file's content from stdin,
//                                                         # reported under <label>
//   node scripts/gates/check-mermaid.mjs --github-version
//        # exit 0 when GitHub's renderer runs the pinned mermaid, 1 when not, 2 when unreadable
//   node scripts/gates/check-mermaid.mjs --github-version --write
//        # the same, but a moved version is written into the override (exit 0), for the
//        # weekly bump pull request
//
// Renders the way GitHub does. GitHub draws each block in an iframe from VIEWSCREEN_URL,
// whose mermaidMarkdown bundle carries its own mermaid and options. package.json overrides
// mermaid to that version and mermaid/github-config.json copies the bundle's initialize
// options; --github-version shows when GitHub moves.
//
// A diagram taller than TALL_RATIO times its width is reported TALL, a warning for now
// (TALL_FAILS). The usual cause: GitHub's mermaid drops a subgraph's `direction` when a
// link crosses the subgraph's boundary, so the subgraphs stack in the flowchart's direction.
//
// File discovery is `git ls-files`, so an untracked scratch file or a vendored submodule
// (never checked out into the index) is never scanned; --stdin bypasses discovery for a
// staged blob that may not match what is on disk.
//
// CHECK_MERMAID_MMDC_ARGS appends extra space-separated arguments to every mmdc
// invocation - CI uses it to pass `-p <puppeteer-config.json>` for a sandboxed Chrome.
//
// The browser is found without setup: whatever PUPPETEER_EXECUTABLE_PATH names when the
// caller set it, else a chromium or chrome on PATH, else the Chrome mermaid-cli bundles.
//
// Runs the mermaid-cli scripts/gates/mermaid/package.json pins (npm ci --prefix
// scripts/gates/mermaid), else an mmdc on PATH with a note naming both versions when they
// differ; see docs/tools/Mermaid-Diagrams.md.

import { execFile } from 'node:child_process';
import { existsSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { dirname, join, posix, relative, resolve, win32 } from 'node:path';
import { promisify } from 'node:util';
import { fileURLToPath, pathToFileURL } from 'node:url';

const execFileAsync = promisify(execFile);

const CONCURRENCY = 4;
const FENCE = /^[ \t]*```+[ \t]*mermaid[ \t]*$/;
const CLOSING_FENCE = /^[ \t]*```+[ \t]*$/;
const IS_WINDOWS = process.platform === 'win32';
const PINNED_DIR = join(dirname(fileURLToPath(import.meta.url)), 'mermaid');
const PINNED_PACKAGE = '@mermaid-js/mermaid-cli';
const INSTALL = 'npm ci --prefix scripts/gates/mermaid';
const GITHUB_CONFIG = join(PINNED_DIR, 'github-config.json');
const VIEWSCREEN_URL = 'https://viewscreen.githubusercontent.com/markdown/mermaid';
const RETRY_DELAY_MS = 5000;

// height / width past which a diagram reads as a tall column on GitHub's page.
export const TALL_RATIO = 1.3;
// Flip to true once no tracked diagram is tall (autana-t56l).
const TALL_FAILS = false;
const TALL_HINT = `Taller than ${TALL_RATIO}x its width, as GitHub shows it. A flowchart subgraph's direction is ` +
  'ignored once a link crosses its boundary: link subgraph to subgraph, or change the flowchart direction.';

// The mmdc npm ci installs beside this script, else whichever is on PATH.
export function mmdcCommand(pinnedDir = PINNED_DIR) {
  const local = join(pinnedDir, 'node_modules', '.bin', IS_WINDOWS ? 'mmdc.cmd' : 'mmdc');
  return existsSync(local) ? local : 'mmdc';
}

function pinnedPackage(pinnedDir) {
  return JSON.parse(readFileSync(join(pinnedDir, 'package.json'), 'utf8'));
}

export function pinnedVersion(pinnedDir = PINNED_DIR) {
  return pinnedPackage(pinnedDir).dependencies[PINNED_PACKAGE];
}

// The mermaid package.json overrides mermaid-cli's own to: GitHub's version.
export function pinnedMermaidVersion(pinnedDir = PINNED_DIR) {
  return pinnedPackage(pinnedDir).overrides.mermaid;
}

// The width and height of an SVG's viewBox, the shape GitHub scales to its page.
export function svgSize(svg) {
  const match = svg.match(/<svg\b[^>]*\bviewBox="([^"]+)"/);
  if (!match) {
    return null;
  }
  const [, , width, height] = match[1].trim().split(/[\s,]+/).map(Number);
  return width > 0 && height > 0 ? { width, height } : null;
}

export function isTall({ width, height }) {
  return height > TALL_RATIO * width;
}

export function formatSize({ width, height }) {
  return `${Math.round(width)}x${Math.round(height)}`;
}

// The mermaid bundle's path in the viewscreen page, and the version that bundle passes to
// every diagram renderer's draw() call.
export function viewscreenBundlePath(html) {
  return html.match(/src="([^"]*\/mermaidMarkdown[^"]*\.js)"/)?.[1] ?? null;
}

export function bundleMermaidVersion(js) {
  return js.match(/renderer\.draw\([\w$]+,[\w$]+,"(\d+\.\d+\.\d+[^"]*)"/)?.[1] ?? null;
}

async function fetchText(url) {
  let lastError;
  // One retry: viewscreen has answered with a transient redirect to github.com before.
  for (let attempt = 0; attempt < 2; attempt++) {
    if (attempt) {
      await new Promise((resolve) => setTimeout(resolve, RETRY_DELAY_MS));
    }
    try {
      const response = await fetch(url, { redirect: 'manual' });
      if (response.ok) {
        return await response.text();
      }
      lastError = new Error(`${url}: HTTP ${response.status}`);
    } catch (error) {
      lastError = error;
    }
  }
  throw lastError;
}

// The mermaid version GitHub renders with now; throws when the page or bundle changed shape.
export async function githubMermaidVersion() {
  const bundle = viewscreenBundlePath(await fetchText(VIEWSCREEN_URL));
  if (!bundle) {
    throw new Error(`no mermaidMarkdown bundle in ${VIEWSCREEN_URL}`);
  }
  const version = bundleMermaidVersion(await fetchText(new URL(bundle, VIEWSCREEN_URL).href));
  if (!version) {
    throw new Error(`no mermaid version in ${bundle}`);
  }
  return version;
}

export function writePinnedMermaidVersion(version, pinnedDir = PINNED_DIR) {
  const file = join(pinnedDir, 'package.json');
  const pkg = pinnedPackage(pinnedDir);
  pkg.overrides.mermaid = version;
  writeFileSync(file, `${JSON.stringify(pkg, null, 2)}
`);
}

async function checkGithubVersion(write) {
  const pinned = pinnedMermaidVersion();
  let github;
  try {
    github = await githubMermaidVersion();
  } catch (error) {
    console.error(`Cannot read GitHub's mermaid version: ${error.message}`);
    process.exitCode = 2;
    return;
  }
  if (github === pinned) {
    console.log(`GitHub runs mermaid ${github}, the pinned version.`);
    return;
  }
  if (write) {
    writePinnedMermaidVersion(github);
    console.log(`GitHub runs mermaid ${github}, pinned ${pinned}: wrote ${github} into the override.`);
    return;
  }
  console.log(`GitHub runs mermaid ${github}, pinned ${pinned}: bump the override in scripts/gates/mermaid/package.json.`);
  process.exitCode = 1;
}

export function extractBlocks(text) {
  const lines = text.split(/\r?\n/);
  const blocks = [];
  let start = -1;
  let body = [];

  for (let i = 0; i < lines.length; i++) {
    if (start === -1) {
      if (FENCE.test(lines[i])) {
        start = i + 1;
        body = [];
      }
      continue;
    }

    if (CLOSING_FENCE.test(lines[i])) {
      blocks.push({ line: start, source: body.join('\n') });
      start = -1;
      continue;
    }

    body.push(lines[i]);
  }

  if (start !== -1) {
    blocks.push({ line: start, source: body.join('\n'), unterminated: true });
  }

  return blocks;
}

async function extractFileBlocks(file) {
  return extractBlocks(await readFile(file, 'utf8'));
}

async function readStdin() {
  const chunks = [];
  for await (const chunk of process.stdin) {
    chunks.push(chunk);
  }
  return Buffer.concat(chunks).toString('utf8');
}

// git ls-files is the one place that knows which .md files are actually tracked - it
// already skips .git, an untracked scratch file, and a submodule's own working tree (a
// gitlink entry, never its files), so nothing here has to restate that list.
async function trackedMarkdownFiles(pathspecs, cwd) {
  const { stdout } = await execFileAsync(
    'git',
    ['ls-files', '-z', '--', ...(pathspecs.length ? pathspecs : ['*.md'])],
    { cwd },
  );
  return stdout
    .split('\0')
    .filter(Boolean)
    .filter((f) => f.endsWith('.md'))
    .map((f) => resolve(cwd, f))
    .sort();
}

const STATE_DIAGRAM_HEADER = /^stateDiagram(-v2)?\b/;
const FRONTMATTER_FENCE = /^---\s*$/;
const DIRECTIVE_LINE = /^%%\{.*\}%%\s*$/;

// The diagram-type keyword is not always the block's first line: a YAML
// frontmatter block (--- ... ---) or one or more %%{init: ...}%% directives
// may come first. Skip past those, and any blank line, to find it.
function diagramTypeLineIndex(lines) {
  let i = 0;
  while (i < lines.length && lines[i].trim().length === 0) {
    i++;
  }
  if (i < lines.length && FRONTMATTER_FENCE.test(lines[i])) {
    i++;
    while (i < lines.length && !FRONTMATTER_FENCE.test(lines[i])) {
      i++;
    }
    i++; // past the closing fence
  }
  while (i < lines.length && (lines[i].trim().length === 0 || DIRECTIVE_LINE.test(lines[i].trim()))) {
    i++;
  }
  return i;
}

// GitHub renders a literal `\n` inside a stateDiagram/stateDiagram-v2
// transition label as the two characters "\n", not a line break. A
// flowchart label renders `\n` as a real break, so this only looks at
// stateDiagram blocks; mmdc itself never catches it, since the source
// still parses.
export function findLiteralNewlineInStateDiagram(source) {
  const lines = source.split('\n');
  const typeIndex = diagramTypeLineIndex(lines);
  const typeLine = lines[typeIndex];
  if (!typeLine || !STATE_DIAGRAM_HEADER.test(typeLine.trim())) {
    return null;
  }

  for (let i = 0; i < lines.length; i++) {
    if (lines[i].includes('\\n')) {
      return { lineIndex: i, line: lines[i] };
    }
  }
  return null;
}

// mmdc reports the line within the block; map it back to the file so the
// output is clickable.
export function rewriteLineNumbers(message, blockStartLine) {
  return message.replace(/(Parse|Lexical) error on line (\d+)/g, (_, kind, n) =>
    `${kind} error on file line ${Number(n) + blockStartLine - 1} (line ${n} of the block)`);
}

export function winQuote(arg) {
  return `"${String(arg).replace(/"/g, '\\"')}"`;
}

export function parseExtraMmdcArgs(raw) {
  return raw ? raw.split(/\s+/).filter(Boolean) : [];
}

const BROWSER_NAMES = ['chromium', 'chromium-browser', 'google-chrome', 'google-chrome-stable', 'chrome'];
const PUPPETEER_CONFIG_FLAGS = ['-p', '--puppeteerConfigFile'];

// The first Chromium or Chrome on `pathVariable`, in BROWSER_NAMES order, else null. A
// machine that has one should not also need an environment variable to say so.
export function findBrowserOnPath(pathVariable, isWindows = IS_WINDOWS, exists = existsSync) {
  const folders = (pathVariable || '').split(isWindows ? ';' : ':').filter(Boolean);
  const joinPath = isWindows ? win32.join : posix.join;
  for (const name of BROWSER_NAMES) {
    for (const folder of folders) {
      const candidate = joinPath(folder, isWindows ? `${name}.exe` : name);
      if (exists(candidate)) {
        return candidate;
      }
    }
  }
  return null;
}

// The mmdc arguments that name the browser: none when the caller already passed a Puppeteer
// config (CI does), named a browser through PUPPETEER_EXECUTABLE_PATH (explicit beats
// discovered), or when no browser is on PATH, which leaves mmdc the Chrome it bundles.
export function browserConfigArgs(extra, browser, configFile, explicitBrowser = '') {
  const given = extra.some((arg) => PUPPETEER_CONFIG_FLAGS.some((flag) => arg === flag || arg.startsWith(`${flag}=`)));
  return given || explicitBrowser || !browser ? [] : ['-p', configFile];
}

let browserConfigDir = null;

async function browserArguments(extra) {
  const browser = findBrowserOnPath(process.env.PATH);
  if (!browser || browserConfigArgs(extra, browser, '', process.env.PUPPETEER_EXECUTABLE_PATH).length === 0) {
    return [];
  }
  if (!browserConfigDir) {
    browserConfigDir = await mkdtemp(join(tmpdir(), 'mermaid-browser-'));
    await writeFile(join(browserConfigDir, 'puppeteer.json'), JSON.stringify({ executablePath: browser }));
    process.on('exit', () => rmSync(browserConfigDir, { recursive: true, force: true }));
  }
  return browserConfigArgs(extra, browser, join(browserConfigDir, 'puppeteer.json'), process.env.PUPPETEER_EXECUTABLE_PATH);
}

// shell:true is required on Windows because mmdc is a .cmd shim, not a real executable -
// but shell:true only concatenates args with spaces, it does not escape them, so a path
// containing a space silently splits into extra arguments. Quoting each argument
// ourselves first is the fix; POSIX needs neither the shell nor the quoting.
async function runMmdc(args) {
  const extra = parseExtraMmdcArgs(process.env.CHECK_MERMAID_MMDC_ARGS);
  const full = [...args, ...extra, ...(await browserArguments(extra))];
  const mmdc = mmdcCommand();
  return execFileAsync(IS_WINDOWS ? winQuote(mmdc) : mmdc, IS_WINDOWS ? full.map(winQuote) : full, {
    shell: IS_WINDOWS,
    maxBuffer: 1024 * 1024 * 10,
  });
}

async function validate(block, label, workDir, index) {
  if (block.unterminated) {
    return { label, ok: false, message: 'Unterminated ```mermaid fence — no closing ```' };
  }

  if (!block.source.trim()) {
    return { label, ok: false, message: 'Empty mermaid block' };
  }

  const literalNewline = findLiteralNewlineInStateDiagram(block.source);
  if (literalNewline) {
    const fileLine = block.line + literalNewline.lineIndex;
    return {
      label,
      ok: false,
      message:
        `File line ${fileLine}: literal "\\n" inside a stateDiagram label renders as text ` +
        `on GitHub, not a line break - use <br/> instead.\n  ${literalNewline.line.trim()}`,
    };
  }

  const input = join(workDir, `block-${index}.mmd`);
  const output = join(workDir, `block-${index}.svg`);
  await writeFile(input, block.source, 'utf8');

  try {
    await runMmdc(['-i', input, '-o', output, '-c', GITHUB_CONFIG, '-q']);
    return { label, line: block.line, ok: true, size: svgSize(await readFile(output, 'utf8')) };
  } catch (error) {
    const raw = `${error.stderr || ''}${error.stdout || ''}`.trim() || error.message;
    const parseError = raw.match(/Error:[\s\S]*?(?=\n\s*at\s|$)/);
    const message = rewriteLineNumbers((parseError ? parseError[0] : raw).trim(), block.line);
    return { label, ok: false, message };
  }
}

async function main() {
  const argv = process.argv.slice(2);
  if (argv.includes('--probe')) {
    await runMmdc(['--version']).catch(() => { process.exitCode = 1; });
    return;
  }
  if (argv.includes('--github-version')) {
    await checkGithubVersion(argv.includes('--write'));
    return;
  }
  const verbose = argv.includes('--verbose');
  const stdinIndex = argv.indexOf('--stdin');
  const stdinLabel = stdinIndex !== -1 ? argv[stdinIndex + 1] : null;

  if (stdinIndex !== -1 && !stdinLabel) {
    console.error('--stdin requires a label argument, e.g. --stdin docs/foo.md');
    process.exitCode = 1;
    return;
  }

  const targets = argv.filter((a, i) => {
    if (a === '--verbose') return false;
    if (stdinIndex !== -1 && (i === stdinIndex || i === stdinIndex + 1)) return false;
    return true;
  });

  const tasks = [];

  if (stdinLabel) {
    for (const block of extractBlocks(await readStdin())) {
      tasks.push({ label: stdinLabel, block });
    }
  } else {
    let files;
    try {
      files = await trackedMarkdownFiles(targets, process.cwd());
    } catch (error) {
      console.error('git ls-files failed - run this from inside a git repository.');
      console.error((error.stderr || error.message || '').trim());
      process.exitCode = 1;
      return;
    }
    for (const file of files) {
      const label = relative(process.cwd(), file);
      for (const block of await extractFileBlocks(file)) {
        tasks.push({ label, block });
      }
    }
  }

  if (!tasks.length) {
    console.log('No mermaid blocks found.');
    return;
  }

  // Probe once, so a missing CLI reads as one actionable line instead of N identical
  // per-block failures.
  let found;
  try {
    found = (await runMmdc(['--version'])).stdout.trim();
  } catch {
    console.error('mermaid-cli (mmdc) not found — cannot validate diagrams.');
    console.error(`Install it with:  ${INSTALL}`);
    process.exitCode = 1;
    return;
  }
  if (found !== pinnedVersion()) {
    console.warn(`note: mermaid-cli ${found} here, ${pinnedVersion()} pinned; CI runs the pinned one (${INSTALL}).`);
  }

  console.log(`Validating ${tasks.length} mermaid block(s)...\n`);

  const results = new Array(tasks.length);
  const workDir = await mkdtemp(join(tmpdir(), 'mermaid-validate-'));
  let cursor = 0;

  try {
    await Promise.all(
      Array.from({ length: Math.min(CONCURRENCY, tasks.length) }, async () => {
        while (cursor < tasks.length) {
          const i = cursor++;
          const { label, block } = tasks[i];
          results[i] = await validate(block, label, workDir, i);
        }
      }),
    );
  } finally {
    await rm(workDir, { recursive: true, force: true });
  }

  const failures = results.filter((r) => !r.ok);
  const tall = results.filter((r) => r.ok && r.size && isTall(r.size));

  if (verbose) {
    for (const result of results) {
      if (result.ok) {
        console.log(`  PASS  ${result.label}:${result.line}  ${result.size ? formatSize(result.size) : 'no viewBox'}`);
      }
    }
  }

  if (tall.length) {
    console.log('');
    for (const result of tall) {
      const shape = `${formatSize(result.size)}, height ${(result.size.height / result.size.width).toFixed(2)}x width`;
      console.log(`  TALL  ${result.label}:${result.line}  ${shape}`);
      if (process.env.GITHUB_ACTIONS === 'true') {
        const level = TALL_FAILS ? 'error' : 'warning';
        console.log(`::${level} file=${result.label.replace(/\\/g, '/')},line=${result.line}::Tall diagram (${shape}). ${TALL_HINT}`);
      }
    }
    console.log(`        ${TALL_HINT}`);
  }

  if (failures.length) {
    console.log('');
    for (const failure of failures) {
      console.log(`  FAIL  ${failure.label}`);
      for (const line of failure.message.split('\n')) {
        console.log(`        ${line}`);
      }
      console.log('');
    }
  }

  console.log(`\n${results.length - failures.length} passed, ${failures.length} failed, ${tall.length} tall.`);
  process.exitCode = failures.length || (TALL_FAILS && tall.length) ? 1 : 0;
}

if (import.meta.url === pathToFileURL(process.argv[1]).href) {
  main().catch((error) => {
    console.error(error);
    process.exitCode = 1;
  });
}
