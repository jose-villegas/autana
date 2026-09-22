// Unit tests for the pure parts of check-mermaid.mjs: fence extraction and
// the line-number rewrite. Nothing here touches mmdc, git, or the
// filesystem, so it runs with plain `node --test`, no mermaid-cli or
// Chrome required.
import test from 'node:test';
import assert from 'node:assert/strict';

import { extractBlocks, rewriteLineNumbers, winQuote, parseExtraMmdcArgs } from '../check-mermaid.mjs';

test('extracts a single fenced block with its start line', () => {
  const text = ['intro', '```mermaid', 'graph TD', 'A --> B', '```', 'outro'].join('\n');
  const blocks = extractBlocks(text);
  assert.equal(blocks.length, 1);
  assert.equal(blocks[0].line, 2);
  assert.equal(blocks[0].source, 'graph TD\nA --> B');
  assert.equal(blocks[0].unterminated, undefined);
});

test('extracts multiple blocks in file order', () => {
  const text = ['```mermaid', 'graph TD', '```', 'text between', '```mermaid', 'pie', '```'].join('\n');
  const blocks = extractBlocks(text);
  assert.equal(blocks.length, 2);
  assert.equal(blocks[0].line, 1);
  assert.equal(blocks[1].line, 5);
});

test('flags a fence with no closing fence', () => {
  const text = ['```mermaid', 'graph TD', 'A --> B'].join('\n');
  const blocks = extractBlocks(text);
  assert.equal(blocks.length, 1);
  assert.equal(blocks[0].unterminated, true);
});

test('ignores text outside a fence', () => {
  const blocks = extractBlocks('no diagram here at all');
  assert.equal(blocks.length, 0);
});

test('rewrites a parse-error line number onto the file line', () => {
  const message = rewriteLineNumbers('Parse error on line 3:\n...boom', 10);
  assert.equal(message, 'Parse error on file line 12 (line 3 of the block):\n...boom');
});

test('rewrites every occurrence, lexical errors included', () => {
  const message = rewriteLineNumbers('Lexical error on line 1: bad token', 1);
  assert.equal(message, 'Lexical error on file line 1 (line 1 of the block): bad token');
});

// windows-only bug: shell:true concatenates args with spaces instead of escaping them,
// so an unquoted path with a space splits into extra mmdc arguments. winQuote is the fix.
test('winQuote wraps an argument containing a space as one token', () => {
  const quoted = winQuote('C:\\Users\\ville\\mermaid test dir\\block-0.mmd');
  assert.equal(quoted, '"C:\\Users\\ville\\mermaid test dir\\block-0.mmd"');
});

test('winQuote escapes an embedded double quote', () => {
  assert.equal(winQuote('a"b'), '"a\\"b"');
});

test('parseExtraMmdcArgs splits on whitespace and drops empties', () => {
  assert.deepEqual(parseExtraMmdcArgs('-p  puppeteer-config.json'), ['-p', 'puppeteer-config.json']);
});

test('parseExtraMmdcArgs returns an empty array for unset or blank input', () => {
  assert.deepEqual(parseExtraMmdcArgs(undefined), []);
  assert.deepEqual(parseExtraMmdcArgs(''), []);
});
