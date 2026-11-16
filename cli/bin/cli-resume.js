#!/usr/bin/env node
'use strict';
// cli-resume —— 从 codebuddy / claude 的会话历史里还原上次的工作现场，
// 用 Windows Terminal 多标签一键 --resume 回来。

const fs = require('fs');
const path = require('path');
const readline = require('readline');
const scan = require('../src/scan');
const term = require('../src/term');

const DEFAULTS = {
  limit: 4,           // 默认恢复最近几个工作区
  maxAgeHours: 72,    // 太久没动的目录不自动带上
  both: false,        // 同一目录 codebuddy / claude 都恢复
  continueFlag: false,// 用 -c 让 CLI 自己找最近会话（不指定 id）
  window: 0,          // wt 的 -w：0 = 复用最近窗口
  shell: '',          // 留空自动检测
  exclude: [
    '[\\\\/]Temp[\\\\/]',
    '\\\\system32\\\\',
    '\\\\node_modules\\\\',
  ],
};

function loadConfig() {
  const candidates = [
    path.join(__dirname, '..', 'config.json'),
    path.join(process.env.USERPROFILE || '', '.cli-resume.json'),
  ];
  for (const p of candidates) {
    try {
      if (!fs.existsSync(p)) continue;
      const raw = fs.readFileSync(p, 'utf8').replace(/^\uFEFF/, '');
      return { ...DEFAULTS, ...JSON.parse(raw), _from: p };
    } catch (e) {
      console.error(`[warn] 配置读取失败，用默认值：${p} (${e.message})`);
    }
  }
  return { ...DEFAULTS };
}

function parseArgs(argv) {
  const out = { _: [] };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (!a.startsWith('-')) { out._.push(a); continue; }
    const key = a.replace(/^-+/, '');
    const next = argv[i + 1];
    const bools = ['json', 'all', 'pick', 'both', 'dry-run', 'continue', 'help', 'no-age', 'quiet'];
    if (bools.includes(key)) { out[key] = true; continue; }
    if (next !== undefined && !next.startsWith('-')) { out[key] = next; i++; continue; }
    out[key] = true;
  }
  return out;
}

// 终端里中文占 2 列，直接用 length 对不齐
function dispWidth(s) {
  let w = 0;
  for (const ch of String(s)) {
    const c = ch.codePointAt(0);
    w += (c >= 0x1100 && (
      c <= 0x115f || c === 0x2329 || c === 0x232a ||
      (c >= 0x2e80 && c <= 0xa4cf) || (c >= 0xac00 && c <= 0xd7a3) ||
      (c >= 0xf900 && c <= 0xfaff) || (c >= 0xfe30 && c <= 0xfe6f) ||
      (c >= 0xff00 && c <= 0xff60) || (c >= 0xffe0 && c <= 0xffe6) ||
      (c >= 0x1f300 && c <= 0x1f64f) || (c >= 0x1f900 && c <= 0x1f9ff)
    )) ? 2 : 1;
  }
  return w;
}

function pad(s, width) {
  const d = width - dispWidth(s);
  return d > 0 ? s + ' '.repeat(d) : s;
}

function relTime(ts) {
  const d = Date.now() - ts;
  if (d < 60e3) return '刚刚';
  if (d < 3600e3) return Math.floor(d / 60e3) + ' 分钟前';
  if (d < 86400e3) return Math.floor(d / 3600e3) + ' 小时前';
  return Math.floor(d / 86400e3) + ' 天前';
}

// 表格里要省地方，用 3m / 15h / 2d 这种
function shortTime(ts) {
  const d = Date.now() - ts;
  if (d < 60e3) return '刚刚';
  if (d < 3600e3) return Math.floor(d / 60e3) + 'm';
  if (d < 86400e3) return Math.floor(d / 3600e3) + 'h';
  return Math.floor(d / 86400e3) + 'd';
}

// 路径太长就保留盘符 + 尽可能多的尾部层级，别把表格撑爆，也别把关键信息截没了
function fitPath(p, max) {
  if (dispWidth(p) <= max) return p;
  const parts = p.split(/[\\/]/).filter(Boolean);
  const drive = parts.length && /^[a-zA-Z]:$/.test(parts[0]) ? parts[0] + '\\…\\' : '…\\';
  for (let keep = parts.length - 1; keep >= 1; keep--) {
    const s = drive + parts.slice(-keep).join('\\');
    if (dispWidth(s) <= max) return s;
  }
  const tail = parts[parts.length - 1] || p;
  return tail.slice(0, Math.max(4, max - 1)) + '…';
}

