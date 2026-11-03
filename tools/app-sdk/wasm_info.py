"""看一个 app.wasm 导入了哪些系统接口、导出了哪些函数、要多少内存（不用装 wabt）。

用法：python tools/app-sdk/wasm_info.py app.wasm
"""

import sys


def leb(b, i):
    r = s = 0
    while True:
        x = b[i]
        i += 1
        r |= (x & 0x7F) << s
        s += 7
        if x < 0x80:
            return r, i


def name(b, i):
    n, i = leb(b, i)
    return b[i:i + n].decode(), i + n


def main():
    data = open(sys.argv[1], "rb").read()
    assert data[:4] == b"\0asm", "不是 wasm 文件"
    i = 8
    while i < len(data):
        sid = data[i]
        i += 1
        size, i = leb(data, i)
        end = i + size
        if sid == 2:  # import
            cnt, j = leb(data, i)
            for _ in range(cnt):
                mod, j = name(data, j)
                nm, j = name(data, j)
                kind = data[j]
                j += 1
                if kind == 0:
                    _, j = leb(data, j)
                elif kind == 2:
                    flags = data[j]
                    j += 1
                    _, j = leb(data, j)
                    if flags & 1:
                        _, j = leb(data, j)
                else:
                    j += 2
                print("导入", f"{mod}.{nm}", ["函数", "表", "内存", "全局"][kind])
        elif sid == 5:  # memory
            cnt, j = leb(data, i)
            flags = data[j]
            j += 1
            mn, j = leb(data, j)
            mx = leb(data, j)[0] if flags & 1 else None
            print("内存", f"{mn} 页（{mn * 64}KB）", f"最多 {mx} 页" if mx is not None else "不限")
        elif sid == 7:  # export
            cnt, j = leb(data, i)
            for _ in range(cnt):
                nm, j = name(data, j)
                kind = data[j]
                j += 1
                _, j = leb(data, j)
                print("导出", nm, ["函数", "表", "内存", "全局"][kind])
        i = end


if __name__ == "__main__":
    main()
