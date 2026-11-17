'use strict';

const $ = (id) => document.getElementById(id);
const listEl = $('list');

// demo.js 检测到 ?demo=1 时会挂上 window.CR_DEMO，此时所有数据来自它，
// 不扫本机历史、不调拉起接口，纯粹用来截图和预览界面
const DEMO = !!window.CR_DEMO;

// --skin xx 启动参数只用于截图和脚本：地址栏带 #skin= 时强制这套皮肤，
// 不写回 settings.json，免得截个图把用户自己的主题改了
const FORCED_SKIN = ((location.hash.match(/[#&]skin=([a-z0-9]+)/i) || [])[1] || '').toLowerCase();

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
    theme: 'neon',
    termMode: 'embedded',
    restoreLayout: true,
    llmUrl: 'http://127.0.0.1:11434',
    llmModel: '',
    exclude: [],
  },
  stats: {},
  roots: {},
  busy: false,
  deep: null,   // 全文搜索结果：null 表示当前是普通列表
  brief: null,  // 本地模型复盘：null 表示没在这个视图
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
  const tm = $('set-term');
  if (tm) tm.value = S.settings.termMode === 'external' ? 'external' : 'embedded';
  const rs = $('set-restore');
  if (rs) rs.value = S.settings.restoreLayout === false ? '0' : '1';
  if ($('set-llm-url')) $('set-llm-url').value = S.settings.llmUrl || '';
  if ($('set-llm-model')) $('set-llm-model').value = S.settings.llmModel || '';
  $('set-exclude').value = (S.settings.exclude || []).join('\n');
  syncRangeButtons();
  applyTheme(FORCED_SKIN || S.settings.theme);
}

// ---------------- 外观主题 ----------------
// 皮肤全部挂在 html[data-skin] 上，样式在 style.css 与 theme-win98.css 里，
// 这里只负责换属性和记下来；别处不要再写内联样式，否则换代就漏。
const SKINS = ['neon', 'win98'];
const SKIN_NAME = { neon: '80s 霓虹 CRT', win98: 'Windows 98' };

function applyTheme(name) {
  const skin = SKINS.includes(name) ? name : 'neon';
  S.settings.theme = skin;
  document.documentElement.dataset.skin = skin;
  const sel = $('set-theme');
  if (sel && sel.value !== skin) sel.value = skin;
  const btn = $('theme-btn');
  if (btn) {
    const next = skin === 'win98' ? 'neon' : 'win98';
    btn.title = `外观：${SKIN_NAME[skin]}（点击切到 ${SKIN_NAME[next]}）`;
  }
  // 只作为下次启动首帧的垫片，真正生效以 settings.json 为准；
  // 命令行强制的皮肤不写回去，否则截图会污染用户自己的设置
  if (!FORCED_SKIN) {
    try { localStorage.setItem('cli-resume-theme', skin); } catch (e) { /* 隐私模式 */ }
  }
  // 终端模块在文件末尾，用钩子避免 const 的 TDZ
  if (typeof window.__termApplySkin === 'function') window.__termApplySkin(skin);
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
        <button class="mini-btn brief" data-brief="${esc(w.cwd)}" title="让本地模型复盘这个目录最近一次会话">复盘</button>
        <button class="mini-btn" data-reveal="${esc(w.cwd)}" title="在资源管理器打开">定位</button>
        <button class="mini-btn go" data-open="${esc(key)}" title="进入这个目录最近的一个对话">进入</button>
        <span class="caret" title="${open ? '收起' : '展开对话列表'}"></span>
      </div>
    </div>
    ${panel}
  </div>`;
}

// ---------------- 全文搜索 ----------------
// 顶部搜索框默认只过滤目录和标题；敲回车才去会话正文里翻。
// 索引是后端在后台建的，没建完时先给已有结果，顶上挂一条进度。

function baseOf(p) {
  const s = String(p || '').replace(/[\\/]+$/, '');
  const i = Math.max(s.lastIndexOf('\\'), s.lastIndexOf('/'));
  return i >= 0 ? s.slice(i + 1) : s;
}

function roleLabel(r) {
  if (r === 'user') return '你';
  if (r === 'tool') return '工具';
  return '答';
}

function renderDeep() {
  const d = S.deep;
  if (d.loading) {
    listEl.innerHTML = `<div class="empty"><h3>正在翻“${esc(d.q)}”…</h3><p>稍等，正在把会话正文过一遍。</p></div>`;
    $('meta').textContent = '全文搜索中…';
    updateBar();
    return;
  }

  const r = d.data || { hits: [], matched: 0, matches: 0, elapsedMs: 0, ready: true, files: 0, totalFiles: 0 };
  const groups = new Map();
  for (const h of r.hits || []) {
    const key = `${h.tool}:${h.sessionId}`;
    let g = groups.get(key);
    if (!g) {
      g = { head: h, rows: [], more: 0 };
      groups.set(key, g);
    }
    // 一个会话最多铺 6 条，剩下的收成一行，不然一屏只能看到一个会话
    if (g.rows.length < 6) g.rows.push(h);
    else g.more += 1;
  }

  const head = `
    <div class="deep-head">
      <span class="deep-q">“${esc(d.q)}”</span>
      <span class="deep-sub">${r.ready
        ? `${r.matched} 个会话 · ${r.matches} 处命中 · ${r.elapsedMs} ms`
        : `索引 ${r.files}/${r.totalFiles} · 先显示已有结果`}</span>
      <button class="mini-btn" data-back="1">返回列表</button>
    </div>`;

  const body = groups.size
    ? [...groups.values()].map((g, i) => {
      const h = g.head;
      const attrs = `data-enter="1" data-cwd="${esc(h.cwd)}" data-tool="${esc(h.tool)}"` +
        ` data-sid="${esc(h.sessionId)}" data-title="${esc(h.title || h.cwd)}"`;
      const rows = g.rows.map((x) => `
          <div class="hit-row" ${attrs} title="进入这个对话：${esc(h.title || h.cwd)}">
            <span class="hit-role ${esc(x.role)}">${roleLabel(x.role)}</span>
            <span class="hit-text">${esc(x.before)}<mark>${esc(x.hit)}</mark>${esc(x.after)}</span>
            <span class="hit-when">${esc(shortTime(x.ts))}</span>
          </div>`).join('');
      const more = g.more ? `<div class="hit-more">这个会话还有 ${g.more} 处没铺开</div>` : '';
      return `
        <div class="hit-group">
          <div class="hit-grouphead" ${attrs}>
            <span class="hit-no">${String(i + 1).padStart(2, '0')}</span>
            <span class="hit-ws">${esc(baseOf(h.cwd))}</span>
            <span class="hit-title">${esc(h.title || '（无标题）')}</span>
            <span class="hit-count">${g.rows.length + g.more} 处</span>
            <span class="sess-go">进入</span>
          </div>
          ${rows}${more}
        </div>`;
    }).join('')
    : `<div class="empty"><h3>正文里没有“${esc(d.q)}”</h3>
       <p>换个说法试试，或者想想这个词是在哪个项目里说过的。</p></div>`;

  listEl.innerHTML = head + body;
  $('meta').textContent = r.ready
    ? `正文索引 ${r.totalFiles} 个会话 · ${r.entries} 条消息`
    : `正在建立正文索引 ${r.files}/${r.totalFiles}`;
  updateBar();
}

async function runDeepSearch(retry) {
  const q = $('search').value.trim();
  if (q.length < 2) {
    toast('至少输两个字；只想过滤目录不用回车', true);
    return;
  }
  if (DEMO) {
    // 示例模式没有真实正文，demo.js 里备了几句样例，够看清结果长什么样
    const data = typeof window.CR_DEMO_SEARCH === 'function' ? window.CR_DEMO_SEARCH(q) : null;
    S.deep = data
      ? { q, loading: false, data, tries: 0 }
      : { q, loading: false, data: { hits: [], matched: 0, matches: 0, elapsedMs: 0, ready: true, files: 0, totalFiles: 0, entries: 0 }, tries: 0 };
    render();
    return;
  }
  if (!retry) S.deep = { q, loading: true, data: null, tries: 0 };

  let r = null;
  try {
    r = await fetch(`/api/search?q=${encodeURIComponent(q)}&limit=200`).then((x) => x.json());
  } catch (e) {
    // 走到下面统一报错
  }
  // 等待期间又搜了别的词，这次的结果就丢掉
  if (!S.deep || S.deep.q !== q) return;
  if (!r || !r.ok) {
    S.deep = null;
    toast('搜索失败', true);
    render();
    return;
  }

  const tries = S.deep.tries || 0;
  S.deep = { q, loading: false, data: r, tries };
  render();

  // 索引还在建：过一秒多自动再拉一次，用户不用自己按第二遍
  if (!r.ready && tries < 30) {
    S.deep.tries = tries + 1;
    setTimeout(() => {
      if (S.deep && S.deep.q === q) runDeepSearch(true);
    }, 1200);
  }
}

// ---------------- 本地模型复盘 ----------------
// 把最近一次会话的头尾交给本机模型（Ollama 或 OpenAI 兼容），回「做了 / 卡在 / 下一步」。
// 模型没起来时不装死：照样列出最后几条原文，至少能看出上次停在哪。

const BRIEF_LABELS = ['做了', '卡在', '下一步', '进度', '待办', '阻塞'];

function openBrief(cwd) {
  const w = S.byId.get(String(cwd).toLowerCase());
  if (!w || !(w.sessions || []).length) {
    toast('这个目录还没有对话', true);
    return;
  }
  const s = w.sessions[0];  // sessions 已按时间倒序，第一条就是最近那次
  S.brief = {
    cwd: w.cwd, name: w.name, tool: s.tool, sid: s.sessionId,
    title: s.title || w.name, loading: true, data: null, error: '',
  };
  render();
  fetchBrief(false);
}

async function fetchBrief(force) {
  const b = S.brief;
  if (!b) return;
  b.loading = true;
  b.error = '';
  render();

  if (DEMO) {
    await new Promise((r) => setTimeout(r, 700));
    if (S.brief !== b) return;
    b.loading = false;
    b.data = {
      ok: true, llmOk: true, api: 'ollama', model: 'qwen3:8b', cached: false,
      elapsedMs: 1840, entries: 35, turns: [],
      text: '做了：把 BSR 渲染改成流式输出，补了 3 个回归用例。\n' +
        '卡在：旧版 Safari 首屏仍有 200ms 白屏，没定位到是字体还是合成层。\n' +
        '下一步：先加一帧骨架屏兜住，再单独排查 Safari 的图层。',
    };
    render();
    return;
  }

  try {
    const u = `/api/summary?tool=${encodeURIComponent(b.tool)}&sid=${encodeURIComponent(b.sid)}` +
      (force ? '&force=1' : '');
    const r = await fetch(u).then((x) => x.json());
    if (S.brief !== b) return;  // 等的时候已经切走了
    b.data = r;
    b.loading = false;
    if (!r.ok) b.error = r.error || '没拿到这个会话';
  } catch (e) {
    if (S.brief !== b) return;
    b.loading = false;
    b.error = e.message;
  }
  render();
}

// 模型偶尔会加粗、加序号，这里按行拆，认出「做了：」这类前缀就给个标签
function briefLines(text) {
  return String(text || '')
    .split(/\r?\n/)
    .map((s) => s.replace(/^[\s*·\-]+/, '').trim())
    .filter(Boolean)
    .map((line) => {
      const m = line.match(/^([^：:]{1,6})[：:]\s*(.+)$/);
      if (m && BRIEF_LABELS.includes(m[1].trim())) return { k: m[1].trim(), v: m[2] };
      return { k: '', v: line };
    });
}

function briefTurns(turns) {
  return (turns || []).map((t) => `
    <div class="brief-turn">
      <span class="hit-role ${esc(t.role)}">${roleLabel(t.role)}</span>
      <span class="brief-turn-text">${esc(t.text)}</span>
    </div>`).join('');
}

function renderBrief() {
  const b = S.brief;
  const d = b.data || {};
  const head = `
    <div class="deep-head">
      <span class="deep-q">复盘 · ${esc(b.name || b.cwd)}</span>
      <span class="deep-sub">${esc(b.title)}${d.entries ? ` · ${d.entries} 条消息` : ''}${
        d.cached ? ' · 缓存' : ''}</span>
      <button class="mini-btn" data-back="1">返回列表</button>
    </div>`;

  let body = '';
  if (b.loading) {
    body = `<div class="brief-wrap"><div class="brief-card">
      <div class="brief-wait"><i class="brief-dot"></i><i class="brief-dot"></i><i class="brief-dot"></i>
        <span>本地模型正在读这个会话…首次调用要等模型加载，可能十几秒</span></div>
    </div></div>`;
  } else if (b.error) {
    body = `<div class="brief-wrap"><div class="brief-card bad">
      <p>${esc(b.error)}</p>
      <p class="brief-hint">索引可能还在建，稍等片刻点「重新生成」。</p>
    </div></div>`;
  } else if (!d.llmOk) {
    body = `<div class="brief-wrap">
      <div class="brief-card warn">
        <h3>本地模型没连上</h3>
        <p>${esc(d.llmError || '没有可用的模型')}</p>
        <p class="brief-hint">在左栏「本地模型地址 / 模型」里填本机服务，Ollama 默认
          <code>http://127.0.0.1:11434</code>，LM Studio 是 <code>http://127.0.0.1:1234</code>。
          下面是最后几条原始记录，先凑合看。</p>
      </div>
      <div class="brief-card">
        <h3 class="brief-sub">最近几条</h3>
        ${briefTurns(d.turns) || '<p class="brief-hint">索引里还没有这个会话的正文。</p>'}
      </div>
    </div>`;
  } else {
    const rows = briefLines(d.text).map((x) => `
      <div class="brief-line${x.k ? ' key' : ''}"${x.k ? ` data-k="${esc(x.k)}"` : ''}>
        ${x.k ? `<span class="brief-k">${esc(x.k)}</span>` : ''}
        <span class="brief-v">${esc(x.v)}</span>
      </div>`).join('');
    body = `<div class="brief-wrap">
      <div class="brief-card">
        <div class="brief-lines">${rows}</div>
        <div class="brief-foot">
          <span class="brief-meta">${esc(d.model || '')}${d.api ? ` · ${esc(d.api)}` : ''}${
            d.cached ? ' · 来自缓存' : ` · ${d.elapsedMs || 0} ms`}</span>
          <button class="mini-btn" data-brief-redo="1">重新生成</button>
          <button class="mini-btn go" data-brief-enter="1">进入这个会话</button>
        </div>
      </div>
      ${d.turns && d.turns.length ? `<details class="brief-raw">
        <summary>看原始记录（最近 ${d.turns.length} 条）</summary>
        ${briefTurns(d.turns)}
      </details>` : ''}
    </div>`;
  }

  listEl.innerHTML = head + body;
  $('meta').textContent = b.loading ? '复盘生成中…' : `复盘 · ${b.name || b.cwd}`;
  updateBar();
}

function render() {
  if (S.brief) {
    renderBrief();
    return;
  }
  if (S.deep) {
    renderDeep();
    return;
  }
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
  const back = e.target.closest('[data-back]');
  if (back) {
    S.deep = null;
    S.brief = null;
    render();
    return;
  }

  // 复盘：让本地模型读一遍这个目录最近一次会话
  const briefBtn = e.target.closest('[data-brief]');
  if (briefBtn) {
    e.stopPropagation();
    openBrief(briefBtn.dataset.brief);
    return;
  }

  if (e.target.closest('[data-brief-redo]')) {
    fetchBrief(true);
    return;
  }

  const briefEnter = e.target.closest('[data-brief-enter]');
  if (briefEnter && S.brief) {
    launchItems([{
      cwd: S.brief.cwd, tool: S.brief.tool,
      sessionId: S.brief.sid, title: S.brief.title,
    }], 1);
    return;
  }

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
  // 默认走应用内终端；设置里切成 external 才回到 Windows Terminal
  if (S.settings.termMode !== 'external') {
    await termOpenItems(items);
    toast(items.length === 1
      ? `已在应用内终端打开「${items[0].title || '该对话'}」`
      : `已在终端面板并排打开 ${items.length} 个会话`);
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
  // 已经出了全文结果，一改词就先退回列表，避免看着旧结果以为是新的
  if (S.deep) S.deep = null;
  searchTimer = setTimeout(render, 120);
});

// 回车 = 去正文里翻，跟列表过滤是两回事
$('search').addEventListener('keydown', (e) => {
  if (e.key === 'Enter') {
    e.preventDefault();
    runDeepSearch();
  }
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

// 顶栏那个按钮只是「换一套」，设置里的下拉才是明确选择
$('theme-btn').addEventListener('click', () => {
  const next = S.settings.theme === 'win98' ? 'neon' : 'win98';
  applyTheme(next);
  saveSettings({ theme: next });
  toast(`外观：${SKIN_NAME[next]}`);
});

$('set-theme').addEventListener('change', () => {
  applyTheme($('set-theme').value);
  saveSettings({ theme: S.settings.theme });
});

$('set-save').addEventListener('click', async () => {
  const exclude = $('set-exclude').value.split('\n').map((s) => s.trim()).filter(Boolean);
  const maxAgeHours = Number($('set-range').value);
  const win = Number($('set-window').value);
  const termMode = $('set-term') ? $('set-term').value : 'embedded';
  const restoreLayout = !$('set-restore') || $('set-restore').value !== '0';
  const llmUrl = $('set-llm-url') ? $('set-llm-url').value.trim() : S.settings.llmUrl;
  const llmModel = $('set-llm-model') ? $('set-llm-model').value.trim() : S.settings.llmModel;
  await saveSettings({ exclude, maxAgeHours, window: win, termMode, restoreLayout, llmUrl, llmModel });
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

// ---------------- 内嵌终端 ----------------
// 一个 pane = 一个 xterm 实例 + 一条 WebSocket。二进制帧是 PTY 原始字节（进出都走它），
// 文本帧只放控制消息（resize）。后端是 src/terminal.cpp 里的 ConPTY，
// 所以 codebuddy / claude 在这里拿到的是真 TTY，颜色和按键都正常。

const TERM_THEMES = {
  neon: {
    background: '#07030f', foreground: '#d8e4f6',
    cursor: '#00f0ff', cursorAccent: '#07030f',
    selectionBackground: 'rgba(0, 240, 255, .28)',
    black: '#120a24', red: '#ff2e97', green: '#39d98a', yellow: '#ffb31a',
    blue: '#6c8af5', magenta: '#a855f7', cyan: '#00f0ff', white: '#d8e4f6',
    brightBlack: '#6b5a8a', brightRed: '#ff6fb5', brightGreen: '#6ff0b0',
    brightYellow: '#ffcf66', brightBlue: '#9db4ff', brightMagenta: '#c98cff',
    brightCyan: '#7df9ff', brightWhite: '#ffffff',
  },
  // 98 的「MS-DOS 方式」：黑底、灰白字、深色 16 色
  win98: {
    background: '#000000', foreground: '#c0c0c0',
    cursor: '#c0c0c0', cursorAccent: '#000000',
    selectionBackground: 'rgba(0, 0, 128, .6)',
    black: '#000000', red: '#800000', green: '#008000', yellow: '#808000',
    blue: '#000080', magenta: '#800080', cyan: '#008080', white: '#c0c0c0',
    brightBlack: '#808080', brightRed: '#ff0000', brightGreen: '#00ff00',
    brightYellow: '#ffff00', brightBlue: '#0000ff', brightMagenta: '#ff00ff',
    brightCyan: '#00ffff', brightWhite: '#ffffff',
  },
};

const TM = { panes: [], active: null, restoring: false, restored: false };

function termSkin() {
  return S.settings.theme === 'win98' ? 'win98' : 'neon';
}

function termOptions() {
  const skin = termSkin();
  return {
    theme: TERM_THEMES[skin],
    fontFamily: skin === 'win98'
      ? '"Fixedsys", "Lucida Console", Consolas, "SimSun", monospace'
      : 'Consolas, "Cascadia Mono", "JetBrains Mono", monospace',
    fontSize: skin === 'win98' ? 13 : 12.5,
    lineHeight: 1.18,
    letterSpacing: 0,
    cursorBlink: true,
    scrollback: 5000,
    allowProposedApi: true,
    macOptionIsMeta: false,
  };
}

// 换皮肤时把已经开着的终端也刷一遍配色
window.__termApplySkin = function (skin) {
  if (typeof TM === 'undefined' || !TM.panes) return;
  const th = TERM_THEMES[skin === 'win98' ? 'win98' : 'neon'];
  const opts = termOptions();
  for (const p of TM.panes) {
    p.term.options.theme = th;
    p.term.options.fontFamily = opts.fontFamily;
    p.term.options.fontSize = opts.fontSize;
    termFit(p);
  }
};

function termShow() {
  const d = $('termdock');
  d.hidden = false;
  d.classList.remove('collapsed');
  termSaveLayout();
}

function termCollapse() {
  $('termdock').classList.add('collapsed');
  termSaveLayout();
}

// ---------------- 布局记忆 ----------------
// 存的是「怎么起来」：目录 + 启动命令，而命令里已经带着 --resume，
// 所以下次启动铺回来还是那几个会话，不是几个空 shell。

let layoutTimer = null;

function termSnapshot() {
  const dock = $('termdock');
  return {
    visible: !dock.hidden && !dock.classList.contains('collapsed'),
    height: Math.round(dock.getBoundingClientRect().height) || 0,
    active: Math.max(0, TM.panes.indexOf(TM.active)),
    panes: TM.panes.map((p) => ({
      cwd: p.cwd || '',
      cmd: p.cmd || '',
      title: p.title || '',
      tool: p.tool || '',
      sessionId: p.sessionId || '',
    })),
  };
}

function termSaveLayout() {
  // 恢复过程中每次 termAddPane 都会写一次，那时 panes 还不齐，写下去会把布局截断
  if (DEMO || TM.restoring) return;
  clearTimeout(layoutTimer);
  layoutTimer = setTimeout(() => {
    fetch('/api/layout', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(termSnapshot()),
    }).catch(() => {});
  }, 350);
}

// 铺回上次的窗格。空布局什么都不做 —— 别让第一次打开的人凭空多出一块面板。
async function termRestoreLayout() {
  if (DEMO || TM.restored) return;
  TM.restored = true;
  if (S.settings.restoreLayout === false) return;

  let lay = null;
  try {
    lay = await fetch('/api/layout').then((r) => r.json());
  } catch (e) {
    return;
  }
  const panes = lay && Array.isArray(lay.panes) ? lay.panes.slice(0, 4) : [];
  if (!panes.length) return;
  // 上次是收起状态就不恢复了：掀起来只会得到几个看不见的 CLI 进程，
  // 用户既没看见也没法操作，白占内存
  if (!lay.visible) return;

  TM.restoring = true;
  // 高度先定下来，窗格按最终尺寸开，省得开完再缩一次、ConPTY 白重排两遍。
  // 上次是在大窗口里拖的 600px，这次窗口小了就按视口压回来，别把列表挤没
  const dock = $('termdock');
  const h = Math.min(Math.round(Number(lay.height) || 0), Math.round(window.innerHeight * 0.7));
  if (h >= 120) dock.style.flexBasis = h + 'px';
  for (const p of panes) {
    await termAddPane({
      cwd: p.cwd || '',
      cmd: p.cmd || '',
      title: p.title || 'shell',
      tool: p.tool || '',
      sessionId: p.sessionId || '',
    });
  }
  const idx = Math.min(Math.max(0, Number(lay.active) || 0), TM.panes.length - 1);
  if (TM.panes[idx]) {
    TM.active = TM.panes[idx];
    termLayout();
  }
  TM.restoring = false;
  termSaveLayout();
}

// 1 个占满、2 个并排、3~4 个两行两列 —— 就是「分窗口并行看」
function termLayout() {
  const grid = $('term-grid');
  const n = TM.panes.length;
  const cols = n <= 1 ? 1 : 2;
  const rows = Math.max(1, Math.ceil(n / cols));
  grid.style.gridTemplateColumns = `repeat(${cols}, minmax(0, 1fr))`;
  grid.style.gridTemplateRows = `repeat(${rows}, minmax(0, 1fr))`;
  for (const p of TM.panes) {
    p.el.classList.toggle('on', p === TM.active);
    setTimeout(() => termFit(p), 0);
  }
  termRenderTabs();
}

function termRenderTabs() {
  const box = $('term-tabs');
  if (!box) return;
  box.innerHTML = TM.panes
    .map((p, i) =>
      `<button class="term-tab${p === TM.active ? ' on' : ''}${p.ws ? '' : ' dead'}"` +
      ` data-i="${i}" title="${esc(p.cwd || p.title)}"><b>${i + 1}</b>${esc(p.title)}</button>`)
    .join('');
}

function termFit(pane) {
  if (!pane || !pane.fit) return;
  try {
    pane.fit.fit();
  } catch (e) {
    return;  // 面板收起时容器是 0 尺寸，算不出行列，直接跳过
  }
  const cols = pane.term.cols;
  const rows = pane.term.rows;
  if (!cols || !rows) return;
  if (pane.cols === cols && pane.rows === rows) return;
  pane.cols = cols;
  pane.rows = rows;
  if (pane.ws && pane.ws.readyState === 1) {
    pane.ws.send(JSON.stringify({ t: 'resize', cols, rows }));
  } else if (pane.id && !DEMO) {
    fetch('/api/term/resize', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ id: pane.id, cols, rows }),
    }).catch(() => {});
  }
}

function termConnect(pane) {
  const ws = new WebSocket(`ws://${location.host}/api/term/ws?id=${encodeURIComponent(pane.id)}`);
  ws.binaryType = 'arraybuffer';
  pane.ws = ws;
  ws.onopen = () => {
    pane.el.classList.remove('dead');
    termFit(pane);
    if (TM.active === pane) pane.term.focus();
    termRenderTabs();
  };
  ws.onmessage = (ev) => {
    if (typeof ev.data === 'string') return;
    pane.term.write(new Uint8Array(ev.data));
  };
  ws.onclose = () => {
    pane.ws = null;
    pane.el.classList.add('dead');
    termRenderTabs();
  };
}

async function termAddPane(opt = {}) {
  if (TM.panes.length >= 4) {
    toast('最多同时 4 个终端，先关掉一个', true);
    return null;
  }
  termShow();
  const el = document.createElement('div');
  el.className = 'term-pane';
  el.innerHTML =
    `<div class="term-hd"><span class="term-t">${esc(opt.title || 'terminal')}</span>` +
    `<span class="term-meta"></span>` +
    `<button class="term-x" title="关闭这个终端">×</button></div>` +
    `<div class="term-box"></div>`;
  $('term-grid').appendChild(el);

  const term = new Terminal(termOptions());
  const fit = new FitAddon.FitAddon();
  term.loadAddon(fit);
  term.open(el.querySelector('.term-box'));

  const pane = {
    id: '', term, fit, el, ws: null,
    title: opt.title || 'terminal', cwd: opt.cwd || '', cmd: opt.cmd || '',
    tool: opt.tool || '', sessionId: opt.sessionId || '',
    cols: 0, rows: 0, ro: null,
  };
  TM.panes.push(pane);
  TM.active = pane;
  termLayout();

  el.querySelector('.term-x').addEventListener('click', (e) => {
    e.stopPropagation();
    termClosePane(pane);
  });
  el.addEventListener('mousedown', () => {
    if (TM.active === pane) return;
    TM.active = pane;
    termLayout();
  });
  term.onData((d) => {
    if (pane.ws && pane.ws.readyState === 1) pane.ws.send(new TextEncoder().encode(d));
  });

  pane.ro = new ResizeObserver(() => termFit(pane));
  pane.ro.observe(el.querySelector('.term-box'));
  termFit(pane);

  if (DEMO) {
    // 示例模式不真的起 shell，写几行说明，截图和首次体验都能看懂这块是干什么的
    const dim = '\x1b[90m';
    const rst = '\x1b[0m';
    const cy = termSkin() === 'win98' ? '\x1b[36m' : '\x1b[96m';
    const at = termSkin() === 'win98' ? '\x1b[33m' : '\x1b[95m';
    term.writeln(`${dim}$${rst} ${opt.cmd || 'pwsh'}`);
    term.writeln(`${at}⏺${rst} 示例模式：这里会是真实终端`);
    term.writeln(`${dim}  真跑起来时，codebuddy / claude 直接在应用内执行，${rst}`);
    term.writeln(`${dim}  点「▧」可以并排开第二个、第三个，互不干扰。${rst}`);
    term.writeln('');
    term.writeln(`${cy}>${rst} `);
    el.querySelector('.term-meta').textContent = 'demo';
    termRenderTabs();
    return pane;
  }

  try {
    const r = await fetch('/api/term/open', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        cwd: pane.cwd, cmd: pane.cmd, title: pane.title,
        cols: pane.cols || 100, rows: pane.rows || 30,
      }),
    }).then((x) => x.json());
    if (!r.ok) throw new Error(r.error || '终端启动失败');
    pane.id = r.id;
    el.querySelector('.term-meta').textContent = pane.cwd ? pane.cwd.split(/[\\/]/).pop() : 'shell';
    termConnect(pane);
  } catch (e) {
    el.classList.add('dead');
    term.writeln(`\x1b[31m${e.message}\x1b[0m`);
    plog('term open fail ' + e.message);
  }
  termRenderTabs();
  termSaveLayout();
  return pane;
}