function hourBucket(ts) {
  const d = new Date(ts);
  const hh = String(d.getHours()).padStart(2, '0');
  const mm = String(d.getMinutes()).padStart(2, '0');
  const now = new Date();
  const sameDay = d.toDateString() === now.toDateString();
  return sameDay ? `今天 ${hh}:${mm}` : `${d.getMonth() + 1}/${d.getDate()} ${hh}:${mm}`;
}

function workspacesFrom(args, cfg) {
  const exclude = (cfg.exclude || []).map((s) => new RegExp(s, 'i'));
  const { workspaces } = scan.scanAll({ exclude });

  const maxAge = args['no-age'] ? Infinity : (Number(args['max-age'] || cfg.maxAgeHours) * 3600e3);
  const now = Date.now();
  const filtered = workspaces.filter((w) => now - w.lastActive <= maxAge);

  const limit = args.all ? filtered.length : Number(args.limit || cfg.limit || DEFAULTS.limit);
  return filtered.slice(0, limit);
}

function toolCell(w) {
  const order = ['codebuddy', 'claude'];
  const parts = [];
  for (const t of order) {
    const hit = w.tools[t];
    if (!hit) continue;
    parts.push(`${t === 'codebuddy' ? 'cb' : 'cl'} ${shortTime(hit.mtime)}`);
  }
  return parts.join(' · ');
}

function printTable(list) {
  if (!list.length) {
    console.log('没找到可恢复的工作区。先跑一次 codebuddy 或 claude，再回来试试。');
    return;
  }
  const cols = Math.max(80, process.stdout.columns || 100);
  const rows = list.map((w, i) => ({
    n: String(i + 1),
    when: hourBucket(w.lastActive),
    tools: toolCell(w),
    cwd: w.cwd,
  }));

  const wN = 3;
  const GAP = 2;
  const wWhen = Math.max(dispWidth('最后活跃'), ...rows.map((r) => dispWidth(r.when)));
  const wTools = Math.max(dispWidth('CLI'), ...rows.map((r) => dispWidth(r.tools)));
  const wPath = Math.max(12, cols - 2 - wN - wWhen - wTools - GAP * 2);

  console.log('');
  console.log('  ' + pad('#', wN) + pad('最后活跃', wWhen) + ' '.repeat(GAP) +
    pad('CLI', wTools) + ' '.repeat(GAP) + '项目目录');
  console.log('  ' + '─'.repeat(Math.min(cols - 4, wN + wWhen + wTools + GAP * 2 + wPath)));
  for (const r of rows) {
    console.log('  ' + pad(r.n, wN) + pad(r.when, wWhen) + ' '.repeat(GAP) +
      pad(r.tools, wTools) + ' '.repeat(GAP) + fitPath(r.cwd, wPath));
  }
  console.log('');
}

function askSelection(list) {
  return new Promise((resolve) => {
    const rl = readline.createInterface({ input: process.stdin, output: process.stdout });
    const hint = `选择要恢复的编号（逗号分隔，a = 全部，回车 = 前 ${Math.min(4, list.length)} 个）：`;
    rl.question('  ' + hint, (answer) => {
      rl.close();
      const s = String(answer || '').trim().toLowerCase();
      if (!s) return resolve(list.slice(0, DEFAULTS.limit));
      if (s === 'a' || s === 'all') return resolve(list);
      const picked = [];
      for (const part of s.split(/[,，\s]+/)) {
        const n = parseInt(part, 10);
        if (!Number.isNaN(n) && n >= 1 && n <= list.length) picked.push(list[n - 1]);
      }
      resolve(picked);
    });
  });
}

