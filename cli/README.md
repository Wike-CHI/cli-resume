# cli-resume（命令行版）

重启后把 `codebuddy` / `claude` 的终端现场拉回来，一行命令搞定。

```
$ cr list

  #  最后活跃     CLI             项目目录
  ──────────────────────────────────────────────────────────────────────
  1  今天 11:47   cb 7m           d:\work\acme-portal
  2  今天 04:39   cb 15h · cl 7h  d:\work\internal-tools
  3  11/12 18:11  cb 17h · cl 1d  c:\Users\you

$ cr open
  即将恢复 2 个标签页：
   · acme-portal · codebuddy      d:\work\acme-portal
                                  codebuddy --resume 01a1cf71-…
   · internal-tools · claude      d:\work\internal-tools
                                  claude --resume 022945b6-…
```

## 装

只要 Node 18+（本机 CLI 都是 npm 装的，肯定有）。

```bat
npm link
```

这会在全局挂一个 `cr`。不想动全局就用 PATH：

```bat
:: 临时，只对当前窗口有效
set PATH=%PATH%;<仓库路径>\cli
```

永久加：系统属性 → 环境变量 → 用户变量 `Path` → 新建 → 填上面那个目录。
`cr.cmd` 就在 `cli/` 里，它会调 `bin\cli-resume.js`。

不想动 PATH 也可以直接 `node bin\cli-resume.js open`。

## 用

| 命令 | 做什么 |
|---|---|
| `cr list` | 看最近在哪些目录干过活（`--all` 全部、`--json` 给脚本用） |
| `cr open` | 恢复最近 4 个工作区，每个目录只恢复最近用过的那一个 CLI |
| `cr open --pick` | 先列出来，输入 `1,3,5` 或 `a` 挑 |
| `cr open --both` | 同一目录 codebuddy 和 claude 都开 |
| `cr open --continue` | 用 `-c` 而不是 `--resume <id>`，让 CLI 自己找最近会话 |
| `cr open --dry-run` | 只打印将要执行的 `wt.exe` 命令，不动手 |
| `cr autostart on` | 登录 Windows 后自动恢复最近 2 个 |
| `cr gui` | 打开桌面端（见 [`../desktop`](../desktop)） |
| `cr doctor` | 自检：shell、两个历史目录、能扫到多少会话 |

`autostart on` 会在启动文件夹写一个 `cli-resume.vbs` —— 不是 `.cmd`，因为只有 VBS 的
`Run(..., 0)` 能真正隐藏窗口，登录时不会闪黑框。里面记的是**系统 node 的绝对路径**，
不是某个宿主程序内置的那个（后者随版本升级会换路径，会让自启静默失效）。
`autostart off` 会把 `.vbs` 和旧版遗留的 `.cmd` 一起清掉。

默认只看 **72 小时内**活跃过的目录，免得把三周前的项目也拉起来；`--no-age` 取消这个限制。

## 配置

`config.json`（放在 `cli/` 下）或 `~/.cli-resume.json`，后者优先。两项都没有就用内置默认值，
所以直接跑也能用。照 `config.example.json` 抄一份改：

```json
{
  "limit": 4,            // 默认恢复几个工作区
  "maxAgeHours": 72,     // 多久没动过就不算「现场」
  "both": false,         // 同一目录是否两个 CLI 都开
  "continueFlag": false, // true = 用 -c
  "window": 0,           // wt 的 -w：0 复用最近窗口，-1 强制新窗口
  "shell": "",           // 留空自动找 pwsh.exe → powershell.exe → cmd.exe
  "exclude": ["[\\\\/]Temp[\\\\/]", "\\\\system32\\\\", "\\\\node_modules\\\\"]
}
```

`exclude` 是正则，匹配**完整路径**。默认排掉临时目录、system32 和 node_modules。

## 原理

两套 CLI 都把会话落在 `%USERPROFILE%` 下，格式一样（jsonl，每条消息带 `cwd` 和 `sessionId`）：

```
~/.codebuddy/projects/<编码目录>/<sessionId>.jsonl
~/.claude/projects/<编码目录>/<sessionId>.jsonl
```

- **codebuddy**：目录名编码不可逆（非字母数字全变 `-`），所以直接从 jsonl 里读 `cwd`。
  只读文件头 64KB —— 最大的会话有 4MB，全读没必要。
- **claude**：jsonl 不一定带 `cwd`，改用 `~/.claude.json` 里的 `projects` 键（真实绝对路径）
  反算编码去对表，对不上的再退回嗅探。
- 文件 **mtime** 当最后活跃时间，同一个（目录, CLI）只留最新那一个会话。
- 目录已被删除的会话直接跳过，不会拉起一个不存在的路径。
- 拉起方式：`wt -w 0 new-tab --title … -d <dir> powershell -NoExit -Command "codebuddy --resume <id>"`，
  多个标签用独立的 `;` 参数分隔。

## 坑

三条都写在[根目录 README](../README.md#三个坑)里：`.cmd` 不能写 UTF-8 中文、命令里不能带分号、
`--resume` 必须在原 cwd 里跑。这里不重复。

## 边界

只支持 Windows + Windows Terminal。只认 codebuddy 和 claude 两套历史格式。
能不能真续上取决于 CLI 自己的鉴权 —— 工具只负责把命令和目录递对。
