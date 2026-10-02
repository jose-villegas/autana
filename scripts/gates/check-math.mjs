#!/usr/bin/env node
// Validates every formula in the repo's tracked Markdown for GitHub, which shows a broken
// formula as raw text or a red box and never fails a check. It enforces the forms that
// Markdown leaves alone, and renders each formula with GitHub's MathJax configuration.
//
//   node scripts/gates/check-math.mjs                 # every git-tracked *.md
//   node scripts/gates/check-math.mjs path/to/file.md # one file or directory
//   node scripts/gates/check-math.mjs --verbose       # also list the files that passed
//   node scripts/gates/check-math.mjs --stdin <label> # one file's content from stdin
//
// Finding formulas. GitHub parses the Markdown first and looks for `$` only in the text
// that comes out, outside emphasis, <b>, links, code and footnotes. Those delimiter rules
// were measured by rendering probe files in a secret gist's file view (the renderer of a
// repository file; `gh api markdown` renders a comment instead) and are pinned by
// tests/test_check_math.mjs. Rather than predict every rewrite, the gate refuses what
// Markdown can rewrite: `_`, `*` or a backslash escape inside `$...$`, and `$$`.
//
// Rendering. GitHub's <math-renderer> element, chunk-lazy-element-math-renderer-*.js on
// github.githubassets.com, runs MathJax 3.2.0 with AllPackages minus REMOVED_PACKAGES and
// MAX_MACROS, refuses any formula naming a REFUSED_MACROS entry ("The following macros
// are not allowed: ..."), and stops past the BRACE_LIMIT_* opening braces. To refresh:
// open a repository .md file with a formula on github.com, find that chunk in the
// network panel, and copy its `h=[...]`, `packages:{"[-]":[...]}`, `maxMacros` and brace
// limits here. noundefined is dropped on top, so an undefined macro, which GitHub
// paints red, fails here.
//
// Dependencies are pinned in package.json beside this script: npm ci --prefix scripts/gates.
// See docs/tools/Math-Formulas.md.

import { execFile } from 'node:child_process';
import { readFileSync } from 'node:fs';
import { readFile } from 'node:fs/promises';
import { createRequire } from 'node:module';
import { relative, resolve } from 'node:path';
import { promisify } from 'node:util';
import { fileURLToPath, pathToFileURL } from 'node:url';

const execFileAsync = promisify(execFile);

export const REFUSED_MACROS = [
  'DeclareMathOperator', 'DeclarePairedDelimiters', 'renewtagform', 'newtagform', 'colorbox',
  'fcolorbox', 'hphantom', 'vphantom', 'phantom', 'operatorname', 'Newextarrow',
  'definecolor', 'mathchoice', 'unicode', 'mmlToken',
];
export const REMOVED_PACKAGES = ['noerrors', 'bbox', 'html', 'require', 'newcommand', 'action', 'colortbl'];
export const MAX_MACROS = 1000;
export const BRACE_LIMIT_FORMULA = 1000;
export const BRACE_LIMIT_PAGE = 2000;

