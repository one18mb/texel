#!/usr/bin/env python3
# 生成 texel/字画 的图标：挂轴（顶轴+宣纸+底轴，等长）+ 纸上墨迹「字」(子带竖钩) + 红印章
# 输出 src/texel.ico，含 16/32/48/256 四尺寸（整数放大，保持像素锐利）
import struct, zlib, sys

N = 16
OUT = sys.argv[1] if len(sys.argv) > 1 else "src/texel.ico"

def build():
    px = [0] * (N * N * 4)
    def s(x, y, c):
        if 0 <= x < N and 0 <= y < N:
            i = (y * N + x) * 4
            px[i:i+4] = list(c)
    ROD   = (0x6b, 0x4a, 0x24, 255)   # 轴(木)
    RODH  = (0x9a, 0x70, 0x38, 255)   # 轴头高光
    PAPER = (0xf7, 0xf0, 0xdc, 255)   # 宣纸
    PEDGE = (0xdc, 0xce, 0xa8, 255)   # 纸边
    INK   = (0x24, 0x24, 0x24, 255)   # 墨
    SEAL  = (0xbe, 0x38, 0x2b, 255)   # 印章
    for x in range(1, 15): s(x, 1, ROD)          # 顶轴
    s(1, 1, RODH); s(14, 1, RODH)
    for x in range(1, 15): s(x, 14, ROD)         # 底轴(与顶轴等长)
    s(1, 14, RODH); s(14, 14, RODH)
    for y in range(2, 14):
        for x in range(2, 14): s(x, y, PAPER)    # 宣纸
    for x in range(2, 14): s(x, 2, PEDGE); s(x, 13, PEDGE)
    for y in range(2, 14): s(2, y, PEDGE); s(13, y, PEDGE)
    Z = [(7, 3)]                                 # 宀 点
    Z += [(x, 4) for x in range(4, 12)]          # 宀 横
    Z += [(4, 5), (11, 5)]                        # 宀 两端
    Z += [(x, 6) for x in range(5, 11)]          # 子 横
    Z += [(9, 7)]                                # 竖钩 起
    Z += [(x, 8) for x in range(4, 12)]          # 子 一
    Z += [(8, 9), (8, 10)]                        # 竖钩 中
    Z += [(7, 11), (8, 11)]                       # 竖钩 勾(左)
    for x, y in Z: s(x, y, INK)
    for y in range(11, 13):                       # 红印章
        for x in range(11, 13): s(x, y, SEAL)
    return px

def upscale(flat, k):
    n = N * k
    out = [0] * (n * n * 4)
    for y in range(n):
        for x in range(n):
            si = ((y // k) * N + (x // k)) * 4
            di = (y * n + x) * 4
            out[di:di+4] = flat[si:si+4]
    return out

def png(n, rgba):
    raw = bytearray()
    for y in range(n):
        raw.append(0)
        raw += bytes(rgba[y*n*4:(y+1)*n*4])
    comp = zlib.compress(bytes(raw), 9)
    def ch(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t+d) & 0xffffffff)
    return (b'\x89PNG\r\n\x1a\n'
            + ch(b'IHDR', struct.pack('>IIBBBBB', n, n, 8, 6, 0, 0, 0))
            + ch(b'IDAT', comp) + ch(b'IEND', b''))

def main():
    base = build()
    entries = [(16, png(16, base))]
    for k in (2, 3, 16):
        entries.append((N*k, png(N*k, upscale(base, k))))
    out = struct.pack('<HHH', 0, 1, len(entries))
    dirs = b''; datas = b''; off = 6 + 16 * len(entries)
    for n, d in entries:
        w = 0 if n >= 256 else n
        dirs += struct.pack('<BBBBHHII', w, w, 0, 0, 1, 32, len(d), off)
        off += len(d); datas += d
    open(OUT, 'wb').write(out + dirs + datas)
    print("wrote", OUT, len(out + dirs + datas), "bytes", [n for n, _ in entries])

if __name__ == '__main__':
    main()
