'use strict';

const $ = (id) => document.getElementById(id);
const listEl = $('list');

// demo.js 检测到 ?demo=1 时会挂上 window.CR_DEMO，此时所有数据来自它，
// 不扫本机历史、不调拉起接口，纯粹用来截图和预览界面
const DEMO = !!window.CR_DEMO;

const S = {
  all: [],
  byId: new Map(),
  sel: new Set(),          // 选中的 cwd（小写）
  expanded: new Set(),     // 展开了对话列表的 cwd（小写）
  showAll: new Set(),      // 已经点开「更早的对话」的 cwd
  range: 720,
  settings: {
    maxAgeHours: 720,
    openTwo: false,
    useContinue: false,
    window: 0,
    exclude: [],
  },
  stats: {},
  roots: {},
  busy: false,
};

// 卡片展开后先显示这么多条，剩下的藏在「还有 N 个」后面
const INLINE_SESSIONS = 6;

function plog(msg) {
  fetch('/api/log', { method: 'POST', body: String(msg) }).catch(() => {});
}

function esc(s) {
  return String(s == null ? '' : s)
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;');
}

function relTime(ts) {
  if (!ts) return '未知';
  const d = Date.now() - ts;
  if (d < 60e3) return '刚刚';
  if (d < 3600e3) return Math.floor(d / 60e3) + ' 分钟前';
  if (d < 86400e3) return Math.floor(d / 3600e3) + ' 小时前';
  const days = Math.floor(d / 86400e3);
  if (days === 1) return '昨天';
  if (days < 30) return days + ' 天前';
  if (days < 365) return Math.floor(days / 30) + ' 个月前';
  return Math.floor(days / 365) + ' 年前';
}

// 一个目录下有很多对话，相对时间分不清先后，直接给到分钟
function shortTime(ts) {
  if (!ts) return '';
  const d = new Date(ts);
  const now = new Date();
  const hh = String(d.getHours()).padStart(2, '0');
  const mm = String(d.getMinutes()).padStart(2, '0');
  if (d.toDateString() === now.toDateString()) return `今天 ${hh}:${mm}`;
  const y = new Date(now.getTime() - 86400e3);
  if (d.toDateString() === y.toDateString()) return `昨天 ${hh}:${mm}`;
  if (d.getFullYear() === now.getFullYear()) return `${d.getMonth() + 1}/${d.getDate()} ${hh}:${mm}`;
  return `${d.getFullYear()}/${d.getMonth() + 1}/${d.getDate()}`;
}

let toastTimer = null;
function toast(text, isErr) {
  const t = $('toast');
  t.textContent = text;
  t.className = 'toast on' + (isErr ? ' err' : '');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { t.className = 'toast'; }, isErr ? 4200 : 2400);
}

// ---------------- 数据 ----------------

async function load(force) {
  if (S.busy) return;
  S.busy = true;
  const btn = $('refresh');
  btn.classList.add('spin');

  if (DEMO) {
    const d = window.CR_DEMO;
    S.all = d.workspaces;
    S.stats = d.stats;
    S.roots = d.roots;
    applySettings(d.settings);
    indexWorkspaces();
    render();
    S.busy = false;
    btn.classList.remove('spin');
    return;
  }

  renderSkeleton();

  try {
    const url = `/api/scan?maxAge=${S.range}${force ? '&refresh=1' : ''}`;
    const r = await fetch(url).then((x) => x.json());
    S.all = r.workspaces || [];
    S.stats = r.stats || {};
    S.roots = r.roots || {};
    if (r.settings) applySettings(r.settings);
    indexWorkspaces();
    render();
    plog(`scan ok n=${S.all.length} elapsed=${S.stats.elapsedMs}ms cached=${r.cached}`);
  } catch (e) {
    listEl.innerHTML = `<div class="empty"><h3>扫描失败</h3><p>${esc(e.message)}</p></div>`;
    plog('scan error ' + e.message);
  } finally {
    S.busy = false;
    btn.classList.remove('spin');
  }
}

function indexWorkspaces() {
  S.byId.clear();
  for (const w of S.all) S.byId.set(w.cwd.toLowerCase(), w);
  // 选中项如果已经被过滤掉了，顺手清掉
  const alive = new Set(S.all.map((w) => w.cwd.toLowerCase()));
  for (const k of [...S.sel]) if (!alive.has(k)) S.sel.delete(k);
  for (const k of [...S.expanded]) if (!alive.has(k)) S.expanded.delete(k);
}

