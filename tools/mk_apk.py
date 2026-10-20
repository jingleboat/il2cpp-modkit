"""Repack an APK (raw copy, no recompression) with entry replacements, then sign v1 + v2.

usage:
  python mk_apk.py build --src in.apk --out out.apk --replace NAME=FILE [--replace ...]
  python mk_apk.py sign  --apk file.apk
  python mk_apk.py keygen
"""
import argparse
import hashlib
import os
import struct
import zipfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
KEY_DIR = os.path.join(HERE, "keystore")

ALGO_SHA256 = 1
ALGO_SHA512 = 2
CHUNK = 1024 * 1024
V2_BLOCK_ID = 0x7109871A
STRIPPING_PROT_ID = 0xBEEFF00D


# ------------------------------------------------------------------ key
def ensure_key():
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
    from cryptography.x509.oid import NameOID
    import datetime

    os.makedirs(KEY_DIR, exist_ok=True)
    key_p = os.path.join(KEY_DIR, "debug.pem")
    crt_p = os.path.join(KEY_DIR, "debug.crt")
    if os.path.exists(key_p) and os.path.exists(crt_p):
        key = serialization.load_pem_private_key(open(key_p, "rb").read(), None)
        crt = x509.load_pem_x509_certificate(open(crt_p, "rb").read())
        return key, crt
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, u"ModKit Debug")])
    crt = (
        x509.CertificateBuilder()
        .subject_name(name).issuer_name(name)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(datetime.datetime(2020, 1, 1))
        .not_valid_after(datetime.datetime(2040, 1, 1))
        .sign(key, hashes.SHA256())
    )
    open(key_p, "wb").write(key.private_bytes(
        serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
        serialization.NoEncryption()))
    open(crt_p, "wb").write(crt.public_bytes(serialization.Encoding.PEM))
    print("generated key in", KEY_DIR)
    return key, crt


