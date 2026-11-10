"""Assemble the release ZIP (drop-in layout: copy the contents into the game folder).

usage:
    python -I build_release.py ORIGINAL_ROOT GAME_ROOT OUT_DIR VERSION PAGE_URL

ORIGINAL_ROOT: a game-folder-shaped copy of the untouched files ("Data/...").
GAME_ROOT:     a game-folder-shaped tree with the Japanese files built in.
The DLL, ini and exe-string table are taken from this mod folder.
Writes OUT_DIR/Avernum6_JP_<VERSION>.zip with
    dsound.dll, avernum_jp.ini, avernum_jp/jp_patch.bin, avernum_jp/avernum_jp_exe.tsv,
    avernum_jp/README.txt
"""
import os
import shutil
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
MOD = os.path.dirname(HERE)
SRC = os.path.join(MOD, 'dist_src')


def add_nth(tsv, exe, out):
    """Copy the .data table with a 5th column: which occurrence (0-based, in address order) of the
    English text, standing alone between zero bytes in .data, each row is. Another build of the exe
    can then find texts that occur more than once (the DLL uses it when the Steam address does not fit)."""
    import re
    import pefile
    pe = pefile.PE(exe, fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    sec = [x for x in pe.sections if x.Name.rstrip(b'\0') == b'.data'][0]
    d0 = base + sec.VirtualAddress
    raw = sec.get_data()
    unmap = {'n': '\n', 'r': '\r', 't': '\t', '\\': '\\'}

    def unesc(t):
        return re.sub(r'\\(.)', lambda m: unmap.get(m.group(1), m.group(0)), t)

    res = []
    for ln in open(tsv, encoding='utf-8').read().split('\n'):
        if not ln.startswith('0x'):
            res.append(ln + '\tnth' if ln.startswith('va\t') else ln)
            continue
        col = ln.split('\t')
        va = int(col[0], 16)
        key = b'\0' + unesc(col[2]).encode('cp1252') + b'\0'
        nth, i = 0, raw.find(key)
        while 0 <= i and d0 + i + 1 < va:
            nth += 1
            i = raw.find(key, i + 1)
        if i < 0 or d0 + i + 1 != va:
            raise SystemExit(f'{col[0]}: not standing alone at its address')
        res.append('\t'.join(col[:4] + [str(nth)]))
    open(out, 'w', encoding='utf-8', newline='\n').write('\n'.join(res))


def main(argv):
    if len(argv) != 5:
        print(__doc__)
        return 2
    orig, game, out, version, page = argv
    name = 'Avernum6_JP_' + version
    stage = os.path.join(out, name)
    shutil.rmtree(stage, ignore_errors=True)
    data = os.path.join(stage, 'avernum_jp')
    os.makedirs(data)
    subprocess.check_call([sys.executable, '-I', os.path.join(HERE, 'make_patch.py'), orig, game,
                           os.path.join(data, 'jp_patch.bin')])
    shutil.copy2(os.path.join(MOD, 'dsound.dll'), os.path.join(stage, 'dsound.dll'))
    shutil.copy2(os.path.join(SRC, 'avernum_jp.ini'), os.path.join(stage, 'avernum_jp.ini'))
    shutil.copy2(os.path.join(MOD, 'exe_strings.tsv'), os.path.join(data, 'avernum_jp_exe.tsv'))
    add_nth(os.path.join(MOD, 'data_strings.tsv'), os.path.join(os.path.dirname(MOD), 'Avernum 6.exe'),
            os.path.join(data, 'avernum_jp_data.tsv'))   # .data text tables
    t = open(os.path.join(SRC, 'README.txt'), encoding='utf-8').read()
    t = t.replace('__VERSION__', version).replace('__PAGE_URL__', page).replace('\r\n', '\n').replace('\n', '\r\n')
    with open(os.path.join(data, 'README.txt'), 'w', encoding='utf-8-sig', newline='') as fh:
        fh.write(t)

    zpath = os.path.join(out, name + '.zip')
    with zipfile.ZipFile(zpath, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for dirpath, _, files in os.walk(stage):
            for fn in sorted(files):
                p = os.path.join(dirpath, fn)
                z.write(p, os.path.relpath(p, stage).replace('\\', '/'))   # contents at the ZIP root
    print('wrote', zpath, os.path.getsize(zpath), 'bytes')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
