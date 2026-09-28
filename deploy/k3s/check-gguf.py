#!/usr/bin/env python3
"""GGUF 完整性检查：张量数 / block_count / 层号覆盖 / 文件大小。

用途：模型下载/转换后先跑一遍，避免"只有一半层"的文件进生产。
用法：python3 deploy/k3s/check-gguf.py /path/to/model.gguf
退出码：0=看起来完整；1=缺层/截断（不可用）。
"""
import os
import re
import struct
import sys

SIZES = {0: 1, 1: 1, 2: 2, 3: 2, 4: 4, 5: 4, 6: 4, 7: 1, 10: 8, 11: 8, 12: 8}


def main(path: str) -> int:
    size = os.path.getsize(path)
    f = open(path, "rb")
    if f.read(4) != b"GGUF":
        print("not a GGUF file")
        return 1
    struct.unpack("<I", f.read(4))          # version
    n_tensors = struct.unpack("<Q", f.read(8))[0]
    n_kv = struct.unpack("<Q", f.read(8))[0]

    def rstr() -> str:
        n = struct.unpack("<Q", f.read(8))[0]
        return f.read(n).decode("utf-8", "replace")

    def skip(t: int) -> None:
        if t in SIZES:
            f.read(SIZES[t]); return
        if t == 8:
            rstr(); return
        if t == 9:
            et = struct.unpack("<I", f.read(4))[0]
            n = struct.unpack("<Q", f.read(8))[0]
            for _ in range(n):
                skip(et)
            return
        raise ValueError("unknown value type %d" % t)

    def rval(t: int):
        if t == 8:
            return rstr()
        if t == 9:
            et = struct.unpack("<I", f.read(4))[0]
            n = struct.unpack("<Q", f.read(8))[0]
            return [rval(et) for _ in range(n)]
        return int.from_bytes(f.read(SIZES[t]), "little")

    arch = name = None
    block_count = None
    for _ in range(n_kv):
        k = rstr()
        t = struct.unpack("<I", f.read(4))[0]
        if k == "general.architecture":
            arch = rval(t)
        elif k == "general.name":
            name = rval(t)
        elif k.endswith(".block_count"):
            block_count = rval(t)
        else:
            skip(t)

    names = []
    for _ in range(n_tensors):
        nm = rstr()
        nd = struct.unpack("<I", f.read(4))[0]
        f.read(8 * nd)
        struct.unpack("<I", f.read(4))[0]   # ggml type
        struct.unpack("<Q", f.read(8))[0]   # offset
        names.append(nm)

    layers = sorted({int(m.group(1)) for n in names for m in [re.search(r"blk\.(\d+)\.", n)] if m})
    gib = size / (1024 ** 3)
    print("file      : %s" % path)
    print("size      : %.2f GiB" % gib)
    print("arch/name : %s / %s" % (arch, name))
    print("tensors   : %d" % n_tensors)
    print("block_cnt : %s" % block_count)
    print("layers    : %s" % (("%d..%d (%d)" % (layers[0], layers[-1], len(layers))) if layers else "none"))

    ok = True
    if block_count is not None and layers and len(layers) < int(block_count):
        print("FAIL      : 缺层 —— 文件应为 %d 层，实际只有 %d 层（下载/转换不完整）"
              % (int(block_count), len(layers)))
        ok = False
    if block_count is not None and n_tensors < int(block_count):
        print("FAIL      : 张量数 %d < block_count %s" % (n_tensors, block_count))
        ok = False
    print("RESULT    : %s" % ("looks complete" if ok else "incomplete / unusable"))
    return 0 if ok else 1


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