function termClosePane(pane) {
  const i = TM.panes.indexOf(pane);
  if (i < 0) return;
  TM.panes.splice(i, 1);
  if (pane.ro) pane.ro.disconnect();
  if (pane.ws) {
    try { pane.ws.close(); } catch (e) { /* 已经关过了 */ }
  }
  try { pane.term.dispose(); } catch (e) { /* 已经释放 */ }
  pane.el.remove();
  // 后端在 WS 断开时就会回收会话，这里只做兜底（接口是幂等的）
  if (pane.id && !DEMO) {
    fetch('/api/term/close', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ id: pane.id }),
    }).catch(() => {});
  }
  TM.active = TM.panes[Math.min(i, TM.panes.length - 1)] || null;
  termLayout();
  if (!TM.panes.length) termCollapse();
  termSaveLayout();
}

function termCmdFor(it) {
  const bin = it.tool === 'claude' ? 'claude' : 'codebuddy';
  if (S.settings.useContinue || !it.sessionId) return bin;
  return `${bin} --resume ${it.sessionId}`;
}

async function termOpenItems(items) {
  for (const it of items) {
    await termAddPane({
      cwd: it.cwd, cmd: termCmdFor(it), title: it.title || it.cwd,
      tool: it.tool, sessionId: it.sessionId,
    });
  }
}