function applySettings(s) {
  S.settings = Object.assign(S.settings, s);
  if (typeof s.maxAgeHours === 'number' && s.maxAgeHours !== S.range) {
    // 首次加载时让时间范围和配置对齐
    if (!S.rangeTouched) S.range = s.maxAgeHours;
  }
  $('opt-two').checked = !!S.settings.openTwo;
  $('opt-cont').checked = !!S.settings.useContinue;
  $('set-range').value = String(S.settings.maxAgeHours);
  $('set-window').value = String(S.settings.window);
  $('set-exclude').value = (S.settings.exclude || []).join('\n');
  syncRangeButtons();
}

function syncRangeButtons() {
  for (const b of $('range').querySelectorAll('button')) {
    b.classList.toggle('on', Number(b.dataset.range) === S.range);
  }
}

// ---------------- 渲染 ----------------

function visibleList() {
  const q = $('search').value.trim().toLowerCase();
  const cutoff = S.range > 0 ? Date.now() - S.range * 3600e3 : 0;
  return S.all.filter((w) => {
    if (cutoff && w.lastActive < cutoff) return false;
    if (!q) return true;
    if (w.cwd.toLowerCase().includes(q) || w.name.toLowerCase().includes(q)) return true;
    // 也可以按对话标题找：记得住聊过什么，未必记得在哪个目录
    return (w.sessions || []).some((s) => (s.title || '').toLowerCase().includes(q));
  });
}

function renderSkeleton() {
  listEl.innerHTML = '<div class="skeleton"></div><div class="skeleton"></div><div class="skeleton"></div>' +
    '<div class="skeleton"></div><div class="skeleton"></div>';
  $('meta').textContent = '正在扫描会话历史…';
}

function sessionRowHtml(ws, s, index) {
  const cls = s.tool === 'claude' ? 'cl' : 'cb';
  const extra = index >= INLINE_SESSIONS ? ' sess-extra' : '';
  const title = s.title || '（没有标题的会话）';
  return `
    <div class="sess-row${extra}" data-enter="1"
         data-cwd="${esc(ws.cwd)}" data-tool="${esc(s.tool)}"
         data-sid="${esc(s.sessionId)}" data-title="${esc(title)}"
         title="进入这个对话：${esc(title)}">
      <span class="tool-dot ${cls}"></span>
      <span class="sess-title">${esc(title)}</span>
      <span class="sess-when">${esc(shortTime(s.mtime))}</span>
      <span class="sess-go">进入</span>
    </div>`;
}

function itemHtml(w, index) {
  const key = w.cwd.toLowerCase();
  const no = String(index + 1).padStart(2, '0');
  const on = S.sel.has(key);
  const open = S.expanded.has(key);
  const showAll = S.showAll.has(key);
  const sessions = w.sessions || [];
  const latest = sessions[0];

  // 一个目录下两个 CLI 混着用，标签按 tool 去重
  const tools = [...new Set(sessions.map((s) => s.tool))];
  const chips = tools.map((t) => {
    const cls = t === 'claude' ? 'cl' : 'cb';
    return `<span class="tool-chip ${cls}"><i></i>${esc(t)}</span>`;
  }).join('');

  const rest = sessions.length - INLINE_SESSIONS;
  const moreRow = rest > 0
    ? `<div class="more-row"><button class="more-btn" data-more="${esc(key)}">
         ${showAll ? '收起更早的对话' : `还有 ${rest} 个更早的对话`}
       </button></div>`
    : '';

  const panel = sessions.length
    ? `<div class="sess-panel">
         ${sessions.map((s, i) => sessionRowHtml(w, s, i)).join('')}
         ${moreRow}
       </div>`
    : '';

  return `
  <div class="ws-item${open ? ' open' : ''}${showAll ? ' showall' : ''}" data-item="${esc(key)}">
    <div class="ws-card${on ? ' sel' : ''}" data-key="${esc(key)}">
      <span class="ws-no">${no}</span>
      <input type="checkbox" class="ws-check" ${on ? 'checked' : ''} tabindex="-1">
      <div class="ws-main">
        <div class="ws-title-row">
          <span class="ws-name">${esc(w.name)}</span>
          <span class="ws-path" title="${esc(w.cwd)}"><bdi>${esc(w.cwd)}</bdi></span>
        </div>
        <div class="ws-sub">
          ${chips}
          <span class="dot-sep">·</span>
          <span class="ws-count">${w.sessionCount || sessions.length} 个对话</span>
          <span class="dot-sep">·</span>
          <span class="ws-when">${esc(relTime(w.lastActive))}</span>
          ${latest ? `<span class="ws-latest">最近「${esc(latest.title || '无标题')}」</span>` : ''}
        </div>
      </div>
      <div class="ws-side">
        <button class="mini-btn" data-reveal="${esc(w.cwd)}" title="在资源管理器打开">定位</button>
        <button class="mini-btn go" data-open="${esc(key)}" title="进入这个目录最近的一个对话">进入</button>
        <span class="caret" title="${open ? '收起' : '展开对话列表'}"></span>
      </div>
    </div>
    ${panel}
  </div>`;
}

