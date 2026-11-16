'use strict';

// 示例数据：地址栏带 #demo（或 ?demo=1）时启用。
// 用途有两个：README 截图（不暴露真实目录），以及没装 CLI 的人先看一眼界面长什么样。
// 示例模式下不会扫本机历史，也不会真的拉起终端或打开资源管理器。

(function () {
  const on = location.hash.indexOf('demo') >= 0 ||
    new URLSearchParams(location.search).has('demo');
  if (!on) return;

  // #demo&skin=win98 可以直接用某套皮肤进入示例，截图脚本靠它省掉一次点击
  const skinHit = /skin=([a-z0-9]+)/i.exec(location.hash);
  const skin = skinHit ? skinHit[1] : 'neon';

  const MIN = 60e3;
  const HOUR = 60 * MIN;
  const now = Date.now();
  const at = (ms) => now - ms;

  const workspaces = [
    {
      cwd: 'D:\\work\\acme-portal',
      name: 'acme-portal',
      lastActive: at(8 * MIN),
      sessionCount: 8,
      sessions: [
        { tool: 'codebuddy', sessionId: 'a1b2c3d4-1001-4f00-9a01-000000000001', title: '把首页的 SSR 渲染改成流式输出', mtime: at(8 * MIN) },
        { tool: 'codebuddy', sessionId: 'a1b2c3d4-1001-4f00-9a01-000000000002', title: '这个 hydration mismatch 是怎么来的', mtime: at(2 * HOUR) },
        { tool: 'codebuddy', sessionId: 'a1b2c3d4-1001-4f00-9a01-000000000003', title: '给商品列表加骨架屏，避免首屏跳动', mtime: at(5 * HOUR) },
        { tool: 'codebuddy', sessionId: 'a1b2c3d4-1001-4f00-9a01-000000000004', title: '把图片组件换成响应式 srcset', mtime: at(9 * HOUR) },
        { tool: 'codebuddy', sessionId: 'a1b2c3d4-1001-4f00-9a01-000000000005', title: '重构表单项的校验逻辑', mtime: at(26 * HOUR) },
        { tool: 'codebuddy', sessionId: 'a1b2c3d4-1001-4f00-9a01-000000000006', title: '排查移动端 300ms 点击延迟', mtime: at(29 * HOUR) },
        { tool: 'codebuddy', sessionId: 'a1b2c3d4-1001-4f00-9a01-000000000007', title: '补一批 Playwright 冒烟测试', mtime: at(2 * 24 * HOUR) },
        { tool: 'codebuddy', sessionId: 'a1b2c3d4-1001-4f00-9a01-000000000008', title: '把散落的颜色变量整理成一套 token', mtime: at(3 * 24 * HOUR) },
      ],
    },
    {
      cwd: 'C:\\Users\\demo\\code\\data-pipeline',
      name: 'data-pipeline',
      lastActive: at(35 * MIN),
      sessionCount: 5,
      sessions: [
        { tool: 'claude', sessionId: 'b2c3d4e5-2002-4f00-9a02-000000000001', title: '订单表的历史迁移脚本怎么分批跑', mtime: at(35 * MIN) },
        { tool: 'claude', sessionId: 'b2c3d4e5-2002-4f00-9a02-000000000002', title: '这个 join 的索引为什么没走', mtime: at(3 * HOUR) },
        { tool: 'claude', sessionId: 'b2c3d4e5-2002-4f00-9a02-000000000003', title: '给 ETL 加失败重试和告警', mtime: at(7 * HOUR) },
        { tool: 'claude', sessionId: 'b2c3d4e5-2002-4f00-9a02-000000000004', title: '把日志切成结构化 JSON', mtime: at(27 * HOUR) },
        { tool: 'claude', sessionId: 'b2c3d4e5-2002-4f00-9a02-000000000005', title: '和产品确认分页参数的口径', mtime: at(2 * 24 * HOUR) },
      ],
    },
    {
      cwd: 'D:\\work\\internal-tools',
      name: 'internal-tools',
      lastActive: at(12 * MIN),
      sessionCount: 7,
      sessions: [
        { tool: 'codebuddy', sessionId: 'c3d4e5f6-3003-4f00-9a03-000000000001', title: '给审批流加一个超时提醒', mtime: at(12 * MIN) },
        { tool: 'claude', sessionId: 'c3d4e5f6-3003-4f00-9a03-000000000002', title: '表格导出超过一万行就卡死', mtime: at(70 * MIN) },
        { tool: 'codebuddy', sessionId: 'c3d4e5f6-3003-4f00-9a03-000000000003', title: '用 SQLite 替掉那个越写越大的 JSON 配置', mtime: at(4 * HOUR) },
        { tool: 'claude', sessionId: 'c3d4e5f6-3003-4f00-9a03-000000000004', title: '权限校验漏了部门维度', mtime: at(10 * HOUR) },
        { tool: 'codebuddy', sessionId: 'c3d4e5f6-3003-4f00-9a03-000000000005', title: '把构建从 4 分钟压到 90 秒', mtime: at(25 * HOUR) },
        { tool: 'claude', sessionId: 'c3d4e5f6-3003-4f00-9a03-000000000006', title: '组件库升级后样式全乱了', mtime: at(2 * 24 * HOUR) },
        { tool: 'codebuddy', sessionId: 'c3d4e5f6-3003-4f00-9a03-000000000007', title: '写一份接口变更说明', mtime: at(3 * 24 * HOUR) },
      ],
    },
    {
      cwd: 'C:\\Users\\demo\\code\\docs-site',
      name: 'docs-site',
      lastActive: at(2 * HOUR),
      sessionCount: 3,
      sessions: [
        { tool: 'claude', sessionId: 'd4e5f6a7-4004-4f00-9a04-000000000001', title: '文档站的搜索索引重建太慢', mtime: at(2 * HOUR) },
        { tool: 'claude', sessionId: 'd4e5f6a7-4004-4f00-9a04-000000000002', title: '把侧栏目录改成按文件结构自动生成', mtime: at(30 * HOUR) },
        { tool: 'claude', sessionId: 'd4e5f6a7-4004-4f00-9a04-000000000003', title: '中英文术语表统一一下', mtime: at(4 * 24 * HOUR) },
      ],
    },
    {
      cwd: 'D:\\work\\mobile-app',
      name: 'mobile-app',
      lastActive: at(6 * HOUR),
      sessionCount: 2,
      sessions: [
        { tool: 'codebuddy', sessionId: 'e5f6a7b8-5005-4f00-9a05-000000000001', title: '离线缓存策略要不要换成 stale-while-revalidate', mtime: at(6 * HOUR) },
        { tool: 'codebuddy', sessionId: 'e5f6a7b8-5005-4f00-9a05-000000000002', title: '打包体积超了两兆，先砍哪个依赖', mtime: at(2 * 24 * HOUR) },
      ],
    },
    {
      cwd: 'C:\\Users\\demo',
      name: 'demo',
      lastActive: at(5 * 24 * HOUR),
      sessionCount: 1,
      sessions: [
        { tool: 'claude', sessionId: 'f6a7b8c9-6006-4f00-9a06-000000000001', title: '试一下新的代码生成模板好不好用', mtime: at(5 * 24 * HOUR) },
      ],
    },
    {
      cwd: 'C:\\Users\\demo\\code\\api-gateway',
      name: 'api-gateway',
      lastActive: at(4 * HOUR),
      sessionCount: 4,
      sessions: [
        { tool: 'codebuddy', sessionId: 'a7b8c9d0-7007-4f00-9a07-000000000001', title: '限流规则挪到配置中心', mtime: at(4 * HOUR) },
        { tool: 'codebuddy', sessionId: 'a7b8c9d0-7007-4f00-9a07-000000000002', title: '上游超时重试要不要加抖动', mtime: at(8 * HOUR) },
        { tool: 'codebuddy', sessionId: 'a7b8c9d0-7007-4f00-9a07-000000000003', title: '这个 502 是网关还是服务方的问题', mtime: at(28 * HOUR) },
        { tool: 'codebuddy', sessionId: 'a7b8c9d0-7007-4f00-9a07-000000000004', title: '给路由表加一层缓存', mtime: at(3 * 24 * HOUR) },
      ],
    },
    {
      cwd: 'D:\\work\\legacy-admin',
      name: 'legacy-admin',
      lastActive: at(9 * HOUR),
      sessionCount: 2,
      sessions: [
        { tool: 'claude', sessionId: 'b8c9d0e1-8008-4f00-9a08-000000000001', title: '老后台的 jQuery 插件换不换', mtime: at(9 * HOUR) },
        { tool: 'claude', sessionId: 'b8c9d0e1-8008-4f00-9a08-000000000002', title: '把表格分页改成服务端渲染', mtime: at(3 * 24 * HOUR) },
      ],
    },
    {
      cwd: 'D:\\work\\infra',
      name: 'infra',
      lastActive: at(3 * 24 * HOUR),
      sessionCount: 3,
      sessions: [
        { tool: 'codebuddy', sessionId: 'c9d0e1f2-9009-4f00-9a09-000000000001', title: '镜像构建拆成多阶段', mtime: at(3 * 24 * HOUR) },
        { tool: 'codebuddy', sessionId: 'c9d0e1f2-9009-4f00-9a09-000000000002', title: '健康检查路径老是误判', mtime: at(4 * 24 * HOUR) },
        { tool: 'codebuddy', sessionId: 'c9d0e1f2-9009-4f00-9a09-000000000003', title: '日志轮转策略配错了', mtime: at(6 * 24 * HOUR) },
      ],
    },
  ];

  const total = workspaces.reduce((n, w) => n + w.sessions.length, 0);
  workspaces.sort((a, b) => b.lastActive - a.lastActive);

  window.CR_DEMO = {
    workspaces,
    stats: { totalSessions: total, elapsedMs: 38 },
    roots: {
      codebuddy: 'C:\\Users\\demo\\.codebuddy\\projects',
      codebuddyFound: true,
      claude: 'C:\\Users\\demo\\.claude\\projects',
      claudeFound: true,
    },
    settings: {
      maxAgeHours: 720,
      openTwo: false,
      useContinue: false,
      window: 0,
      theme: skin,
      exclude: [],
    },
  };
})();
