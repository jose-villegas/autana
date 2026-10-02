#!/usr/bin/env node
// Validates every formula in the repo's tracked Markdown the way GitHub renders it:
// ```math fences, $$...$$, $...$ and $`...`$. GitHub shows a broken formula as raw
// text or a red error box, never a failed check; this is the only thing that catches it.
//
//   node scripts/gates/check-math.mjs                 # every git-tracked *.md
//   node scripts/gates/check-math.mjs path/to/file.md # one file or directory
//   node scripts/gates/check-math.mjs --verbose       # also list the formulas that passed
//   node scripts/gates/check-math.mjs --stdin <label> # one file's content from stdin,
//                                                      # reported under <label>
//
// Two halves, both copied from GitHub rather than guessed:
//
// Finding the formulas. GitHub parses the Markdown first and looks for `$` only in the
// text it produced, so backslash escapes are already gone (`\{` reaches MathJax as `{`)
// and a `_` or `*` pair that became emphasis splits a formula into plain text. Text
// inside emphasis, <b>, a link or code is never searched. The delimiter rules below were
// measured by rendering probe files in a secret gist's file view (the same renderer as a
// repository file) and are pinned by scripts/gates/tests/test_check_math.mjs.
//
// Rendering. GitHub's <math-renderer> element, chunk-lazy-element-math-renderer-*.js on
// github.githubassets.com, runs MathJax 3.2.0 with AllPackages minus REMOVED_PACKAGES,
// maxMacros 1000, refuses any expression naming a REFUSED_MACROS entry ("The following
// macros are not allowed: ..."), and refuses expressions past BRACE_LIMIT_* opening
// braces. To refresh: open a repository .md file with a formula on github.com, find that
// chunk in the network panel, and copy its `h=[...]` list, its `packages:{"[-]":[...]}`
// and its brace limits here. noundefined is dropped on top, so an undefined macro, which
// GitHub paints red, fails here instead of passing.
//
// Requires mathjax-full@3.2.0 and markdown-it@14.1.0, installed globally or beside this
// script; see docs/tools/Math-Formulas.md.

import { execFile, execSync } from 'node:child_process';
import { readFile } from 'node:fs/promises';
import { createRequire } from 'node:module';
import { join, relative, resolve } from 'node:path';
import { promisify } from 'node:util';
import { fileURLToPath, pathToFileURL } from 'node:url';

const execFileAsync = promisify(execFile);

export const MATHJAX_VERSION = '3.2.0';
export const MARKDOWN_IT_VERSION = '14.1.0';

export const REFUSED_MACROS = [
  'DeclareMathOperator', 'DeclarePairedDelimiters', 'renewtagform', 'newtagform', 'colorbox',
  'fcolorbox', 'hphantom', 'vphantom', 'phantom', 'operatorname', 'Newextarrow',
  'definecolor', 'mathchoice', 'unicode', 'mmlToken',
];
export const REMOVED_PACKAGES = ['noerrors', 'bbox', 'html', 'require', 'newcommand', 'action', 'colortbl'];
export const BRACE_LIMIT_FORMULA = 1000;
export const BRACE_LIMIT_PAGE = 2000;

