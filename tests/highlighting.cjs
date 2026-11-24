const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');

// Load the highlighter without starting the page's network and event wiring.
const source = fs.readFileSync(path.join(__dirname, '../static/app.js'), 'utf8');
const { highlight, functionScopes } = vm.runInNewContext(
  source.slice(0, source.indexOf('\nfunction renderOutput(')) +
    '\n({ highlight, functionScopes })',
  { document: { getElementById: () => ({}) } }
);
const refs = (line, scope = 'main') =>
  [...highlight(line, scope).matchAll(/data-ref="([^"]+)"/g)]
    .map((match) => decodeURIComponent(match[1]));

assert.deepEqual(refs('v1 : int = add v0, v0'),
  ['value:main:v1', 'value:main:v0', 'value:main:v0']);
assert.deepEqual(refs('store @result v1'), ['slot:main:@result', 'value:main:v1']);
assert.notDeepEqual(refs('load @result', 'one'), refs('load @result', 'two'));
assert.notDeepEqual(refs('ret v0', 'one'), refs('ret v0', 'two'));
assert.deepEqual(refs('branch v3 ? 1 : 2'),
  ['value:main:v3', 'block:main:1', 'block:main:2']);
assert.deepEqual(refs('jump 2'), refs('.block 2'));
assert.deepEqual(refs('.entry 2'), refs('.block 2'));
assert.notDeepEqual(refs('jump 2', 'one'), refs('jump 2', 'two'));
assert.deepEqual(refs('v0 : int = add int 1, int 2'), ['value:main:v0']);
assert.deepEqual(refs('v0 : int = call helper, int 21'),
  ['value:main:v0', 'function:helper']);
assert.deepEqual(refs('call helper', 'main'), refs('.func helper : int', 'helper'));
assert.deepEqual(refs('.entrypoint helper'), ['function:helper']);
assert.deepEqual(refs('call print, string "v0 @x jump 2" # v1 @y'), []);
const builtins = ['print', 'readLine', 'readInt', 'readBoolean', 'readDouble',
  'toInt', 'toStr', 'strLength', 'subStr', 'ord', 'chr', 'readBool', 'strLen', 'strSub'];
for (const name of builtins) {
  assert.deepEqual(refs(`call ${name}, v0`), ['value:main:v0']);
  assert.deepEqual(refs(`v1 : int = call ${name}, v0`),
    ['value:main:v1', 'value:main:v0']);
  assert.ok(highlight(`call ${name}`).includes(`<span class="t-builtin">${name}</span>`));
}
assert.ok(highlight('call helper').includes('class="t-symbol" data-ref='));
assert.ok(!highlight('# call print').includes('t-builtin'));
assert.ok(!highlight('call helper, string "call print"').includes('t-builtin'));
assert.deepEqual(refs('# .func fake v0 @x'), []);
assert.equal(JSON.stringify(functionScopes('.func one : int\nret v0\n.func two : int\nret v0')),
  JSON.stringify(['one', 'one', 'two', 'two']));
assert.ok(highlight('call print, string "<script>&"').includes('&lt;script&gt;&amp;'));

const elements = new Map();
const page = vm.createContext({
  document: {
    getElementById: (id) => {
      if (!elements.has(id)) elements.set(id, { scrollTop: 0 });
      return elements.get(id);
    },
  },
  clearReferenceHighlight: () => {},
});
vm.runInContext(source.slice(0, source.indexOf("els.input.addEventListener('input'")), page);
vm.runInContext(String.raw`
  const before = '.func main : unit\n.block 0\n  v0 : int = add int 1, int 2\n  ret unit';
  const after = '.func main : unit\n.block 0\n  ret unit';
  lastGood = { input: before, output: after };
  currentRows = computeDiffLayout(before, after).rows;
  els.output.scrollTop = 20;
  renderOutputPane();
`, page);
assert.equal(elements.get('output-gutter').textContent, '1\n2\n\n3\n');
assert.equal(elements.get('output-gutter').scrollTop, 20);
assert.equal((elements.get('output').innerHTML.match(/class="ol-line/g) || []).length, 4);
vm.runInContext('renderOutput(null)', page);
assert.equal(elements.get('output-gutter').textContent, '');
console.log('Symbol highlighting and output line-number checks passed');
