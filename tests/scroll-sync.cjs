const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const source = fs.readFileSync(path.join(__dirname, '../static/app.js'), 'utf8');
const { syncPaneScroll } = vm.runInNewContext(
  source.slice(0, source.indexOf('// Linked highlighting:')) + '\n({ syncPaneScroll })',
  { document: { getElementById: () => ({ addEventListener() {} }) } }
);
function pane(max) {
  let position = 0;
  return {
    get scrollTop() { return position; },
    set scrollTop(value) { position = Math.max(0, Math.min(max, value)); },
  };
}
const input = pane(1000);
const output = pane(100);
input.scrollTop = 300;
syncPaneScroll(input, output);
assert.equal(output.scrollTop, 100);
syncPaneScroll(output, input); // The browser emits the programmatic scroll event.
assert.equal(input.scrollTop, 300);
input.scrollTop = 400;
syncPaneScroll(input, output);
assert.equal(input.scrollTop, 400);
output.scrollTop = 50; // Scrolling the other pane should still work.
syncPaneScroll(output, input);
assert.equal(input.scrollTop, 50);
syncPaneScroll(input, output);
assert.equal(output.scrollTop, 50);

// A user scroll can arrive before the programmatic scroll event is delivered.
input.scrollTop = 70;
syncPaneScroll(input, output);
output.scrollTop = 80;
syncPaneScroll(output, input);
assert.equal(input.scrollTop, 80);
console.log('Scroll sync checks passed');
