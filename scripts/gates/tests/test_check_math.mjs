// Unit tests for check-math.mjs. The delimiter and rendering cases are GitHub's measured
// behaviour; check-math.mjs's header says how it was measured. Needs
// `npm ci --prefix scripts/gates`; without it every test skips, except under CI.
import test from 'node:test';
import assert from 'node:assert/strict';

import {
  checkDocument,
  dependencyProblem,
  extractFormulas,
  markdownHazard,
  packagesInUse,
  pinnedVersions,
  refusedMacrosIn,
  renderError,
  scanDollars,
  BRACE_LIMIT_PAGE,
} from '../check-math.mjs';

const ready = dependencyProblem() === null;
const skip = ready || process.env.CI ? false : 'run npm ci --prefix scripts/gates';

const formulasIn = (markdown) => extractFormulas(markdown).formulas.map((f) => f.source);
const problemsIn = (markdown) => checkDocument(markdown).problems;
const messagesIn = (markdown) => problemsIn(markdown).map((p) => p.message);

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
    ['q $a +\nb$ end', 1],
    ['q *$y$* end', 0],
    ['q **$y$** end', 1],
    ['q [a $y$ b](http://a) end', 0],
    ['q <b>$y$</b> end', 0],
    ['q <i>$y$</i> end', 1],
    ['q `$x$` end', 0],
    ['q $`a_b`$ end', 1],
    ['q a$`x`$ end', 0],
  ];
  for (const [markdown, count] of cases) {
    assert.equal(formulasIn(markdown).length, count, JSON.stringify(markdown));
  }
});

test('the $`...`$ form reaches MathJax untouched by Markdown', { skip }, () => {
  assert.deepEqual(formulasIn('a $`\\{x\\}_1 * 2`$ b'), ['$\\{x\\}_1 * 2$']);
  const line = '$`\\mathcal{E}_{\\Delta E}`$ below names the first term and $`\\mathcal{E}_{\\mathcal{L}}`$ the second.';
  assert.equal(formulasIn(line).length, 2);
  assert.deepEqual(problemsIn(line), []);
});

test('`_` or `*` inside $...$ is refused as written', { skip }, () => {
  const line = '$\\mathcal{E}_{\\Delta E}$ below names the first term and $\\mathcal{E}_{\\mathcal{L}}$ the second.';
  const problems = problemsIn(`Intro.\n\n${line}\n`);
  assert.deepEqual(problems.map((p) => p.line), [3, 3]);
  assert.match(problems[0].message, /`_` can pair up as Markdown emphasis.*\$`\.\.\.`\$/);
  assert.match(messagesIn('q $a * b$ end')[0], /`\*` can pair up/);
  assert.equal(markdownHazard('x^2'), null);
});

test('a backslash escape inside $...$ is refused as written', { skip }, () => {
  assert.match(markdownHazard('0.2126\\,r'), /`\\,` as an escape and hands MathJax `,`/);
  assert.match(markdownHazard('a \\\\ b'), /`\\\\`/);
  assert.match(markdownHazard('\\{x\\}'), /`\\\{`/);
  assert.equal(markdownHazard('\\mathrm{x} + \\alpha'), null);
  assert.match(messagesIn('Luma is $y = 0.2126\\,r$ here.')[0], /escape/);
  assert.deepEqual(problemsIn('```math\ny = 0.2126\\,r\n```'), []);
});

test('$$ is refused, in a paragraph, a quote or a list', { skip }, () => {
  for (const markdown of ['$$\nx + y\n$$', '> $$\n> x + y\n> $$', '- $$\n  x + y\n  $$', 'q $$x$$ end']) {
    const messages = messagesIn(markdown);
    assert.equal(messages.length, 1, JSON.stringify(markdown));
    assert.match(messages[0], /```math fence/);
  }
  assert.deepEqual(problemsIn('cost `$$` in code'), []);
});

test('a formula GitHub leaves as text in emphasis, a link or <b> is reported', { skip }, () => {
  for (const markdown of ['q *$x$* end', 'q [a $x$ b](http://a) end', 'q <b>$x$</b> end']) {
    assert.match(messagesIn(markdown)[0], /shows this as text/, markdown);
  }
  assert.deepEqual(problemsIn('q **$x$** and <i>$y$</i> end'), []);
});

test('a formula in a footnote is reported, one line or several', { skip }, () => {
  assert.deepEqual(formulasIn('Text[^1].\n\n[^1]: Note $x$\n  more.'), []);
  assert.match(messagesIn('Text[^1].\n\n[^1]: Note $x$\n  more.')[0], /footnote/);
  assert.match(messagesIn('Text[^1].\n\n[^1]: $\\foo$')[0], /footnotes after maths/);
});

test('a raw HTML block is scanned outside <b>, and its formulas rendered', { skip }, () => {
  const html = '<div>\nq $x$ and <b>$y$</b> end\n</div>';
  assert.deepEqual(formulasIn(html), ['$x$']);
  const problems = problemsIn('<div>\nq $\\foo$ end\n</div>');
  assert.equal(problems.length, 1);
  assert.match(problems[0].message, /Undefined control sequence \\foo/);
  assert.equal(problems[0].line, 2);
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

test("GitHub's package set is AllPackages minus its removals, noundefined dropped", { skip }, () => {
  const github = [
    'base', 'ams', 'amscd', 'boldsymbol', 'braket', 'bussproofs', 'cancel', 'cases', 'centernot', 'color',
    'empheq', 'enclose', 'extpfeil', 'gensymb', 'mathtools', 'mhchem', 'noundefined', 'upgreek', 'unicode',
    'verb', 'configmacros', 'tagformat', 'textcomp', 'textmacros',
  ];
  assert.deepEqual(packagesInUse(), github.filter((name) => name !== 'noundefined'));
});

test('a < that opens a tag empties a ```math fence but not inline maths', { skip }, () => {
  assert.match(messagesIn('```math\na<b\n```')[0], /HTML tag/);
  assert.deepEqual(problemsIn('```math\na < b\n```'), []);
  assert.deepEqual(problemsIn('q $a<b$ end'), []);
});

test('a backslash ending a line of a math fence is doubled', { skip }, () => {
  assert.deepEqual(formulasIn('```math\na\\\\\nb\\\nc\n```'), ['$$a\\\\\\\nb\\\\\nc$$']);
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

test('package.json pins exact versions, and those are installed', { skip }, () => {
  for (const version of Object.values(pinnedVersions())) {
    assert.match(version, /^\d+\.\d+\.\d+$/);
  }
  assert.equal(dependencyProblem(), null);
  assert.match(dependencyProblem({ 'markdown-it': '0.0.1' }), /markdown-it 14\.1\.0 is installed; package.json pins 0\.0\.1/);
  assert.match(dependencyProblem({ 'no-such-package': '1.0.0' }), /not installed/);
});

test('scanDollars returns content and offsets', () => {
  assert.deepEqual(scanDollars('a $x$ b'), [{ start: 2, end: 5, delimiter: '$', content: 'x' }]);
});
