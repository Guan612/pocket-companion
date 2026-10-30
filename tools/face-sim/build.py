"""把固件里的表情代码编成 face.wasm（浏览器模拟器用），并可渲染六个状态的截图。

用法：
  python build.py            # 生成 face.wasm，然后 python -m http.server 8800 打开 sim.html
  python build.py --frames   # 另外输出 out/*.ppm，每个状态一张
需要 `pip install ziglang`。
"""

import argparse
import pathlib
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
FACE_DIR = HERE.parent.parent / "firmware" / "main" / "boards" / "spotpear" / "spotpear-esp32s3-max35"
FACE_SOURCES = [str(FACE_DIR / "face.c"), str(FACE_DIR / "raster.c")]
EXPORTS = ["fb", "w", "h", "init", "set_mode", "get_mode", "set_level", "step", "render",
           "dirty_x0", "dirty_y0", "dirty_x1", "dirty_y1"]


def zig_cc(*args):
    subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O2", "-Wall", "-Wextra",
                    f"-I{FACE_DIR}", *args], check=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--frames", action="store_true", help="同时渲染各状态截图到 out/")
    args = parser.parse_args()

    zig_cc("-target", "wasm32-wasi", "-mexec-model=reactor",
           *(f"-Wl,--export=sim_{name}" for name in EXPORTS),
           str(HERE / "sim_exports.c"), *FACE_SOURCES, "-o", str(HERE / "face.wasm"))
    print(f"built {HERE / 'face.wasm'}")

    if args.frames:
        out = HERE / "out"
        out.mkdir(exist_ok=True)
        exe = out / ("render.exe" if sys.platform == "win32" else "render")
        zig_cc(str(HERE / "render_frames.c"), *FACE_SOURCES, "-o", str(exe))
        subprocess.run([str(exe), str(out)], check=True)


if __name__ == "__main__":
    main()
