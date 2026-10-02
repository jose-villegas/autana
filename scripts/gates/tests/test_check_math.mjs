// Unit tests for check-math.mjs. The delimiter cases are what GitHub's file view did
// with each one, measured through a secret gist; the rendering cases are what GitHub's
// own <math-renderer> did with each formula. Needs mathjax-full@3.2.0 and
// markdown-it@14.1.0; without them every test skips, except under CI, where it fails.
import test from 'node:test';
import assert from 'node:assert/strict';

import {
  checkDocument,
  consumedEscape,
  extractFormulas,
  loadDependencies,
  packagesInUse,
  refusedMacrosIn,
  renderError,
  scanDollars,
  BRACE_LIMIT_PAGE,
} from '../check-math.mjs';

const ready = Boolean(loadDependencies());
const skip = ready || process.env.CI ? false : 'mathjax-full and markdown-it are not installed';

const formulasIn = (markdown) => extractFormulas(markdown).formulas.map((f) => f.source);
const problemsIn = (markdown) => checkDocument(markdown).problems;

test('GitHub finds inline maths only between delimiters it accepts', { skip }, () => {
  const cases = [
    ['q $x$ end', 1],
    ['qa$x$ end', 0],
    ['q $x$a end', 0],
    ['q $x$_ end', 0],
    ['q ($x$) end', 1],
    ['q "$x$" end', 0],
    ['q $x$. end', 1],
    ['q $ x$ end', 0],
    ['q $x $. end', 1],
    ['q $x $ y', 0],
    ['cost $5 and $6 here', 0],
    ['q $a$b$ end', 0],
    ['q $$x$$ end', 1],
    ['q $$x$$y end', 0],
    ['q $$ x $$ end', 0],
    ['q $a +\nb$ end', 1],
    ['q *$y$* end', 0],
    ['q **$y$** end', 1],
    ['q [a $y$ b](http://a) end', 0],
    ['q <b>$y$</b> end', 0],
    ['q <i>$y$</i> end', 1],
    ['q `$x$` end', 0],
    ['q $`a_b`$ end', 1],
    ['q a$`x`$ end', 0],
    ['$$\ny_1\n$$', 1],
  ];
  for (const [markdown, count] of cases) {
    assert.equal(formulasIn(markdown).length, count, JSON.stringify(markdown));
  }
});

test('MathJax receives the text after Markdown, escapes and all', { skip }, () => {
  assert.deepEqual(formulasIn('a $\\{x\\}$ b'), ['${x}$']);
  assert.deepEqual(formulasIn('a $`\\{x\\}`$ b'), ['$\\{x\\}$']);
  assert.deepEqual(formulasIn('```math\n\\{x\\}\n```'), ['$$\\{x\\}$$']);
});

test('an emphasis pair across two formulas hides both from MathJax', { skip }, () => {
  const line = '$\\mathcal{E}_{\\Delta E}$ below names the first term and $\\mathcal{E}_{\\mathcal{L}}$ the second.';
  assert.deepEqual(formulasIn(line), []);
  const problems = problemsIn(`Intro.\n\n${line}\n`);
  assert.deepEqual(
    problems.map((p) => p.line),
    [3, 3],
  );
  assert.match(problems[0].message, /plain text/);
});

test('the same formulas written as $`...`$ reach MathJax and pass', { skip }, () => {
  const line = '$`\\mathcal{E}_{\\Delta E}`$ below names the first term and $`\\mathcal{E}_{\\mathcal{L}}`$ the second.';
  assert.equal(formulasIn(line).length, 2);
  assert.deepEqual(problemsIn(line), []);
});

test('a backslash escape Markdown eats inside inline maths is reported', { skip }, () => {
  assert.equal(consumedEscape('0.2126\\,r'), '\\,');
  assert.equal(consumedEscape('a \\\\ b'), '\\\\');
  assert.equal(consumedEscape('a\\_b'), null);
  assert.equal(consumedEscape('\\mathrm{x}'), null);
  const problems = problemsIn('Luma is $y = 0.2126\\,r + 0.7152\\,g$ here.');
  assert.equal(problems.length, 1);
  assert.match(problems[0].message, /turns `\\,` into `,`/);
  assert.deepEqual(problemsIn('```math\ny = 0.2126\\,r\n```'), []);
});

