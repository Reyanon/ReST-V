# Generate ST-V ROM layout table (ADR-106): ROM_START "cart" regions in MAME stv.cpp → src/core/stv_games.h
#   python tools/gen_stv_games.py [MAME source dir (stv.cpp)]
# Emits only file names, offsets, lengths, CRCs and protection keys (no ROM data). Skips sets hand-written in src/core/stv.c (those win).
#   ROM_LOAD16_BYTE      -> HEADER_BLOB
#   ROM_LOAD             -> BYTE_BLOB   (a following ROM_RELOAD is also BYTE_BLOB)
#   ROM_LOAD16_WORD_SWAP -> WORD_BLOB   (a following ROM_RELOAD is also WORD_BLOB)
#   ROM_RELOAD_PLAIN     -> BYTE_BLOB
import os
import re
import sys

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
src_dir = sys.argv[1] if len(sys.argv) > 1 else r'E:\Dev\Vita Port\ref\mame'
src = open(os.path.join(src_dir, 'stv.cpp'), encoding='utf-8', errors='replace').read()
ren = {'sandr': 'sandor'}

stv_c = open(os.path.join(root, 'src', 'core', 'stv.c'), encoding='utf-8').read()
manual = set(re.findall(r'\{ \{ "(\w+)"', stv_c))

games = {}
gpat = r'^GAME\(\s*(\d+)\s*,\s*(\w+)\s*,\s*(\w+)\s*,\s*(\w+)\s*,\s*(\w+)\s*,\s*(\w+)\s*,\s*(\w+)\s*,\s*(\w+)\s*,\s*"((?:[^"\\]|\\.)*)"\s*,\s*"((?:[^"\\]|\\.)*)"'
for m in re.finditer(gpat, src, re.M):
    year, set_, parent, machine, inp, cls, init, rot, maker, title = m.groups()
    games[set_] = dict(parent=parent, machine=machine, input=inp, init=init, title=title)

num = r'(0x[0-9a-fA-F]+|\d+)'

# ReST-V: pure idle loops found from hardware and PC run logs (loops that only poll a flag; countdown delay loops are excluded)
EXTRA_HINTS = {
    # master: while (flag > 0) counter++ / slave: for (;;) sound_update() — about once a scan line already, so once a
    # time slice is the same rate (the slave spent the whole frame in it during the attract mode)
    'bakubaku': ([0x06036dba], [0x060105c8]),
    'ejihon': ([0x060068b4], []),     # master: while (!flag) { poll(); n++; } (poll returns at once while the flag is 0)
    'fhboxers': ([], [0x06021caa]),   # slave: while (flag) sound_update()
    # master: polls a VDP status bit; while (flag) poll() (same engine as ejihon)
    'gaxeduel': ([0x0601d790, 0x06011dce], []),
    'introdon': ([0x0602f788], []),   # master: t = *cnt + n; while (*cnt < t); (cnt = interrupt counter 0x0609ea18)
    # master: task scheduler, no ready task -> idle() (rts) -> scan again (~3500 scans a frame)
    'kiwames': ([0x06057f90], []),
}
# ADR-042 once_fn: master function called over and over while the game waits for vblank (runs once a frame on the Vita)
EXTRA_ONCE = {
    'techbowl': 0x060144a6,  # SGL slSynch user callback: game work + bookkeeping save to backup RAM, ~55 times a frame
    'vfkids': 0x06047118,    # while (gbr[0x13] >= 0) update 32 slots: ~300 times a frame in the attract demo (f1800~2400)
}

# MAME init_*: sh2drc_add_pcflush(addr) = idle-loop hints (ADR-106); commented-out lines are skipped
pcflush = {}
for m in re.finditer(r'^void stv_state::(init_\w+)\(\)\n\{\n(.*?)^\}', src, re.M | re.S):
    body = '\n'.join(l for l in m.group(2).split('\n') if not l.strip().startswith('//'))
    ms = [int(x, 16) for x in re.findall(r'm_maincpu->sh2drc_add_pcflush\((0x[0-9a-fA-F]+)\)', body)]
    ss = [int(x, 16) for x in re.findall(r'm_slave->sh2drc_add_pcflush\((0x[0-9a-fA-F]+)\)', body)]
    pcflush[m.group(1)] = (ms[:4], ss[:4])
