#!/usr/bin/env python3
"""重新生成 docs/promo.mp4 和 docs/promo.gif（含 Edge TTS 配音）。

画面清楚的关键不是后期放大，而是抓图时就把界面放大渲染：
`--zoom 1.55` 走 WebView2 的 ZoomFactor，文字是原生像素，不是插值糊出来的；
`--size 1600x1000` 保证有足够的像素底子。

依赖：ffmpeg（drawtext/zoompan/xfade）、edge-tts（pip install edge-tts）、
Windows 中文字体、已编译的 bin/cli-resume.exe。

用法：python promo_build.py             # 全流程
     python promo_build.py --stills    # 只重抓截图
     python promo_build.py --no-audio  # 不配音
"""
import asyncio
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
EXE = os.path.join(HERE, "..", "bin", "cli-resume.exe")
SHOTS = os.path.join(HERE, ".promo-shots")
WORK = os.path.join(SHOTS, "build")
OUT = HERE

FONT_B = r"C\:/Windows/Fonts/msyhbd.ttc"
VOICE = "zh-CN-XiaoxiaoNeural"
RATE = "+8%"
W, H, FPS = 1920, 1080, 30
XF = 0.55          # 交叉淡化
SHOT_H = 940       # 截图在画面里的高度（余下留给字幕）
SHOT_Y = 16

# (文件名, 秒数, 屏幕字幕, 出图参数, 配音)
SCENES = [
    ("neon.png", 4.2, "把散落在硬盘里的终端会话，一次收回来",
     ["--skin", "neon"], "打开就是上次干活的目录。"),
    ("amber.png", 3.0, "四套外观：80s 霓虹 / 70s 琥珀 / 98 / XP",
     ["--skin", "amber"], "四套外观，随手切换。"),
    ("win98.png", 2.6, "", ["--skin", "win98"], ""),
    ("xp.png", 2.6, "", ["--skin", "xp"], ""),
    ("search.png", 4.0, "全文搜索：按内容找，不是按文件名",
     ["--skin", "neon", "--zoom", "1.3", "--ui", "promosearch"], "想不起在哪说过，直接搜正文。"),
    ("newmenu.png", 4.0, "任意目录，开一个全新会话",
     ["--skin", "neon", "--zoom", "1.55", "--ui", "newmenu"], "任意目录，开一个全新会话。"),
    ("wsform.png", 4.2, "还没跑过 CLI 的目录，也能先登记成工作空间",
     ["--skin", "neon", "--zoom", "1.55", "--ui", "wsform"], "还没跑过的目录，先登记成工作空间。"),
    ("term.png", 4.2, "内嵌终端：最多四窗并排，各自独立",
     ["--skin", "neon", "--zoom", "1.2", "--ui", "demoterm"], "最多四窗并排，各自独立。"),
]