$('term-btn').addEventListener('click', () => {
  if (TM.panes.length) {
    termShow();
    termLayout();
    if (TM.active) TM.active.term.focus();
    return;
  }
  termAddPane({ cwd: '', title: 'shell' });
});

$('term-split').addEventListener('click', () => {
  const p = TM.active;
  termAddPane({ cwd: p ? p.cwd : '', title: p ? p.title : 'shell' });
});

$('term-new').addEventListener('click', () => termAddPane({ cwd: '', title: 'shell' }));
$('term-hide').addEventListener('click', termCollapse);

$('term-tabs').addEventListener('click', (e) => {
  const b = e.target.closest('.term-tab');
  if (!b) return;
  const p = TM.panes[Number(b.dataset.i)];
  if (!p) return;
  TM.active = p;
  termLayout();
  p.term.focus();
  termSaveLayout();
});

// 拖顶边改高度
$('term-drag').addEventListener('mousedown', (e) => {
  e.preventDefault();
  const dock = $('termdock');
  const startY = e.clientY;
  const startH = dock.getBoundingClientRect().height;
  const move = (ev) => {
    const h = Math.max(120, Math.min(window.innerHeight - 200, startH - (ev.clientY - startY)));
    dock.style.flexBasis = h + 'px';
    for (const p of TM.panes) termFit(p);
  };
  const up = () => {
    document.removeEventListener('mousemove', move);
    document.removeEventListener('mouseup', up);
    termSaveLayout();
  };
  document.addEventListener('mousemove', move);
  document.addEventListener('mouseup', up);
});

// ---------------- 快捷键 ----------------

document.addEventListener('keydown', (e) => {
  // 焦点在终端里时所有键都归 xterm（Ctrl+A 是行首，不是全选目录）
  const ae = document.activeElement;
  if (ae && ae.closest && ae.closest('.termdock')) return;
  if (e.key === '/' && document.activeElement !== $('search')) {
    e.preventDefault();
    $('search').focus();
    $('search').select();
    return;
  }
  if (e.key === 'Escape') {
    if (S.brief) {
      S.brief = null;
      render();
      return;
    }
    if (S.deep) {
      S.deep = null;
      render();
      return;
    }
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

// 启动：先扫目录，再按上次的布局把终端铺回来（示例模式不恢复）
load(false).then(() => termRestoreLayout());
