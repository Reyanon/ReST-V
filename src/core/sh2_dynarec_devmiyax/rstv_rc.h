/*
  ReST-V (ADR-135): register cache for the ARM32 block compiler (DynarecSh2.cpp EmmitCode).
  Common SH-2 instructions are emitted directly with R0-R15 and SR held in host registers r4, r5, r6, r10 and r11
  (callee-saved, so the getmem/setmem calls keep them; the block prologue saves r3-r11). Everything else still uses
  the templates: before a template the cache is written back and dropped, because templates read and write the
  registers in the context and use r4-r6/r10 as scratch. Results and cycle counts are those of the templates.
  Context layout (tagSH2): R[n] at n*4, SR 64, GBR 68, MACH 76, MACL 80, PR 84; getmem byte/word/long at
  100/104/108, setmem at 112/116/120, high work RAM base at 148.
*/
#pragma once

struct RstvRc {
  u32 *w;          // emit pointer
  s8 host[17];     // guest (R0-R15, 16 = SR) -> host register, -1 when not cached
  u8 dirty[17];
  s8 guest[16];    // host register -> guest, -1 when free
  u32 used[16];
  u32 clk;
  u32 lock;        // host registers the instruction being emitted already uses

  void reset()
  {
    for (int g = 0; g < 17; g++) { host[g] = -1; dirty[g] = 0; }
    for (int h = 0; h < 16; h++) { guest[h] = -1; used[h] = 0; }
    clk = 0;
    lock = 0;
  }
  void e(u32 x) { *w++ = x; }
  void ldr(int t, int g) { e(0xE5970000u | ((u32)t << 12) | (u32)(g * 4)); }
  void str(int t, int g) { e(0xE5870000u | ((u32)t << 12) | (u32)(g * 4)); }
  int alloc()
  {
    static const int pool[5] = { 4, 5, 6, 10, 11 };
    int best = -1;
    for (int k = 0; k < 5; k++) {
      const int p = pool[k];
      if (lock & (1u << p))
        continue;
      if (guest[p] < 0)
        return p;
      if (best < 0 || used[p] < used[best])
        best = p;
    }
    const int g = guest[best];
    if (dirty[g])
      str(best, g);
    host[g] = -1;
    dirty[g] = 0;
    guest[best] = -1;
    return best;
  }
  int get(int g, bool load)
  {
    int h = host[g];
    if (h < 0) {
      h = alloc();
      host[g] = (s8)h;
      guest[h] = (s8)g;
      dirty[g] = 0;
      if (load)
        ldr(h, g);
    }
    used[h] = ++clk;
    lock |= 1u << h;
    return h;
  }
  // sources first (rd), then destinations (wr: value not needed, rw: read and written)
  int rd(int g) { return get(g, true); }
  int wr(int g) { const int h = get(g, false); dirty[g] = 1; return h; }
  int rw(int g) { const int h = get(g, true); dirty[g] = 1; return h; }
  void wb(int g)
  {
    if (host[g] >= 0 && dirty[g]) {
      str(host[g], g);
      dirty[g] = 0;
    }
  }
  void drop(int g)
  {
    wb(g);
    if (host[g] >= 0) {
      guest[host[g]] = -1;
      host[g] = -1;
    }
  }
  void flush()
  {
    for (int g = 0; g < 17; g++)
      drop(g);
  }
  int cached() const
  {
    int n = 0;
    for (int g = 0; g < 17; g++)
      n += host[g] >= 0;
    return n;
  }

