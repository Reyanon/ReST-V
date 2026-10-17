import struct, sys, os, glob
rows = []
for p in glob.glob(sys.argv[1] + '/*.gxp'):
    b = open(p, 'rb').read()
    if b[:3] != b'GXP':
        continue
    size, = struct.unpack_from('<I', b, 8)
    ptype, = struct.unpack_from('<I', b, 20)
    tex0, tex1 = struct.unpack_from('<II', b, 28)
    pa, sa = struct.unpack_from('<HH', b, 48)
    t1, = struct.unpack_from('<I', b, 52)
    t2, pre = struct.unpack_from('<HH', b, 56)
    ninstr, = struct.unpack_from('<I', b, 60)
    nondep = dep = 0
    for k in range(16):
        fl = ((tex0 if k < 8 else tex1) >> ((k % 8) * 4)) & 15
        if fl & 1: nondep += 1
        if fl & 2: dep += 1
    rows.append((os.path.getmtime(p), os.path.basename(p), size, ptype & 1, ninstr, pa, sa, t1, t2, nondep, dep))
rows.sort()
for r in rows[-int(sys.argv[2]):]:
    print('%s size %5d frag? %d instr %4d PA %3d SA %3d temp %d/%d tex nondep %d dep %d' % r[1:])
