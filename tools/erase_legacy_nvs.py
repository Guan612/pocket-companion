#!/usr/bin/env python3
"""一次性清理：把老固件留在 nvs 里的电台台单、播客、音乐网站擦掉（不迁移）。

第 ④ 步删掉固件里的电台 / 播客 / 在线业务后，nomi_radio / nomi_pod / nomi_music 三个 NVS 命名空间
没人读了，白占 nvs（一共才 16KB）。固件里不放清理代码（只跑一次的事），在电脑上做：

    1. esptool 把 nvs 分区（0x9000 起，16KB，见 docs/flash-partitions.md）读下来（留作备份）
    2. 用 IDF 自带的 nvs_partition_tool 解析出条目
    3. 丢掉那三个命名空间的条目；其余的（Wi-Fi、大模型配置、闹钟、长期记忆……）原样写回
    4. nvs_partition_gen 生成新镜像，esptool 写回去（会自动复位重启设备）

用法（要在 ESP-IDF 的 Python 环境里跑，要 esptool；Windows 上先
. C:\\Espressif\\tools\\Microsoft.v6.1.PowerShell_profile.ps1）：

    python tools/erase_legacy_nvs.py --port COM11            # 对着设备做
    python tools/erase_legacy_nvs.py --port COM11 --dry-run  # 只看会删掉什么，不动设备
    python tools/erase_legacy_nvs.py --bin 备份.bin          # 只转换一个镜像文件，不碰设备

后悔药：脚本会把读下来的分区存成 nvs-backup-<时间>.bin，要恢复就
esptool write-flash 0x9000 那个文件。
"""

import argparse
import csv
import datetime
import json
import os
import subprocess
import sys
import tempfile

# 老固件的三个命名空间（删掉的 nomi_radio / nomi_podcast / nomi_music 用的）
LEGACY_NAMESPACES = ["nomi_radio", "nomi_pod", "nomi_music"]

# nvs 分区在闪存上的位置（firmware/partitions/v2/16m_apps.csv）
NVS_OFFSET = 0x9000
NVS_SIZE = 0x4000

# 解析出来的编码名（nvs_partition_tool）→ 生成时要的编码名（nvs_partition_gen）
ENCODINGS = {
    "uint8_t": "u8", "int8_t": "i8", "uint16_t": "u16", "int16_t": "i16",
    "uint32_t": "u32", "int32_t": "i32", "uint64_t": "u64", "int64_t": "i64",
    "string": "string",
    "blob_data": "base64",  # 解析给的就是 base64，生成也认 base64
    "blob": "base64",
}


def idf_path() -> str:
    """找 ESP-IDF 的安装目录（要用它 components 里的 nvs_partition_tool 解析分区）。"""
    for p in [os.environ.get("IDF_PATH"), r"C:\Espressif\frameworks\esp-idf",
              os.path.expanduser("~/esp/esp-idf"), "/opt/esp-idf"]:
        if p and os.path.isfile(os.path.join(p, "components", "nvs_flash", "nvs_partition_tool", "nvs_tool.py")):
            return p
    sys.exit("找不到 ESP-IDF（看了 IDF_PATH 和几个常见位置）；跑这个脚本的 Python 得是 IDF 那个环境")


def parse_entries(bin_path: str) -> list:
    """NVS 镜像 → [{namespace, key, encoding, value}]（只留还写着的；字符串拼好、blob 是 base64）。"""
    tool = os.path.join(idf_path(), "components", "nvs_flash", "nvs_partition_tool", "nvs_tool.py")
    out = subprocess.run(
        [sys.executable, tool, "--format", "json", "--dump", "minimal", bin_path],
        check=True, capture_output=True, text=True).stdout
    return json.loads(out)


def build_csv(entries: list, drop: list[str], csv_path: str) -> tuple[int, int]:
    """条目重新写成 nvs_partition_gen 认的 CSV：丢掉 drop 里的命名空间，别的原样。
    同一个命名空间里同名的键可能出现两条都"写着的"（NVS 追加写改值、旧的还没标擦除）：
    取最后一条（后写的生效），不然生成器会把重复键写进去。返回（保留、丢掉）条数"""
    latest, dropped, namespaces = {}, 0, []
    for e in entries:
        if e["namespace"] in drop:
            dropped += 1
            continue
        if (e["namespace"], e["key"]) not in latest and e["namespace"] not in namespaces:
            namespaces.append(e["namespace"])
        latest[(e["namespace"], e["key"])] = e
    with open(csv_path, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["key", "type", "encoding", "value"])
        for ns in namespaces:
            w.writerow([ns, "namespace", "", ""])
            for (ens, key), e in latest.items():
                if ens != ns:
                    continue
                if e["encoding"] not in ENCODINGS:
                    sys.exit(f"不认识的编码 {e['encoding']}（{ns}:{key}），别硬写，先看看")
                w.writerow([key, "data", ENCODINGS[e["encoding"]], e["data"]])
    return len(latest), dropped