  // ---- encodings ----
  static bool enc(u32 v, u32 *imm12)
  {
    for (u32 r = 0; r < 16; r++) {
      const u32 x = r ? ((v << (2 * r)) | (v >> (32 - 2 * r))) : v;
      if (x <= 0xFF) {
        *imm12 = (r << 8) | x;
        return true;
      }
    }
    return false;
  }
  void dpr(u32 opc, int d, int n, int m, u32 s = 0) { e(0xE0000000u | (opc << 21) | (s << 20) | ((u32)n << 16) | ((u32)d << 12) | (u32)m); }
  void dpi(u32 opc, int d, int n, u32 imm12, u32 s = 0) { e(0xE2000000u | (opc << 21) | (s << 20) | ((u32)n << 16) | ((u32)d << 12) | imm12); }
  void sh(int d, int m, u32 type, u32 amt, u32 s = 0) { e(0xE1A00000u | (s << 20) | ((u32)d << 12) | (amt << 7) | (type << 5) | (u32)m); }
  void mov(int d, int m) { if (d != m) dpr(0xD, d, 0, m); }
  void movimm(int d, u32 v)
  {
    u32 imm;
    if (enc(v, &imm))
      dpi(0xD, d, 0, imm);
    else if (enc(~v, &imm))
      dpi(0xF, d, 0, imm);
    else {
      e(0xE3000000u | (((v >> 12) & 15) << 16) | ((u32)d << 12) | (v & 0xFFF));
      if (v >> 16)
        e(0xE3400000u | (((v >> 28) & 15) << 16) | ((u32)d << 12) | ((v >> 16) & 0xFFF));
    }
  }
  // d = n + v (any v; r12 as scratch when it does not fit)
  void addimm(int d, int n, s32 v)
  {
    u32 imm;
    if (v == 0)
      mov(d, n);
    else if (v > 0 && enc((u32)v, &imm))
      dpi(0x4, d, n, imm);
    else if (v < 0 && enc((u32)-v, &imm))
      dpi(0x2, d, n, imm);
    else {
      movimm(12, (u32)v);
      dpr(0x4, d, n, 12);
    }
  }
  // T = cond (ARM condition code after the flag-setting instruction just emitted); s = cached SR
  void set_t(int s, u32 cond)
  {
    e((cond << 28) | 0x03800001u | ((u32)s << 16) | ((u32)s << 12));         // orr<cond> s, s, #1
    e(((cond ^ 1) << 28) | 0x03C00001u | ((u32)s << 16) | ((u32)s << 12));   // bic<!cond> s, s, #1
  }
  static void patch(u32 *at, u32 *target) { *at = (*at & 0xFF000000u) | ((u32)(target - at - 2) & 0x00FFFFFFu); }
  // r0 = address -> r0 = value (zero extended for 1/2, as the getmem functions return it)
  void load(int size)
  {
    e(0xE1A0CA20u); // lsr r12, r0, #20
    e(0xE35C0060u); // cmp r12, #0x60
    u32 *bne = w;
    e(0x1A000000u);
    e(0xE597C094u); // ldr r12, [r7, #148]
    e(0xE7DF0A1Fu); // bfc r0, #20, #12
    if (size == 4) {
      e(0xE79C0000u); // ldr r0, [r12, r0]
      e(0xE1A00860u); // ror r0, r0, #16
    } else if (size == 2)
      e(0xE19C00B0u); // ldrh r0, [r12, r0]
    else {
      e(0xE2200001u); // eor r0, r0, #1
      e(0xE7DC0000u); // ldrb r0, [r12, r0]
    }
    u32 *b = w;
    e(0xEA000000u);
    patch(bne, w);
    e(0xE597C000u | (size == 4 ? 108u : size == 2 ? 104u : 100u)); // ldr r12, [r7, #getmem]
    e(0xE12FFF3Cu); // blx r12
    patch(b, w);
  }
  // r0 = address, r1 = value
  void store(int size)
  {
    e(0xE597C000u | (size == 4 ? 120u : size == 2 ? 116u : 112u)); // ldr r12, [r7, #setmem]
    e(0xE12FFF3Cu); // blx r12
  }
  // loaded value in r0 -> host d (sign extended as the templates do)
  void loaded(int d, int size)
  {
    if (size == 4)
      mov(d, 0);
    else if (size == 2)
      e(0xE6BF0070u | ((u32)d << 12)); // sxth d, r0
    else
      e(0xE6AF0070u | ((u32)d << 12)); // sxtb d, r0
  }
  // value for a store into r1 (bytes as the templates pass them: zero extended)
  void value(int h, int size)
  {
    if (size == 1)
      e(0xE6EF0070u | (1u << 12) | (u32)h); // uxtb r1, h
    else
      mov(1, h);
  }