function render() {
  const list = visibleList();
  if (!list.length) {
    const hasAny = S.all.length > 0;
    listEl.innerHTML = hasAny
      ? `<div class="empty"><h3>这个时间范围内没有对话</h3><p>上面切到「全部」再看看，或者换个关键词。</p></div>`
      : `<div class="empty"><h3>还没找到会话历史</h3>
         <p>先在一个项目目录里跑一次 <code>codebuddy</code> 或 <code>claude</code>，<br>
         再回来点右上角的刷新。</p></div>`;
  } else {
    listEl.innerHTML = list.map((w, i) => itemHtml(w, i)).join('');
  }
  updateBar();
  // 自检区一直有内容，不用等点设置才填上
  renderDoctor();
  $('meta').textContent = S.all.length
    ? `${DEMO ? '示例数据 · ' : ''}共 ${S.all.length} 个目录 · ${S.stats.totalSessions || 0} 个对话 · 扫描 ${S.stats.elapsedMs || 0} ms`
    : '';
}

function updateBar() {
  const n = S.sel.size;
  $('open-btn').disabled = n === 0;
  $('open-text').textContent = n ? `进入对话（${n}）` : '进入对话';

  const boxes = [...listEl.querySelectorAll('.ws-check')];
  const all = $('sel-all');
  const selVisible = boxes.filter((b) => b.checked).length;
  all.checked = boxes.length > 0 && selVisible === boxes.length;
  all.indeterminate = selVisible > 0 && selVisible < boxes.length;
  $('sel-label').textContent = n ? `已选 ${n} 个` : '全选';
}

// ---------------- 选择 ----------------

function toggleKey(key, on) {
  if (on) S.sel.add(key);
  else S.sel.delete(key);
  const card = listEl.querySelector(`.ws-card[data-key="${CSS.escape(key)}"]`);
  if (card) {
    card.classList.toggle('sel', on);
    const cb = card.querySelector('.ws-check');
    if (cb) cb.checked = on;
  }
  updateBar();
}

// 供宿主脚本（--shot 验证）调用：把 DOM 上的勾选状态同步回内存
window.__onSelChange = function () {
  S.sel.clear();
  for (const cb of listEl.querySelectorAll('.ws-check')) {
    const card = cb.closest('.ws-card');
    if (!card) continue;
    card.classList.toggle('sel', cb.checked);
    if (cb.checked) S.sel.add(card.dataset.key);
  }
  updateBar();
};