def run(args):
    r = subprocess.run(args, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if r.returncode != 0:
        print("FAILED:", " ".join(args[:10]))
        print(r.stderr[-2000:])
        sys.exit(1)


def shoot():
    os.makedirs(SHOTS, exist_ok=True)
    for name, _sec, _cap, extra, _voice in SCENES:
        dst = os.path.join(SHOTS, name)
        run([EXE, "--shot", dst, "--wait", "8000", "--demo", "--size", "1600x1000"] + extra)
        print("shot", name)


def compose(src, dst):
    """截图铺满画面：高度 940、居中，底部约 120px 留给字幕。"""
    vf = (f"scale=-1:{SHOT_H},pad=iw+2:ih+2:1:1:color=0x2b3757[s];"
          f"color=c=0x07030f:s={W}x{H}:d=1[bg];[bg][s]overlay=(W-w)/2:{SHOT_Y}")
    run(["ffmpeg", "-y", "-loglevel", "error", "-i", src, "-filter_complex", vf,
         "-frames:v", "1", dst])


def segment(still, seconds, caption, idx):
    frames = int(seconds * FPS)
    # 推镜要克制：画面已经铺满，放大反而糊
    vf = [f"zoompan=z='min(1+0.00022*on,1.03)':x='iw/2-(iw/zoom/2)':y='ih/2-(ih/zoom/2)'"
          f":d={frames}:s={W}x{H}:fps={FPS}"]
    if caption:
        safe = caption.replace(":", r"\:").replace("'", r"\'")
        vf.append("drawtext="
                  f"fontfile='{FONT_B}':text='{safe}':fontcolor=white:fontsize=40"
                  ":box=1:boxcolor=black@0.45:boxborderw=20:x=(w-text_w)/2:y=h-104"
                  ":alpha='if(lt(t,0.4),t/0.4,1)'"
                  ":shadowcolor=black@0.6:shadowx=2:shadowy=2")
    vf.append("format=yuv420p")
    out = os.path.join(WORK, f"seg{idx}.mp4")
    run(["ffmpeg", "-y", "-loglevel", "error", "-loop", "1", "-i", still, "-vf", ",".join(vf),
         "-t", str(seconds), "-r", str(FPS), "-c:v", "libx264", "-crf", "17", out])
    return out


def card(lines, seconds, out):
    texts = []
    for txt, size, color, dy in lines:
        safe = txt.replace(":", r"\:").replace("'", r"\'")
        texts.append("drawtext="
                     f"fontfile='{FONT_B}':text='{safe}':fontcolor={color}:fontsize={size}"
                     f":x=(w-text_w)/2:y=(h-text_h)/2+{dy}:alpha='if(lt(t,0.5),t/0.5,1)'")
    run(["ffmpeg", "-y", "-loglevel", "error", "-f", "lavfi",
         "-i", f"color=c=0x07030f:s={W}x{H}:d={seconds}:r={FPS}",
         "-vf", ",".join(texts + ["format=yuv420p"]), "-c:v", "libx264", "-crf", "17", out])
    return out


def tts(text, out):
    """Edge TTS 合成一句。没装或没网就跳过，不影响成片。"""
    if not text:
        return False
    try:
        import edge_tts
    except ImportError:
        print("没装 edge-tts，跳过配音")
        return False
    # 微软那个接口偶尔抽风，重试三次再放弃
    for attempt in range(3):
        try:
            asyncio.run(edge_tts.Communicate(text, VOICE, rate=RATE).save(out))
            if os.path.getsize(out) > 1000:
                return True
        except Exception as e:
            if attempt == 2:
                print("TTS 失败:", e)
            else:
                asyncio.sleep(1.2)
    return False


def dur_of(path):
    d = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "format=duration",
                        "-of", "csv=p=0", path], capture_output=True, text=True)
    return float(d.stdout.strip())