def generate(csv_path: str, bin_path: str) -> None:
    subprocess.run(
        [sys.executable, "-m", "esp_idf_nvs_partition_gen", "generate", csv_path, bin_path, hex(NVS_SIZE)],
        check=True, capture_output=True, text=True)


def esptool(port: str, *args: str) -> None:
    cmd = [sys.executable, "-m", "esptool", "--chip", "esp32s3", "-p", port, "-b", "460800",
           "--before", "default-reset", "--after", "hard-reset", *args]
    subprocess.run(cmd, check=True)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="串口，比如 COM11；给了就对设备做")
    ap.add_argument("--bin", help="对一个镜像文件做（不碰设备），输出写到 <名>-clean.bin")
    ap.add_argument("--dry-run", action="store_true", help="只看会删掉什么，不写回")
    ap.add_argument("--drop", nargs="*", default=LEGACY_NAMESPACES,
                    help=f"要清掉的命名空间（默认 {' '.join(LEGACY_NAMESPACES)}）")
    args = ap.parse_args()

    if not args.port and not args.bin:
        ap.error("--port 和 --bin 至少给一个")

    backup = None
    if args.port:
        backup = f"nvs-backup-{datetime.datetime.now():%Y%m%d-%H%M%S}.bin"
        print(f"把 nvs 分区（0x{NVS_OFFSET:x}，{NVS_SIZE // 1024}KB）读到 {backup} …")
        esptool(args.port, "read-flash", hex(NVS_OFFSET), hex(NVS_SIZE), backup)
        source = backup
    else:
        source = args.bin

    entries = parse_entries(source)
    by_ns = {}
    for e in entries:
        by_ns.setdefault(e["namespace"], []).append(e)
    print(f"读出 {len(entries)} 条：")
    for ns, items in by_ns.items():
        mark = "  ← 清掉" if ns in args.drop else ""
        print(f"  {ns}: {len(items)} 条{mark}")
    if not any(ns in args.drop for ns in by_ns):
        print("要清的命名空间都不在，没什么可做的。")
        return

    with tempfile.TemporaryDirectory() as tmp:
        csv_path = os.path.join(tmp, "nvs.csv")
        new_bin = os.path.join(tmp, "nvs.bin")
        keep, dropped = build_csv(entries, args.drop, csv_path)
        if keep == 0:
            sys.exit("除了要清的之外一条都不剩，这不对劲，不写了——要全清用 esptool erase-region")
        generate(csv_path, new_bin)
        # 生成出来的再解析一遍逐条比对（键、编码、值），别把好数据弄丢
        reread = parse_entries(new_bin)
        # 同键取最后一条，和 build_csv 一个口径
        want = {}
        for e in entries:
            if e["namespace"] not in args.drop:
                want[(e["namespace"], e["key"])] = (e["encoding"], e["data"])
        got = {(e["namespace"], e["key"]): (e["encoding"], e["data"]) for e in reread}
        if got != want:
            print("写回去一查对不上，不写了。差异：", file=sys.stderr)
            for k in sorted(set(want) | set(got)):
                if want.get(k) != got.get(k):
                    print(f"  {k}: 原来的 {want.get(k)} → 新镜像里 {got.get(k)}", file=sys.stderr)
            sys.exit(1)
        print(f"保留 {keep} 条，清掉 {dropped} 条，逐条比对一致。")
        if args.dry_run:
            print("（--dry-run：不写回）")
            return
        if args.port:
            print(f"写回 {backup} 同目录的新镜像并重启设备 …")
            esptool(args.port, "write-flash", hex(NVS_OFFSET), new_bin)
            print(f"好了。原样留着 {backup}，要恢复：esptool write-flash 0x{NVS_OFFSET:x} {backup}")
        else:
            out = args.bin.rsplit(".", 1)[0] + "-clean.bin"
            with open(new_bin, "rb") as i, open(out, "wb") as o:
                o.write(i.read())
            print(f"写了 {out}")


if __name__ == "__main__":
    main()