test('a macro GitHub refuses fails, by its own substring test', { skip }, () => {
  assert.deepEqual(refusedMacrosIn('$\\operatorname{trunc}$'), ['operatorname']);
  assert.deepEqual(refusedMacrosIn('$\\hphantom{x}$'), ['hphantom']);
  assert.deepEqual(refusedMacrosIn('$\\mathrm{trunc}$'), []);
  const problems = problemsIn('```math\n\\text{pixel}_x = \\operatorname{trunc}(x)\n```');
  assert.equal(problems.length, 1);
  assert.match(problems[0].message, /not allowed: operatorname/);
  assert.equal(problems[0].line, 2);
});

test('TeX errors fail and valid TeX passes', { skip }, () => {
  const error = (tex) => renderError({ source: `$${tex}$`, display: false });
  assert.match(error('\\foo x'), /Undefined control sequence \\foo/);
  assert.match(error('\\bbox[red]{x}'), /Undefined control sequence/);
  assert.match(error('x^{'), /Extra open brace/);
  assert.match(error('\\frac{1}'), /Missing argument/);
  assert.match(error('a &amp; b'), /Misplaced &/);
  for (const tex of ['\\mathrm{trunc}', '\\boldsymbol{x}', '\\cancel{x}', '\\ce{H2O}', '\\lVert x\\rVert', '\\tfrac12']) {
    assert.equal(error(tex), null, tex);
  }
});

test('GitHub\'s package set is AllPackages minus its removals, noundefined dropped', { skip }, () => {
  const github = [
    'base', 'ams', 'amscd', 'boldsymbol', 'braket', 'bussproofs', 'cancel', 'cases', 'centernot', 'color',
    'empheq', 'enclose', 'extpfeil', 'gensymb', 'mathtools', 'mhchem', 'noundefined', 'upgreek', 'unicode',
    'verb', 'configmacros', 'tagformat', 'textcomp', 'textmacros',
  ];
  assert.deepEqual(packagesInUse(), github.filter((name) => name !== 'noundefined'));
});

test('a < that opens a tag empties a ```math block but not inline maths', { skip }, () => {
  assert.match(problemsIn('```math\na<b\n```')[0].message, /HTML tag/);
  assert.deepEqual(problemsIn('```math\na < b\n```'), []);
  assert.deepEqual(problemsIn('q $a<b$ end'), []);
});

test('the page-wide brace budget stops the formulas past it', { skip }, () => {
  const formula = `$${'{x}'.repeat(400)}$`;
  const page = Array.from({ length: 6 }, () => `q ${formula} end`).join('\n\n');
  const problems = problemsIn(page);
  assert.ok(problems.length > 0);
  assert.ok(problems.every((p) => /braces/.test(p.message)));
  assert.equal(problems[0].line, 1 + 2 * Math.floor(BRACE_LIMIT_PAGE / 401));
});

test('formula lines point at the file line', { skip }, () => {
  const text = ['# Title', '', 'one $a$ two', 'three **b** $c$', '', '```math', 'x', '```'].join('\n');
  assert.deepEqual(
    extractFormulas(text).formulas.map((f) => f.line),
    [3, 4, 7],
  );
});

test('a $$ paragraph reaches MathJax as GitHub writes it back, and a change is reported', { skip }, () => {
  assert.deepEqual(formulasIn('$$\na \\\\ b *x* c\n$$'), ['$$\na \\ b _x_ c\n$$']);
  assert.match(problemsIn('$$\na \\\\ b \\{ c \\}\n$$')[0].message, /```math fence/);
  assert.deepEqual(problemsIn('$$\np_1 \\\\\nq_2\n$$'), []);
});

test('a backslash ending a line of display maths is doubled', { skip }, () => {
  assert.deepEqual(formulasIn('```math\na\\\\\nb\\\nc\n```'), ['$$a\\\\\\\nb\\\\\nc$$']);
});

test('scanDollars returns content and offsets', () => {
  assert.deepEqual(scanDollars('a $x$ b'), [{ start: 2, end: 5, delimiter: '$', content: 'x' }]);
});
