"""Text tables in the .data section of Avernum 6.exe (skills, spells, help, quests, story ...).

Avernum 6 keeps most of its interface text as fixed-size char arrays in .data (no pointers to
redirect), so the DLL writes the translations into the arrays in place. A translation must fit
in the array slot (max_bytes = bytes up to the next non-zero byte, minus the terminator).

usage:
    python -I data_tool.py extract EXE OUT_DIR            OUT_DIR/units.json  [{"id", "en", "group", "max_bytes"}]
    python -I data_tool.py chunk OUT_DIR [KNOWN_JSON]      OUT_DIR/chunks/datNN.json, OUT_DIR/jobs.json,
                                                          OUT_DIR/ja/known.json (exact matches in KNOWN_JSON)
    python -I data_tool.py check OUT_DIR [CHUNK ...]      check OUT_DIR/ja/*.json against the chunks
    python -I data_tool.py build OUT_DIR OUT_TSV          the table the DLL reads (va, max_bytes, english, japanese)
"""
import json
import os
import re
import sys

sys.stdout.reconfigure(encoding='utf-8')
NORMALIZE = str.maketrans({'～': '〜', '－': '−', '∥': '‖'})

# ranges of .data that hold game text (the script-language keyword tables in between are left out)
RANGES = [(0x5d2200, 0x69d000), (0x6a8a00, 0x6ac200)]
# first address of each kind of text -> what it is (shown to translators)
GROUPS = [
    (0x5d2200, 'statistic / status name'), (0x5d4a00, 'spell name'), (0x5d8500, 'statistic / status description'),
    (0x5dad00, 'spell description'), (0x5dde00, 'skill name'), (0x5e4e00, 'message / confirmation'),
    (0x5f1500, 'menu / character type'), (0x5f2900, 'character type description'), (0x5f5200, 'special item name'),
    (0x5fb600, 'special item description'), (0x601a00, 'help (instructions)'), (0x627200, 'keyboard help / tip'),
    (0x62ae00, 'trait name'), (0x62cb00, 'trait description'), (0x62fe00, 'item / shop description'),
    (0x634400, 'spell help'), (0x63b200, 'battle discipline name'), (0x63c600, 'battle discipline description'),
    (0x63ee00, 'quest title'), (0x648300, 'job board title'), (0x651a00, 'quest description'),
    (0x65af00, 'job description'), (0x665b00, 'quest note'), (0x677e00, 'story (ending)'),
    (0x679000, 'story (introduction)'), (0x67c800, 'story / event text'), (0x6a8a00, 'button label'),
    (0x6a9500, 'place name'),
]


def group_of(va):
    g = 'other'
    for lo, name in GROUPS:
        if va >= lo:
            g = name
    return g


def extract(exe, out):
    import pefile
    pe = pefile.PE(exe)
    base = pe.OPTIONAL_HEADER.ImageBase
    sec = next(s for s in pe.sections if s.Name.startswith(b'.data'))
    d = sec.get_data()
    va0 = base + sec.VirtualAddress
    units = []
    for m in re.finditer(rb'(?<=\x00)[\x20-\x7e\r\n]{2,}(?=\x00)', d):
        va = va0 + m.start()
        if not any(lo <= va < hi for lo, hi in RANGES):
            continue
        s = m.group().decode('latin-1')
        if not re.search(r'[A-Za-z]{3}', s) or re.fullmatch(r'[a-z0-9_]+', s):
            continue
        j = m.end()
        while j < len(d) and d[j] == 0:
            j += 1
        units.append({'id': '%06x' % va, 'en': s, 'group': group_of(va), 'max_bytes': j - m.start() - 1})
    os.makedirs(out, exist_ok=True)
    json.dump(units, open(os.path.join(out, 'units.json'), 'w', encoding='utf-8'), ensure_ascii=False, indent=0)
    print(len(units), 'strings', sum(len(u['en']) for u in units), 'chars')


