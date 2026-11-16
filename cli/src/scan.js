'use strict';
// 扫描 codebuddy / claude 的会话历史，还原出「上次在哪些目录干活」。
// 关键点：会话 id 和 cwd 都能从历史文件里拿到，所以不需要用户手工维护项目清单。

const fs = require('fs');
const path = require('path');
const os = require('os');

const HOME = os.homedir();

// 只看文件头部：会话文件可能几 MB，全读没必要，cwd 就在前几条消息里
const SNIFF_BYTES = 64 * 1024;

function statSafe(p) {
  try {
    return fs.statSync(p);
  } catch {
    return null;
  }
}

function isDir(p) {
  const st = statSafe(p);
  return !!st && st.isDirectory();
}

// jsonl 里每条消息都带 cwd，取第一条能读到的
function sniffCwd(file) {
  const st = statSafe(file);
  if (!st || st.size === 0) return '';
  const len = Math.min(st.size, SNIFF_BYTES);
  const buf = Buffer.alloc(len);
  let fd = null;
  try {
    fd = fs.openSync(file, 'r');
    fs.readSync(fd, buf, 0, len, 0);
  } catch {
    return '';
  } finally {
    if (fd !== null) {
      try { fs.closeSync(fd); } catch { /* 忽略 */ }
    }
  }
  const text = buf.toString('utf8');
  for (const line of text.split('\n')) {
    if (!line.includes('"cwd"')) continue;
    const m = line.match(/"cwd"\s*:\s*"((?:[^"\\]|\\.)*)"/);
    if (!m) continue;
    try {
      const v = JSON.parse('"' + m[1] + '"');  // 还原 \\ 转义
      if (v) return v;
    } catch {
      return m[1];
    }
  }
  return '';
}

function listJsonl(dir) {
  const out = [];
  let names = [];
  try {
    names = fs.readdirSync(dir);
  } catch {
    return out;
  }
  for (const f of names) {
    if (!f.endsWith('.jsonl')) continue;
    const full = path.join(dir, f);
    const st = statSafe(full);
    if (!st || !st.isFile()) continue;
    out.push({ file: full, sessionId: f.replace(/\.jsonl$/, ''), mtime: st.mtimeMs });
  }
  return out;
}

// claude 的目录名是绝对路径把非字母数字全换成 '-'，不可逆，
// 所以用 ~/.claude.json 里的真实路径反算编码来对表
function encodeProjectDir(p) {
  return String(p).replace(/[^a-zA-Z0-9]/g, '-');
}

function claudeCwdMap() {
  const map = new Map();  // 小写编码 -> 真实路径
  const cfg = path.join(HOME, '.claude.json');
  if (!fs.existsSync(cfg)) return map;
  let doc;
  try {
    doc = JSON.parse(fs.readFileSync(cfg, 'utf8'));
  } catch {
    return map;  // 配置很大或损坏，退回只靠 jsonl 嗅探
  }
  for (const p of Object.keys(doc.projects || {})) {
    map.set(encodeProjectDir(p).toLowerCase(), p);
  }
  return map;
}

function scanCodebuddy() {
  const root = path.join(HOME, '.codebuddy', 'projects');
  const rows = [];
  if (!isDir(root)) return rows;
  for (const d of fs.readdirSync(root)) {
    const full = path.join(root, d);
    if (!isDir(full)) continue;
    for (const it of listJsonl(full)) {
      const cwd = sniffCwd(it.file);
      if (!cwd) continue;
      rows.push({ tool: 'codebuddy', cwd, sessionId: it.sessionId, mtime: it.mtime });
    }
  }
  return rows;
}

function scanClaude() {
  const root = path.join(HOME, '.claude', 'projects');
  const rows = [];
  if (!isDir(root)) return rows;
  const enc = claudeCwdMap();
  for (const d of fs.readdirSync(root)) {
    const full = path.join(root, d);
    if (!isDir(full)) continue;
    const mapped = enc.get(d.toLowerCase()) || '';
    for (const it of listJsonl(full)) {
      const cwd = mapped || sniffCwd(it.file);
      if (!cwd) continue;
      rows.push({ tool: 'claude', cwd, sessionId: it.sessionId, mtime: it.mtime });
    }
  }
  return rows;
}

// 每个 (目录, CLI) 只留最近一次会话，再按目录聚合成工作区
function groupByWorkspace(rows, opts = {}) {
  const exclude = opts.exclude || [];
  const latest = new Map();  // `${cwd.toLowerCase()}|${tool}` -> row

  for (const r of rows) {
    const cwd = path.normalize(r.cwd);
    if (exclude.some((re) => re.test(cwd))) continue;
    if (!isDir(cwd)) continue;  // 目录没了就别再拉起来
    const key = cwd.toLowerCase() + '|' + r.tool;
    const prev = latest.get(key);
    if (!prev || r.mtime > prev.mtime) latest.set(key, { ...r, cwd });
  }

  const byCwd = new Map();
  for (const r of latest.values()) {
    const key = r.cwd.toLowerCase();
    let ws = byCwd.get(key);
    if (!ws) {
      ws = { cwd: r.cwd, tools: {}, lastActive: 0, sessionCount: 0 };
      byCwd.set(key, ws);
    }
    ws.tools[r.tool] = r;
    ws.lastActive = Math.max(ws.lastActive, r.mtime);
  }

  const list = [...byCwd.values()];
  // 同一目录可能因为大小写差异出现两份，合并时取 cwd 更"新"的那个写法
  list.sort((a, b) => b.lastActive - a.lastActive);
  return list;
}

function scanAll(opts = {}) {
  const rows = [...scanCodebuddy(), ...scanClaude()];
  return { rows, workspaces: groupByWorkspace(rows, opts) };
}

module.exports = { scanAll, scanCodebuddy, scanClaude, groupByWorkspace, sniffCwd, encodeProjectDir };
