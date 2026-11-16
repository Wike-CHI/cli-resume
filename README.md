# cli-resume

重启电脑后，把 `codebuddy` / `claude` 的终端现场一把拉回来。

不维护项目清单、不记笔记、不用手动 `cd` —— 直接读两个 CLI 自己落的会话历史，
还原出「上次在哪些目录干活、会话 id 是多少」，然后 `--resume` 回去。
桌面端还能把会话直接开在应用自己的终端面板里（ConPTY + xterm.js），不再弹外部终端窗口；
多个会话并排看，也能随时切回外部终端。

<img src="desktop/docs/desktop.png" width="49%" alt="80s 霓虹皮肤">
<img src="desktop/docs/w98-desktop.png" width="49%" alt="Windows 98 皮肤">

桌面端有两套皮肤，顶栏一个按钮切换：80 年代合成器浪潮（紫黑底、青与品红双霓虹、CRT 扫描线、
网格地平线）和 Windows 98（灰面板、四层 1px 立体线、深蓝选中、宋体小字、16px 经典滚动条）。
选择记在 `settings.json` 里，下次启动就是上次那套。

## 两种形态，同一套思路

| | 命令行版 | 桌面端 |
|---|---|---|
| 入口 | `cr list` / `cr open` | 双击窗口，可设开机自启 |
| 颗粒度 | 工作区级：每个目录开一个标签 | 对话级：点标题直接进那一条 |
| 技术栈 | Node 18+，零依赖 | C++17 + Win32 + WebView2 |
| 适合 | 开机自动恢复、脚本里调 | 先看一眼有哪些现场，再决定进哪个 |

命令行版在 [`cli/`](cli/)，桌面端在 [`desktop/`](desktop/)，各有详细文档。

## 快速开始

### 命令行版

```bat
git clone https://github.com/Wike-CHI/cli-resume.git
cd cli-resume\cli
npm link
```

之后任何目录下敲 `cr` 就能用（不想装全局，也可以把 `cli` 目录加进 PATH，
`cr.cmd` 就在里面）。

```
$ cr list

  #  最后活跃     CLI             项目目录
  ──────────────────────────────────────────────────────────────
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

### 桌面端

需要 Visual Studio 2022 的 C++ 工具链和 WebView2 SDK，SDK 不随仓库分发：

```bat
cd cli-resume\desktop
build.bat
bin\cli-resume.exe
```

装好 CLI 后 `cr gui` 也能直接拉起来。SDK 怎么放、编译常见问题见 [`desktop/README.md`](desktop/README.md)。

没装 CLI 也想看看界面长什么样：`bin\cli-resume.exe --demo`，会用内置示例数据渲染，
不会扫本机历史也不会真的开终端。

## 内嵌终端

`desktop/src/terminal.cpp` 用 ConPTY 起一个真 shell（优先 PowerShell 7，退回系统自带的 5.1），
前端是 xterm.js，中间一条 WebSocket，二进制帧传 PTY 的原始字节。子进程拿到的是真正的 TTY，
codebuddy / claude 的颜色、光标、按键、TUI 布局和在终端窗口里完全一样。

- **并排**：面板顶部「▧」以当前窗格的目录再开一个；2 个并排，3~4 个两行两列
- **切换**：顶部标签点哪个，键盘就归哪个；亮边框的是当前窗格
- **高度**：拖面板顶边；「▾」收起面板但不杀会话
- **入口**：顶栏终端图标随手开一个空 shell；左栏「进入方式」决定「进入」是内嵌还是弹外部终端
- **回收**：关掉窗格或退出应用，ConPTY 会话连同里面的 node 进程一起结束（子进程挂在 job 上，
  应用被强杀也不留孤儿）

<img src="desktop/docs/term.png" width="49%" alt="霓虹：两个终端并排">
<img src="desktop/docs/w98-term.png" width="49%" alt="98：两个终端并排">

## 它怎么知道你在哪些目录干过活

两套 CLI 都把会话落在 `%USERPROFILE%` 下，格式还一样（jsonl，每条消息带 `cwd` 和 `sessionId`）：

```
~/.codebuddy/projects/<编码目录>/<sessionId>.jsonl
~/.claude/projects/<编码目录>/<sessionId>.jsonl
```

- **codebuddy**：目录名编码不可逆（非字母数字全变 `-`），所以直接从 jsonl 里读 `cwd`。
  只读文件头 64KB —— 最大的会话有 4MB，全读没必要。
- **claude**：jsonl 不一定带 `cwd`，改用 `~/.claude.json` 里的 `projects` 键（真实绝对路径）
  反算编码去对表，对不上的再退回嗅探。
- 文件 **mtime** 当最后活跃时间。目录已经被删掉的会话直接跳过，不会拉起一个不存在的路径。
- 桌面端还会从 jsonl 尾部读 `ai-title` 记录（模型给会话起的标题），老会话没有就退回首条用户消息。

## 三个坑

**`.cmd` 里不能写 UTF-8 中文。** cmd.exe 按当前代码页（GBK）解析批处理文件，
UTF-8 中文字节会被解成乱码，其中某些字节还能把后面的换行吃掉，导致下一行命令被拼进去，
报 `'...node' 不是内部或外部命令`。所以 `cr.cmd` 全 ASCII，注释也用英文。

**命令里不能带分号。** `wt` 会把整个命令行里任何位置的 `;` 当标签分隔符，哪怕它在
`-Command` 的引号里。实测 `powershell -Command "pwd; codebuddy --version"`，后半段会被切掉变成新标签。

**`--resume` 必须在会话原本的 cwd 里跑。** 每个标签的 `-d` 要精确对应扫出来的 `cwd`，
不能用当前目录凑合，否则 CLI 找不到那条会话。

## 边界

- 只支持 Windows，终端用 Windows Terminal（`wt.exe`）。没有 `wt` 就没有标签页这一说。
- 只认 `codebuddy` 和 `claude` 两套历史格式。别家 CLI 的落盘格式变了就得跟着改。
- **能不能真续上，取决于 CLI 自己的鉴权状态。** 工具只负责把正确的命令和正确的目录递过去；
  没登录、模型配置不认，终端里照样会报错。好在标签用 `-NoExit` 打开，报错留在窗口里不会闪退。
- 扫描是同步的，几千个会话时会慢。目前 70 多条对话的量级，扫描 30ms 上下。

## 许可

[MIT](LICENSE)