// Inside these elements GitHub never looks for `$`.
const SKIPPED_TAGS = new Set(['em', 'b', 'a', 'code', 'pre']);
const WHITESPACE = /\s/;
const OPENER_PRECEDER = /[\s(]/;
const CLOSER_FORBIDDEN_FOLLOWER = /[A-Za-z0-9_`]/;

// ---------------------------------------------------------------------------------------
// Dependencies: beside this script first, then npm's global root, which is where CI and
// a developer's `npm install -g` put them.

let deps = null;

function globalNodeModules() {
  try {
    return execSync('npm root -g', { encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'] }).trim();
  } catch {
    return null;
  }
}

export function loadDependencies() {
  if (deps) {
    return deps;
  }
  const roots = [fileURLToPath(import.meta.url)];
  const globalRoot = globalNodeModules();
  if (globalRoot) {
    roots.push(join(globalRoot, 'noop.js'));
  }
  for (const root of roots) {
    const require = createRequire(root);
    try {
      const MarkdownIt = require('markdown-it');
      const version = require('mathjax-full/package.json').version;
      deps = { MarkdownIt, require, version };
      return deps;
    } catch {
      // try the next root
    }
  }
  return null;
}

export const INSTALL_HINT = `npm install -g mathjax-full@${MATHJAX_VERSION} markdown-it@${MARKDOWN_IT_VERSION}`;

// ---------------------------------------------------------------------------------------
// Finding the formulas.

// `$` and `$$` delimiters in one run of text that GitHub sees as a single text node.
// Returns [{ start, end, delimiter, content }] with offsets into `text`.
export function scanDollars(text) {
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
    if ((before && !OPENER_PRECEDER.test(before)) || first === undefined || WHITESPACE.test(first) || first === '$') {
      i += width;
      continue;
    }
    const close = text.indexOf('$', i + width);
    if (close === -1) {
      break;
    }
    if (isCloser(text, close, width)) {
      found.push({
        start: i,
        end: close + width,
        delimiter: '$'.repeat(width),
        content: text.slice(i + width, close),
      });
      i = close + width;
    } else {
      i += width;
    }
  }
  return found;
}

function isCloser(text, at, width) {
  if (width === 2 && text[at + 1] !== '$') {
    return false;
  }
  const after = text[at + width];
  if (after !== undefined && CLOSER_FORBIDDEN_FOLLOWER.test(after)) {
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

// A paragraph that is nothing but `$$ ... $$` is display maths even with whitespace
// after the opener, which inline `$$` refuses. GitHub writes such a paragraph back out
// after parsing it: escapes resolved, emphasis as `_`.
const DISPLAY_PARAGRAPH = /^\$\$([\s\S]*?)\$\$$/;

function displayParagraph(children) {
  const text = children
    .map((t) => {
      if (t.type === 'softbreak' || t.type === 'hardbreak') {
        return '\n';
      }
      if (t.type === 'em_open' || t.type === 'em_close') {
        return '_';
      }
      if (t.type === 'code_inline') {
        return `${t.markup}${t.content}${t.markup}`;
      }
      return t.type === 'text' || t.type === 'html_inline' ? t.content : t.markup || '';
    })
    .join('');
  const match = DISPLAY_PARAGRAPH.exec(text);
  return match && !match[1].includes('$$') ? text : null;
}

// GitHub doubles a backslash that ends a line of display maths, so `x\` reaches MathJax
// as `x\\`, a line break.
export function displayLines(text) {
  return text.replace(/\\$/gm, '\\\\');
}

function inlineFormulas(children, baseLine, isWholeParagraph) {
  const whole = isWholeParagraph ? displayParagraph(children) : null;
  if (whole) {
    return [{ line: baseLine, delimiter: '$$', source: displayLines(whole), display: true }];
  }
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
      return false;
    }
    const at = run.text.length - 1;
    const before = at > 0 ? run.text[at - 1] : '';
    if (before && !OPENER_PRECEDER.test(before)) {
      return false;
    }
    formulas.push({
      line: baseLine + run.line + lineOf(run.text, at),
      delimiter: '$`',
      source: `$${escapeHtml(token.content)}$`,
      display: false,
    });
    run.text = run.text.slice(0, at);
    next.content = next.content.slice(1);
    return true;
  };

  const visit = (token, next) => {
    if (token.type === 'em_open' || token.type === 'link_open' || token.type === 'em_close' || token.type === 'link_close') {
      skipDepth = Math.max(0, skipDepth + (token.nesting > 0 ? 1 : -1));
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
    lineInBlock += token.type === 'softbreak' || token.type === 'hardbreak' ? 1 : lineOf(token.content || '', (token.content || '').length);
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

const FOOTNOTE_DEFINITION = /^\[\^[^\]]+\]:/;

// Every formula GitHub would hand to MathJax, in document order:
// [{ line, delimiter, source, display }], `source` being the element's text with its
// delimiters, exactly what <math-renderer> receives. `blocks` are the top-level blocks
// whose source Markdown may change: [{ first, last }] as 1-based inclusive lines.
export function extractFormulas(text) {
  const tokens = markdownParser().parse(text, {});
  const formulas = [];
  const blocks = [];
  let line = 0;
  let paragraphOpen = false;
  let footnote = false;

  for (const token of tokens) {
    if (token.map) {
      line = token.map[0] + 1;
      if (token.level === 0 && token.nesting >= 0 && !['fence', 'code_block', 'html_block'].includes(token.type)) {
        blocks.push({ first: token.map[0] + 1, last: token.map[1] });
      }
    }
    if (token.type === 'fence' && token.info.trim().split(/\s+/)[0] === 'math') {
      formulas.push({ line: line + 1, delimiter: '```math', source: `$$${displayLines(token.content).trim()}$$`, display: true });
    } else if (token.type === 'html_block') {
      formulas.push(...htmlBlockFormulas(token.content, line));
    } else if (token.type === 'paragraph_open') {
      paragraphOpen = true;
      footnote = false;
    } else if (token.type === 'paragraph_close') {
      paragraphOpen = false;
    } else if (token.type === 'inline') {
      // GitHub renders footnote text after it has looked for maths.
      if (paragraphOpen && FOOTNOTE_DEFINITION.test(token.content)) {
        footnote = true;
      }
      if (!footnote) {
        formulas.push(...inlineFormulas(token.children || [], line, paragraphOpen));
      }
    }
  }
  formulas.sort((a, b) => a.line - b.line);
  return { formulas, blocks };
}

// The formulas as written in a block's source, before Markdown touches it, by the same
// delimiter rules: [{ line, content, codeForm }]. Code spans are blanked unless they sit
// between `$` and `$`.
export function writtenFormulas(source, firstLine) {
  const blanked = source.replace(/(`+)([^`][\s\S]*?)\1(?!`)/g, (span, _ticks, _body, offset) =>
    source[offset - 1] === '$' && source[offset + span.length] === '$' ? span : span.replace(/[^\n]/g, ' '),
  );
  return scanDollars(blanked).map((hit) => ({
    line: firstLine + lineOf(blanked, hit.start),
    delimiter: hit.delimiter,
    content: hit.content,
    codeForm: hit.delimiter === '$' && hit.content.length > 1 && hit.content.startsWith('`') && hit.content.endsWith('`'),
  }));
}

// A backslash escape Markdown consumes before MathJax runs, so `\,` reaches it as `,`,
// `\{` as `{` and `\\` as `\`. `\_` and `\*` are left out: their Markdown meaning, a
// literal `_` or `*`, is the TeX the author wanted.
export function consumedEscape(content) {
  for (let k = 0; k < content.length - 1; k++) {
    if (content[k] !== '\\') {
      continue;
    }
    const next = content[k + 1];
    if (/[!-/:-@[-`{-~]/.test(next) && next !== '_' && next !== '*') {
      return `\\${next}`;
    }
    k++;
  }
  return null;
}

// What Markdown did to the formulas as written: a formula that never reached MathJax
// (its `_` or `*` became emphasis, or an escape broke a delimiter) or one whose
// backslash Markdown ate.
export function markdownDamage(text, formulas, blocks) {
  const lines = text.split(/\r?\n/);
  const reached = new Map();
  for (const f of formulas) {
    reached.set(f.line, (reached.get(f.line) || 0) + 1);
  }
  const problems = [];
  for (const block of blocks) {
    const source = lines.slice(block.first - 1, block.last).join('\n');
    const display = formulas.find((f) => f.delimiter === '$$' && f.display && f.line === block.first);
    const changed = display && display.source !== source.trim();
    if (changed) {
      problems.push({
        line: block.first,
        delimiter: '$$',
        source: source.trim(),
        message:
          'Markdown changes this $$ block before MathJax sees it (escapes such as `\\\\` or `\\{`, ' +
          'or `*` read as emphasis). Write it as a ```math fence, which Markdown leaves alone.',
      });
    }
    for (const written of writtenFormulas(source, block.first)) {
      const shown = `${written.delimiter}${written.content}${written.delimiter}`;
      if (reached.get(written.line)) {
        reached.set(written.line, reached.get(written.line) - 1);
      } else {
        problems.push({
          line: written.line,
          delimiter: written.delimiter,
          source: shown,
          message:
            'GitHub shows this as plain text: Markdown read a `_` or `*` in it as emphasis, or it sits in ' +
            'emphasis, a link or after an escaped `$`. Write it as $`...`$, which Markdown leaves alone.',
        });
        continue;
      }
      const escape = written.codeForm || changed ? null : consumedEscape(written.content);
      if (escape) {
        problems.push({
          line: written.line,
          delimiter: written.delimiter,
          source: shown,
          message:
            `Markdown turns \`${escape}\` into \`${escape.slice(1)}\` before MathJax sees it. ` +
            'Write the formula as $`...`$, which Markdown leaves alone.',
        });
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
// because GitHub escapes them twice; ```math and $$ blocks are escaped once, so there
// `a<b` reaches MathJax as `a`. Returns { tex, swallowed }.
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
    maxMacros: 1000,
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
  const { formulas, blocks } = extractFormulas(text);
  const problems = [];
  let pageBraces = 0;
  for (const formula of formulas) {
    const message = renderError(formula, pageBraces);
    pageBraces += braceCount(formula.source);
    if (message) {
      problems.push({ ...formula, message });
    }
  }
  problems.push(...markdownDamage(text, formulas, blocks));
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

  const loaded = loadDependencies();
  if (!loaded) {
    console.error('mathjax-full and markdown-it not found - cannot validate formulas.');
    console.error(`Install them with:  ${INSTALL_HINT}`);
    process.exitCode = 1;
    return;
  }
  if (loaded.version !== MATHJAX_VERSION) {
    console.error(`mathjax-full ${loaded.version} found; GitHub runs ${MATHJAX_VERSION}.`);
    console.error(`Install the matching one with:  ${INSTALL_HINT}`);
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
    if (verbose && formulas.length) {
      console.log(`  PASS  ${label}: ${formulas.length - problems.length} formula(s)`);
    }
    for (const problem of problems) {
      console.log(`  FAIL  ${label}:${problem.line} (${problem.delimiter})`);
      console.log(`        ${decodeEntities(problem.source).split('\n').join('\n        ')}`);
      console.log(`        ${problem.message}`);
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