# ------------------------------------------------------------------ zip writer
def _dos(dt):
    y, mo, d, h, mi, s = dt
    return (h << 11) | (mi << 5) | (s // 2), ((y - 1980) << 9) | (mo << 5) | d


def _pad_extra(offset, align, namelen):
    """extra bytes so that (offset + 30 + namelen + len(extra)) % align == 0."""
    if align <= 1:
        return b""
    need = (align - ((offset + 30 + namelen) % align)) % align
    if need == 0:
        return b""
    if need < 4:
        need += align
    body = b"\x00" * (need - 4)
    return struct.pack("<HH", 0xD935, len(body)) + body


def build(src, out, replacements, additions=None):
    additions = additions or {}
    zin = zipfile.ZipFile(src)
    src_f = open(src, "rb")
    entries = []  # (name, sha256(plain))
    cen = []
    offset = 0
    n = 0
    with open(out, "wb") as f:
        for info in zin.infolist():
            name = info.filename
            upper = name.upper()
            if name.startswith("META-INF/") and (
                upper.endswith((".SF", ".RSA", ".DSA", ".EC")) or name == "META-INF/MANIFEST.MF"
            ):
                continue
            raw_name = name.encode("utf8")
            if name in replacements:
                plain = open(replacements[name], "rb").read()
                method = zipfile.ZIP_DEFLATED
                co = zlib.compressobj(9, zlib.DEFLATED, -15)
                raw = co.compress(plain) + co.flush()
                crc = zlib.crc32(plain) & 0xFFFFFFFF
                usize = len(plain)
                print("replace %-40s %d -> %d bytes" % (name, usize, len(raw)))
            else:
                src_f.seek(info.header_offset)
                lh = src_f.read(30)
                assert lh[:4] == b"PK\x03\x04", name
                nm_len, ex_len = struct.unpack("<HH", lh[26:30])
                src_f.seek(info.header_offset + 30 + nm_len + ex_len)
                raw = src_f.read(info.compress_size)
                method = info.compress_type
                crc = info.CRC
                usize = info.file_size
                plain = raw if method == zipfile.ZIP_STORED else zlib.decompress(raw, -15)
            entries.append((name, hashlib.sha256(plain).digest()))
            align = 4
            if name == "resources.arsc":
                align = 4
            elif name.startswith("lib/") and name.endswith(".so") and method == zipfile.ZIP_STORED:
                align = 4096
            ft, dt = _dos(info.date_time)
            extra = _pad_extra(offset, align, len(raw_name))
            hdr = struct.pack("<IHHHHHIIIHH", 0x04034B50, 20, 0, method, ft, dt, crc,
                              len(raw), usize, len(raw_name), len(extra))
            f.write(hdr + raw_name + extra)
            data_off = offset + len(hdr) + len(raw_name) + len(extra)
            assert data_off % align == 0, (name, data_off, align)
            f.write(raw)
            cen.append((hdr, raw_name, extra, offset))
            offset = data_off + len(raw)
            n += 1

        # ---- 新增条目（例如把自研 so 放进去）
        seen = {e[0] for e in entries}
        for name, path in additions.items():
            if name in seen:
                print("add %-40s 已存在，跳过" % name)
                continue
            plain = open(path, "rb").read()
            method = zipfile.ZIP_DEFLATED
            co = zlib.compressobj(9, zlib.DEFLATED, -15)
            raw = co.compress(plain) + co.flush()
            crc = zlib.crc32(plain) & 0xFFFFFFFF
            usize = len(plain)
            raw_name = name.encode("utf8")
            align = 4096 if (name.startswith("lib/") and name.endswith(".so")) else 4
            ft, dt = _dos((2020, 1, 1, 0, 0, 0))
            extra = _pad_extra(offset, align, len(raw_name))
            hdr = struct.pack("<IHHHHHIIIHH", 0x04034B50, 20, 0, method, ft, dt, crc,
                              len(raw), usize, len(raw_name), len(extra))
            f.write(hdr + raw_name + extra)
            data_off = offset + len(hdr) + len(raw_name) + len(extra)
            assert data_off % align == 0, (name, data_off, align)
            f.write(raw)
            entries.append((name, hashlib.sha256(plain).digest()))
            cen.append((hdr, raw_name, extra, offset))
            offset = data_off + len(raw)
            n += 1
            print("add     %-40s %d -> %d bytes" % (name, usize, len(raw)))

        for mname, mdata in v1_files(entries):
            raw_name = mname.encode("utf8")
            co = zlib.compressobj(9, zlib.DEFLATED, -15)
            raw = co.compress(mdata) + co.flush()
            crc = zlib.crc32(mdata) & 0xFFFFFFFF
            ft, dt = _dos((2020, 1, 1, 0, 0, 0))
            hdr = struct.pack("<IHHHHHIIIHH", 0x04034B50, 20, 0, zipfile.ZIP_DEFLATED,
                              ft, dt, crc, len(raw), len(mdata), len(raw_name), 0)
            f.write(hdr + raw_name)
            cen.append((hdr, raw_name, b"", offset))
            offset += len(hdr) + len(raw_name)
            f.write(raw)
            offset += len(raw)
            n += 1

        cd_off = offset
        for hdr, raw_name, extra, lho in cen:
            _, ver, flags, method, ft, dt, crc, csize, usize, nl, _el = struct.unpack(
                "<IHHHHHIIIHH", hdr)
            f.write(struct.pack("<IHHHHHHIIIHHHHHII", 0x02014B50, ver, 20, flags, method,
                                ft, dt, crc, csize, usize, nl, len(extra), 0, 0, 0, 0, lho))
            f.write(raw_name + extra)
        cd_size = f.tell() - cd_off
        f.write(struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, n, n, cd_size, cd_off, 0))
    src_f.close()
    print("entries: %d  cd_off: %d  cd_size: %d" % (n, cd_off, cd_size))


def v1_files(entries):
    import base64
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.serialization import pkcs7

    key, crt = ensure_key()
    b64 = lambda b: base64.b64encode(b).decode("ascii")
    head = "Manifest-Version: 1.0\r\nCreated-By: il2cpp-modkit\r\n\r\n"
    sections = ["Name: %s\r\nSHA-256-Digest: %s\r\n\r\n" % (nm, b64(dg)) for nm, dg in entries]
    manifest = (head + "".join(sections)).encode("utf8")

    sf = ["Signature-Version: 1.0\r\n",
          "Created-By: il2cpp-modkit\r\n",
          "SHA-256-Digest-Manifest: %s\r\n" % b64(hashlib.sha256(manifest).digest()),
          "\r\n"]
    for sec in sections:
        nm = sec.split("\r\n", 1)[0] + "\r\n"
        sf.append("%sSHA-256-Digest: %s\r\n\r\n"
                  % (nm, b64(hashlib.sha256(sec.encode("utf8")).digest())))
    sf_bytes = "".join(sf).encode("utf8")

    rsa = (pkcs7.PKCS7SignatureBuilder().set_data(sf_bytes)
           .add_signer(crt, key, hashes.SHA256())
           .sign(serialization.Encoding.DER,
                 [pkcs7.PKCS7Options.DetachedSignature,
                  pkcs7.PKCS7Options.Binary,
                  pkcs7.PKCS7Options.NoCapabilities]))
    print("v1: manifest %d, sf %d, rsa %d" % (len(manifest), len(sf_bytes), len(rsa)))
    return [("META-INF/MANIFEST.MF", manifest),
            ("META-INF/CERT.SF", sf_bytes),
            ("META-INF/CERT.RSA", rsa)]


