#!/usr/bin/env python3
# 合并一个或多个 unifont .hex，编译成紧凑二进制点阵表
# 记录 37 字节：码点(uint32 LE) + 宽度(1=半角 2=全角) + 16 行 × uint16(大端, bit15=最左)
# 用法: mkfont.py 输出.bin 输入1.hex [输入2.hex ...]
import sys, struct

def parse(path, glyphs):
    with open(path, 'r', encoding='latin-1') as f:
        for line in f:
            line = line.strip()
            if not line or ':' not in line:
                continue
            cp_s, hex_s = line.split(':', 1)
            try:
                cp = int(cp_s, 16)
            except ValueError:
                continue
            h = hex_s.strip()
            try:
                if len(h) == 32:
                    b = bytes.fromhex(h); rows = [b[r] << 8 for r in range(16)]; w = 1
                elif len(h) == 64:
                    b = bytes.fromhex(h); rows = [(b[r*2] << 8) | b[r*2+1] for r in range(16)]; w = 2
                else:
                    continue
            except ValueError:
                continue
            glyphs[cp] = (w, rows)

def main(dst, srcs):
    glyphs = {}
    for s in srcs:
        parse(s, glyphs)
    out = bytearray()
    for cp in sorted(glyphs):
        w, rows = glyphs[cp]
        out += struct.pack('<I', cp)
        out.append(w)
        out += b''.join(struct.pack('>H', r) for r in rows)
    with open(dst, 'wb') as f:
        f.write(out)
    print("glyphs=%d bytes=%d" % (len(glyphs), len(out)))

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2:])