listEl.addEventListener('click', (e) => {
  // 会话行：直接进入这个对话
  const row = e.target.closest('[data-enter]');
  if (row) {
    e.stopPropagation();
    launchItems([{
      cwd: row.dataset.cwd,
      tool: row.dataset.tool,
      sessionId: row.dataset.sid,
      title: row.dataset.title,
    }], 1);
    return;
  }

  const openBtn = e.target.closest('[data-open]');
  if (openBtn) {
    e.stopPropagation();
    doOpen([openBtn.dataset.open]);
    return;
  }

  const revealBtn = e.target.closest('[data-reveal]');
  if (revealBtn) {
    e.stopPropagation();
    if (DEMO) {
      toast('示例模式：不会打开资源管理器');
      return;
    }
    fetch('/api/reveal', { method: 'POST', body: JSON.stringify({ path: revealBtn.dataset.reveal }) })
      .then((r) => r.json())
      .then((r) => { if (!r.ok) toast('打开资源管理器失败', true); })
      .catch(() => toast('打开资源管理器失败', true));
    return;
  }

  const moreBtn = e.target.closest('[data-more]');
  if (moreBtn) {
    e.stopPropagation();
    const key = moreBtn.dataset.more;
    const item = listEl.querySelector(`.ws-item[data-item="${CSS.escape(key)}"]`);
    if (!item) return;
    const showAll = !item.classList.contains('showall');
    item.classList.toggle('showall', showAll);
    if (showAll) S.showAll.add(key);
    else S.showAll.delete(key);
    refreshMoreLabel(item, key);
    return;
  }

  const card = e.target.closest('.ws-card');
  if (!card) return;
  const key = card.dataset.key;
  // 勾选框只负责批量选择，点卡片其他地方是展开对话列表
  if (e.target.classList.contains('ws-check')) {
    toggleKey(key, e.target.checked);
    return;
  }
  toggleExpand(key);
});

function refreshMoreLabel(item, key) {
  const btn = item.querySelector('[data-more]');
  if (!btn) return;
  const ws = S.byId.get(key);
  const total = (ws && ws.sessions ? ws.sessions.length : 0) - INLINE_SESSIONS;
  const showAll = item.classList.contains('showall');
  btn.textContent = showAll ? '收起更早的对话' : `还有 ${total} 个更早的对话`;
}

function toggleExpand(key) {
  const item = listEl.querySelector(`.ws-item[data-item="${CSS.escape(key)}"]`);
  if (!item) return;
  const open = !item.classList.contains('open');
  item.classList.toggle('open', open);
  if (open) S.expanded.add(key);
  else S.expanded.delete(key);
  const caret = item.querySelector('.caret');
  if (caret) caret.title = open ? '收起' : '展开对话列表';
}

$('sel-all').addEventListener('change', () => {
  const on = $('sel-all').checked;
  const keys = visibleList().map((w) => w.cwd.toLowerCase());
  for (const k of keys) {
    if (on) S.sel.add(k);
    else S.sel.delete(k);
  }
  render();
});

// ---------------- 恢复 ----------------

// 一个工作区要开哪些标签：默认只开最近那个对话，开了「两个 CLI」就各开一个
function pickFromWorkspace(ws) {
  const sessions = ws.sessions || [];
  if (!sessions.length) return [];
  if (!S.settings.openTwo) {
    const s = sessions[0];
    return [{ cwd: ws.cwd, tool: s.tool, sessionId: s.sessionId, title: s.title }];
  }
  const seen = new Set();
  const out = [];
  for (const s of sessions) {
    if (seen.has(s.tool)) continue;
    seen.add(s.tool);
    out.push({ cwd: ws.cwd, tool: s.tool, sessionId: s.sessionId, title: s.title });
  }
  return out;
}

async function launchItems(items, note) {
  if (DEMO) {
    toast('示例模式：不会真的打开终端');
    return;
  }
  if (!items || !items.length) {
    toast('没有可进入的对话', true);
    return;
  }
  try {
    const r = await fetch('/api/open', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        items,
        useContinue: !!S.settings.useContinue,
        window: Number(S.settings.window) || 0,
      }),
    }).then((x) => x.json());

    if (r.ok) {
      const n = note || items.length;
      toast(n === 1 ? `已进入「${items[0].title || '该对话'}」` : `已在 Windows Terminal 打开 ${n} 个标签页`);
      plog('open ok ' + (r.command || ''));
    } else {
      toast(r.error || '打开失败', true);
      plog('open fail ' + (r.error || ''));
    }
  } catch (e) {
    toast('打开失败：' + e.message, true);
  }
}

async function doOpen(keys) {
  const items = [];
  for (const key of keys) {
    const ws = S.byId.get(key);
    if (ws) items.push(...pickFromWorkspace(ws));
  }
  await launchItems(items);
}

$('open-btn').addEventListener('click', async () => {
  const btn = $('open-btn');
  btn.disabled = true;
  try {
    await doOpen([...S.sel]);
  } finally {
    btn.disabled = S.sel.size === 0;
  }
});

// ---------------- 筛选与搜索 ----------------

