"""检查 Nomi 的能力开关（firmware/main/nomi/Kconfig）：关掉一部分能力也编得过。

从全开编过的 firmware/sdkconfig 改出几份，放进 firmware/build/v<名字>/sdkconfig（build/ 不进 git），然后各编一遍：

    python tools/nomi_variants.py                 # 只生成 sdkconfig
    python tools/nomi_variants.py --build         # 生成并逐个编（要在 IDF 环境里跑，一份要几分钟）
    python tools/nomi_variants.py --build min     # 只编其中几份

改了跨能力的代码（nomi_system.cc、nomi_display.cc、nomi_wasm.cc、nomi_pkg.cc、板子文件）后跑一遍。
"""

import argparse
import pathlib
import re
import subprocess
import sys

FW = pathlib.Path(__file__).resolve().parent.parent / "firmware"
VARIANTS = {
    # 全关：只剩表情、对话、时钟、设置
    "min": ["SDCARD", "APPS", "PLAYER", "READER", "PHOTO", "CAMERA", "PHONE", "HOTSPOT"],
    # 有 SD 卡和应用平台，其他都关
    "apps": ["PLAYER", "READER", "PHOTO", "CAMERA", "PHONE", "HOTSPOT"],
    # 有 SD 卡、摄像头、蓝牙、热点，没有应用平台
    "sd": ["APPS", "PLAYER", "READER", "PHOTO"],
}


def write_sdkconfigs(names):
    base = (FW / "sdkconfig").read_text(encoding="utf-8")
    if "CONFIG_NOMI=y" not in base:
        sys.exit("firmware/sdkconfig 里没有 CONFIG_NOMI=y：先按板子编一次全开的")
    for name in names:
        text = base
        for cap in VARIANTS[name]:
            sym = f"CONFIG_NOMI_{cap}"
            text = re.sub(rf"^{sym}=y$", f"# {sym} is not set", text, flags=re.M)
        out = FW / "build" / f"v{name}"
        out.mkdir(parents=True, exist_ok=True)
        (out / "sdkconfig").write_text(text, encoding="utf-8", newline="\n")
        print(f"build/v{name}/sdkconfig：关掉 {' '.join(VARIANTS[name])}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", action="store_true", help="生成后逐个 idf.py build")
    parser.add_argument("names", nargs="*", help=f"只处理这几份（{' / '.join(VARIANTS)}，默认全部）")
    args = parser.parse_args()
    names = args.names or list(VARIANTS)
    unknown = [n for n in names if n not in VARIANTS]
    if unknown:
        sys.exit(f"不认识：{' '.join(unknown)}（可选 {' / '.join(VARIANTS)}）")
    write_sdkconfigs(names)
    if not args.build:
        return
    failed = []
    for name in names:
        build_dir = f"build/v{name}"
        print(f"== 编 {build_dir}", flush=True)
        # 链接那一步命令行太长，Windows 上 ninja 会报 CreateProcess 参数不对：一律走响应文件
        r = subprocess.run(["idf.py", "-B", build_dir, "-D", f"SDKCONFIG={build_dir}/sdkconfig",
                            "-D", "CMAKE_NINJA_FORCE_RESPONSE_FILE=1", "build"],
                           cwd=FW, shell=sys.platform == "win32")
        if r.returncode != 0:
            failed.append(name)
    print("全部编过" if not failed else f"没编过：{' '.join(failed)}")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