  // Emit one instruction (not a branch, not in a delay slot). Returns false when the template must be used.
  bool op(u16 op, u32 pc)
  {
    (void)pc;
    lock = 0;
    const int n = (op >> 8) & 15, m = (op >> 4) & 15;
    const s32 simm = (s32)(s8)(op & 0xFF);
    const u32 uimm = op & 0xFF;
    switch (op >> 12) {
    case 0x0:
      switch (op & 0xF) {
      case 0x4: case 0x5: case 0x6: { // mov.x Rm, @(R0, Rn)
        const int size = (op & 0xF) == 4 ? 1 : (op & 0xF) == 5 ? 2 : 4;
        const int hm = rd(m), hn = rd(n), h0 = rd(0);
        value(hm, size);
        dpr(0x4, 0, hn, h0);
        store(size);
        return true;
      }
      case 0xC: case 0xD: case 0xE: { // mov.x @(R0, Rm), Rn
        const int size = (op & 0xF) == 0xC ? 1 : (op & 0xF) == 0xD ? 2 : 4;
        const int hm = rd(m), h0 = rd(0);
        dpr(0x4, 0, hm, h0);
        const int hn = wr(n);
        load(size);
        loaded(hn, size);
        return true;
      }
      case 0x7: { // mul.l Rm, Rn -> MACL
        const int hm = rd(m), hn = rd(n);
        e(0xE0000090u | (0u << 16) | ((u32)hm << 8) | (u32)hn); // mul r0, hn, hm
        e(0xE5870050u); // str r0, [r7, #80]
        return true;
      }
      }
      switch (op & 0xFF) {
      case 0x02: { const int hs = rd(16), hn = wr(n); mov(hn, hs); return true; }      // stc sr, Rn
      case 0x12: { const int hn = wr(n); e(0xE5970044u | ((u32)hn << 12)); return true; } // stc gbr, Rn
      case 0x0A: { const int hn = wr(n); e(0xE597004Cu | ((u32)hn << 12)); return true; } // sts mach, Rn
      case 0x1A: { const int hn = wr(n); e(0xE5970050u | ((u32)hn << 12)); return true; } // sts macl, Rn
      case 0x2A: { const int hn = wr(n); e(0xE5970054u | ((u32)hn << 12)); return true; } // sts pr, Rn
      case 0x29: { const int hs = rd(16), hn = wr(n); dpi(0x0, hn, hs, 1); return true; } // movt Rn
      }
      switch (op) {
      case 0x0009: return true;                                          // nop
      case 0x0008: { const int hs = rw(16); dpi(0xE, hs, hs, 1); return true; } // clrt
      case 0x0018: { const int hs = rw(16); dpi(0xC, hs, hs, 1); return true; } // sett
      case 0x0028: e(0xE3A00000u); e(0xE587004Cu); e(0xE5870050u); return true; // clrmac
      }
      return false;
    case 0x1: { // mov.l Rm, @(disp, Rn)
      const int hm = rd(m), hn = rd(n);
      value(hm, 4);
      addimm(0, hn, (s32)((op & 0xF) * 4));
      store(4);
      return true;
    }
    case 0x2:
      switch (op & 0xF) {
      case 0x0: case 0x1: case 0x2: { // mov.x Rm, @Rn
        const int size = (op & 0xF) == 0 ? 1 : (op & 0xF) == 1 ? 2 : 4;
        const int hm = rd(m), hn = rd(n);
        value(hm, size);
        mov(0, hn);
        store(size);
        return true;
      }
      case 0x4: case 0x5: case 0x6: { // mov.x Rm, @-Rn (the value is Rm before the decrement)
        const int size = (op & 0xF) == 4 ? 1 : (op & 0xF) == 5 ? 2 : 4;
        const int hm = rd(m);
        value(hm, size);
        const int hn = rw(n);
        dpi(0x2, hn, hn, (u32)size);
        mov(0, hn);
        store(size);
        return true;
      }
      case 0x8: { const int hm = rd(m), hn = rd(n), hs = rw(16); dpr(0x8, 0, hn, hm, 1); set_t(hs, 0x0); return true; } // tst
      case 0x9: { const int hm = rd(m), hn = rw(n); dpr(0x0, hn, hn, hm); return true; } // and
      case 0xA: { const int hm = rd(m), hn = rw(n); dpr(0x1, hn, hn, hm); return true; } // xor
      case 0xB: { const int hm = rd(m), hn = rw(n); dpr(0xC, hn, hn, hm); return true; } // or
      case 0xD: { // xtrct: Rn = (Rm << 16) | (Rn >> 16)
        const int hm = rd(m), hn = rw(n);
        sh(0, hn, 1, 16);
        e(0xE1800000u | (0u << 16) | ((u32)hn << 12) | (16u << 7) | (0u << 5) | (u32)hm); // orr hn, r0, hm, lsl #16
        return true;
      }
      case 0xE: { // mulu.w: MACL = (u16)Rn * (u16)Rm
        const int hm = rd(m), hn = rd(n);
        e(0xE6FF0070u | (0u << 12) | (u32)hn); // uxth r0, hn
        e(0xE6FF0070u | (1u << 12) | (u32)hm); // uxth r1, hm
        e(0xE0000090u | (0u << 16) | (1u << 8) | 0u); // mul r0, r0, r1
        e(0xE5870050u);
        return true;
      }
      case 0xF: { // muls.w: MACL = (s16)Rn * (s16)Rm
        const int hm = rd(m), hn = rd(n);
        e(0xE1600080u | (0u << 16) | ((u32)hm << 8) | (u32)hn); // smulbb r0, hn, hm
        e(0xE5870050u);
        return true;
      }
      }
      return false;
    case 0x3:
      switch (op & 0xF) {
      case 0x0: { const int hm = rd(m), hn = rd(n), hs = rw(16); dpr(0xA, 0, hn, hm, 1); set_t(hs, 0x0); return true; } // cmp/eq
      case 0x2: { const int hm = rd(m), hn = rd(n), hs = rw(16); dpr(0xA, 0, hn, hm, 1); set_t(hs, 0x2); return true; } // cmp/hs
      case 0x3: { const int hm = rd(m), hn = rd(n), hs = rw(16); dpr(0xA, 0, hn, hm, 1); set_t(hs, 0xA); return true; } // cmp/ge
      case 0x6: { const int hm = rd(m), hn = rd(n), hs = rw(16); dpr(0xA, 0, hn, hm, 1); set_t(hs, 0x8); return true; } // cmp/hi
      case 0x7: { const int hm = rd(m), hn = rd(n), hs = rw(16); dpr(0xA, 0, hn, hm, 1); set_t(hs, 0xC); return true; } // cmp/gt
      case 0x8: { const int hm = rd(m), hn = rw(n); dpr(0x2, hn, hn, hm); return true; } // sub
      case 0xC: { const int hm = rd(m), hn = rw(n); dpr(0x4, hn, hn, hm); return true; } // add
      case 0x5: case 0xD: { // dmulu.l / dmuls.l -> MACH:MACL
        const int hm = rd(m), hn = rd(n);
        e(((op & 0xF) == 0xD ? 0xE0C00090u : 0xE0800090u) | (1u << 16) | (0u << 12) | ((u32)hm << 8) | (u32)hn); // [su]mull r0, r1, hn, hm
        e(0xE5870050u); // str r0, MACL
        e(0xE587104Cu); // str r1, MACH
        return true;
      }
      }
      return false;
    case 0x4:
      switch (op & 0xFF) {
      case 0x00: case 0x20: { const int hs = rw(16), hn = rw(n); sh(hn, hn, 0, 1, 1); set_t(hs, 0x2); return true; } // shll / shal
      case 0x01: { const int hs = rw(16), hn = rw(n); sh(hn, hn, 1, 1, 1); set_t(hs, 0x2); return true; } // shlr
      case 0x21: { const int hs = rw(16), hn = rw(n); sh(hn, hn, 2, 1, 1); set_t(hs, 0x2); return true; } // shar
      case 0x04: { const int hs = rw(16), hn = rw(n); sh(hn, hn, 3, 31); dpi(0x8, 0, hn, 1, 1); set_t(hs, 0x1); return true; } // rotl
      case 0x05: { const int hs = rw(16), hn = rw(n); sh(hn, hn, 3, 1, 1); set_t(hs, 0x2); return true; } // rotr
      case 0x24: { // rotcl: Rn = (Rn << 1) | T, T = old bit 31
        const int hs = rw(16), hn = rw(n);
        dpi(0x0, 0, hs, 1);
        sh(hn, hn, 0, 1, 1);
        dpr(0xC, hn, hn, 0);
        set_t(hs, 0x2);
        return true;
      }
      case 0x25: { // rotcr: Rn = (T << 31) | (Rn >> 1), T = old bit 0
        const int hs = rw(16), hn = rw(n);
        dpi(0x0, 0, hs, 1);
        sh(hn, hn, 1, 1, 1);
        e(0xE1800000u | ((u32)hn << 16) | ((u32)hn << 12) | (31u << 7) | (0u << 5) | 0u); // orr hn, hn, r0, lsl #31
        set_t(hs, 0x2);
        return true;
      }
      case 0x08: { const int hn = rw(n); sh(hn, hn, 0, 2); return true; }  // shll2
      case 0x18: { const int hn = rw(n); sh(hn, hn, 0, 8); return true; }  // shll8
      case 0x28: { const int hn = rw(n); sh(hn, hn, 0, 16); return true; } // shll16
      case 0x09: { const int hn = rw(n); sh(hn, hn, 1, 2); return true; }  // shlr2
      case 0x19: { const int hn = rw(n); sh(hn, hn, 1, 8); return true; }  // shlr8
      case 0x29: { const int hn = rw(n); sh(hn, hn, 1, 16); return true; } // shlr16
      case 0x10: { const int hs = rw(16), hn = rw(n); dpi(0x2, hn, hn, 1, 1); set_t(hs, 0x0); return true; } // dt
      case 0x11: { const int hn = rd(n), hs = rw(16); dpi(0xA, 0, hn, 0, 1); set_t(hs, 0xA); return true; } // cmp/pz
      case 0x15: { const int hn = rd(n), hs = rw(16); dpi(0xA, 0, hn, 0, 1); set_t(hs, 0xC); return true; } // cmp/pl
      case 0x0A: { const int hn = rd(n); e(0xE587004Cu | ((u32)hn << 12)); return true; } // lds Rm, mach
      case 0x1A: { const int hn = rd(n); e(0xE5870050u | ((u32)hn << 12)); return true; } // lds Rm, macl
      case 0x2A: { const int hn = rd(n); e(0xE5870054u | ((u32)hn << 12)); return true; } // lds Rm, pr
      case 0x1E: { const int hn = rd(n); e(0xE5870044u | ((u32)hn << 12)); return true; } // ldc Rm, gbr
      }
      return false;
    case 0x5: { // mov.l @(disp, Rm), Rn
      const int hm = rd(m);
      addimm(0, hm, (s32)((op & 0xF) * 4));
      const int hn = wr(n);
      load(4);
      loaded(hn, 4);
      return true;
    }
    case 0x6:
      switch (op & 0xF) {
      case 0x0: case 0x1: case 0x2: { // mov.x @Rm, Rn
        const int size = (op & 0xF) == 0 ? 1 : (op & 0xF) == 1 ? 2 : 4;
        const int hm = rd(m);
        mov(0, hm);
        const int hn = wr(n);
        load(size);
        loaded(hn, size);
        return true;
      }
      case 0x4: case 0x5: case 0x6: { // mov.x @Rm+, Rn (Rn = the value when n == m)
        const int size = (op & 0xF) == 4 ? 1 : (op & 0xF) == 5 ? 2 : 4;
        const int hm = rw(m);
        mov(0, hm);
        load(size);
        dpi(0x4, hm, hm, (u32)size);
        const int hn = wr(n);
        loaded(hn, size);
        return true;
      }
      case 0x3: { const int hm = rd(m), hn = wr(n); mov(hn, hm); return true; } // mov
      case 0x7: { const int hm = rd(m), hn = wr(n); dpr(0xF, hn, 0, hm); return true; } // not
      case 0xB: { const int hm = rd(m), hn = wr(n); dpi(0x3, hn, hm, 0); return true; } // neg
      case 0x8: { // swap.b: low two bytes swapped
        const int hm = rd(m), hn = wr(n);
        e(0xE6BF0FB0u | (0u << 12) | (u32)hm); // rev16 r0, hm
        e(0xE6800010u | (0u << 16) | ((u32)hn << 12) | (u32)hm); // pkhbt hn, r0, hm
        return true;
      }
      case 0x9: { const int hm = rd(m), hn = wr(n); sh(hn, hm, 3, 16); return true; } // swap.w
      case 0xC: { const int hm = rd(m), hn = wr(n); e(0xE6EF0070u | ((u32)hn << 12) | (u32)hm); return true; } // extu.b
      case 0xD: { const int hm = rd(m), hn = wr(n); e(0xE6FF0070u | ((u32)hn << 12) | (u32)hm); return true; } // extu.w
      case 0xE: { const int hm = rd(m), hn = wr(n); e(0xE6AF0070u | ((u32)hn << 12) | (u32)hm); return true; } // exts.b
      case 0xF: { const int hm = rd(m), hn = wr(n); e(0xE6BF0070u | ((u32)hn << 12) | (u32)hm); return true; } // exts.w
      }
      return false;
    case 0x7: { const int hn = rw(n); addimm(hn, hn, simm); return true; } // add #imm, Rn
    case 0x8:
      switch (n) {
      case 0x0: case 0x1: { // mov.b / mov.w R0, @(disp, Rn) (n is the m field here)
        const int size = n == 0 ? 1 : 2;
        const int h0 = rd(0), hb = rd(m);
        value(h0, size);
        addimm(0, hb, (s32)((op & 0xF) * size));
        store(size);
        return true;
      }
      case 0x4: case 0x5: { // mov.b / mov.w @(disp, Rm), R0
        const int size = n == 4 ? 1 : 2;
        const int hb = rd(m);
        addimm(0, hb, (s32)((op & 0xF) * size));
        const int h0 = wr(0);
        load(size);
        loaded(h0, size);
        return true;
      }
      case 0x8: { // cmp/eq #imm, R0
        const int h0 = rd(0), hs = rw(16);
        if (simm >= 0)
          dpi(0xA, 0, h0, (u32)simm, 1);
        else
          dpi(0xB, 0, h0, (u32)-simm, 1);
        set_t(hs, 0x0);
        return true;
      }
      }
      return false;
    case 0xC:
      switch (n) {
      case 0x0: case 0x1: case 0x2: { // mov.x R0, @(disp, GBR)
        const int size = n == 0 ? 1 : n == 1 ? 2 : 4;
        const int h0 = rd(0);
        value(h0, size);
        e(0xE5970044u); // ldr r0, [r7, #68] (GBR)
        addimm(0, 0, (s32)(uimm * (u32)size));
        store(size);
        return true;
      }
      case 0x4: case 0x5: case 0x6: { // mov.x @(disp, GBR), R0
        const int size = n == 4 ? 1 : n == 5 ? 2 : 4;
        e(0xE5970044u);
        addimm(0, 0, (s32)(uimm * (u32)size));
        const int h0 = wr(0);
        load(size);
        loaded(h0, size);
        return true;
      }
      case 0x7: { const int h0 = wr(0); movimm(h0, ((pc & ~3u) + 4 + uimm * 4)); return true; } // mova
      case 0x8: { const int h0 = rd(0), hs = rw(16); dpi(0x8, 0, h0, uimm, 1); set_t(hs, 0x0); return true; } // tst #imm, R0
      case 0x9: { const int h0 = rw(0); dpi(0x0, h0, h0, uimm); return true; } // and #imm, R0
      case 0xA: { const int h0 = rw(0); dpi(0x1, h0, h0, uimm); return true; } // xor #imm, R0
      case 0xB: { const int h0 = rw(0); dpi(0xC, h0, h0, uimm); return true; } // or #imm, R0
      }
      return false;
    case 0xE: { const int hn = wr(n); movimm(hn, (u32)simm); return true; } // mov #imm, Rn
    }
    return false;
  }
};
