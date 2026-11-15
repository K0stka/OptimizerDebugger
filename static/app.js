'use strict';

const $ = (id) => document.getElementById(id);

const els = {
  input: $('input'),
  inputHighlight: $('input-highlight'),
  gutter: $('gutter'),
  output: $('output'),
  preset: $('preset-select'),
  auto: $('auto-chk'),
  optimizeBtn: $('optimize-btn'),
  rebuildBtn: $('rebuild-btn'),
  statsLine: $('stats-line'),
  diag: $('diag'),
  diagTitle: $('diag-title'),
  diagBody: $('diag-body'),
  diagClose: $('diag-close'),
};

const STORAGE_KEY = 'optimizer-debugger:v1';
let saveTimer = null;
let runTimer = null;
let busy = false;
let lastGood = null; // { input, output } of the last successful optimize
let currentMarks = null; // { input: Map, output: Map } diff hunk marks
let currentRows = null; // Paired row sequence for the output pane

////////////////////////////////////////////////////////////////////////////////
// Local storage persistence
////////////////////////////////////////////////////////////////////////////////

function saveState() {
  const state = {
    input: els.input.value,
    preset: els.preset.value,
    auto: els.auto.checked,
  };
  try {
    localStorage.setItem(STORAGE_KEY, JSON.stringify(state));
  } catch (err) {
    /* Storage may be unavailable (private mode) - persistence is best effort */
  }
}

function saveStateDebounced() {
  clearTimeout(saveTimer);
  saveTimer = setTimeout(saveState, 300);
}

function restoreState() {
  try {
    const raw = localStorage.getItem(STORAGE_KEY);
    if (!raw) return false;
    const state = JSON.parse(raw);
    if (typeof state.input === 'string') els.input.value = state.input;
    if (
      typeof state.preset === 'string' &&
      [...els.preset.options].some((o) => o.value === state.preset)
    )
      els.preset.value = state.preset;
    els.auto.checked = state.auto === true;
    return typeof state.input === 'string';
  } catch (err) {
    return false;
  }
}

////////////////////////////////////////////////////////////////////////////////
// Editor gutter + input highlighting
////////////////////////////////////////////////////////////////////////////////

function updateGutter() {
  const lines = els.input.value.split('\n').length;
  let text = '';
  for (let i = 1; i <= lines; i++) text += i + '\n';
  els.gutter.textContent = text;
  els.gutter.scrollTop = els.input.scrollTop;
}

// The textarea holds transparent text for interaction; this layer draws the
// highlighted copy behind it and is scroll-synced with the textarea.
function renderInputHighlight() {
  els.inputHighlight.innerHTML = highlight(els.input.value) + '\u200b';
  syncInputHighlightScroll();
}

function syncInputHighlightScroll() {
  els.inputHighlight.scrollTop = els.input.scrollTop;
  els.inputHighlight.scrollLeft = els.input.scrollLeft;
}

////////////////////////////////////////////////////////////////////////////////
// Syntax highlighting for the IR text
////////////////////////////////////////////////////////////////////////////////

const ESCAPES = { '&': '&amp;', '<': '&lt;', '>': '&gt;' };
const escapeHtml = (text) => text.replace(/[&<>]/g, (c) => ESCAPES[c]);

const HIGHLIGHT_RE = new RegExp(
  [
    '(#.*$)', // 1 comment
    '("(?:[^"\\\\]|\\\\.)*")', // 2 string literal
    '(^\\s*\\.[a-zA-Z]+)', // 3 directive
    '(@[A-Za-z0-9_]+)', // 4 slot
    '\\b(v\\d+)\\b', // 5 value
    '\\b(add|sub|mul|idiv|div|floor|eq|ne|lt|le|gt|ge|not|int2double|concat|' +
      'load|store|call|jump|branch|ret|exit)\\b', // 6 opcode
    '\\b(int|double|string|bool|unit)\\b', // 7 type
    '(\\b\\d+(?:\\.\\d+)?(?:e[+-]?\\d+)?\\b)', // 8 number
  ].join('|'),
  'gm'
);

