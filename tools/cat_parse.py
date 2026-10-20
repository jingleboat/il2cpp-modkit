# -*- coding: utf-8 -*-
"""Addressables catalog 二进制 blob 解析/重建验证

目标：完整解析 m_KeyDataString / m_BucketDataString / m_EntryDataString / m_ExtraDataString，
      再原样重建，逐字节比对，确认理解正确 —— 之后才能往里加新条目。
"""
import base64
import json
import os
import struct
import sys

# 用法: python cat_parse.py <catalog_0.1.json>
# 不带参数时读当前目录下的 catalog_0.1.json
CAT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    os.path.dirname(os.path.abspath(__file__)), 'catalog_0.1.json')


class R:
    def __init__(self, b):
        self.b = b
        self.p = 0

    def i32(self):
        v = struct.unpack_from('<i', self.b, self.p)[0]
        self.p += 4
        return v

    def u8(self):
        v = self.b[self.p]
        self.p += 1
        return v

    def bytes(self, n):
        v = self.b[self.p:self.p + n]
        self.p += n
        return v

    def left(self):
        return len(self.b) - self.p


class W:
    def __init__(self):
        self.out = bytearray()

    def i32(self, v):
        self.out += struct.pack('<i', v)

    def u8(self, v):
        self.out.append(v & 0xFF)

    def bytes(self, b):
        self.out += b

    def data(self):
        return bytes(self.out)


# ---------------------------------------------------------------- KeyData
def parse_keys(blob):
    r = R(blob)
    n = r.i32()
    keys = []
    for _ in range(n):
        t = r.u8()
        if t == 0:      # ASCII string
            ln = r.i32()
            v = r.bytes(ln).decode('utf-8', 'replace')
        elif t == 1:    # Unicode string
            ln = r.i32()
            v = r.bytes(ln * 2).decode('utf-16-le', 'replace')
        elif t in (2, 3, 4):
            v = ('int', struct.unpack_from('<i', blob, r.p)[0])
            r.p += 4
        elif t == 5:    # Hash128
            v = ('hash', r.bytes(16).hex())
        else:
            v = ('?type%d' % t, r.bytes(8).hex())
        keys.append((t, v))
    return keys, r.left()


def build_keys(keys):
    w = W()
    w.i32(len(keys))
    for t, v in keys:
        w.u8(t)
        if t in (0, 1):
            enc = v.encode('utf-8') if t == 0 else v.encode('utf-16-le')
            w.i32(len(v) if t == 0 else len(v))
            w.bytes(enc)
        elif t in (2, 3, 4):
            w.i32(v[1])
        elif t == 5:
            w.bytes(bytes.fromhex(v[1]))
        else:
            w.bytes(bytes.fromhex(v[1]))
    return w.data()


# ------------------------------------------------------------- BucketData
def parse_buckets(blob):
    r = R(blob)
    n = r.i32()
    out = []
    for _ in range(n):
        off = r.i32()
        cnt = r.i32()
        ents = [r.i32() for _ in range(cnt)]
        out.append((off, ents))
    return out, r.left()


def build_buckets(bs):
    w = W()
    w.i32(len(bs))
    for off, ents in bs:
        w.i32(off)
        w.i32(len(ents))
        for e in ents:
            w.i32(e)
    return w.data()


# -------------------------------------------------------------- EntryData
def parse_entries(blob):
    r = R(blob)
    n = r.i32()
    out = []
    for _ in range(n):
        out.append(tuple(r.i32() for _ in range(7)))
    return out, r.left()


def build_entries(es):
    w = W()
    w.i32(len(es))
    for e in es:
        for x in e:
            w.i32(x)
    return w.data()


def main():
    cat = json.load(open(CAT, encoding='utf-8-sig'))
    print('catalog:', CAT)
    print('InternalIds (%d):' % len(cat['m_InternalIds']))

    for name, parse, build in (
        ('m_KeyDataString', parse_keys, build_keys),
        ('m_BucketDataString', parse_buckets, build_buckets),
        ('m_EntryDataString', parse_entries, build_entries),
    ):
        blob = base64.b64decode(cat[name])
        items, left = parse(blob)
        rebuilt = build(items)
        same = rebuilt == blob
        print('\n=== %s  bin=%d  items=%d  剩余=%d  重建一致=%s' % (name, len(blob), len(items), left, same))
        if not same:
            print('   重建 %d 字节，首个差异 @%d' % (len(rebuilt), next(i for i in range(min(len(rebuilt), len(blob))) if rebuilt[i] != blob[i])))
        if name == 'm_KeyDataString':
            for i, (t, v) in enumerate(items):
                print('   [%2d] type=%d %s' % (i, t, v if len(str(v)) < 90 else str(v)[:90] + '…'))
        if name == 'm_BucketDataString':
            for i, (off, ents) in enumerate(items):
                print('   [%2d] keyOffset=%-5d entries=%s' % (i, off, ents))
        if name == 'm_EntryDataString':
            for i, e in enumerate(items):
                print('   [%2d] internalId=%d provider=%d depKey=%d depHash=%08x extra=%d primaryKey=%d resType=%d'
                      % (i, e[0], e[1], e[2], e[3] & 0xFFFFFFFF, e[4], e[5], e[6]))
        if name == 'm_EntryDataString':
            print('\n   InternalIds 对照:')
            for i, s in enumerate(cat['m_InternalIds']):
                print('     [%2d] %s' % (i, s))


if __name__ == '__main__':
    main()
