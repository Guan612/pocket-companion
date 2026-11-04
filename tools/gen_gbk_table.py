"""生成 GBK → Unicode 对照表，阅读器读 GBK 编码的 TXT 用（nomi_book.cc）。

输出 firmware/main/boards/spotpear/spotpear-esp32s3-max35/preinstall/gbk.tab，编译时打进资源分区
（和预装应用包一起，见 main/CMakeLists.txt 的 DEFAULT_ASSETS_EXTRA_FILES），不占固件空间。

格式：首字节 0x81–0xFE（126 个）× 尾字节 0x40–0xFE（191 个，0x7F 那格空着），每格 2 字节小端 Unicode，
0 表示 GBK 里没有这个字。共 126 × 191 × 2 = 48132 字节。

用法：python tools/gen_gbk_table.py
"""
import pathlib
import struct

OUT = pathlib.Path(__file__).resolve().parent.parent / "firmware/main/boards/spotpear/spotpear-esp32s3-max35/preinstall/gbk.tab"


def main():
    table = bytearray()
    mapped = 0
    for lead in range(0x81, 0xFF):
        for trail in range(0x40, 0xFF):
            cp = 0
            if trail != 0x7F:
                try:
                    s = bytes([lead, trail]).decode("gbk")
                    if len(s) == 1 and ord(s) <= 0xFFFF:
                        cp = ord(s)
                except UnicodeDecodeError:
                    pass
            mapped += cp != 0
            table += struct.pack("<H", cp)
    OUT.write_bytes(table)
    print(f"{OUT}: {len(table)} bytes, {mapped} characters")


if __name__ == "__main__":
    main()