def chunk(out, known_path=None, chunk_chars=6000):
    units = json.load(open(os.path.join(out, 'units.json'), encoding='utf-8'))
    known = json.load(open(known_path, encoding='utf-8')) if known_path else {}
    os.makedirs(os.path.join(out, 'chunks'), exist_ok=True)
    os.makedirs(os.path.join(out, 'ja'), exist_ok=True)
    reuse = {u['id']: known[u['en']] for u in units if u['en'] in known
             and len(known[u['en']].translate(NORMALIZE).encode('euc_jp', 'replace')) <= u['max_bytes']}
    json.dump(reuse, open(os.path.join(out, 'ja', 'known.json'), 'w', encoding='utf-8'), ensure_ascii=False, indent=0)
    todo = [u for u in units if u['id'] not in reuse]
    chunks, cur, size = [], [], 0
    for u in todo:
        if cur and (size + len(u['en']) > chunk_chars or u['group'] != cur[-1]['kind'] and size > chunk_chars // 2):
            chunks.append(cur)
            cur, size = [], 0
        cur.append({'id': u['id'], 'kind': u['group'], 'en': u['en'], 'max_bytes': u['max_bytes']})
        size += len(u['en'])
    if cur:
        chunks.append(cur)
    names = []
    for n, c in enumerate(chunks):
        name = f'dat{n:02d}'
        json.dump({'file': 'exe data', 'part': n, 'units': c}, open(os.path.join(out, 'chunks', name + '.json'), 'w',
                  encoding='utf-8'), ensure_ascii=False, indent=0)
        names.append(name)
    json.dump(names, open(os.path.join(out, 'jobs.json'), 'w'), indent=0)
    print(f'{len(reuse)} reused, {len(todo)} to translate in {len(chunks)} chunks ({sum(len(u["en"]) for u in todo)} chars)')


def check(out, names):
    cdir = os.path.join(out, 'chunks')
    if not names:
        names = sorted(f[:-5] for f in os.listdir(cdir) if f.endswith('.json'))
    errs = warns = 0
    for name in names:
        c = json.load(open(os.path.join(cdir, name + '.json'), encoding='utf-8'))
        jp = os.path.join(out, 'ja', name + '.json')
        if not os.path.exists(jp):
            print(f'== {name}: 訳のファイルがありません')
            errs += 1
            continue
        ja = json.load(open(jp, encoding='utf-8'))
        e = w = 0
        msgs = []
        for u in c['units']:
            t = ja.get(u['id'])
            if not isinstance(t, str) or not t.strip():
                msgs.append(f'  E {u["id"]}: 訳がありません')
                e += 1
                continue
            t = t.translate(NORMALIZE)
            try:
                b = len(t.encode('euc_jp'))
            except UnicodeEncodeError as ex:
                msgs.append(f'  E {u["id"]}: EUC-JPにない文字 {t[ex.start:ex.end]!r}')
                e += 1
                continue
            if b > u['max_bytes']:
                msgs.append(f'  E {u["id"]}: {b}/{u["max_bytes"]} バイト（全角{u["max_bytes"] // 2}文字まで）。縮めてください')
                e += 1
            if t.count('|') != u['en'].count('|'):
                msgs.append(f'  E {u["id"]}: 改行記号 | の数が原文（{u["en"].count("|")}）と違います')
                e += 1
            if re.findall(r'%[-+#0-9.]*[A-Za-z]', t) != re.findall(r'%[-+#0-9.]*[A-Za-z]', u['en']):
                msgs.append(f'  E {u["id"]}: 書式指定子が原文と違います')
                e += 1
            if '_' in t or '"' in t or '\\' in t:
                msgs.append(f'  E {u["id"]}: 使えない記号（_ " \\）があります')
                e += 1
            if t.strip() == u['en'].strip() and re.search('[a-z]{3}', t):
                msgs.append(f'  W {u["id"]}: 英語のまま')
                w += 1
        extra = [k for k in ja if not k.startswith('_') and k not in {u['id'] for u in c['units']}]
        if extra:
            msgs.append(f'  W 余分なid: {extra[:5]}')
            w += 1
        print(f'== {name}: エラー{e} 警告{w}')
        for m in msgs:
            print(m)
        errs += e
        warns += w
    print(f'{len(names)} chunks, errors {errs}, warnings {warns}')
    return 1 if errs else 0


def build(out, tsv):
    units = {u['id']: u for u in json.load(open(os.path.join(out, 'units.json'), encoding='utf-8'))}
    tr = {}
    jdir = os.path.join(out, 'ja')
    for fn in sorted(os.listdir(jdir), key=lambda f: (f != 'known.json', f)):   # chunks override known.json
        if fn.endswith('.json'):
            for k, v in json.load(open(os.path.join(jdir, fn), encoding='utf-8')).items():
                if k in units and isinstance(v, str) and v.strip():
                    v = v.translate(NORMALIZE)
                    if len(v.encode('euc_jp', 'replace')) <= units[k]['max_bytes']:
                        tr[k] = v
    esc = lambda s: s.replace('\\', '\\\\').replace('\t', '\\t').replace('\n', '\\n').replace('\r', '\\r')
    with open(tsv, 'w', encoding='utf-8', newline='\n') as f:
        f.write('va\tmax_bytes\tenglish\tjapanese\n')
        for k in sorted(tr):
            f.write(f'0x{k}\t{units[k]["max_bytes"]}\t{esc(units[k]["en"])}\t{esc(tr[k])}\n')
    print(f'{len(tr)}/{len(units)} translated -> {tsv}')


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    cmd, args = argv[0], argv[1:]
    if cmd == 'extract':
        extract(*args)
    elif cmd == 'chunk':
        chunk(*args)
    elif cmd == 'check':
        return check(args[0], args[1:])
    elif cmd == 'build':
        build(*args)
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