$('range').addEventListener('click', (e) => {
  const b = e.target.closest('button');
  if (!b) return;
  S.range = Number(b.dataset.range);
  S.rangeTouched = true;
  syncRangeButtons();
  saveSettings({ maxAgeHours: S.range });
  load(false);
});

let searchTimer = null;
$('search').addEventListener('input', () => {
  clearTimeout(searchTimer);
  searchTimer = setTimeout(render, 120);
});

$('refresh').addEventListener('click', () => load(true));

$('opt-two').addEventListener('change', () => {
  S.settings.openTwo = $('opt-two').checked;
  saveSettings({ openTwo: S.settings.openTwo });
});
$('opt-cont').addEventListener('change', () => {
  S.settings.useContinue = $('opt-cont').checked;
  saveSettings({ useContinue: S.settings.useContinue });
});

async function saveSettings(patch) {
  if (DEMO) return;
  try {
    const r = await fetch('/api/settings', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(patch),
    }).then((x) => x.json());
    if (r.settings) S.settings = Object.assign(S.settings, r.settings);
  } catch (e) {
    plog('save settings error ' + e.message);
  }
}

// ---------------- 设置（左栏常驻） ----------------
// 设置不再是右侧抽屉，改成左栏里的常驻区块；点顶栏的滑杆只是高亮一下，
// 常量显示意味着随时能看到排除规则和环境自检，不用先打开一层。

function openDrawer() {
  const d = $('drawer');
  d.classList.add('on');
  renderDoctor();
  // 只在确实看不到的时候才滚，避免点一下就把上面的时间范围挤出视野
  const box = d.getBoundingClientRect();
  const rail = d.parentElement.getBoundingClientRect();
  if (box.top < rail.top || box.bottom > rail.bottom) {
    d.scrollIntoView({ block: 'nearest', behavior: 'smooth' });
  }
}
function closeDrawer() {
  $('drawer').classList.remove('on');
}

$('settings-btn').addEventListener('click', openDrawer);
$('drawer-close').addEventListener('click', closeDrawer);

$('set-save').addEventListener('click', async () => {
  const exclude = $('set-exclude').value.split('\n').map((s) => s.trim()).filter(Boolean);
  const maxAgeHours = Number($('set-range').value);
  const win = Number($('set-window').value);
  await saveSettings({ exclude, maxAgeHours, window: win });
  S.range = maxAgeHours;
  S.rangeTouched = true;
  syncRangeButtons();
  closeDrawer();
  toast('设置已保存');
  load(true);
});

function renderDoctor() {
  const box = $('doctor');
  const r = S.roots || {};
  const row = (ok, text) =>
    `<div class="doc-row"><span class="mark ${ok ? 'ok' : 'bad'}">${ok ? '✓' : '×'}</span><span class="txt">${esc(text)}</span></div>`;
  box.innerHTML = [
    row(!!r.codebuddyFound, 'codebuddy：' + (r.codebuddy || '')),
    row(!!r.claudeFound, 'claude：' + (r.claude || '')),
    row(true, `${S.all.length} 个可用目录 / ${S.stats.totalSessions || 0} 个对话`),
  ].join('');
}

// ---------------- 快捷键 ----------------

document.addEventListener('keydown', (e) => {
  if (e.key === '/' && document.activeElement !== $('search')) {
    e.preventDefault();
    $('search').focus();
    $('search').select();
    return;
  }
  if (e.key === 'Escape') {
    if ($('drawer').classList.contains('on')) { closeDrawer(); return; }
    if (document.activeElement === $('search') && $('search').value) {
      $('search').value = '';
      render();
      return;
    }
  }
  if (e.key === 'Enter' && (e.ctrlKey || e.metaKey) && S.sel.size) {
    e.preventDefault();
    doOpen([...S.sel]);
  }
  if (e.key === 'a' && (e.ctrlKey || e.metaKey) && document.activeElement !== $('search')) {
    e.preventDefault();
    $('sel-all').checked = true;
    for (const w of visibleList()) S.sel.add(w.cwd.toLowerCase());
    render();
  }
});

// ---------------- 顶栏时钟 ----------------

function tickClock() {
  const d = new Date();
  const p = (n) => String(n).padStart(2, '0');
  $('clock').textContent = `${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`;
}
tickClock();
setInterval(tickClock, 1000);

// 启动
load(false);
