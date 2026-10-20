"""ARM64 disassembler for the APK's libil2cpp.so.

Maps ELF vaddr -> file offset, disassembles a range, and annotates:
  * string literals  (adrp+ldr slot -> ScriptString.Value from script.json)
  * bl targets       (-> ScriptMethod.Name)

usage: python armdis.py <rva_hex> [insn_count]
"""
import sys, json, re, os
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

HERE = os.path.dirname(os.path.abspath(__file__))
_dump = os.environ.get("ARM_DUMP", "dumpout")
SO = os.environ.get("ARM_SO", os.path.join(HERE, "libil2cpp.so"))
SCRIPT_JSON = os.path.join(HERE, _dump, "script.json")
CACHE = os.path.join(HERE, _dump, "_armdis_index.json")

# ---------- ELF ----------
data = open(SO, "rb").read()
assert data[:4] == b"\x7fELF"
import struct
e_phoff, = struct.unpack_from("<Q", data, 0x20)
e_phentsize, e_phnum = struct.unpack_from("<HH", data, 0x36)
SEGS = []
for i in range(e_phnum):
    off = e_phoff + i * e_phentsize
    p_type, p_flags = struct.unpack_from("<II", data, off)
    p_offset, p_vaddr, p_paddr, p_filesz, p_memsz = struct.unpack_from("<QQQQQ", data, off + 8)
    if p_type == 1:  # PT_LOAD
        SEGS.append((p_vaddr, p_offset, p_filesz))


def foff(va):
    for vaddr, offset, filesz in SEGS:
        if vaddr <= va < vaddr + filesz:
            return offset + (va - vaddr)
    return None


# ---------- symbol index ----------
def build_index():
    if os.path.exists(CACHE):
        with open(CACHE, "r", encoding="utf8") as f:
            j = json.load(f)
        return ({int(k): v for k, v in j["strings"].items()},
                {int(k): v for k, v in j["methods"].items()},
                {int(k): v for k, v in j.get("usages", {}).items()})
    raw = open(SCRIPT_JSON, "rb").read()
    strings, methods = {}, {}
    m = raw.find(b'"ScriptString"')
    if m > 0:
        end = raw.find(b"\n  ],", m)
        seg = raw[m:end].decode("utf8", "replace")
        for a, v in re.findall(r'"Address":\s*(\d+),\s*"Value":\s*"((?:[^"\\]|\\.)*)"', seg):
            try:
                strings[int(a)] = json.loads('"' + v + '"')
            except Exception:
                strings[int(a)] = v
    m = raw.find(b'"ScriptMethod"')
    if m > 0:
        end = raw.find(b"\n  ],", m)
        seg = raw[m:end].decode("utf8", "replace")
        for a, n in re.findall(r'"Address":\s*(\d+),\s*"Name":\s*"((?:[^"\\]|\\.)*)"', seg):
            try:
                methods[int(a)] = json.loads('"' + n + '"')
            except Exception:
                methods[int(a)] = n
    m = raw.find(b'"ScriptMetadata"')
    usages = {}
    if m > 0:
        end = raw.find(b"\n  ],", m)
        seg = raw[m:end].decode("utf8", "replace")
        names = {}
        for a, n in re.findall(r'"Address":\s*(\d+),\s*"Name":\s*"((?:[^"\\]|\\.)*)"', seg):
            try:
                names[int(a)] = json.loads('"' + n + '"')
            except Exception:
                names[int(a)] = n
        m2 = raw.find(b'"Addresses"')
        if m2 > 0:
            end2 = raw.find(b"\n  ]", m2)
            seg2 = raw[m2:end2].decode("utf8", "replace")
            seg2 = seg2[seg2.find("[") + 1:]
            addrs = [int(x) for x in re.findall(r"\d+", seg2.replace('"Addresses"', ""))]
            for i, va in enumerate(addrs):
                if i in names:
                    usages[va] = names[i]
    with open(CACHE, "w", encoding="utf8") as f:
        json.dump({"strings": strings, "methods": methods, "usages": usages}, f)
    return strings, methods, usages


STRINGS, METHODS, USAGES = build_index()
md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)


def readq(va):
    o = foff(va)
    if o is None or o + 8 > len(data):
        return None
    import struct as _s
    return _s.unpack_from("<Q", data, o)[0]


def dis(rva, count=80):
    off = foff(rva)
    print("=== RVA %#x file off %#x ===" % (rva, off or 0))
    regs = {}
    n = 0
    for i in md.disasm(data[off: off + count * 4 + 64], rva):
        note = ""
        mnem, ops = i.mnemonic, i.op_str
        # adrp xN, #imm
        if mnem == "adrp" and ", #" in ops:
            r, imm = ops.split(", #")
            try:
                regs[r.strip()] = int(imm, 0)  # capstone gives absolute page
            except ValueError:
                pass
            tgt = regs.get(r.strip())
            if tgt in STRINGS:
                note = "  ; <page only>"
        elif mnem == "add" and ", #" in ops and ops.count(",") == 2:
            r, base, imm = ops.split(",")
            base = base.strip()
            if base in regs:
                try:
                    regs[r.strip()] = regs[base] + int(imm.split("#")[1], 0)
                except ValueError:
                    pass
        elif mnem == "ldr" and "[" in ops:
            dst, rest = ops.split(",", 1)
            inner = rest[rest.find("[") + 1: rest.find("]")]
            parts = [p.strip() for p in inner.split(",")]
            base = parts[0]
            imm = 0
            if len(parts) > 1:
                try:
                    imm = int(parts[1].lstrip("#"), 0)
                except ValueError:
                    imm = 0
            if base in regs and not mnem.endswith("b"):
                slot = regs[base] + imm
                if slot in STRINGS:
                    note = '  ; "%s"' % STRINGS[slot].replace("\n", "\\n")[:70]
                elif slot in USAGES:
                    note = "  ; usage=%s" % USAGES[slot]
                elif slot in METHODS:
                    note = "  ; %s" % METHODS[slot]
                else:
                    q = readq(slot)
                    if q:
                        if q in STRINGS:
                            note = '  ; *"%s"' % STRINGS[q].replace("\n", "\\n")[:70]
                        elif q in METHODS:
                            note = "  ; *%s" % METHODS[q]
        elif mnem in ("bl", "b") and ops.startswith("#"):
            try:
                tgt = int(ops[1:], 0)
            except ValueError:
                tgt = None
            if tgt in METHODS:
                note = "  ; -> %s" % METHODS[tgt]
        elif mnem in ("bl",) and ops.startswith("0x"):
            try:
                tgt = int(ops, 0)
            except ValueError:
                tgt = None
            if tgt in METHODS:
                note = "  ; -> %s" % METHODS[tgt]
        print("%08x: %-8s %-40s%s" % (i.address, mnem, ops, note))
        n += 1
        if n >= count:
            break


if __name__ == "__main__":
    rva = int(sys.argv[1], 16)
    cnt = int(sys.argv[2]) if len(sys.argv) > 2 else 80
    dis(rva, cnt)