async function cmdOpen(args, cfg) {
  const list = workspacesFrom(args, cfg);
  if (!list.length) {
    if (!args.quiet) {
      console.log('没找到可恢复的工作区（可能都超过时间窗口了，加 --no-age 看看全部）。');
    }
    return 1;
  }
  let chosen = list;
  if (args.pick) {
    printTable(list);
    chosen = await askSelection(list);
  }
  if (!chosen.length) {
    console.log('没有选中任何工作区。');
    return 1;
  }

  const tabs = term.buildTabs(chosen, {
    both: args.both || cfg.both,
    continue: args.continue || cfg.continueFlag,
    shell: cfg.shell ? { exe: cfg.shell, args: cfg.shell.endsWith('cmd.exe') ? ['/k'] : ['-NoExit', '-Command'] } : null,
  });

  const quiet = !!args.quiet;
  if (!quiet) {
    console.log(`\n  即将恢复 ${tabs.length} 个标签页：`);
    const wT = Math.max(...tabs.map((t) => dispWidth(t.title))) + 2;
    for (const t of tabs) {
      console.log(`   · ${pad(t.title, wT)}${t.cwd}`);
      console.log(`     ${' '.repeat(wT)}${t.command}`);
    }
  }

  const res = await term.launch(tabs, {
    dryRun: args['dry-run'],
    window: args.window != null ? Number(args.window) : cfg.window,
    shell: cfg.shell ? { exe: cfg.shell, args: cfg.shell.endsWith('cmd.exe') ? ['/k'] : ['-NoExit', '-Command'] } : null,
  });

  if (!res.ok) {
    console.error(`\n[error] ${res.error}`);
    return 1;
  }
  if (res.dryRun) {
    console.log('\n  --dry-run，实际会执行：');
    console.log('  wt.exe ' + res.args.map((a) => (a.includes(' ') ? `"${a}"` : a)).join(' '));
    return 0;
  }
  if (!quiet) console.log('\n  已交给 Windows Terminal。');
  return 0;
}

function cmdList(args, cfg) {
  const list = workspacesFrom(args, cfg);
  if (args.json) {
    const out = list.map((w) => ({
      cwd: w.cwd,
      lastActive: new Date(w.lastActive).toISOString(),
      tools: Object.fromEntries(
        Object.entries(w.tools).map(([k, v]) => [k, { sessionId: v.sessionId, mtime: new Date(v.mtime).toISOString() }])
      ),
    }));
    console.log(JSON.stringify(out, null, 2));
    return 0;
  }
  printTable(list);
  return 0;
}

function startupFile() {
  const appdata = process.env.APPDATA || path.join(process.env.USERPROFILE || '', 'AppData', 'Roaming');
  return path.join(appdata, 'Microsoft', 'Windows', 'Start Menu', 'Programs', 'Startup', 'cli-resume.vbs');
}

// 老版本生成过 .cmd，升级时顺手清掉，免得恢复两次
function legacyStartupFile() {
  return startupFile().replace(/\.vbs$/, '.cmd');
}

// 别把 WorkBuddy 内置的 node 写进启动项：那玩意随版本升级会换路径，
// 哪天升级了开机自启就静默失效。优先用系统装的 node。
function findSystemNode() {
  const dirs = (process.env.PATH || '').split(path.delimiter);
  for (const d of dirs) {
    if (!d || /\.workbuddy/i.test(d)) continue;
    const p = path.join(d, 'node.exe');
    try {
      if (fs.existsSync(p)) return p;
    } catch { /* 忽略 */ }
  }
  return process.execPath;
}

function cmdAutostart(args) {
  const target = startupFile();
  const legacy = legacyStartupFile();
  const sub = args._[1] || 'status';
  if (sub === 'status') {
    const on = fs.existsSync(target);
    console.log(`\n  开机自启：${on ? '已开启' : '未开启'}`);
    console.log(`  启动项路径：${target}`);
    if (on) {
      console.log('  内容：\n' + fs.readFileSync(target, 'utf8').split('\n').map((l) => '    ' + l).join('\n'));
    }
    console.log('\n  开启：cli-resume autostart on [--limit N]');
    console.log('  关闭：cli-resume autostart off\n');
    return 0;
  }
  if (sub === 'off') {
    let removed = false;
    for (const f of [target, legacy]) {
      if (fs.existsSync(f)) {
        fs.rmSync(f);
        removed = true;
      }
    }
    console.log(removed ? '\n  已移除开机自启。\n' : '\n  本来就没开。\n');
    return 0;
  }
  if (sub === 'on') {
    const limit = Number(args.limit || 2);
    const entry = path.resolve(__dirname, 'cli-resume.js');
    const nodeExe = findSystemNode();
    // 用 VBS 的 Run(..., 0) 静默启动：登录时不会闪一个黑窗口
    const body = [
      // 注释保持 ASCII：WScript 按 ANSI 解析 .vbs，UTF-8 中文会变乱码
      "' Generated by cli-resume: restore recent workspaces on login.",
      "' Window style 0 = hidden, False = do not wait.",
      'Set ws = CreateObject("WScript.Shell")',
      `ws.Run """${nodeExe}"" ""${entry}"" open --limit ${limit} --quiet", 0, False`,
      '',
    ].join('\r\n');
    fs.mkdirSync(path.dirname(target), { recursive: true });
    fs.writeFileSync(target, body, 'utf8');
    if (fs.existsSync(legacy)) fs.rmSync(legacy);  // 清掉旧版留在启动项里的 .cmd
    console.log(`\n  已开启开机自启：登录后恢复最近 ${limit} 个工作区。`);
    console.log(`  ${target}`);
    console.log('  关掉：cli-resume autostart off\n');
    return 0;
  }
  console.log('用法：cli-resume autostart [on|off|status] [--limit N]');
  return 1;
}

