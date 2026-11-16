'use strict';
// 把「工作区 + 会话」翻译成 Windows Terminal 的一串标签页，并拉起来。

const fs = require('fs');
const path = require('path');
const { spawn } = require('child_process');

// 优先 pwsh 7，退回 Windows PowerShell 5.1
function detectShell() {
  const candidates = [
    { exe: 'pwsh.exe', args: ['-NoExit', '-Command'] },
    { exe: 'powershell.exe', args: ['-NoExit', '-Command'] },
    { exe: 'cmd.exe', args: ['/k'] },
  ];
  for (const c of candidates) {
    const dirs = (process.env.PATH || '').split(path.delimiter);
    for (const d of dirs) {
      if (!d) continue;
      try {
        if (fs.existsSync(path.join(d, c.exe))) return c;
      } catch { /* 忽略 */ }
    }
  }
  return candidates[1];
}

function baseName(p) {
  const n = path.basename(p.replace(/[\\/]+$/, ''));
  return n || p;
}

// 一个工作区里可能同时有 codebuddy 和 claude 的会话
function sessionsFor(ws, opts = {}) {
  const pick = [];
  const order = ['codebuddy', 'claude'];
  for (const tool of order) {
    const hit = ws.tools[tool];
    if (!hit) continue;
    pick.push(hit);
  }
  if (opts.both) return pick;
  // 默认只恢复这个目录里最近用过的那一个 CLI，避免一次弹出太多窗口
  if (pick.length <= 1) return pick;
  pick.sort((a, b) => b.mtime - a.mtime);
  return [pick[0]];
}

function resumeCommand(tool, sessionId, opts = {}) {
  const bin = tool === 'claude' ? 'claude' : 'codebuddy';
  if (opts.continue) return bin;                       // 直接 -c，交给 CLI 自己找最近的
  if (!sessionId) return bin;
  return `${bin} --resume ${sessionId}`;
}

function buildTabs(workspaces, opts = {}) {
  const shell = opts.shell || detectShell();
  const tabs = [];
  for (const ws of workspaces) {
    for (const s of sessionsFor(ws, opts)) {
      const cmd = resumeCommand(s.tool, s.sessionId, opts);
      tabs.push({
        title: `${baseName(ws.cwd)} · ${s.tool}`,
        cwd: ws.cwd,
        tool: s.tool,
        sessionId: s.sessionId,
        command: cmd,
        line: `${shell.exe} ${shell.args.join(' ')} "${cmd}"`,
      });
    }
  }
  return tabs;
}

// wt 用 ';' 分隔多个 tab；作为独立 argv 传进去，不需要 cmd 层的转义
function buildWtArgs(tabs, opts = {}) {
  if (!tabs.length) return [];
  const shell = opts.shell || detectShell();
  const args = ['-w', String(opts.window == null ? 0 : opts.window)];
  tabs.forEach((t, i) => {
    if (i > 0) args.push(';');
    args.push('new-tab', '--title', t.title, '-d', t.cwd);
    args.push(shell.exe, ...shell.args, t.command);
  });
  return args;
}

function launch(tabs, opts = {}) {
  const args = buildWtArgs(tabs, opts);
  if (!args.length) return { ok: false, error: '没有要恢复的标签页' };
  if (opts.dryRun) return { ok: true, dryRun: true, args };
  return new Promise((resolve) => {
    let child;
    try {
      child = spawn('wt.exe', args, { detached: true, stdio: 'ignore' });
    } catch (e) {
      resolve({ ok: false, error: 'wt.exe 启动失败：' + e.message });
      return;
    }
    child.on('error', (e) => resolve({ ok: false, error: 'wt.exe 启动失败：' + e.message }));
    child.unref();
    // wt 把命令转交给已有的 Terminal 进程后就退出，这里不等它
    setTimeout(() => resolve({ ok: true, args }), 400);
  });
}

module.exports = { detectShell, buildTabs, buildWtArgs, launch, resumeCommand, baseName };