// Match on the RAW text and escape during assembly: matching on pre-escaped
// text would let e.g. the `gt` opcode split the `&gt;` entity in half.
function highlight(source) {
  let out = '';
  let last = 0;
  for (const match of source.matchAll(HIGHLIGHT_RE)) {
    const [full, comment, str, dir, slot, value, op, type] = match;
    out += escapeHtml(source.slice(last, match.index));
    const cls = comment ? 't-comment'
      : str ? 't-string'
      : dir ? 't-directive'
      : slot ? 't-slot'
      : value ? 't-value'
      : op ? 't-opcode'
      : type ? 't-type'
      : 't-number';
    out += `<span class="${cls}">${escapeHtml(full)}</span>`;
    last = match.index + full.length;
  }
  out += escapeHtml(source.slice(last));
  return out;
}

function renderOutput(text) {
  if (text == null) {
    els.output.innerHTML = '<span class="muted">— no output —</span>';
  } else if (text === '') {
    els.output.innerHTML = '<span class="muted">— empty —</span>';
  } else {
    els.output.innerHTML = highlight(text);
  }
}

////////////////////////////////////////////////////////////////////////////////
// Diff marks: git-style line diff, aligned row-for-row across both panes
////////////////////////////////////////////////////////////////////////////////

/**
 * Strips a trailing comment from a line (string-aware, so '#' inside string
 * literals survives) and trims trailing whitespace. Used to compare lines
 * ignoring comments: a line whose only difference is its comment counts as
 * unchanged.
 */
function stripCommentSuffix(line) {
  let inString = false;
  for (let i = 0; i < line.length; i++) {
    const c = line[i];
    if (inString) {
      if (c === '\\') i++;
      else if (c === '"') inString = false;
    } else if (c === '"') {
      inString = true;
    } else if (c === '#') {
      return line.slice(0, i).trimEnd();
    }
  }
  return line.trimEnd();
}

/**
 * Wraps every raw line of the text as a diffable entry. ALL lines take part
 * in the diff (comments and blanks occupy vertical space in the input pane,
 * so the output must mirror them), but the comparison ignores trailing
 * comments. `content` marks real code lines - only those get colored marks.
 */
function makeLineEntries(text) {
  return text.split('\n').map((raw, idx) => {
    const line = raw.replace(/\r$/, '');
    const trimmed = line.trim();
    return {
      text: line,
      idx,
      cmp: stripCommentSuffix(line),
      content: trimmed !== '' && !trimmed.startsWith('#'),
    };
  });
}

/**
 * Classic LCS diff over line entries: same/del/add rows in output order.
 */
function diffLines(a, b) {
  const n = a.length;
  const m = b.length;
  // dp[i][j] = LCS length of a[i..] and b[j..]
  const dp = Array.from({ length: n + 1 }, () => new Uint32Array(m + 1));
  for (let i = n - 1; i >= 0; i--) {
    for (let j = m - 1; j >= 0; j--) {
      dp[i][j] =
        a[i].cmp === b[j].cmp
          ? dp[i + 1][j + 1] + 1
          : Math.max(dp[i + 1][j], dp[i][j + 1]);
    }
  }

  const rows = [];
  let i = 0;
  let j = 0;
  while (i < n && j < m) {
    if (a[i].cmp === b[j].cmp) {
      rows.push({ kind: 'same', l: a[i], r: b[j] });
      i++;
      j++;
    } else if (dp[i + 1][j] >= dp[i][j + 1]) {
      rows.push({ kind: 'del', l: a[i] });
      i++;
    } else {
      rows.push({ kind: 'add', r: b[j] });
      j++;
    }
  }
  while (i < n) rows.push({ kind: 'del', l: a[i++] });
  while (j < m) rows.push({ kind: 'add', r: b[j++] });
  return rows;
}

/**
 * Zips each change hunk (a run of del/add rows) into single aligned rows: an
 * old line paired with its replacement becomes one 'chg' row, extra input
 * lines become 'del' rows and extra output lines become 'add' rows. This
 * keeps both panes advancing row-for-row.
 */