# ------------------------------------------------------------------ v2
def _chunked_sha(data, algo):
    h = hashlib.sha256 if algo == ALGO_SHA256 else hashlib.sha512
    digs = []
    for i in range(0, len(data), CHUNK):
        piece = data[i:i + CHUNK]
        c = h()
        c.update(b"\xa5" + struct.pack("<I", len(piece)) + piece)
        digs.append(c.digest())
    top = h()
    top.update(b"\x5a" + struct.pack("<I", len(digs)))
    for d in digs:
        top.update(d)
    return top.digest()


def _lp(b):
    return struct.pack("<I", len(b)) + b


def sign_v2(apk_path):
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import padding

    key, crt = ensure_key()
    data = open(apk_path, "rb").read()
    eocd_off = data.rfind(b"PK\x05\x06")
    assert eocd_off > 0
    eocd = data[eocd_off:eocd_off + 22]
    cd_size, cd_off = struct.unpack_from("<II", eocd, 12)
    assert cd_off != 0xFFFFFFFF
    cd = data[cd_off:eocd_off]
    eocd_tail = data[eocd_off:]
    assert eocd_off + 22 + struct.unpack_from("<H", eocd, 20)[0] == len(data)

    sections = [data[:cd_off], cd, eocd_tail]
    digest_items = []
    for algo in (ALGO_SHA256, ALGO_SHA512):
        h = hashlib.sha256 if algo == ALGO_SHA256 else hashlib.sha512
        acc = [_chunked_sha(s, algo) for s in sections]
        top = h()
        top.update(b"\x5a" + struct.pack("<I", len(acc)))
        for d in acc:
            top.update(d)
        digest_items.append(_lp(struct.pack("<I", algo) + _lp(top.digest())))

    cert_der = crt.public_bytes(serialization.Encoding.DER)
    pub_der = key.public_key().public_bytes(
        serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo)

    signed_data = (_lp(b"".join(digest_items))
                   + _lp(_lp(cert_der))
                   + _lp(_lp(struct.pack("<I", STRIPPING_PROT_ID) + _lp(struct.pack("<I", 3)))))
    sig = key.sign(signed_data, padding.PKCS1v15(), hashes.SHA256())
    signer = _lp(signed_data) + _lp(_lp(struct.pack("<I", ALGO_SHA256) + _lp(sig))) + _lp(pub_der)
    v2_value = _lp(_lp(signer))

    pair = struct.pack("<I", V2_BLOCK_ID) + _lp(v2_value)
    size = len(pair) + 24
    block = struct.pack("<Q", size) + pair + struct.pack("<Q", size) + b"APK Sig Block 42"

    new_eocd = bytearray(eocd_tail)
    struct.pack_into("<I", new_eocd, 16, cd_off + len(block))
    out = data[:cd_off] + block + cd + bytes(new_eocd)
    open(apk_path, "wb").write(out)
    print("signed v2: block %d, total %d" % (len(block), len(out)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["build", "sign", "keygen"])
    ap.add_argument("--src")
    ap.add_argument("--out")
    ap.add_argument("--replace", action="append", default=[])
    ap.add_argument("--add", action="append", default=[],
                    help="新增条目：NAME=FILE（.so 自动 4096 对齐）")
    ap.add_argument("--apk")
    ap.add_argument("--no-sign", action="store_true")
    a = ap.parse_args()
    if a.cmd == "keygen":
        ensure_key()
    elif a.cmd == "build":
        reps = dict(spec.split("=", 1) for spec in a.replace)
        adds = dict(spec.split("=", 1) for spec in a.add)
        build(a.src, a.out, reps, adds)
        if not a.no_sign:
            sign_v2(a.out)
    else:
        sign_v2(a.apk)


if __name__ == "__main__":
    main()