// 打开桌面版（cli-resume-gui），适合想先看一眼再点选的时候
function cmdGui() {
  // 仓库里是 cli/ 与 desktop/ 平级；早期版本的目录名是 cli-resume-gui，两种都认
  const candidates = [
    path.resolve(__dirname, '..', '..', 'desktop', 'bin', 'cli-resume.exe'),
    path.resolve(__dirname, '..', '..', 'cli-resume-gui', 'bin', 'cli-resume.exe'),
  ];
  const exe = candidates.find((p) => fs.existsSync(p));
  if (!exe) {
    console.log('\n  没找到桌面端程序，找过这几个位置：');
    for (const p of candidates) console.log('  ' + p);
    console.log('\n  先编译一次：desktop\\build.bat\n');
    return 1;
  }
  const { spawn } = require('child_process');
  const child = spawn(exe, [], { detached: true, stdio: 'ignore' });
  child.unref();
  console.log('\n  已打开桌面端。\n');
  return 0;
}

function cmdDoctor(cfg) {
  console.log('\n  cli-resume 环境自检\n');
  const shell = term.detectShell();
  console.log(`  shell        ${shell.exe}`);
  for (const [name, p] of [
    ['codebuddy 历史', path.join(process.env.USERPROFILE || '', '.codebuddy', 'projects')],
    ['claude 历史', path.join(process.env.USERPROFILE || '', '.claude', 'projects')],
  ]) {
    const exists = fs.existsSync(p);
    console.log(`  ${pad(name, 17)}${exists ? '✓  ' + p : '✗  不存在 ' + p}`);
  }
  const exclude = (cfg.exclude || []).map((s) => new RegExp(s, 'i'));
  const { rows, workspaces } = scan.scanAll({ exclude });
  const byTool = rows.reduce((m, r) => ((m[r.tool] = (m[r.tool] || 0) + 1), m), {});
  console.log(`  会话总数      ${rows.length}  (${Object.entries(byTool).map(([k, v]) => `${k} ${v}`).join(', ') || '无'})`);
  console.log(`  有效工作区    ${workspaces.length}`);
  console.log(`  配置文件      ${cfg._from || '（默认值）'}`);
  console.log('');
  return 0;
}

function usage() {
  console.log(`
  cli-resume —— 重启后一键把 codebuddy / claude 的终端现场拉回来

  用法：
    cli-resume list      [--limit N] [--all] [--no-age] [--json]   最近在哪些目录干过活
    cli-resume open      [--limit N] [--all] [--pick] [--both]     恢复到 Windows Terminal
                         [--continue] [--dry-run] [--window N] [--quiet]
    cli-resume autostart [on|off|status] [--limit N]               登录后自动恢复
    cli-resume gui                                                 打开桌面端（可视化点选）
    cli-resume doctor                                              环境自检

  open 的默认行为：取最近 ${DEFAULTS.limit} 个工作区，每个目录只恢复最近用过的那一个 CLI。
    --pick      先列出来让你挑（1,3,5 或 a）
    --both      同一目录 codebuddy 和 claude 都开
    --continue  用 -c 而不是 --resume <id>，交给 CLI 自己找最近会话
    --no-age    不限制时间窗口（默认只看 ${DEFAULTS.maxAgeHours} 小时内活跃过的）
`);
}

async function main() {
  const args = parseArgs(process.argv.slice(2));
  if (args.help || args._.length === 0) { usage(); return 0; }
  const cmd = args._[0];
  const cfg = loadConfig();
  if (cmd === 'list') return cmdList(args, cfg);
  if (cmd === 'open') return cmdOpen(args, cfg);
  if (cmd === 'doctor') return cmdDoctor(cfg);
  if (cmd === 'autostart') return cmdAutostart(args, cfg);
  if (cmd === 'gui') return cmdGui();
  usage();
  return 1;
}

main()
  .then((code) => process.exit(code || 0))
  .catch((e) => {
    console.error('[error] ' + (e && e.stack ? e.stack : e));
    process.exit(1);
  });