function pairChanges(rows) {
  const out = [];
  let hunk = [];

  const flush = () => {
    const dels = hunk.filter((r) => r.kind === 'del').map((r) => r.l);
    const adds = hunk.filter((r) => r.kind === 'add').map((r) => r.r);
    const count = Math.max(dels.length, adds.length);
    for (let k = 0; k < count; k++) {
      out.push({
        kind: dels[k] === undefined ? 'add' : adds[k] === undefined ? 'del' : 'chg',
        l: dels[k] ?? null,
        r: adds[k] ?? null,
      });
    }
    hunk = [];
  };

  for (const row of rows) {
    if (row.kind === 'same') {
      flush();
      out.push(row);
    } else {
      hunk.push(row);
    }
  }
  flush();
  return out;
}

/**
 * Computes the diff layout over the raw lines: per-line hunk marks for both
 * panes plus the paired row sequence the output pane renders. Rows keep both
 * panes advancing together: input-only lines ('del') render as blank fillers
 * in the output, output-only blank lines are skipped, and only genuinely new
 * output content lines cause drift (the optimizer never adds any).
 */
function computeDiffLayout(beforeText, afterText) {
  const before = makeLineEntries(beforeText);
  const after = makeLineEntries(afterText);
  const rows = pairChanges(diffLines(before, after));

  const marks = { input: new Map(), output: new Map() };
  let hunk = 0;
  let inHunk = false;

  for (const row of rows) {
    if (row.kind === 'same') {
      inHunk = false;
      continue;
    }
    if (!inHunk) {
      hunk++;
      inHunk = true;
    }
    row.hunk = hunk;
    // Only content lines carry marks; comments and blanks just take up space
    if (row.l && row.l.content) marks.input.set(row.l.idx, hunk);
    if (row.r && row.r.content) marks.output.set(row.r.idx, hunk);
  }

  return { marks, rows };
}

/**
 * Draws the input text; lines removed/replaced by the optimizer carry a red
 * mark with their hunk id.
 */
function renderInputHighlight() {
  const lines = els.input.value.split('\n');
  const html = lines
    .map((line, i) => {
      const hunk = currentMarks ? currentMarks.input.get(i) : null;
      const cls = hunk != null ? ' mark-del' : '';
      const h = hunk != null ? ` data-h="${hunk}"` : '';
      return `<div class="hl-line${cls}"${h}>${highlight(line) || '\u200b'}</div>`;
    })
    .join('');
  els.inputHighlight.innerHTML = html;
  syncInputHighlightScroll();
}

/**
 * Draws the optimized output from the paired row sequence: input-only lines
 * become blank fillers (keeping the panes row-for-row aligned in both
 * layouts, which is what makes scroll sync possible), output-only blank
 * lines are skipped, and new/replaced content lines carry a green mark.
 */
function renderOutputPane() {
  if (!lastGood || !currentRows) return;
  const html = currentRows
    .map((row) => {
      if (row.kind === 'del') return '<div class="ol-line filler">\u200b</div>';
      // An output-only blank line is pure formatting - skip it so the panes
      // stay aligned
      if (row.kind === 'add' && !row.r.content) return '';
      const marked = row.kind === 'add' || row.kind === 'chg';
      const cls = marked ? ' mark-add' : '';
      const h = marked ? ` data-h="${row.hunk}"` : '';
      return `<div class="ol-line${cls}"${h}>${highlight(row.r.text) || '\u200b'}</div>`;
    })
    .join('');
  els.output.innerHTML = html;
}

/**
 * Surfaces only actionable problems; routine state is visible from the
 * results themselves (stats line + diagnostics drawer).
 */
async function refreshStatus() {
  try {
    const status = await api('/api/status');
    if (!status.repo_ok)
      showDiag(
        'Repository not found',
        `${status.repo}\nSet 'repo' in config.txt and restart the server.`
      );
  } catch (err) {
    showDiag('Server unreachable', String(err.message || err));
  }
}

////////////////////////////////////////////////////////////////////////////////
// API helpers
////////////////////////////////////////////////////////////////////////////////

