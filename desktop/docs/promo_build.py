#!/usr/bin/env python3
"""重新生成 docs/promo.mp4 和 docs/promo.gif。

流程和手动做截图一致：用 `--shot` 抓示例数据（`--demo`，不碰真实目录），
再用 ffmpeg 合成 —— 暗色舞台 + 缓慢推镜 + 中文字幕 + 交叉淡化。

依赖：ffmpeg（要含 drawtext/zoompan/xfade）、Windows 中文字体、已编译的 bin/cli-resume.exe。
用法：python promo_build.py            # 全流程
     python promo_build.py --stills   # 只重抓截图，不重编视频
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
EXE = os.path.join(HERE, "..", "bin", "cli-resume.exe")
SHOTS = os.path.join(HERE, ".promo-shots")
WORK = os.path.join(SHOTS, "build")
OUT = HERE

FONT_B = r"C\:/Windows/Fonts/msyhbd.ttc"
W, H, FPS = 1920, 1080, 30
XF = 0.55  # 交叉淡化时长

# (文件名, 秒数, 字幕, 出图参数)  —— 出图参数就是 --demo 之外的那些 --ui 动作
SCENES = [
    ("neon.png", 4.2, "把散落在硬盘里的终端会话，一次收回来", ["--skin", "neon"]),
    ("amber.png", 3.0, "四套外观：80s 霓虹 / 70s 琥珀 / 98 / XP", ["--skin", "amber"]),
    ("win98.png", 2.6, "", ["--skin", "win98"]),
    ("xp.png", 2.6, "", ["--skin", "xp"]),
    ("search.png", 3.8, "全文搜索：按内容找，不是按文件名", ["--skin", "neon", "--ui", "promosearch"]),
    ("newmenu.png", 3.8, "任意目录，开一个全新会话", ["--skin", "neon", "--ui", "newmenu"]),
    ("wsform.png", 3.8, "还没跑过 CLI 的目录，也能先登记成工作空间", ["--skin", "neon", "--ui", "wsform"]),
    ("term.png", 4.2, "内嵌终端：最多四窗并排，各自独立", ["--skin", "neon", "--ui", "demoterm"]),
]


def run(args):
    r = subprocess.run(args, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if r.returncode != 0:
        print("FAILED:", " ".join(args[:10]))
        print(r.stderr[-2000:])
        sys.exit(1)


def shoot():
    os.makedirs(SHOTS, exist_ok=True)
    for name, _sec, _cap, extra in SCENES:
        dst = os.path.join(SHOTS, name)
        run([EXE, "--shot", dst, "--wait", "8000", "--demo", "--size", "1600x1000"] + extra)
        print("shot", name)


def compose(src, dst):
    """截图放进 1920x1080 舞台：高 880、居中、细边框，底部 200px 留给字幕。"""
    vf = (f"scale=-1:880,pad=iw+2:ih+2:1:1:color=0x2b3757[s];"
          f"color=c=0x07030f:s={W}x{H}:d=1[bg];[bg][s]overlay=(W-w)/2:44")
    run(["ffmpeg", "-y", "-loglevel", "error", "-i", src, "-filter_complex", vf,
         "-frames:v", "1", dst])


def segment(still, seconds, caption, idx):
    frames = int(seconds * FPS)
    vf = [f"zoompan=z='min(1+0.00035*on,1.05)':x='iw/2-(iw/zoom/2)':y='ih/2-(ih/zoom/2)'"
          f":d={frames}:s={W}x{H}:fps={FPS}"]
    if caption:
        safe = caption.replace(":", r"\:").replace("'", r"\'")
        vf.append("drawtext="
                  f"fontfile='{FONT_B}':text='{safe}':fontcolor=white:fontsize=40"
                  ":box=1:boxcolor=black@0.42:boxborderw=18:x=(w-text_w)/2:y=h-118"
                  ":alpha='if(lt(t,0.45),t/0.45,1)'"
                  ":shadowcolor=black@0.5:shadowx=2:shadowy=2")
    vf.append("format=yuv420p")
    out = os.path.join(WORK, f"seg{idx}.mp4")
    run(["ffmpeg", "-y", "-loglevel", "error", "-loop", "1", "-i", still, "-vf", ",".join(vf),
         "-t", str(seconds), "-r", str(FPS), "-c:v", "libx264", "-crf", "18", out])
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
         "-vf", ",".join(texts + ["format=yuv420p"]), "-c:v", "libx264", "-crf", "18", out])
    return out


def build():
    os.makedirs(WORK, exist_ok=True)
    segs = [card([("CLI  //  RESUME", 96, "0x8cff3f", -90),
                  ("把散落在硬盘里的终端会话，一次收回来", 40, "0xd8e6ff", 10),
                  ("四种外观  ·  全文搜索  ·  内嵌终端四分屏", 28, "0x7f8fb3", 78)],
                 3.4, os.path.join(WORK, "seg_title.mp4"))]

    for i, (name, sec, cap, _extra) in enumerate(SCENES):
        composed = os.path.join(WORK, "comp_" + name)
        compose(os.path.join(SHOTS, name), composed)
        segs.append(segment(composed, sec, cap, i))

    segs.append(card([("github.com/Wike-CHI/cli-resume", 52, "0x8cff3f", -40),
                      ("MIT  ·  Windows 10/11  ·  解压即用", 30, "0x9fb0d0", 40)],
                     3.6, os.path.join(WORK, "seg_end.mp4")))

    inputs = []
    for s in segs:
        inputs += ["-i", s]
    durs = []
    for s in segs:
        d = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "format=duration",
                            "-of", "csv=p=0", s], capture_output=True, text=True)
        durs.append(float(d.stdout.strip()))

    filt, prev, offset = [], "[0:v]", 0.0
    for i in range(1, len(segs)):
        offset += durs[i - 1] - XF
        filt.append(f"{prev}[{i}:v]xfade=transition=fade:duration={XF}:offset={offset:.3f}[v{i}]")
        prev = f"[v{i}]"
    filt.append(f"{prev}fade=in:st=0:d=0.4,fade=out:st={offset + durs[-1] - 0.8:.2f}:d=0.8[out]")

    mp4 = os.path.join(OUT, "promo.mp4")
    run(["ffmpeg", "-y", "-loglevel", "error"] + inputs + ["-filter_complex", ";".join(filt),
        "-map", "[out]", "-c:v", "libx264", "-crf", "21", "-preset", "slow",
        "-pix_fmt", "yuv420p", "-movflags", "+faststart", mp4])

    # 内嵌用的 GIF：只取片头 + 四套皮肤那 10 秒，压到 720 宽/8fps/96 色才够轻
    gif = os.path.join(OUT, "promo.gif")
    run(["ffmpeg", "-y", "-loglevel", "error", "-ss", "0", "-t", "10.2", "-i", mp4,
         "-vf", "fps=8,scale=720:-1:flags=lanczos,palettegen=max_colors=96:stats_mode=diff",
         os.path.join(WORK, "pal.png")])
    run(["ffmpeg", "-y", "-loglevel", "error", "-ss", "0", "-t", "10.2", "-i", mp4,
         "-i", os.path.join(WORK, "pal.png"), "-lavfi",
         "fps=8,scale=720:-1:flags=lanczos[x];[x][1:v]paletteuse=dither=none", gif])

    print(f"promo.mp4 {os.path.getsize(mp4)//1024} KB · promo.gif {os.path.getsize(gif)//1024} KB")


if __name__ == "__main__":
    if not os.path.exists(EXE):
        sys.exit("先编译：build.bat")
    shoot()
    if "--stills" not in sys.argv:
        build()
