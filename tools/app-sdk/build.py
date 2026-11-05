"""把一个应用目录编译、打包成 .npk（应用包）。规范见 docs/app-sdk.md。

用法：python tools/app-sdk/build.py 应用目录 [-o 输出.npk]

应用目录里要有 manifest.json 和 .c 源文件，可选 assets/ 目录。编译用 zig（pip install ziglang，模拟器也用它）。
.npk 是不压缩的 tar：manifest.json、app.wasm、assets/…
"""

import argparse
import io
import json
import pathlib
import re
import subprocess
import sys
import tarfile

HERE = pathlib.Path(__file__).resolve().parent
API = 5  # 和 nomi_sdk.h 的 NOMI_API、固件 nomi_pkg.cc 的 kApi 一致
MAX_WASM = 1024 * 1024


def check_manifest(m):
    """和设备上安装时的检查一致，早点报错。"""
    errors = []
    if not re.fullmatch(r"[a-z0-9][a-z0-9._-]{1,47}", str(m.get("id", ""))):
        errors.append("id 只能用小写字母、数字、点、下划线、减号，2～48 个字符，比如 yourname.novel")
    if not str(m.get("name", "")).strip():
        errors.append("name 要填")
    if not re.fullmatch(r"\d+\.\d+\.\d+", str(m.get("version", ""))):
        errors.append("version 要写成 1.0.0 这样")
    if not isinstance(m.get("api"), int) or m["api"] > API:
        errors.append(f"api 要是不大于 {API} 的整数")
    icon = m.get("icon", {})
    for line in icon.get("lines", []):
        if len(line) < 2 or any(len(p) != 2 or not all(0 <= v <= 36 for v in p) for p in line):
            errors.append("icon.lines 里每条线至少两个点，坐标在 0～36")
            break
    for c in icon.get("circles", []):
        if len(c) != 3:
            errors.append("icon.circles 每个是 [x, y, 直径]")
            break
    mem = m.get("memory_kb", 256)
    if not isinstance(mem, int) or not 64 <= mem <= 1024:
        errors.append("memory_kb 在 64～1024")
    if "tile" in m and not isinstance(m["tile"], bool):
        errors.append("tile 是 true / false")
    voice = m.get("voice", [])
    if voice and "assistant" not in m.get("permissions", []):
        errors.append("有 voice 指令要申请 assistant 权限")
    if len(voice) > 8:
        errors.append("voice 指令最多 8 个")
    for v in voice:
        name, desc = str(v.get("name", "")), v.get("description")
        if not re.fullmatch(r"[a-z0-9_]{1,24}", name):
            errors.append(f"voice 指令的 name 只能用小写字母、数字、下划线（最多 24 个）：{name!r}")
        if not isinstance(desc, str) or not desc or len(desc.encode("utf-8")) > 300:
            errors.append(f"voice 指令「{name}」要写 description（UTF-8 最多 300 字节）")
    opens = m.get("opens", [])
    if not isinstance(opens, list) or len(opens) > 16 or not all(
        isinstance(e, str) and re.fullmatch(r"[a-z0-9]{1,8}", e) for e in opens
    ):
        errors.append('opens 里写小写的扩展名、不带点（比如 "txt"），最多 16 个')
    elif opens and not {"sd.read", "sd.write"} & set(m.get("permissions", [])):
        errors.append("能打开卡上的文件（opens）要申请 sd.read 权限")
    return errors


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("app", type=pathlib.Path)
    ap.add_argument("-o", "--output", type=pathlib.Path)
    args = ap.parse_args()

    app = args.app.resolve()
    manifest = json.loads((app / "manifest.json").read_text(encoding="utf-8"))
    errors = check_manifest(manifest)
    if errors:
        sys.exit("manifest.json 不对：\n  " + "\n  ".join(errors))

    sources = sorted(str(p) for p in app.glob("*.c"))
    if not sources:
        sys.exit("应用目录里没有 .c 文件")
    build = app / "build"
    build.mkdir(exist_ok=True)
    wasm = build / "app.wasm"
    # 内存：manifest 的 memory_kb 向上取整到 64KB 一页；栈 16KB；没有 C 库
    pages = (manifest.get("memory_kb", 256) * 1024 + 65535) // 65536
    subprocess.run([
        sys.executable, "-m", "ziglang", "cc",
        "-target", "wasm32-freestanding", "-O2", "-g0", "-nostdlib", f"-I{HERE}",
        "-Wl,--no-entry", "-Wl,--strip-all", "-Wl,-z,stack-size=16384",
        f"-Wl,--initial-memory={pages * 65536}", f"-Wl,--max-memory={pages * 65536}",
        *sources, "-o", str(wasm),
    ], check=True)
    size = wasm.stat().st_size
    if size > MAX_WASM:
        sys.exit(f"app.wasm 有 {size} 字节，超过 1MB")

    out = args.output or app / "build" / f"{manifest['id']}-{manifest['version']}.npk"
    with tarfile.open(out, "w", format=tarfile.USTAR_FORMAT) as tar:
        data = json.dumps(manifest, ensure_ascii=False, indent=1).encode("utf-8")
        info = tarfile.TarInfo("manifest.json")
        info.size = len(data)
        tar.addfile(info, io.BytesIO(data))
        tar.add(wasm, arcname="app.wasm")
        assets = app / "assets"
        if assets.is_dir():
            for p in sorted(assets.rglob("*")):
                if p.is_file():
                    tar.add(p, arcname="assets/" + p.relative_to(assets).as_posix())
    print(f"{out}  （app.wasm {size} 字节）")


if __name__ == "__main__":
    main()
