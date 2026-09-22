#!/usr/bin/env node
// Validates every ```mermaid block in the repo's tracked Markdown by rendering it with
// mermaid-cli. GitHub is the only render target and it fails silently, showing a parse
// error where the diagram should be - this is the only thing that catches that.
//
//   node scripts/gates/check-mermaid.mjs                 # every git-tracked *.md
//   node scripts/gates/check-mermaid.mjs path/to/file.md # one file or directory
//   node scripts/gates/check-mermaid.mjs --verbose       # also list the blocks that passed
//   node scripts/gates/check-mermaid.mjs --stdin <label> # one file's content from stdin,
//                                                         # reported under <label>
//
// File discovery is `git ls-files`, so an untracked scratch file or a vendored submodule
// (never checked out into the index) is never scanned; --stdin bypasses discovery for a
// staged blob that may not match what is on disk.
//
// CHECK_MERMAID_MMDC_ARGS appends extra space-separated arguments to every mmdc
// invocation - CI uses it to pass `-p <puppeteer-config.json>` for a sandboxed Chrome.
//
// Requires @mermaid-js/mermaid-cli (npm install -g @mermaid-js/mermaid-cli) and the Chrome
// it bundles; see docs/tools/Mermaid-Diagrams.md.

import { execFile } from 'node:child_process';
import { mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, relative, resolve } from 'node:path';
import { promisify } from 'node:util';
import { pathToFileURL } from 'node:url';

const execFileAsync = promisify(execFile);

const CONCURRENCY = 4;
const FENCE = /^[ \t]*```+[ \t]*mermaid[ \t]*$/;
const CLOSING_FENCE = /^[ \t]*```+[ \t]*$/;
const IS_WINDOWS = process.platform === 'win32';

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

// GitHub renders a literal `\n` inside a stateDiagram/stateDiagram-v2
// transition label as the two characters "\n", not a line break - the
// same trap the mermaid skill's "not a line break" note covers for a
// `note` block, one syntax over. A flowchart label renders `\n` as a real
// break, so this only looks at stateDiagram blocks; mmdc itself never
// catches it, since the source still parses.
export function findLiteralNewlineInStateDiagram(source) {
  const lines = source.split('\n');
  const firstLine = lines.find((line) => line.trim().length > 0);
  if (!firstLine || !STATE_DIAGRAM_HEADER.test(firstLine.trim())) {
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

// shell:true is required on Windows because mmdc is a .cmd shim, not a real executable -
// but shell:true only concatenates args with spaces, it does not escape them, so a path
// containing a space silently splits into extra arguments. Quoting each argument
// ourselves first is the fix; POSIX needs neither the shell nor the quoting.
async function runMmdc(args) {
  const extra = parseExtraMmdcArgs(process.env.CHECK_MERMAID_MMDC_ARGS);
  const full = [...args, ...extra];
  return execFileAsync('mmdc', IS_WINDOWS ? full.map(winQuote) : full, {
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
    await runMmdc(['-i', input, '-o', output, '-q']);
    return { label, ok: true };
  } catch (error) {
    const raw = `${error.stderr || ''}${error.stdout || ''}`.trim() || error.message;
    const parseError = raw.match(/Error:[\s\S]*?(?=\n\s*at\s|$)/);
    const message = rewriteLineNumbers((parseError ? parseError[0] : raw).trim(), block.line);
    return { label, ok: false, message };
  }
}

async function main() {
  const argv = process.argv.slice(2);
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
      console.error('git ls-files failed - run this from inside the autana repo.');
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
  try {
    await runMmdc(['--version']);
  } catch {
    console.error('mermaid-cli (mmdc) not found — cannot validate diagrams.');
    console.error('Install it with:  npm install -g @mermaid-js/mermaid-cli');
    process.exitCode = 1;
    return;
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

  if (verbose) {
    for (const result of results) {
      if (result.ok) {
        console.log(`  PASS  ${result.label}`);
      }
    }
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

  console.log(`\n${results.length - failures.length} passed, ${failures.length} failed.`);
  process.exitCode = failures.length ? 1 : 0;
}

if (import.meta.url === pathToFileURL(process.argv[1]).href) {
  main().catch((error) => {
    console.error(error);
    process.exitCode = 1;
  });
}