def build(with_audio=True):
    os.makedirs(WORK, exist_ok=True)
    segs = [card([("CLI  //  RESUME", 96, "0x8cff3f", -90),
                  ("把散落在硬盘里的终端会话，一次收回来", 40, "0xd8e6ff", 10),
                  ("四种外观  ·  全文搜索  ·  内嵌终端四分屏", 28, "0x7f8fb3", 78)],
                 3.6, os.path.join(WORK, "seg_title.mp4"))]

    for i, (name, sec, cap, _extra, _voice) in enumerate(SCENES):
        composed = os.path.join(WORK, "comp_" + name)
        compose(os.path.join(SHOTS, name), composed)
        segs.append(segment(composed, sec, cap, i))

    segs.append(card([("github.com/Wike-CHI/cli-resume", 52, "0x8cff3f", -40),
                      ("MIT  ·  Windows 10/11  ·  解压即用", 30, "0x9fb0d0", 40)],
                     3.6, os.path.join(WORK, "seg_end.mp4")))

    inputs = []
    for s in segs:
        inputs += ["-i", s]
    durs = [dur_of(s) for s in segs]

    # 每段在时间轴上的起点：xfade 会把前一段吃掉 XF 秒
    starts, acc = [], 0.0
    for d in durs:
        starts.append(acc)
        acc += d - XF

    filt, prev, offset = [], "[0:v]", 0.0
    for i in range(1, len(segs)):
        offset += durs[i - 1] - XF
        filt.append(f"{prev}[{i}:v]xfade=transition=fade:duration={XF}:offset={offset:.3f}[v{i}]")
        prev = f"[v{i}]"
    total = offset + durs[-1]
    filt.append(f"{prev}fade=in:st=0:d=0.4,fade=out:st={total - 0.8:.2f}:d=0.8[out]")

    silent = os.path.join(WORK, "silent.mp4")
    run(["ffmpeg", "-y", "-loglevel", "error"] + inputs + ["-filter_complex", ";".join(filt),
        "-map", "[out]", "-c:v", "libx264", "-crf", "20", "-preset", "slow",
        "-pix_fmt", "yuv420p", silent])

    mp4 = os.path.join(OUT, "promo.mp4")

    # 配音从场景起点开始，音画对齐
    clips = [("把散落在硬盘里的终端会话，一次收回来。", 0.5)]
    for (_name, _sec, _cap, _extra, voice), st in zip(SCENES, starts[1:]):
        if voice:
            clips.append((voice, st + 0.35))
    clips.append(("github 上搜 cli-resume。", starts[-1] + 0.4))

    audio_inputs, delays = [], []
    if with_audio:
        for i, (text, at) in enumerate(clips):
            path = os.path.join(WORK, f"tts{i}.mp3")
            if not tts(text, path):
                continue
            audio_inputs += ["-i", path]
            # 输入 0 是无声视频，配音从 1 开始编号
            delays.append((len(delays) + 1, int(at * 1000)))

    if audio_inputs:
        chains, mixes = [], []
        for n, (idx, ms) in enumerate(delays):
            chains.append(f"[{idx}:a]adelay={ms}|{ms}[a{n}]")
            mixes.append(f"[a{n}]")
        graph = (";".join(chains) + ";" + "".join(mixes) +
                 f"amix=inputs={len(mixes)}:normalize=0,loudnorm=I=-16:TP=-1.5:LRA=11[aout]")
        run(["ffmpeg", "-y", "-loglevel", "error", "-i", silent] + audio_inputs +
            ["-filter_complex", graph, "-map", "0:v", "-map", "[aout]",
             "-c:v", "copy", "-c:a", "aac", "-b:a", "128k", "-movflags", "+faststart", mp4])
    else:
        run(["ffmpeg", "-y", "-loglevel", "error", "-i", silent, "-c", "copy", mp4])

    # 内嵌用的 GIF：720 宽在 README 里读不清，提到 1000；帧率降到 7 换体积
    gif = os.path.join(OUT, "promo.gif")
    run(["ffmpeg", "-y", "-loglevel", "error", "-ss", "0", "-t", "10.6", "-i", mp4,
         "-vf", "fps=7,scale=900:-1:flags=lanczos,palettegen=max_colors=96:stats_mode=diff",
         os.path.join(WORK, "pal.png")])
    run(["ffmpeg", "-y", "-loglevel", "error", "-ss", "0", "-t", "10.6", "-i", mp4,
         "-i", os.path.join(WORK, "pal.png"), "-lavfi",
         "fps=7,scale=900:-1:flags=lanczos[x];[x][1:v]paletteuse=dither=none", gif])

    print(f"promo.mp4 {total:.1f}s {os.path.getsize(mp4)//1024} KB · "
          f"promo.gif {os.path.getsize(gif)//1024} KB · 配音 {len(delays)} 句")


if __name__ == "__main__":
    if not os.path.exists(EXE):
        sys.exit("先编译：build.bat")
    if "--build" not in sys.argv:
        shoot()
    if "--stills" not in sys.argv:
        build(with_audio="--no-audio" not in sys.argv)