roms = {}
for m in re.finditer(r'^ROM_START\(\s*(\w+)\s*\)(.*?)^ROM_END', src, re.M | re.S):
    name, body = m.groups()
    blobs, key, in_cart, last, nv, in_ee = [], 0, False, None, None, False
    for line in body.split('\n'):
        line = line.split('//')[0].strip()
        r = re.match(r'ROM_REGION\w*\(\s*' + num + r'\s*,\s*"([^"]+)"', line)
        if r:
            in_cart = r.group(2) == 'cart'
            in_ee = r.group(2) == 'eeprom'
            continue
        r = re.match(r'ROM_PARAMETER\(\s*":315_5881:key"\s*,\s*"([0-9a-fA-F]+)"', line)
        if r:
            key = int(r.group(1), 16)
            continue
        if in_ee:  # default EEPROM (game settings checked by the BIOS)
            r = re.match(r'ROM_LOAD\s*\(\s*"([^"]+)"\s*,\s*' + num + r'\s*,\s*' + num + r'\s*,\s*CRC\(([0-9a-fA-F]+)\)', line)
            if r:
                nv = (r.group(1), int(r.group(4), 16))
            continue
        if not in_cart:
            continue
        r = re.match(r'(ROM_LOAD16_BYTE|ROM_LOAD16_WORD_SWAP|ROM_LOAD)\s*\(\s*"([^"]+)"\s*,\s*' + num + r'\s*,\s*' + num +
                     r'\s*,\s*(?:BAD_DUMP\s*)?CRC\(([0-9a-fA-F]+)\)', line)
        if r:
            kind = {'ROM_LOAD16_BYTE': 'HEADER_BLOB', 'ROM_LOAD16_WORD_SWAP': 'WORD_BLOB', 'ROM_LOAD': 'BYTE_BLOB'}[r.group(1)]
            last = (kind, r.group(2), int(r.group(5), 16))
            blobs.append((kind, r.group(2), int(r.group(3), 0), int(r.group(4), 0), last[2]))
            continue
        r = re.match(r'(ROM_RELOAD_PLAIN|ROM_RELOAD)\s*\(\s*' + num + r'\s*,\s*' + num, line)
        if r and last:
            kind = 'BYTE_BLOB' if r.group(1) == 'ROM_RELOAD_PLAIN' else last[0]
            blobs.append((kind, last[1], int(r.group(2), 0), int(r.group(3), 0), last[2]))
            continue
        if line.startswith('ROM_') and line not in ('ROM_END',):
            print('skip', name, line, file=sys.stderr)
    roms[name] = (blobs, key, nv)


def c(s):
    return 'NULL' if s is None else '"' + s + '"'


out = ['// Generated by tools/gen_stv_games.py (ADR-106). Source: ROM_START "cart" regions in MAME src/mame/sega/stv.cpp',
       '// (file names, offsets, lengths, CRCs and 315-5881 keys only, no ROM data). Hand-written sets (stv.c s_games) are not here.',
       '// No idle loops, HLE or per-game hacks (move a set to stv.c and tune it there once it runs).', '']
maxb = 0
n = 0
for set_, g in sorted(games.items(), key=lambda kv: kv[0]):
    s2 = ren.get(set_, set_)
    if s2 in manual or set_ == 'stvbios' or set_ not in roms:
        continue
    blobs, key, nv = roms[set_]
    if not blobs:
        continue
    parent = ren.get(g['parent'], g['parent'])
    if parent in ('stvbios', '0'):
        parent = None
    maxb = max(maxb, len(blobs))
    title = g['title']
    out.append('\t// %s, %s, %s' % (g['machine'], g['input'], g['init']))
    once = ', { 0 }, 0, 0x%08x' % EXTRA_ONCE[s2] if s2 in EXTRA_ONCE else ''
    out.append('\t{ { %s, %s, "%s"%s }, NULL, {' % (c(s2), c(parent), title, once))
    for b in blobs:
        out.append('\t\t{ %-11s %-16s 0x%07x, 0x%07x, 0x%08x },' % (b[0] + ',', '"' + b[1] + '",', b[2], b[3], b[4]))
    io = {'hopper': 'STV_IO_HOPPER', 'stvmp': 'STV_IO_MAHJONG'}.get(g['machine'])
    # Sets without a default EEPROM region get a blank EEPROM like MAME (all 0xFF, "-"). With BIOS defaults the BIOS took the "SYSTEM CHECKING"
    # path and started the game with the 68K halted (seabass, sss etc. silent with black screen, ADR-106)
    nv = nv or ('-', 0)
    tail = ', 0x%08x, "%s", 0x%08x' % (key, nv[0], nv[1])
    hm, hs = pcflush.get(g['init'], ([], []))
    if set_ in EXTRA_HINTS:  # idle loops found by ReST-V profiling (sets with no MAME hint)
        hm, hs = hm + EXTRA_HINTS[set_][0], hs + EXTRA_HINTS[set_][1]
    if io or hm or hs:
        tail += ', ' + (io or '0')
    if hm or hs:
        tail += ', { %s }, { %s }' % (', '.join('0x%07x' % x for x in hm) or '0', ', '.join('0x%07x' % x for x in hs) or '0')
    out.append('\t\t{ BLOB_END } }%s },' % tail)
    n += 1
open(os.path.join(root, 'src', 'core', 'stv_games.h'), 'w', encoding='utf-8', newline='\n').write('\n'.join(out) + '\n')
print('games', n, 'max blobs', maxb)
