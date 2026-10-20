# -*- coding: utf-8 -*-
"""
把一个本地 mod 目录换进 APK 的某个「mod 槽位」
=============================================
为什么需要"槽位"：国际版的模组清单**不是扫目录**得来的，是
`ModSelfModFinder.GetListOfSelfModsPaths()` 从配置表读一个字符串
（`GameConfig.GetConfigStr(...)`）→ `Split` → 逐个拼成
`assets/Mods/Android/<名字>/catalog_0.1.json`。
所以：
  * 目录名必须命中配置表里已有的 40 个名字之一；
  * 新 mod 的文件要**成套**塞进那个目录（Info.json / Logo.png /
    catalog_0.1.json / catalog_0.1.hash / 它引用的全部 *.bundle）。

本脚本做的事：
  1. 读本地 mod 目录，列出所有文件；
  2. 对每个文件算目标路径 `assets/Mods/Android/<槽位>/<文件名>`；
     APK 里已存在同名条目 -> --replace，不存在 -> --add（多个 bundle 也自动处理）；
  3. 调 mk_apk.py 重打包（可选同时替换 libil2cpp、加自研 so）；
  4. zipalign + apksigner + 可选 adb install。

用法：
  python -X utf8 swap_mod.py --mod D:\\mods\\XXX --slot BaiBaoNang
  python -X utf8 swap_mod.py --src base.apk --mod ... --slot ... --out out.apk \\
  python -X utf8 swap_mod.py --list-slots          # 看有哪些槽位可用
  python -X utf8 swap_mod.py --mod ... --slot ... --no-install
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))

# 全部走环境变量 / 命令行参数，仓库里不放任何本机路径。
#   ANDROID_BUILD_TOOLS  build-tools 目录（含 zipalign / apksigner）
#   JAVA_HOME            JDK 目录（apksigner 需要）
#   ADB                  adb 可执行文件（默认走 PATH）
SRC_APK = None
SO = None
LIB = None
BT = os.environ.get('ANDROID_BUILD_TOOLS', '')
JDK = os.environ.get('JAVA_HOME', '')
ADB = os.environ.get('ADB', 'adb')
PKG = ''

MODS_PREFIX = 'assets/Mods/Android/'   # 不同游戏路径不同，按自己 APK 里的实际结构改
REQUIRED = ['Info.json', 'catalog_0.1.json', 'catalog_0.1.hash']


def die(msg):
    print('!! ' + msg)
    sys.exit(2)


def slots(apk):
    with zipfile.ZipFile(apk) as z:
        names = {n.split('/')[3] for n in z.namelist()
                 if n.startswith(MODS_PREFIX) and n.count('/') >= 4}
    return sorted(names)


def main():
    global BT, JDK, PKG
    ap = argparse.ArgumentParser()
    ap.add_argument('--mod', help='本地 mod 目录（里面直接是 Info.json / catalog_0.1.json / *.bundle）')
    ap.add_argument('--slot', help='要顶替的槽位目录名（默认取 --mod 目录名）')
    ap.add_argument('--src', required=True, help='底包 APK')
    ap.add_argument('--out', default='out.apk')
    ap.add_argument('--so', help='替换进 APK 的 libil2cpp.so（配合 --lib 用）')
    ap.add_argument('--lib', help='要加进 APK 的自研 so')
    ap.add_argument('--pkg', default='', help='装完顺带启动的包名（可选）')
    ap.add_argument('--build-tools', default=BT, help='build-tools 目录（或用环境变量 ANDROID_BUILD_TOOLS）')
    ap.add_argument('--java-home', default=JDK, help='JDK 目录（或用环境变量 JAVA_HOME）')
    ap.add_argument('--keystore', default=os.environ.get('SIGN_KEYSTORE', ''), help='签名用 jks 路径')
    ap.add_argument('--ks-pass', default=os.environ.get('SIGN_KS_PASS', 'android'), help='keystore 口令')
    ap.add_argument('--key-pass', default=os.environ.get('SIGN_KEY_PASS', 'android'), help='key 口令')
    ap.add_argument('--no-install', action='store_true')
    ap.add_argument('--no-tweaks', action='store_true', help='不动 so，只换 mod（纯净包）')
    ap.add_argument('--list-slots', action='store_true')
    a = ap.parse_args()

    BT = a.build_tools or BT
    JDK = a.java_home or JDK
    PKG = a.pkg or PKG

    if a.list_slots:
        print('可用槽位（%s）：' % a.src)
        for s in slots(a.src):
            print('  ' + s)
        return

    with zipfile.ZipFile(a.src) as z:
        existing = set(z.namelist())

    repl, add = [], []

    # ---- mod 目录（可选：不带 --mod 就只换 so）
    if a.mod:
        if not os.path.isdir(a.mod):
            die('mod 目录不存在: ' + a.mod)

        slot = a.slot or os.path.basename(os.path.normpath(a.mod))
        have = set(slots(a.src))
        if slot not in have:
            die('槽位 "%s" 不在游戏的 mod 清单里。可用：%s' % (slot, ', '.join(sorted(have))))

        # 只取这一层，不递归子目录
        files = sorted(f for f in os.listdir(a.mod)
                       if os.path.isfile(os.path.join(a.mod, f)))
        if not files:
            die('mod 目录里没有文件')
        missing = [r for r in REQUIRED if r not in files]
        if missing:
            die('mod 目录缺 %s（必须成套，光有 bundle 不行）' % ', '.join(missing))
        if not [f for f in files if f.endswith('.bundle')]:
            die('mod 目录里没有 *.bundle')

        for f in files:
            tgt = MODS_PREFIX + slot + '/' + f
            (repl if tgt in existing else add).append('%s=%s' % (tgt, os.path.join(a.mod, f)))

        print('槽位 %s：替换 %d 个、新增 %d 个' % (slot, len(repl), len(add)))
        for x in repl + add:
            print('   ' + x.split('=')[0])

    tmp_raw = os.path.join(tempfile.gettempdir(), 'modkit_raw.apk')
    tmp_alg = os.path.join(tempfile.gettempdir(), 'modkit_aligned.apk')
    for p in (tmp_raw, tmp_alg):
        if os.path.exists(p):
            os.remove(p)

    cmd = [sys.executable, '-X', 'utf8', os.path.join(HERE, 'mk_apk.py'), 'build',
           '--src', a.src, '--out', tmp_raw, '--no-sign']
    if not a.no_tweaks:
        if not a.so:
            die('没指定 --so（要替换 libil2cpp.so），或者加 --no-tweaks 只换 mod')
        if not os.path.exists(a.so):
            die('找不到 %s' % a.so)
        cmd += ['--replace', 'lib/arm64-v8a/libil2cpp.so=' + a.so]
        if a.lib and os.path.exists(a.lib):
            libname = 'lib/arm64-v8a/libmodkit.so'
            # 底包里已经有同名 so 时必须 --replace，--add 会被跳过（保留旧库）
            cmd += [('--replace' if libname in existing else '--add'), libname + '=' + a.lib]
    for x in repl:
        cmd += ['--replace', x]
    for x in add:
        cmd += ['--add', x]

    print('\n--- 重打包')
    r = subprocess.run(cmd)
    if r.returncode:
        die('mk_apk 失败')

    print('--- zipalign')
    r = subprocess.run([os.path.join(BT, 'zipalign.exe'), '-p', '-f', '4', tmp_raw, tmp_alg])
    if r.returncode:
        die('zipalign 失败')

    print('--- apksigner')
    env = dict(os.environ)
    env['JAVA_HOME'] = JDK
    env['PATH'] = os.path.join(JDK, 'bin') + os.pathsep + env.get('PATH', '')
    ks = a.keystore or os.environ.get('SIGN_KEYSTORE', '')
    if not ks:
        die('需要签名密钥：传 --keystore 或设 SIGN_KEYSTORE（jks 路径），口令用 --ks-pass / --key-pass')
    r = subprocess.run([os.path.join(BT, 'apksigner.bat'), 'sign',
                        '--ks', ks,
                        '--ks-pass', 'pass:' + a.ks_pass, '--key-pass', 'pass:' + a.key_pass,
                        '--v1-signing-enabled', 'true', '--v2-signing-enabled', 'true',
                        '--v3-signing-enabled', 'true', '--min-sdk-version', '23',
                        '--out', a.out, tmp_alg], env=env)
    if r.returncode:
        die('apksigner 失败')
    for p in (tmp_raw, tmp_alg):
        if os.path.exists(p):
            os.remove(p)
    print('已签名 -> %s  (%.1f MB)' % (a.out, os.path.getsize(a.out) / 1024 / 1024))

    if not a.no_install:
        print('--- 安装')
        subprocess.run([ADB, 'install', '-r', '--no-incremental', a.out])
        if PKG:
            subprocess.run([ADB, 'shell', 'monkey', '-p', PKG, '-c',
                            'android.intent.category.LAUNCHER', '1'])


if __name__ == '__main__':
    main()