async function api(path, options) {
  const response = await fetch(path, options);
  let data = null;
  try {
    data = await response.json();
  } catch (err) {
    throw new Error(`server returned ${response.status} with non-JSON body`);
  }
  if (!response.ok) {
    throw new Error(data && data.error ? data.error : `HTTP ${response.status}`);
  }
  return data;
}

////////////////////////////////////////////////////////////////////////////////
// Diagnostics drawer
////////////////////////////////////////////////////////////////////////////////

function showDiag(title, text) {
  if (!text || !text.trim()) {
    hideDiag();
    return;
  }
  els.diagTitle.textContent = title;
  els.diagBody.textContent = text.trimEnd();
  els.diag.classList.remove('hidden');
}

function hideDiag() {
  els.diag.classList.add('hidden');
}

////////////////////////////////////////////////////////////////////////////////
// Core actions
////////////////////////////////////////////////////////////////////////////////

async function optimize() {
  if (busy) return;
  busy = true;
  els.optimizeBtn.disabled = true;
  const buttonLabel = els.optimizeBtn.innerHTML;
  els.optimizeBtn.textContent = 'Optimizing…';

  try {
    // Diff against exactly what was sent (the user may keep typing meanwhile)
    const sent = els.input.value;
    const data = await api('/api/optimize?flavor=release', {
      method: 'POST',
      headers: { 'Content-Type': 'text/plain' },
      body: sent,
    });

    // A hot-reload rebuild may have happened before the run
    let rebuiltNote = '';
    if (data.build && data.build.performed) {
      if (data.build.ok) {
        rebuiltNote = `rebuilt in ${Math.round(data.build.ms)} ms · `;
      } else {
        lastGood = null;
        currentMarks = null;
        currentRows = null;
        renderInputHighlight();
        renderOutput(null);
        els.statsLine.textContent = '';
        showDiag('Build failed — clang output', data.build.log);
        return;
      }
    }

    const result = data.result;
    if (result && result.ok) {
      lastGood = { input: sent, output: result.ir };
      const layout = computeDiffLayout(sent, result.ir);
      currentMarks = layout.marks;
      currentRows = layout.rows;
      renderInputHighlight();
      renderOutputPane();
      const s = result.stats;
      const delta = s.instructions_after - s.instructions_before;
      els.statsLine.textContent =
        rebuiltNote +
        `instructions ${s.instructions_before} → ${s.instructions_after}` +
        (delta !== 0 ? ` (${delta > 0 ? '+' : ''}${delta})` : '');
      showDiag(
        'Optimizer stderr',
        (data.stderr || '') + (result.error ? '\n' + result.error : '')
      );
    } else {
      lastGood = null;
      currentMarks = null;
      currentRows = null;
      renderInputHighlight();
      renderOutput(null);
      els.statsLine.textContent = '';
      const detail = result && result.error ? result.error : 'unknown error';
      showDiag('Error', (data.stderr ? data.stderr + '\n' : '') + detail);
    }
  } catch (err) {
    showDiag('Request failed', String(err.message || err));
  } finally {
    busy = false;
    els.optimizeBtn.disabled = false;
    els.optimizeBtn.innerHTML = buttonLabel;
  }
}

async function rebuild() {
  if (busy) return;
  busy = true;
  els.rebuildBtn.disabled = true;
  const buttonLabel = els.rebuildBtn.textContent;
  els.rebuildBtn.textContent = 'Rebuilding…';

  try {
    const data = await api('/api/rebuild?flavor=release', {
      method: 'POST',
    });
    showDiag('Rebuild', data.ok ? data.log : 'REBUILD FAILED:\n' + data.log);
  } catch (err) {
    showDiag('Rebuild failed', String(err.message || err));
  } finally {
    busy = false;
    els.rebuildBtn.disabled = false;
    els.rebuildBtn.textContent = buttonLabel;
    optimize();
  }
}

async function loadPresets() {
  try {
    const names = await api('/api/presets');
    for (const name of names) {
      const option = document.createElement('option');
      option.value = name;
      option.textContent = name.replace(/^\d+_/, '').replace(/_/g, ' ');
      els.preset.appendChild(option);
    }
  } catch (err) {
    /* Presets are optional; leave the dropdown empty */
  }
}

