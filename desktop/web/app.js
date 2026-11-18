'use strict';

const $ = (id) => document.getElementById(id);
const listEl = $('list');

// demo.js 检测到 ?demo=1 时会挂上 window.CR_DEMO，此时所有数据来自它，
// 不扫本机历史、不调拉起接口，纯粹用来截图和预览界面
const DEMO = !!window.CR_DEMO;

// --skin xx 启动参数只用于截图和脚本：地址栏带 #skin= 时强制这套皮肤，
// 不写回 settings.json，免得截个图把用户自己的主题改了
const FORCED_SKIN = ((location.hash.match(/[#&]skin=([a-z0-9]+)/i) || [])[1] || '').toLowerCase();

// --only <目录> 会以 #only=<urlencoded> 传进来：这扇窗只服务一个工作区。
// 大目录名里全是中文和空格，所以必须 decode 一次
const ONLY_INIT = (() => {
  const m = location.hash.match(/[#&]only=([^&]*)/);
  if (!m) return '';
  try {
    return decodeURIComponent(m[1]);
  } catch (e) {
    return '';
  }
})();
let onlyCwd = ONLY_INIT;

// 收藏 = 置顶：集合里存 "tool:sid"，跨窗口共享（后端一份 favorites.json）
const FAV = new Set();
const favKey = (s) => `${s.tool}:${s.sessionId}`;
function isFav(s) { return FAV.has(favKey(s)); }

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
  agents: null,   // 本机装了哪些 agent（/api/agents）
  busy: false,
  deep: null,   // 全文搜索结果：null 表示当前是普通列表
  brief: null,  // 本地模型复盘：null 表示没在这个视图
  statsView: null,  // token 统计：null 表示没在这个视图
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
    applyFavOrder();
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
    applyFavOrder();  // 收藏置顶：每次扫描后的新对象都要重排
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
const SKINS = ['neon', 'amber', 'win98', 'xp'];
const SKIN_NAME = {
  neon: '80s 霓虹 CRT', amber: '70s 琥珀终端', win98: 'Windows 98', xp: 'Windows XP Luna',
};

// 每家 CLI 一个色标，卡片标签和会话行的圆点共用
const TOOL_CLS = { claude: 'cl', codex: 'cx', kimi: 'km', codebuddy: 'cb' };

function applyTheme(name) {
  const skin = SKINS.includes(name) ? name : 'neon';
  S.settings.theme = skin;
  document.documentElement.dataset.skin = skin;
  const sel = $('set-theme');
  if (sel && sel.value !== skin) sel.value = skin;
  const btn = $('theme-btn');
  if (btn) {
    const next = SKINS[(SKINS.indexOf(skin) + 1) % SKINS.length];
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
    if (onlyCwd && !samePath(w.cwd, onlyCwd)) return false;
    if ($('opt-fav') && $('opt-fav').checked && !(w.sessions || []).some(isFav)) return false;
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
  const cls = TOOL_CLS[s.tool] || 'cb';
  const extra = index >= INLINE_SESSIONS ? ' sess-extra' : '';
  const title = s.title || '（没有标题的会话）';
  const fav = isFav(s);
  return `
    <div class="sess-row${extra}${fav ? ' fav' : ''}" data-enter="1"
         data-cwd="${esc(ws.cwd)}" data-tool="${esc(s.tool)}"
         data-sid="${esc(s.sessionId)}" data-title="${esc(title)}"
         title="进入这个对话：${esc(title)}">
      <span class="tool-dot ${cls}"></span>
      <span class="sess-star${fav ? ' on' : ''}" data-fav="1"
            data-tool="${esc(s.tool)}" data-sid="${esc(s.sessionId)}"
            data-cwd="${esc(ws.cwd)}" data-title="${esc(title)}"
            title="${fav ? '取消收藏' : '收藏并置顶'}">★</span>
      <span class="sess-title">${esc(title)}</span>
      <span class="sess-when">${esc(shortTime(s.mtime))}</span>
      <span class="sess-exp" data-export="1" data-cwd="${esc(ws.cwd)}" data-tool="${esc(s.tool)}"
            data-sid="${esc(s.sessionId)}" data-title="${esc(title)}"
            title="导出成 Markdown">导出</span>
      <span class="sess-go">进入</span>
    </div>`;
}

function itemHtml(w, index) {
  const key = w.cwd.toLowerCase();
  const no = String(index + 1).padStart(2, '0');
  const on = S.sel.has(key);
  const open = S.expanded.has(key);
  const showAll = S.showAll.has(key);
  const allSessions = w.sessions || [];
  const favCount = allSessions.filter(isFav).length;
  const onlyFav = $('opt-fav') && $('opt-fav').checked;
  const sessions = onlyFav ? allSessions.filter(isFav) : allSessions;
  const latest = sessions[0];

  // 一个目录下两个 CLI 混着用，标签按 tool 去重
  const tools = [...new Set(sessions.map((s) => s.tool))];
  const chips = tools.map((t) => {
    const cls = TOOL_CLS[t] || 'cb';
    return `<span class="tool-chip ${cls}"><i></i>${esc(t)}</span>`;
  }).join('');

  // 一个目录有多条会话时，给一个「一次全开」的入口；上限由终端形态决定
  const batchCap = S.settings.termMode === 'external' ? 8 : 4;
  const batchRow = sessions.length >= 2
    ? `<div class="batch-row"><button class="batch-btn" data-batch="${esc(key)}">
         ⚡ 一次恢复最近 ${Math.min(batchCap, sessions.length)} 个会话
       </button></div>`
    : '';
  const rest = sessions.length - INLINE_SESSIONS;
  const moreRow = rest > 0
    ? `<div class="more-row"><button class="more-btn" data-more="${esc(key)}">
         ${showAll ? '收起更早的对话' : `还有 ${rest} 个更早的对话`}
       </button></div>`
    : '';

  const panel = sessions.length
    ? `<div class="sess-panel">
         ${batchRow}
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
          ${w.added ? '<span class="ws-new" title="手动登记的工作空间">新</span>' : ''}
          <span class="ws-path" title="${esc(w.cwd)}"><bdi>${esc(w.cwd)}</bdi></span>
        </div>
        <div class="ws-sub">
          ${chips}
          <span class="dot-sep">·</span>
          ${favCount ? `<span class="ws-fav" title="已收藏 ${favCount} 个会话">★ ${favCount}</span><span class="dot-sep">·</span>` : ''}
          ${w.added
            ? `<span class="ws-count">还没开始</span><span class="dot-sep">·</span><span class="ws-when">手动登记</span>`
            : `<span class="ws-count">${w.sessionCount || sessions.length} 个对话</span>
               <span class="dot-sep">·</span>
               <span class="ws-when">${esc(relTime(w.lastActive))}</span>`}
          ${latest ? `<span class="ws-latest">最近「${esc(latest.title || '无标题')}」</span>` : ''}
        </div>
      </div>
      <div class="ws-side">
        <button class="mini-btn new${w.added ? ' go' : ''}" data-newsess="${esc(w.cwd)}"
                title="在这个目录开一个全新的会话（不续接）">新会话</button>
        ${w.added ? '' : `<button class="mini-btn brief" data-brief="${esc(w.cwd)}" title="让本地模型复盘这个目录最近一次会话">复盘</button>`}
        <button class="mini-btn" data-window="${esc(w.cwd)}" title="把这个目录单独开一个窗口">窗口</button>
        <button class="mini-btn" data-reveal="${esc(w.cwd)}" title="在资源管理器打开">定位</button>
        ${w.added
          ? `<button class="mini-btn del" data-delws="${esc(w.cwd)}" title="只是从列表移除，不删目录">移除</button>`
          : `<button class="mini-btn go" data-open="${esc(key)}" title="进入这个目录最近的一个对话">进入</button>`}
        ${w.added ? '' : `<span class="caret" title="${open ? '收起' : '展开对话列表'}"></span>`}
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

// ---------------- 收藏（置顶） ----------------
// 收藏只影响排序：同一个目录里收藏的排前面；「只看收藏」把它们筛出来单独看。

function applyFavOrder() {
  for (const w of S.all) {
    const ss = w.sessions;
    if (!ss || ss.length < 2) continue;
    ss.sort((a, b) => {
      const fa = isFav(a) ? 1 : 0;
      const fb = isFav(b) ? 1 : 0;
      if (fa !== fb) return fb - fa;
      return (b.mtime || 0) - (a.mtime || 0);
    });
  }
}

async function loadFavorites() {
  // 示例模式也读（只读不写）：否则截图里收藏状态永远是空的
  try {
    const r = await fetch('/api/favorites').then((x) => x.json());
    FAV.clear();
    for (const it of r.items || []) FAV.add(`${it.tool}:${it.sessionId}`);
    applyFavOrder();
    if (S.all.length) render();  // 首屏启动时目录还没扫出来，别先闪一下空列表
  } catch (e) {
    // 收藏读不到不影响主流程，当作没有收藏
  }
}

async function toggleFav(el) {
  if (DEMO) {
    toast('示例模式不改收藏');
    return;
  }
  const ds = el.dataset;
  const key = `${ds.tool}:${ds.sid}`;
  const was = FAV.has(key);
  if (was) FAV.delete(key);
  else FAV.add(key);
  applyFavOrder();
  render();

  try {
    const r = await fetch('/api/favorites/toggle', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ tool: ds.tool, sid: ds.sid, cwd: ds.cwd, title: ds.title }),
    }).then((x) => x.json());
    if (!r.ok) throw new Error(r.error || '收藏失败');
    // 服务端才是准的（另一个窗口可能刚改过），按返回值纠正一次
    if (!!r.on !== !was) {
      if (r.on) FAV.add(key);
      else FAV.delete(key);
      applyFavOrder();
      render();
    }
    toast(r.on ? '已收藏并置顶' : '已取消收藏');
  } catch (e) {
    if (was) FAV.add(key);
    else FAV.delete(key);
    applyFavOrder();
    render();
    toast(e.message, true);
  }
}

// 一键恢复一个目录的会话：内嵌面板最多 4 个窗格，外部终端一次开 8 个标签
async function batchOpen(key) {
  const ws = S.byId.get(key);
  if (!ws) return;
  const embedded = S.settings.termMode !== 'external';
  const cap = embedded ? 4 : 8;
  // 尊重当前视图：勾了「只看收藏」就只恢复收藏的，别偷偷把别的会话也拉起来
  const onlyFav = $('opt-fav') && $('opt-fav').checked;
  const pool = onlyFav ? (ws.sessions || []).filter(isFav) : (ws.sessions || []);
  const list = pool.slice(0, cap);
  if (!list.length) return;

  const items = list.map((s) => ({
    cwd: ws.cwd, tool: s.tool, sessionId: s.sessionId, title: s.title,
  }));
  const ok = await launchItems(items, items.length, true);
  if (!ok) return;
  const rest = pool.length - items.length;
  toast(`已恢复 ${items.length} 个会话` +
    (rest > 0 ? `，还有 ${rest} 个更早的没开（${embedded ? '面板上限 4 个' : '一次最多 8 个'}）` : ''));
}

// ---------------- 新会话 ----------------
// 和「进入」不同：这里不续接任何历史，就是在该目录起一个新会话。
// 一个目录可能同时用几家 CLI，所以先让人选；默认高亮这家最近用过的那个。

const TOOL_LIST = ['claude', 'codebuddy', 'codex', 'kimi'];

function lastToolFor(cwd) {
  const ws = S.byId.get(String(cwd).toLowerCase());
  const s = ws && (ws.sessions || [])[0];
  return (s && s.tool) || S.settings.defaultTool || 'claude';
}

function showCliMenu(anchor, cwd) {
  const menu = $('cli-menu');
  const def = lastToolFor(cwd);
  menu.innerHTML = TOOL_LIST.map((t) =>
    `<button data-cli="${t}"${t === def ? ' class="on"' : ''}>${t}</button>`).join('') +
    `<div class="cli-menu-hint">新会话 · ${esc(baseOf(cwd))}</div>`;
  menu.dataset.cwd = cwd;
  menu.hidden = false;
  const r = anchor.getBoundingClientRect();
  const w = menu.offsetWidth || 132;
  const h = menu.offsetHeight || 120;
  menu.style.left = Math.max(8, Math.min(window.innerWidth - w - 8, r.right - w)) + 'px';
  menu.style.top = Math.min(window.innerHeight - h - 8, r.bottom + 5) + 'px';
}

function hideCliMenu() {
  const m = $('cli-menu');
  if (m) m.hidden = true;
}

async function newSession(cwd, tool) {
  if (!cwd) return;
  const t = TOOL_LIST.indexOf(tool) >= 0 ? tool : lastToolFor(cwd);
  if (DEMO) {
    toast(`示例模式：这里会开一个 ${t} 新会话`);
    return;
  }

  if (S.settings.termMode === 'external') {
    try {
      const r = await fetch('/api/open', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          items: [{ cwd, tool: t, sessionId: '', title: '新会话' }],
          // 新会话不能用 -c：那是「续接最近一个」，正好和这里相反
          useContinue: false,
          window: Number(S.settings.window) || 0,
        }),
      }).then((x) => x.json());
      if (r.ok) toast(`已在 Windows Terminal 打开 ${t}`);
      else toast(r.error || '打开失败', true);
    } catch (e) {
      toast('打开失败：' + e.message, true);
    }
    return;
  }

  // ephemeral：还没说过话的窗格不写进布局，免得重启时又拉起一堆空会话
  const pane = await termAddPane({
    cwd, cmd: t, title: `${t} · 新会话`, tool: t, sessionId: '', ephemeral: true,
  });
  if (pane) toast(`已在 ${baseOf(cwd)} 开一个 ${t} 新会话`);
}

// ---------------- 新建工作空间 ----------------
// 扫描只能发现「有过会话的目录」；全新的仓库得手动登记，才不会连入口都没有。

async function addWorkspace() {
  const cwd = $('ws-path').value.trim();
  if (!cwd) {
    toast('先填一个目录', true);
    return;
  }
  if (DEMO) {
    toast('示例模式不会真的添加');
    return;
  }

  try {
    const r = await fetch('/api/workspaces', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ cwd, create: $('ws-create').checked }),
    }).then((x) => x.json());
    if (!r.ok) {
      toast(r.error || '添加失败', true);
      return;
    }

    $('ws-form').hidden = true;
    $('ws-path').value = '';
    $('ws-create').checked = false;
    toast(r.duplicate ? '这个工作空间已经在了' : `已添加 ${r.name}`);
    await load(true);
    if (r.cwd) S.expanded.add(r.cwd.toLowerCase());
    render();
  } catch (e) {
    toast('添加失败：' + e.message, true);
  }
}

async function removeWorkspace(cwd) {
  if (DEMO) {
    toast('示例模式不改动');
    return;
  }
  try {
    const r = await fetch('/api/workspaces?cwd=' + encodeURIComponent(cwd), { method: 'DELETE' })
      .then((x) => x.json());
    if (!r.ok) {
      toast('移除失败', true);
      return;
    }
    toast('已从列表移除（目录没动）');
    await load(true);
  } catch (e) {
    toast('移除失败：' + e.message, true);
  }
}

// ---------------- 工作区窗口 ----------------
// 一个目录一扇窗：--only 把窗口锁在这个目录，卡片上的「窗口」按钮开新的一扇。
// 同一目录再次点击会找到已经开着的那扇，而不是叠出第三个。

// 目录比较一律先归一：一个用正斜杠一个用反斜杠，不做这层就会「明明有却空了」
function samePath(a, b) {
  const n = (p) => String(p || '').toLowerCase().replace(/\//g, '\\');
  return n(a) === n(b);
}

function onlyName(p) {
  const s = String(p || '').replace(/[\\/]+$/, '');
  const i = Math.max(s.lastIndexOf('\\'), s.lastIndexOf('/'));
  return i >= 0 ? s.slice(i + 1) : s;
}

function initOnlyChip() {
  if (!onlyCwd) return;
  const chip = $('only-chip');
  if (!chip) return;
  $('only-name').textContent = onlyName(onlyCwd);
  chip.hidden = false;
}

// 等目录扫出来再展开：--only 传进来的路径可能和会话里的大小写/斜杠不一致
function expandOnly() {
  if (!onlyCwd) return;
  const hit = S.all.find((w) => samePath(w.cwd, onlyCwd));
  if (hit) S.expanded.add(hit.cwd.toLowerCase());
}

async function openWorkspaceWindow(cwd) {
  if (DEMO) {
    toast('示例模式不会真的开窗口');
    return;
  }
  try {
    const r = await fetch('/api/window/new', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ cwd }),
    }).then((x) => x.json());
    if (!r.ok) throw new Error(r.error || '打不开新窗口');
    toast(r.focused ? '这个工作区已经有窗口，已切过去' : '已打开工作区窗口');
  } catch (e) {
    toast(e.message, true);
  }
}

$('only-clear').addEventListener('click', () => {
  onlyCwd = '';
  $('only-chip').hidden = true;
  render();
});

// ---------------- 开机自启 ----------------
async function loadAutoStart() {
  if (DEMO || !$('set-auto')) return;
  try {
    const r = await fetch('/api/autostart').then((x) => x.json());
    $('set-auto').value = r.on ? '1' : '0';
  } catch (e) {
    /* 读不到就保持默认，不影响别的设置 */
  }
}

// ---------------- 导出 Markdown ----------------
// 一个会话一个 .md，落在「文档\cli-resume」：标题、目录、时间范围、逐轮对话，
// 工具调用收成引用行，读起来像一份稿子而不是日志。
async function exportSession(ds) {
  if (DEMO) {
    toast('示例模式没有真实会话可导出');
    return;
  }
  toast(`正在导出「${ds.title || '会话'}」…`);
  try {
    const r = await fetch('/api/export', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(ds),
    }).then((x) => x.json());
    if (!r.ok) throw new Error(r.error || '导出失败');
    const name = String(r.path).split(/[\\/]/).pop();
    // 不自动弹资源管理器：连续导出几个会话时会被窗口糊脸。路径说全，自己能找
    toast(`已导出 ${r.turns} 条 → ${name}（文档\\cli-resume）`);
  } catch (e) {
    toast(e.message, true);
  }
}

// ---------------- Token / 成本统计 ----------------
function fmtTok(n) {
  const v = Number(n) || 0;
  if (v >= 1e9) return (v / 1e9).toFixed(2) + 'B';
  if (v >= 1e6) return (v / 1e6).toFixed(v >= 1e8 ? 0 : 1) + 'M';
  if (v >= 1e3) return (v / 1e3).toFixed(v >= 1e5 ? 0 : 1) + 'K';
  return String(v);
}

function fmtNum(n) {
  const v = Number(n) || 0;
  return v >= 1e4 ? (v / 1e3).toFixed(0) + 'K' : String(v);
}

function openStats() {
  if (S.statsView) {
    S.statsView = null;
    render();
    return;
  }
  S.statsView = { loading: true, data: null, days: 30, tries: 0 };
  render();
  fetchStats(false);
}

// 示例模式编一份形状真实的用量，截图和首次体验都能看懂这张图在说什么
function demoStats(days) {
  const daily = [];
  let tIn = 0, tOut = 0, tCr = 0, tCalls = 0;
  for (let i = 0; i < days; i++) {
    const wave = 0.6 + 0.4 * Math.sin(i / 2.7);
    const base = 180e6 * wave * (i % 7 === 0 ? 0.35 : 1);
    const inTok = Math.round(base * 0.55);
    const outTok = Math.round(base * 0.018);
    const cr = Math.round(base * 0.44);
    tIn += inTok; tOut += outTok; tCr += cr; tCalls += Math.round(160 * wave);
    daily.push({ d: '', in: inTok, out: outTok, cacheRead: cr, tokens: inTok + outTok + cr, calls: 0 });
  }
  return {
    ok: true, ready: true, files: 21, totalFiles: 21, days,
    total: { in: tIn, out: tOut, cacheRead: tCr, cacheWrite: 0, calls: tCalls,
      tokens: tIn + tOut + tCr, cost: (tIn * 3 + tOut * 15 + tCr * 0.3) / 1e6 },
    daily,
    models: [
      { model: 'claude-sonnet-4-5', in: Math.round(tIn * 0.6), out: Math.round(tOut * 0.6),
        cacheRead: Math.round(tCr * 0.6), calls: Math.round(tCalls * 0.6), priced: true,
        cost: (tIn * 0.6 * 3 + tOut * 0.6 * 15 + tCr * 0.6 * 0.3) / 1e6,
        tokens: Math.round((tIn + tOut + tCr) * 0.6) },
      { model: 'kimi-for-coding', in: Math.round(tIn * 0.4), out: Math.round(tOut * 0.4),
        cacheRead: Math.round(tCr * 0.4), calls: Math.round(tCalls * 0.4), priced: false,
        cost: 0, tokens: Math.round((tIn + tOut + tCr) * 0.4) },
    ],
    top: [
      { title: '把首屏的 SSR 渲染改成流式输出', cwd: 'D:\\work\\acme-portal', tool: 'claude',
        sessionId: 'demo-1', tokens: Math.round((tIn + tOut + tCr) * 0.18), cost: 0 },
      { title: 'Bulk Export 评估与 16 个月回填', cwd: 'D:\\work\\blogo', tool: 'codebuddy',
        sessionId: 'demo-2', tokens: Math.round((tIn + tOut + tCr) * 0.12), cost: 0 },
      { title: 'WebView2 单实例与截图漂移', cwd: 'D:\\work\\cli-resume', tool: 'claude',
        sessionId: 'demo-3', tokens: Math.round((tIn + tOut + tCr) * 0.07), cost: 0 },
    ],
    unpriced: ['kimi-for-coding'],
  };
}

async function fetchStats(retry) {
  const v = S.statsView;
  if (!v) return;
  if (!retry) {
    v.loading = true;
    render();
  }
  if (DEMO) {
    v.data = demoStats(v.days);
    v.loading = false;
    render();
    return;
  }
  try {
    const r = await fetch(`/api/stats?days=${v.days}`).then((x) => x.json());
    if (S.statsView !== v) return;
    v.data = r;
    v.loading = false;
    render();
    // 索引还在建：过一会儿自己再拉一次，数字会往上走
    if (!r.ready && (v.tries || 0) < 20) {
      v.tries = (v.tries || 0) + 1;
      setTimeout(() => {
        if (S.statsView === v) fetchStats(true);
      }, 1500);
    }
  } catch (e) {
    if (S.statsView !== v) return;
    v.loading = false;
    v.error = e.message;
    render();
  }
}

function renderStats() {
  const v = S.statsView;
  const d = v.data || {};
  const t = d.total || {};
  const days = v.days;

  const head = `
    <div class="deep-head">
      <span class="deep-q">Token 统计</span>
      <span class="deep-sub">${d.ready
        ? `${d.files} 个会话 · ${fmtTok(t.tokens || 0)} token`
        : `索引 ${d.files || 0}/${d.totalFiles || 0} · 先显示已有部分`}</span>
      <span class="stat-ranges">
        ${[7, 30, 90].map((n) =>
          `<button class="mini-btn${n === days ? ' on' : ''}" data-days="${n}">${n} 天</button>`).join('')}
      </span>
      <button class="mini-btn" data-back="1">返回列表</button>
    </div>`;

  if (v.error) {
    listEl.innerHTML = head + `<div class="empty"><h3>统计没拿到</h3><p>${esc(v.error)}</p></div>`;
    $('meta').textContent = '统计失败';
    updateBar();
    return;
  }

  if (!d.daily) {
    listEl.innerHTML = head + `<div class="empty"><h3>正在汇总 token…</h3>
      <p>用量是从会话索引里直接读的，不用重扫历史文件。</p></div>`;
    $('meta').textContent = '统计中…';
    updateBar();
    return;
  }

  const daily = d.daily;
  const max = Math.max(1, ...daily.map((x) => x.tokens || 0));
  const bars = daily.map((x) => {
    const total = x.tokens || 0;
    const h = Math.max(1, Math.round((total / max) * 100));
    const outH = total ? (x.out || 0) / total * h : 0;
    const cacheH = total ? (x.cacheRead || 0) / total * h : 0;
    const inH = Math.max(0, h - outH - cacheH);
    return `<div class="stat-bar" title="${esc(x.d || '')}：${fmtTok(total)} token">
      <i class="seg out" style="height:${outH.toFixed(2)}%"></i>
      <i class="seg cache" style="height:${cacheH.toFixed(2)}%"></i>
      <i class="seg in" style="height:${inH.toFixed(2)}%"></i>
    </div>`;
  }).join('');

  const cost = t.cost || 0;
  const cards = [
    ['输入', fmtTok(t.in || 0), '每轮重新送达的上下文'],
    ['输出', fmtTok(t.out || 0), '模型生成的正文'],
    ['缓存读', fmtTok(t.cacheRead || 0), '命中缓存的输入'],
    ['调用', fmtNum(t.calls || 0), '一次 API 请求算一次'],
    ['估算成本', cost > 0 ? '$' + cost.toFixed(2) : '—',
      cost > 0 ? '按 Anthropic 公开单价' : '本机模型无公开单价'],
  ].map(([k, val, hint]) => `
    <div class="stat-card"><span class="stat-k">${k}</span>
      <b class="stat-v">${val}</b><span class="stat-h">${hint}</span></div>`).join('');

  const modelHead = `
    <div class="stat-row head">
      <span class="stat-m">模型</span><span class="stat-n">调用</span>
      <span class="stat-n">输入</span><span class="stat-n">输出</span>
      <span class="stat-n">缓存读</span><span class="stat-n">成本</span>
    </div>`;
  const modelRows = (d.models || []).map((m) => `
    <div class="stat-row">
      <span class="stat-m" title="${esc(m.model)}">${esc(m.model)}</span>
      <span class="stat-n">${fmtNum(m.calls)}</span>
      <span class="stat-n">${fmtTok(m.in)}</span>
      <span class="stat-n">${fmtTok(m.out)}</span>
      <span class="stat-n">${fmtTok(m.cacheRead)}</span>
      <span class="stat-n cost">${m.priced ? '$' + (m.cost || 0).toFixed(2) : '—'}</span>
    </div>`).join('');

  const topRows = (d.top || []).map((x) => `
    <div class="stat-sess" data-enter="1" data-cwd="${esc(x.cwd || '')}"
         data-tool="${esc(x.tool || '')}" data-sid="${esc(x.sessionId || '')}"
         data-title="${esc(x.title || '')}" title="进入这个会话">
      <span class="stat-sess-t">${esc(x.title || '（无标题）')}</span>
      <span class="stat-sess-c">${esc(x.cwd || '')}</span>
      <span class="stat-n">${fmtTok(x.tokens)}</span>
      <span class="sess-exp" data-export="1" data-cwd="${esc(x.cwd || '')}"
            data-tool="${esc(x.tool || '')}" data-sid="${esc(x.sessionId || '')}"
            data-title="${esc(x.title || '')}">导出</span>
    </div>`).join('');

  const unpriced = (d.unpriced || []).filter(Boolean);
  const note = cost > 0
    ? `成本按 Anthropic 公开单价换算（Sonnet $3 / $15、Opus $15 / $75、Haiku $1 / $5，每百万 token），仅供参考。`
    : `当前会话都来自订阅制或未公开单价的模型，只统计 token，不折算成本。`;

  const body = `<div class="stats-wrap">
    <div class="stat-cards">${cards}</div>
    <div class="stat-chart">
      <div class="stat-bars">${bars}</div>
      <div class="stat-legend">
        <span><i class="seg in"></i>输入</span>
        <span><i class="seg out"></i>输出</span>
        <span><i class="seg cache"></i>缓存读</span>
        <span class="stat-range-note">${esc(d.from || '')} → ${esc(d.to || '')}</span>
      </div>
    </div>
    <div class="stat-panel">
      <h3>按模型</h3>
      ${modelHead}${modelRows || '<p class="stat-empty">还没有用量记录</p>'}
    </div>
    <div class="stat-panel">
      <h3>最费 token 的会话</h3>
      ${topRows || '<p class="stat-empty">还没有用量记录</p>'}
    </div>
    <p class="stat-note">${note}${unpriced.length ? ` 未计价：${esc(unpriced.join('、'))}` : ''}</p>
  </div>`;

  listEl.innerHTML = head + body;
  $('meta').textContent = `近 ${days} 天 · ${fmtTok(t.tokens || 0)} token`;
  updateBar();
}

function render() {
  if (S.statsView) {
    renderStats();
    return;
  }
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
         再回来点右上角的刷新。</p>
         <button class="primary small" data-addws>＋ 直接新建一个工作空间</button></div>`;
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
    S.statsView = null;
    render();
    return;
  }

  // 统计页的时间范围
  const dbtn = e.target.closest('[data-days]');
  if (dbtn && S.statsView) {
    const n = Number(dbtn.dataset.days);
    if (n) {
      S.statsView.days = n;
      S.statsView.tries = 0;
      fetchStats(false);
    }
    return;
  }

  // 收藏：点星切换，立刻重排
  const star = e.target.closest('[data-fav]');
  if (star) {
    e.stopPropagation();
    toggleFav(star);
    return;
  }

  // 批量恢复一个目录的会话
  const batch = e.target.closest('[data-batch]');
  if (batch) {
    e.stopPropagation();
    batchOpen(batch.dataset.batch);
    return;
  }

  // 导出：按会话恢复成 Markdown，落在「文档\cli-resume」
  const exp = e.target.closest('[data-export]');
  if (exp) {
    e.stopPropagation();
    exportSession({
      cwd: exp.dataset.cwd, tool: exp.dataset.tool,
      sid: exp.dataset.sid, title: exp.dataset.title,
    });
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

  // 新会话：先选 CLI，再在该目录起一个不带 --resume 的会话
  const newBtn = e.target.closest('[data-newsess]');
  if (newBtn) {
    e.stopPropagation();
    showCliMenu(newBtn, newBtn.dataset.newsess);
    return;
  }

  // 空白列表里的「新建工作空间」直接展开左栏表单
  if (e.target.closest('[data-addws]')) {
    e.preventDefault();
    $('ws-form').hidden = false;
    $('ws-add').scrollIntoView({ block: 'center' });
    $('ws-path').focus();
    return;
  }

  const delBtn = e.target.closest('[data-delws]');
  if (delBtn) {
    e.stopPropagation();
    removeWorkspace(delBtn.dataset.delws);
    return;
  }

  const winBtn = e.target.closest('[data-window]');
  if (winBtn) {
    e.stopPropagation();
    openWorkspaceWindow(winBtn.dataset.window);
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

async function launchItems(items, note, quiet) {
  if (DEMO) {
    toast('示例模式：不会真的打开终端');
    return false;
  }
  if (!items || !items.length) {
    toast('没有可进入的对话', true);
    return false;
  }
  // 默认走应用内终端；设置里切成 external 才回到 Windows Terminal
  if (S.settings.termMode !== 'external') {
    await termOpenItems(items);
    if (!quiet) {
      toast(items.length === 1
        ? `已在应用内终端打开「${items[0].title || '该对话'}」`
        : `已在终端面板并排打开 ${items.length} 个会话`);
    }
    return true;
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
      if (!quiet) {
        toast(n === 1 ? `已进入「${items[0].title || '该对话'}」` : `已在 Windows Terminal 打开 ${n} 个标签页`);
      }
      plog('open ok ' + (r.command || ''));
      return true;
    }
    toast(r.error || '打开失败', true);
    plog('open fail ' + (r.error || ''));
  } catch (e) {
    toast('打开失败：' + e.message, true);
  }
  return false;
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
// 只看收藏是视图开关，不落盘
$('opt-fav').addEventListener('change', render);

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
  const next = SKINS[(SKINS.indexOf(S.settings.theme) + 1) % SKINS.length];
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
  // 开机自启落在注册表里，不在 settings.json，单独发一次
  if ($('set-auto') && !DEMO) {
    fetch('/api/autostart', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ on: $('set-auto').value === '1' }),
    }).catch(() => {});
  }
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
  if (!box) return;
  const r = S.roots || {};
  // ok = 已接入；mid = 只检测到、暂不支持恢复
  const row = (state, text) => {
    const cls = state === 'mid' ? 'mid' : (state ? 'ok' : 'bad');
    const glyph = state === 'mid' ? '·' : (state ? '✓' : '×');
    return `<div class="doc-row"><span class="mark ${cls}">${glyph}</span>` +
      `<span class="txt">${esc(text)}</span></div>`;
  };

  const lines = [];
  const agents = S.agents || [];
  if (agents.length) {
    for (const a of agents) {
      const full = a.support === 'full';
      lines.push(row(full ? true : 'mid',
        `${a.name}：${full ? '' : '仅检测 · '}${a.sessions} 个会话`));
    }
  } else {
    lines.push(row(!!r.codebuddyFound, 'codebuddy：' + (r.codebuddy || '')));
    lines.push(row(!!r.claudeFound, 'claude：' + (r.claude || '')));
  }
  lines.push(row(true, `${S.all.length} 个可用目录 / ${S.stats.totalSessions || 0} 个对话`));
  box.innerHTML = lines.join('');
}

async function loadAgents() {
  if (DEMO) {
    // 示例模式给一份和示例数据量级对得上的清单，别把本机真实会话数混进截图
    S.agents = [
      { id: 'codebuddy', name: 'CodeBuddy CLI', support: 'full', sessions: 20 },
      { id: 'claude', name: 'Claude Code', support: 'full', sessions: 7 },
      { id: 'codex', name: 'Codex CLI', support: 'full', sessions: 6 },
      { id: 'kimi', name: 'Kimi CLI', support: 'detect', sessions: 2 },
    ];
    renderDoctor();
    return;
  }
  try {
    const r = await fetch('/api/agents').then((x) => x.json());
    S.agents = r.agents || [];
    renderDoctor();
  } catch (e) {
    // 探测失败就退回只看 codebuddy/claude 两行
  }
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
  // 70s 单色屏：只有琥珀的亮暗，红绿蓝都退化成深浅不同的橙
  amber: {
    background: '#080500', foreground: '#ffc247',
    cursor: '#ffd166', cursorAccent: '#080500',
    selectionBackground: 'rgba(255, 176, 0, .28)',
    black: '#1a1206', red: '#ff7a00', green: '#ffb000', yellow: '#ffd166',
    blue: '#d68a00', magenta: '#ff9a3c', cyan: '#ffb000', white: '#ffc247',
    brightBlack: '#70501a', brightRed: '#ff9a3c', brightGreen: '#ffd166',
    brightYellow: '#ffe3a3', brightBlue: '#e0a040', brightMagenta: '#ffb870',
    brightCyan: '#ffd166', brightWhite: '#fff0d0',
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
  xp: {
    background: '#000000', foreground: '#c0c0c0',
    cursor: '#ffffff', cursorAccent: '#000000',
    selectionBackground: 'rgba(49, 106, 197, .55)',
    black: '#000000', red: '#800000', green: '#008000', yellow: '#808000',
    blue: '#000080', magenta: '#800080', cyan: '#008080', white: '#c0c0c0',
    brightBlack: '#808080', brightRed: '#ff0000', brightGreen: '#00ff00',
    brightYellow: '#ffff00', brightBlue: '#0000ff', brightMagenta: '#ff00ff',
    brightCyan: '#00ffff', brightWhite: '#ffffff',
  },
};

const TM = { panes: [], active: null, restoring: false, restored: false };

function termSkin() {
  return TERM_THEMES[S.settings.theme] ? S.settings.theme : 'neon';
}

const TERM_FONT = {
  neon: { family: 'Consolas, "Cascadia Mono", "JetBrains Mono", monospace', size: 12.5 },
  amber: { family: '"Cascadia Mono", Consolas, "Lucida Console", monospace', size: 13 },
  win98: { family: '"Fixedsys", "Lucida Console", Consolas, "SimSun", monospace', size: 13 },
  xp: { family: '"Lucida Console", "Consolas", "SimSun", monospace', size: 12.5 },
};

function termOptions() {
  const skin = termSkin();
  const font = TERM_FONT[skin] || TERM_FONT.neon;
  return {
    theme: TERM_THEMES[skin],
    fontFamily: font.family,
    fontSize: font.size,
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
  // 新开的空会话（ephemeral）不落盘：重启后再拉起一堆没说过话的窗格没有意义
  const keep = TM.panes.filter((p) => !p.ephemeral);
  return {
    visible: !dock.hidden && !dock.classList.contains('collapsed'),
    height: Math.round(dock.getBoundingClientRect().height) || 0,
    active: Math.max(0, keep.indexOf(TM.active)),
    panes: keep.map((p) => ({
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
    ephemeral: !!opt.ephemeral,
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
  if (it.tool === 'codex') {
    // codex 用子命令续接，--last 是它自己的「最近一个」
    if (S.settings.useContinue || !it.sessionId) return 'codex resume --last';
    return `codex resume ${it.sessionId}`;
  }
  // kimi 用 -S：id 是 session_<uuid>，不带 id 会进交互选择
  if (it.tool === 'kimi') {
    if (S.settings.useContinue || !it.sessionId) return 'kimi -c';
    return `kimi -S ${it.sessionId}`;
  }
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

$('stats-btn').addEventListener('click', openStats);

// ---------------- 工作空间表单与 CLI 菜单 ----------------
$('ws-add').addEventListener('click', () => {
  const f = $('ws-form');
  f.hidden = !f.hidden;
  if (!f.hidden) {
    // 左栏高度有限，终端面板开着时表单会落到可视区外
    f.scrollIntoView({ block: 'nearest' });
    $('ws-path').focus();
  }
});

$('ws-cancel').addEventListener('click', () => {
  $('ws-form').hidden = true;
});

$('ws-browse').addEventListener('click', async () => {
  try {
    const r = await fetch('/api/pickdir').then((x) => x.json());
    if (r.ok && r.path) {
      $('ws-path').value = r.path;
      $('ws-create').checked = false;  // 选到的目录一定存在，别再建
    }
  } catch (e) {
    toast('打不开选择框，直接粘路径也行', true);
  }
});

$('ws-ok').addEventListener('click', addWorkspace);

// 路径框里回车 = 添加
$('ws-path').addEventListener('keydown', (e) => {
  if (e.key === 'Enter') {
    e.preventDefault();
    addWorkspace();
  }
});

$('cli-menu').addEventListener('click', (e) => {
  const b = e.target.closest('[data-cli]');
  if (!b) return;
  newSession($('cli-menu').dataset.cwd, b.dataset.cli);
  hideCliMenu();
});

// 点别处、滚动、缩放都关掉菜单，别让它挂在半空
document.addEventListener('mousedown', (e) => {
  const m = $('cli-menu');
  if (!m || m.hidden) return;
  if (e.target.closest('#cli-menu') || e.target.closest('[data-newsess]')) return;
  hideCliMenu();
});
window.addEventListener('resize', hideCliMenu);
window.addEventListener('blur', hideCliMenu);

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

// 新建：优先在当前窗格的目录里开一个同款会话，没有窗格才退回空 shell
$('term-new').addEventListener('click', () => {
  const p = TM.active;
  if (p && p.cwd) {
    newSession(p.cwd, p.tool);
    return;
  }
  const sel = [...S.sel][0];
  if (sel && S.byId.get(sel)) {
    newSession(S.byId.get(sel).cwd, '');
    return;
  }
  termAddPane({ cwd: '', title: 'shell' });
});
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
  // Ctrl+N：在选中的（没有就第一个）工作区里开一个新会话
  if ((e.ctrlKey || e.metaKey) && (e.key === 'n' || e.key === 'N')) {
    const sel = [...S.sel][0];
    const ws = (sel && S.byId.get(sel)) || visibleList()[0];
    if (ws) {
      e.preventDefault();
      newSession(ws.cwd, '');
    }
    return;
  }
  if (e.key === 'Escape') {
    if (!$('cli-menu').hidden) {
      hideCliMenu();
      return;
    }
    if (S.statsView) {
      S.statsView = null;
      render();
      return;
    }
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

// 窗口收进托盘后，装饰动画（CRT 光带、呼吸灯）没有任何意义，
// 但在弱显卡上它们是实打实的合成开销。不可见就停掉，回来再开。
document.addEventListener('visibilitychange', () => {
  document.body.classList.toggle('page-idle', document.hidden);
});

// 启动：先扫目录，再按上次的布局把终端铺回来（示例模式不恢复）
initOnlyChip();
loadAutoStart();
loadAgents();
// 先读收藏再扫目录：首屏就是「收藏在前的顺序」，不会先抖一下
loadFavorites()
  .then(() => load(false))
  .then(() => {
    expandOnly();
    if (onlyCwd) render();  // 展开状态是加在 S.expanded 上的，要重绘一次才看得见
    termRestoreLayout();
  });

// 多个窗口之间同步收藏：切回来时静默对一次，变了才重绘
window.addEventListener('focus', async () => {
  try {
    const r = await fetch('/api/favorites').then((x) => x.json());
    const next = new Set((r.items || []).map((it) => `${it.tool}:${it.sessionId}`));
    if (next.size === FAV.size && [...next].every((k) => FAV.has(k))) return;
    FAV.clear();
    next.forEach((k) => FAV.add(k));
    applyFavOrder();
    render();
  } catch (e) {
    /* 网络抖动忽略 */
  }
});
