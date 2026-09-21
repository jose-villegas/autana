// Unit tests for the pure parts of validate-mermaid.mjs: fence extraction and
// the line-number rewrite. Nothing here touches mmdc or the filesystem walk,
// so it runs with plain `node --test`, no mermaid-cli or Chrome required.
import test from 'node:test';
import assert from 'node:assert/strict';

import { extractBlocks, rewriteLineNumbers } from '../validate-mermaid.mjs';

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