async function selectPreset(name) {
  if (!name) return;
  try {
    const response = await fetch('/presets/' + encodeURIComponent(name));
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    els.input.value = await response.text();
    currentMarks = null;
    currentRows = null;
    updateGutter();
    renderInputHighlight();
    saveState();
    if (els.auto.checked) optimize();
  } catch (err) {
    showDiag('Preset failed to load', String(err.message || err));
  }
}

////////////////////////////////////////////////////////////////////////////////
// Wiring
////////////////////////////////////////////////////////////////////////////////

els.input.addEventListener('input', () => {
  currentMarks = null; // Stale after an edit; recomputed on the next run
  currentRows = null;
  updateGutter();
  renderInputHighlight();
  saveStateDebounced();
  clearTimeout(runTimer);
  if (els.auto.checked) runTimer = setTimeout(optimize, 600);
});

// Keep the two panes' vertical scroll in lockstep (the alignment fillers
// make their line positions correspond 1:1 in both layouts)
els.input.addEventListener('scroll', () => {
  els.gutter.scrollTop = els.input.scrollTop;
  syncInputHighlightScroll();
  if (els.output.scrollTop !== els.input.scrollTop)
    els.output.scrollTop = els.input.scrollTop;
});

els.output.addEventListener('scroll', () => {
  if (els.input.scrollTop !== els.output.scrollTop)
    els.input.scrollTop = els.output.scrollTop;
});

// Linked highlighting: hovering a marked line brightens the whole hunk it
// belongs to, on both the input and the output side
let hoverHunk = null;

function setHunkHighlight(hunk) {
  if (hunk === hoverHunk) return;
  clearHunkHighlight();
  if (hunk != null) {
    hoverHunk = hunk;
    document
      .querySelectorAll(`[data-h="${hunk}"]`)
      .forEach((el) => el.classList.add('hl'));
  }
}

function clearHunkHighlight() {
  hoverHunk = null;
  document
    .querySelectorAll('.hl')
    .forEach((el) => el.classList.remove('hl'));
}

els.output.addEventListener('mouseover', (event) => {
  const line = event.target.closest('.ol-line');
  setHunkHighlight(line && line.dataset.h ? Number(line.dataset.h) : null);
});
els.output.addEventListener('mouseleave', clearHunkHighlight);

// The input is a textarea, so the hovered line is derived from the cursor;
// mousemove (not mouseover) is needed since the textarea is one element and
// mouseover would fire only on entry
function hoveredInputLine(event) {
  const style = getComputedStyle(els.input);
  const lineHeight = parseFloat(style.lineHeight) || 19.5;
  const padTop = parseFloat(style.paddingTop) || 0;
  const y = event.clientY - els.input.getBoundingClientRect().top +
    els.input.scrollTop - padTop;
  return Math.floor(y / lineHeight);
}

els.input.addEventListener('mousemove', (event) => {
  if (!currentMarks || currentMarks.input.size === 0) return;
  const hunk = currentMarks.input.get(hoveredInputLine(event));
  setHunkHighlight(hunk != null ? hunk : null);
});
els.input.addEventListener('mouseleave', clearHunkHighlight);

els.input.addEventListener('keydown', (event) => {
  if ((event.ctrlKey || event.metaKey) && event.key === 'Enter') {
    event.preventDefault();
    optimize();
  }
});

els.optimizeBtn.addEventListener('click', optimize);
els.rebuildBtn.addEventListener('click', rebuild);
els.diagClose.addEventListener('click', hideDiag);
els.preset.addEventListener('change', () => selectPreset(els.preset.value));
els.auto.addEventListener('change', () => {
  saveState();
  if (els.auto.checked) optimize();
});

(async function init() {
  // Presets must be populated before state restore so the picked preset can
  // be re-selected
  await loadPresets();
  const restored = restoreState();
  updateGutter();
  renderInputHighlight();
  await refreshStatus();

  // First visit: load the first preset so the page shows something alive
  if (!restored && els.preset.options.length > 1) {
    els.preset.selectedIndex = 1;
    await selectPreset(els.preset.value);
  } else if (els.auto.checked && els.input.value.trim()) {
    optimize();
  }
})();