// Inside these elements GitHub never looks for `$`.
const SKIPPED_TAGS = new Set(['em', 'b', 'a', 'code', 'pre']);
const WHITESPACE = /\s/;
const OPENER_PRECEDER = /[\s(]/;
const GITHUB_RULES = { opener: OPENER_PRECEDER, closerForbidden: /[A-Za-z0-9_`]/ };
// Reading the source, Markdown's own `*`, `_`, `~`, `[` and `>` may sit against a `$`
// that GitHub, after removing them, accepts.
const WRITTEN_RULES = { opener: /[\s(*_~[>]/, closerForbidden: /[A-Za-z0-9`]/ };
const FOOTNOTE_DEFINITION = /^\s{0,3}\[\^[^\]]+\]:/;

export const INSTALL_HINT = 'npm ci --prefix scripts/gates';

// ---------------------------------------------------------------------------------------
// Dependencies, from scripts/gates/node_modules, at the versions package.json pins.

let deps = null;

export function pinnedVersions() {
  const manifest = JSON.parse(readFileSync(new URL('./package.json', import.meta.url), 'utf8'));
  return manifest.dependencies;
}

// null when the pinned versions are installed, else what is wrong.
export function dependencyProblem(pins = pinnedVersions()) {
  const require = createRequire(fileURLToPath(import.meta.url));
  for (const [name, wanted] of Object.entries(pins)) {
    let found;
    try {
      found = require(`${name}/package.json`).version;
    } catch {
      return `${name} is not installed`;
    }
    if (found !== wanted) {
      return `${name} ${found} is installed; package.json pins ${wanted}`;
    }
  }
  return null;
}

export function loadDependencies() {
  if (!deps) {
    const require = createRequire(fileURLToPath(import.meta.url));
    deps = { MarkdownIt: require('markdown-it'), require };
  }
  return deps;
}

// ---------------------------------------------------------------------------------------
// Finding the formulas.

// `$` and `$$` delimiters in one run of text that GitHub sees as a single text node.
// Returns [{ start, end, delimiter, content }] with offsets into `text`.
export function scanDollars(text, rules = GITHUB_RULES) {
  const found = [];
  let i = 0;
  while (i < text.length) {
    if (text[i] !== '$') {
      i++;
      continue;
    }
    const width = text[i + 1] === '$' ? 2 : 1;
    const before = i > 0 ? text[i - 1] : '';
    const first = text[i + width];
    if ((before && !rules.opener.test(before)) || first === undefined || WHITESPACE.test(first) || first === '$') {
      i += width;
      continue;
    }
    const close = text.indexOf('$', i + width);
    if (close === -1) {
      break;
    }
    if (isCloser(text, close, width, rules)) {
      found.push({ start: i, end: close + width, delimiter: '$'.repeat(width), content: text.slice(i + width, close) });
      i = close + width;
    } else {
      i += width;
    }
  }
  return found;
}

function isCloser(text, at, width, rules) {
  if (width === 2 && text[at + 1] !== '$') {
    return false;
  }
  const after = text[at + width];
  if (after !== undefined && rules.closerForbidden.test(after)) {
    return false;
  }
  // A single `$` with a space on both sides is a literal dollar, never a delimiter.
  return !(width === 1 && text[at - 1] === ' ' && after === ' ');
}

function lineOf(text, offset) {
  let line = 0;
  for (let k = 0; k < offset; k++) {
    if (text[k] === '\n') {
      line++;
    }
  }
  return line;
}

// GitHub doubles a backslash that ends a line of a math fence, so `x\` reaches MathJax
// as `x\\`, a line break.
export function displayLines(text) {
  return text.replace(/\\$/gm, '\\\\');
}

function inlineFormulas(children, baseLine) {
  const formulas = [];
  const runs = [];
  let run = null;
  let skipDepth = 0;
  let lineInBlock = 0;

  const endRun = () => {
    if (run) {
      runs.push(run);
      run = null;
    }
  };

  // $`...`$: a run ending in `$`, the code span, and a text starting with `$`.
  const codeForm = (token, next) => {
    if (!run || !run.text.endsWith('$') || !next || next.type !== 'text' || !next.content.startsWith('$')) {
      return;
    }
    const at = run.text.length - 1;
    const before = at > 0 ? run.text[at - 1] : '';
    if (before && !OPENER_PRECEDER.test(before)) {
      return;
    }
    formulas.push({
      line: baseLine + run.line + lineOf(run.text, at),
      delimiter: '$`',
      source: `$${escapeHtml(token.content)}$`,
      display: false,
    });
    run.text = run.text.slice(0, at);
    next.content = next.content.slice(1);
  };

  const visit = (token, next) => {
    if (['em_open', 'em_close', 'link_open', 'link_close'].includes(token.type)) {
      skipDepth = Math.max(0, skipDepth + token.nesting);
    } else if (token.type === 'html_inline') {
      const tag = token.content.match(/^<(\/?)([A-Za-z][\w-]*)/);
      if (tag && SKIPPED_TAGS.has(tag[2].toLowerCase()) && !/\/>$/.test(token.content)) {
        skipDepth = Math.max(0, skipDepth + (tag[1] ? -1 : 1));
      }
    } else if (skipDepth === 0 && (token.type === 'text' || token.type === 'softbreak')) {
      run = run || { text: '', line: lineInBlock };
      run.text += token.type === 'text' ? token.content : '\n';
      return;
    } else if (skipDepth === 0 && token.type === 'code_inline') {
      codeForm(token, next);
    }
    endRun();
  };

  for (let k = 0; k < children.length; k++) {
    const token = children[k];
    visit(token, children[k + 1]);
    const content = token.content || '';
    lineInBlock += token.type === 'softbreak' || token.type === 'hardbreak' ? 1 : lineOf(content, content.length);
  }
  endRun();

  for (const r of runs) {
    for (const hit of scanDollars(r.text)) {
      formulas.push({
        line: baseLine + r.line + lineOf(r.text, hit.start),
        delimiter: hit.delimiter,
        source: `${hit.delimiter}${escapeHtml(hit.content)}${hit.delimiter}`,
        display: false,
      });
    }
  }
  return formulas;
}

// Text nodes of a raw HTML block, outside SKIPPED_TAGS. Markdown is not applied there.
function htmlBlockFormulas(html, baseLine) {
  const formulas = [];
  const tag = /<(\/?)([A-Za-z][\w-]*)[^>]*?(\/?)>|<!--[\s\S]*?-->/g;
  let skipDepth = 0;
  let last = 0;
  const scanText = (text, offset) => {
    if (skipDepth > 0) {
      return;
    }
    const decoded = decodeEntities(text);
    for (const hit of scanDollars(decoded)) {
      formulas.push({
        line: baseLine + lineOf(html, offset) + lineOf(decoded, hit.start),
        delimiter: hit.delimiter,
        source: `${hit.delimiter}${escapeHtml(hit.content)}${hit.delimiter}`,
        display: false,
      });
    }
  };
  for (let m = tag.exec(html); m; m = tag.exec(html)) {
    scanText(html.slice(last, m.index), last);
    last = m.index + m[0].length;
    if (m[2] && SKIPPED_TAGS.has(m[2].toLowerCase()) && !m[3]) {
      skipDepth = Math.max(0, skipDepth + (m[1] ? -1 : 1));
    }
  }
  scanText(html.slice(last), last);
  return formulas;
}

const NAMED_ENTITIES = { amp: '&', lt: '<', gt: '>', quot: '"', apos: "'", nbsp: ' ' };

export function escapeHtml(text) {
  return text.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
}

export function decodeEntities(text) {
  return text.replace(/&(#[0-9]+|#[xX][0-9a-fA-F]+|[A-Za-z]+);/g, (whole, name) => {
    if (name[0] === '#') {
      const code = name[1] === 'x' || name[1] === 'X' ? parseInt(name.slice(2), 16) : parseInt(name.slice(1), 10);
      return Number.isFinite(code) ? String.fromCodePoint(code) : whole;
    }
    return NAMED_ENTITIES[name] ?? whole;
  });
}

let markdown = null;

function markdownParser() {
  if (!markdown) {
    const { MarkdownIt } = loadDependencies();
    markdown = new MarkdownIt({ html: true, linkify: true });
  }
  return markdown;
}

// Every formula GitHub hands to MathJax, in document order: [{ line, delimiter, source,
// display }], `source` being what <math-renderer> receives. `blocks` are the paragraphs,
// headings and table rows Markdown parses, as 1-based inclusive line ranges; `opaque` are
// the lines of code and HTML blocks.
export function extractFormulas(text) {
  const tokens = markdownParser().parse(text, {});
  const formulas = [];
  const blocks = new Map();
  const opaque = [];
  let line = 0;
  let footnote = false;

  for (const token of tokens) {
    if (token.map) {
      line = token.map[0] + 1;
      if (['paragraph_open', 'heading_open', 'tr_open'].includes(token.type)) {
        blocks.set(token.map[0], { first: token.map[0] + 1, last: token.map[1] });
      } else if (['fence', 'code_block', 'html_block'].includes(token.type)) {
        opaque.push({ first: token.map[0] + 1, last: token.map[1] });
      }
    }
    if (token.type === 'fence' && token.info.trim().split(/\s+/)[0] === 'math') {
      const source = `$$${displayLines(token.content).trim()}$$`;
      formulas.push({ line: line + 1, delimiter: '```math', source, display: true });
    } else if (token.type === 'html_block') {
      formulas.push(...htmlBlockFormulas(token.content, line));
    } else if (token.type === 'paragraph_open') {
      footnote = false;
    } else if (token.type === 'inline') {
      // GitHub renders footnote text after it has looked for maths.
      footnote = footnote || FOOTNOTE_DEFINITION.test(token.content);
      if (!footnote) {
        formulas.push(...inlineFormulas(token.children || [], line));
      }
    }
  }
  formulas.sort((a, b) => a.line - b.line);
  return { formulas, blocks: [...blocks.values()], opaque };
}

// The formulas as written in some Markdown source, before Markdown touches it, by
// GitHub's delimiter rules: [{ line, delimiter, content, codeForm }]. Code spans are
// blanked unless they sit between `$` and `$`.
export function writtenFormulas(source, firstLine) {
  const blanked = blankCodeSpans(source);
  return scanDollars(blanked, WRITTEN_RULES).map((hit) => ({
    line: firstLine + lineOf(blanked, hit.start),
    delimiter: hit.delimiter,
    content: hit.content,
    codeForm: hit.delimiter === '$' && hit.content.length > 1 && hit.content.startsWith('`') && hit.content.endsWith('`'),
  }));
}

function blankCodeSpans(source) {
  return source.replace(/(`+)([^`][\s\S]*?)\1(?!`)/g, (span, _ticks, _body, offset) =>
    source[offset - 1] === '$' && source[offset + span.length] === '$' ? span : span.replace(/[^\n]/g, ' '),
  );
}

// What Markdown could rewrite inside `$...$`: emphasis markers, and a backslash before
// ASCII punctuation, which is an escape (`\,` reaches MathJax as `,`). null when none.
export function markdownHazard(content) {
  const marker = content.match(/[_*]/);
  if (marker) {
    return `\`${marker[0]}\` can pair up as Markdown emphasis`;
  }
  const escape = content.match(/\\[!-/:-@[-`{-~]/);
  return escape ? `Markdown reads \`${escape[0]}\` as an escape and hands MathJax \`${escape[0][1]}\`` : null;
}

const USE_CODE_FORM = 'Write it as $`...`$, which Markdown leaves alone.';
const USE_FENCE = 'Write display maths as a ```math fence and inline maths as $`...`$.';

// The forms Markdown can rewrite, refused as written, and formulas GitHub never hands to
// MathJax because they sit in emphasis, a link, <b> or a footnote.
export function writingProblems(text, formulas, blocks, opaque) {
  const lines = text.split(/\r?\n/);
  const reached = new Map();
  for (const f of formulas) {
    reached.set(f.line, (reached.get(f.line) || 0) + 1);
  }
  const problems = [];
  const report = (line, delimiter, source, message) => problems.push({ line, delimiter, source, message });

  for (const block of blocks) {
    const source = lines.slice(block.first - 1, block.last).join('\n');
    const blanked = blankCodeSpans(source);
    const display = blanked.search(/(?<![\\$])\$\$(?!\$)/);
    if (display !== -1) {
      const line = block.first + lineOf(blanked, display);
      report(line, '$$', lines[line - 1].trim(), `GitHub's $$ depends on the Markdown around it. ${USE_FENCE}`);
      continue;
    }
    for (const written of writtenFormulas(source, block.first)) {
      const shown = `$${written.content}$`;
      const wasReached = reached.get(written.line) > 0;
      if (wasReached) {
        reached.set(written.line, reached.get(written.line) - 1);
      }
      const hazard = written.codeForm ? null : markdownHazard(written.content);
      if (hazard) {
        report(written.line, '$', shown, `${hazard}. ${USE_CODE_FORM}`);
      } else if (!wasReached) {
        report(written.line, '$', shown, 'GitHub shows this as text: it sits in emphasis, a link, <b> or a footnote.');
      }
    }
  }

  // A one-line footnote definition parses as a link reference here, so no block holds it.
  const covered = new Set();
  for (const range of [...blocks, ...opaque]) {
    for (let n = range.first; n <= range.last; n++) {
      covered.add(n);
    }
  }
  for (let n = 1; n <= lines.length; n++) {
    if (!covered.has(n) && FOOTNOTE_DEFINITION.test(lines[n - 1])) {
      for (const written of writtenFormulas(lines[n - 1], n)) {
        report(n, '$', `$${written.content}$`, 'GitHub renders footnotes after maths, so this stays text.');
      }
    }
  }
  return problems;
}

// ---------------------------------------------------------------------------------------
// Rendering, as <math-renderer> does.

// The macros GitHub refuses, by the same substring test its element applies.
export function refusedMacrosIn(source) {
  return REFUSED_MACROS.filter(
    (name) => source.includes(`\\${name}`) || source.includes(`\\$${name}`) || source.includes(`\\\${${name}`),
  );
}

export function braceCount(source) {
  return source.split('{').length;
}

// The element re-reads its text as HTML before MathJax sees it: entities decode, and a
// `<` that opens a tag swallows the rest, up to the next `>`. Inline formulas survive
// because GitHub escapes them twice; a ```math fence is escaped once, so there `a<b`
// reaches MathJax as `a`. Returns { tex, swallowed }.
export function rereadAsHtml(source) {
  let swallowed = null;
  const text = source.replace(/<!--[\s\S]*?(?:-->|$)|<[/]?[A-Za-z][^>]*(?:>|$)|<[!?][^>]*(?:>|$)/g, (tag) => {
    swallowed = swallowed ?? tag;
    return '';
  });
  return { tex: decodeEntities(text), swallowed };
}

// What MathJax finally parses: the re-read text, trimmed, without its delimiters.
export function texFor(source) {
  return rereadAsHtml(source).tex.trim().replace(/^\${1,2}|\${1,2}$/g, '');
}

let texDocument = null;
let compiledState = 0;

export function packagesInUse() {
  const { require } = loadDependencies();
  const { AllPackages } = require('mathjax-full/js/input/tex/AllPackages.js');
  return AllPackages.filter((name) => !REMOVED_PACKAGES.includes(name) && name !== 'noundefined');
}

function texConverter() {
  if (texDocument) {
    return texDocument;
  }
  const { require } = loadDependencies();
  const { mathjax } = require('mathjax-full/js/mathjax.js');
  const { TeX } = require('mathjax-full/js/input/tex.js');
  const { liteAdaptor } = require('mathjax-full/js/adaptors/liteAdaptor.js');
  const { RegisterHTMLHandler } = require('mathjax-full/js/handlers/html.js');
  // No picture is drawn; bussproofs alone refuses to load without an output jax.
  const { SVG } = require('mathjax-full/js/output/svg.js');

  RegisterHTMLHandler(liteAdaptor());
  const tex = new TeX({
    packages: packagesInUse(),
    maxMacros: MAX_MACROS,
    formatError: (_jax, error) => {
      throw error;
    },
  });
  compiledState = require('mathjax-full/js/core/MathItem.js').STATE.COMPILED;
  texDocument = mathjax.document('', { InputJax: tex, OutputJax: new SVG({ fontCache: 'none' }) });
  return texDocument;
}

// null when GitHub renders it, else the reason it does not.
export function renderError(formula, pageBraces = 0) {
  const refused = refusedMacrosIn(formula.source);
  if (refused.length) {
    return `The following macros are not allowed: ${refused.join(', ')}`;
  }
  const braces = braceCount(formula.source);
  if (braces > BRACE_LIMIT_FORMULA || pageBraces + braces > BRACE_LIMIT_PAGE) {
    return `Unable to render expression: over ${BRACE_LIMIT_FORMULA} braces in it or ${BRACE_LIMIT_PAGE} on the page`;
  }
  const { swallowed } = rereadAsHtml(formula.source);
  if (swallowed) {
    return `GitHub reads "${swallowed.slice(0, 20)}" as an HTML tag and drops it; write \\lt or put a space after <`;
  }
  try {
    texConverter().convert(texFor(formula.source), { display: formula.display, end: compiledState });
    return null;
  } catch (error) {
    return error.message || String(error);
  }
}

// ---------------------------------------------------------------------------------------
// Checking a document.

// Every problem in one Markdown document: [{ line, delimiter, source, message }].
export function checkDocument(text) {
  const { formulas, blocks, opaque } = extractFormulas(text);
  const problems = [];
  let pageBraces = 0;
  for (const formula of formulas) {
    const message = renderError(formula, pageBraces);
    pageBraces += braceCount(formula.source);
    if (message) {
      problems.push({ ...formula, message });
    }
  }
  problems.push(...writingProblems(text, formulas, blocks, opaque));
  return { formulas, problems: problems.sort((a, b) => a.line - b.line) };
}

// ---------------------------------------------------------------------------------------
// Command line.

async function readStdin() {
  const chunks = [];
  for await (const chunk of process.stdin) {
    chunks.push(chunk);
  }
  return Buffer.concat(chunks).toString('utf8');
}

async function trackedMarkdownFiles(pathspecs, cwd) {
  const { stdout } = await execFileAsync('git', ['ls-files', '-z', '--', ...(pathspecs.length ? pathspecs : ['*.md'])], {
    cwd,
  });
  return stdout
    .split('\0')
    .filter(Boolean)
    .filter((f) => f.endsWith('.md'))
    .map((f) => resolve(cwd, f))
    .sort();
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

  const missing = dependencyProblem();
  if (missing) {
    console.error(`${missing} - cannot validate formulas.`);
    console.error(`Install the pinned versions with:  ${INSTALL_HINT}`);
    process.exitCode = 1;
    return;
  }

  const targets = argv.filter((a, i) => a !== '--verbose' && !(stdinIndex !== -1 && (i === stdinIndex || i === stdinIndex + 1)));
  const documents = [];
  if (stdinLabel) {
    documents.push({ label: stdinLabel, text: await readStdin() });
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
      documents.push({ label: relative(process.cwd(), file), text: await readFile(file, 'utf8') });
    }
  }

  let total = 0;
  let failed = 0;
  for (const { label, text } of documents) {
    const { formulas, problems } = checkDocument(text);
    total += formulas.length;
    failed += problems.length;
    if (verbose && formulas.length && !problems.length) {
      console.log(`  PASS  ${label}: ${formulas.length} formula(s)`);
    }
    for (const p of problems) {
      console.log(`  FAIL  ${label}:${p.line} (${p.delimiter})`);
      console.log(`        ${decodeEntities(p.source).split('\n').join('\n        ')}`);
      console.log(`        ${p.message}`);
      console.log('');
    }
  }

  if (!total && !failed) {
    console.log('No formulas found.');
    return;
  }
  console.log(`${total} formula(s) checked, ${failed} problem(s).`);
  process.exitCode = failed ? 1 : 0;
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  main().catch((error) => {
    console.error(error);
    process.exitCode = 1;
  });
}
