/*  Copyright 2015 Theo Berkau

    This file is part of Yabause.

    Yabause is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    Yabause is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Yabause; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
*/
/*
        Copyright 2019 devMiyax(smiyaxdev@gmail.com)

This file is part of YabaSanshiro.

        YabaSanshiro is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

YabaSanshiro is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

        You should have received a copy of the GNU General Public License
along with YabaSanshiro; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
*/

#include "scsp.h"
#include "scspdsp.h"


//saturate 24 bit signed integer
static INLINE s32 saturate_24(s32 value)
{
   if (value > 8388607)
      value = 8388607;

   if (value < (-8388608))
      value = (-8388608);

   return value;
}


#define sign_x_to_s32(_bits, _value) (((int)((u32)(_value) << (32 - _bits))) >> (32 - _bits))
#define min(a,b) (a<b)?a:b

static INLINE unsigned clz(u32 v)
{
#if defined(__GNUC__) || defined(__clang__) || defined(__ICC) || defined(__INTEL_COMPILER)
  return __builtin_clz(v);
#elif defined(_MSC_VER)
  unsigned long idx;

  _BitScanReverse(&idx, v);

  return 31 ^ idx;
#else
  unsigned ret = 0;
  unsigned tmp;

  tmp = !(v & 0xFFFF0000) << 4; v <<= tmp; ret += tmp;
  tmp = !(v & 0xFF000000) << 3; v <<= tmp; ret += tmp;
  tmp = !(v & 0xF0000000) << 2; v <<= tmp; ret += tmp;
  tmp = !(v & 0xC0000000) << 1; v <<= tmp; ret += tmp;
  tmp = !(v & 0x80000000) << 0;            ret += tmp;

  return(ret);
#endif
}

//sign extended to 32 bits instead of 24
static s32 INLINE float_to_int(u16 f_val)
{
   u32 sign = (f_val >> 15) & 1;
   u32 sign_inverse = (!sign) & 1;
   u32 exponent = (f_val >> 11) & 0xf;
   u32 mantissa = f_val & 0x7FF;

   s32 ret_val = sign << 31;

   if (exponent > 11)
   {
      exponent = 11;
      ret_val |= (sign << 30);
   }
   else
      ret_val |= (sign_inverse << 30);

   ret_val |= mantissa << 19;

   ret_val = ret_val >> (exponent + (1 << 3));

   return ret_val;
}

static u16 INLINE int_to_float(u32 i_val)
{
   u32 sign = (i_val >> 23) & 1;
   u32 exponent = 0;

   if (sign != 0)
      i_val = (~i_val) & 0x7FFFFF;

   if (i_val <= 0x1FFFF)
   {
      i_val *= 64;
      exponent += 0x3000;
   }

   if (i_val <= 0xFFFFF)
   {
      i_val *= 8;
      exponent += 0x1800;
   }

   if (i_val <= 0x3FFFFF)
   {
      i_val *= 2;
      exponent += 0x800;
   }

   if (i_val <= 0x3FFFFF)
   {
      i_val *= 2;
      exponent += 0x800;
   }

   if (i_val <= 0x3FFFFF)
      exponent += 0x800;

   i_val >>= 11;
   i_val &= 0x7ff;
   i_val |= exponent;

   if (sign != 0)
      i_val ^= (0x7ff | (1 << 15));

   return i_val;
}

u32 rstv_dsp_i2f(u32 v) { return int_to_float(v); } // ADR-110: the DSP JIT calls it for MWT without NOFL

void ScspDspExec(ScspDsp* dsp, int addr, u8 * sound_ram)
{
  u16* sound_ram_16 = (u16*)sound_ram;
  u64 mul_temp = 0;
  int nofl = 0;
  u32 x_temp = 0;
  s32 y_extended = 0;
  union ScspDspInstruction inst;
  u32 address = 0;
  s32 shift_temp = 0;

  inst.all = scsp_dsp.mpro[addr];

  const unsigned TEMPWriteAddr = (inst.part.twa + dsp->mdec_ct) & 0x7F;
  const unsigned TEMPReadAddr = (inst.part.tra + dsp->mdec_ct) & 0x7F;

  if (inst.part.ira & 0x20) {
    if (inst.part.ira & 0x10) {
      if (!(inst.part.ira & 0xE))
        dsp->inputs = dsp->exts[inst.part.ira & 0x1] << 8;
    }else{
      dsp->inputs = dsp->mixs[inst.part.ira & 0xF] << 4;
    }
  }else{
    dsp->inputs = dsp->mems[inst.part.ira & 0x1F];
  }

  const int INPUTS = sign_x_to_s32(24, dsp->inputs);
  const int TEMP = sign_x_to_s32(24, dsp->temp[TEMPReadAddr]);
  const int X_SEL_Inputs[2] = { TEMP, INPUTS };
  const u16 Y_SEL_Inputs[4] = { 
    dsp->frc_reg, dsp->coef[inst.part.coef],
    (u16)((dsp->y_reg >> 11) & 0x1FFF), 
    (u16)((dsp->y_reg >> 4) & 0x0FFF) 
  };
  const u32 SGA_Inputs[2] = { (u32)TEMP, dsp->shift_reg }; // ToDO:?

  if (inst.part.yrl) {
    dsp->y_reg = INPUTS & 0xFFFFFF;
  }

  int ShifterOutput;

  ShifterOutput = (u32)sign_x_to_s32(26, dsp->shift_reg) << (inst.part.shift0 ^ inst.part.shift1);

  if (!inst.part.shift1)
  {
    if(ShifterOutput > 0x7FFFFF)
      ShifterOutput = 0x7FFFFF;
    else if(ShifterOutput < -0x800000)
      ShifterOutput = 0x800000;
  }
  ShifterOutput &= 0xFFFFFF;

  if (inst.part.ewt)
    dsp->efreg[inst.part.ewa] = (ShifterOutput >> 8);

  if (inst.part.twt)
    dsp->temp[TEMPWriteAddr] = ShifterOutput;

  if (inst.part.frcl)
  {
    const unsigned F_SEL_Inputs[2] = { (unsigned)(ShifterOutput >> 11), (unsigned)(ShifterOutput & 0xFFF) };

    dsp->frc_reg = F_SEL_Inputs[inst.part.shift0 & inst.part.shift1];
    //printf("FRCL: 0x%08x\n", DSP.FRC_REG);
  }

  dsp->product = ((s64)sign_x_to_s32(13, Y_SEL_Inputs[inst.part.ysel]) * X_SEL_Inputs[inst.part.xsel]) >> 12;

  u32 SGAOutput;

  SGAOutput = SGA_Inputs[inst.part.bsel];

  if (inst.part.negb)
    SGAOutput = -SGAOutput;

  if (inst.part.zero)
    SGAOutput = 0;

  dsp->shift_reg = (dsp->product + SGAOutput) & 0x3FFFFFF;
  //
  //
  if (inst.part.iwt)
  {
    dsp->mems[inst.part.iwa] = dsp->read_value;
  }

  if (dsp->read_pending)
  {
    u16 tmp = sound_ram_16[dsp->io_addr];
    dsp->read_value = (dsp->read_pending == 2) ? (tmp << 8) : float_to_int(tmp);
    dsp->read_pending = 0;
  }
  else if (dsp->write_pending)
  {
    if (!(dsp->io_addr & 0x40000))
      sound_ram_16[dsp->io_addr] = dsp->write_value;
    dsp->write_pending = 0;
  }
  {
    u16 addr;

    addr = dsp->madrs[inst.part.masa];
    addr += inst.part.nxadr;

    if (inst.part.adreb)
    {
      addr += sign_x_to_s32(12, dsp->adrs_reg);
    }

    if (!inst.part.table)
    {
      addr += dsp->mdec_ct;
      addr &= (0x2000 << dsp->rbl) - 1;
    }

    dsp->io_addr = (addr + (dsp->rbp << 12)) & 0x3FFFF;

    if (inst.part.mrd)
    {
      dsp->read_pending = 1 + inst.part.nofl;
    }
    if (inst.part.mwt)
    {
      dsp->write_pending = 1;
      dsp->write_value = inst.part.nofl ? (ShifterOutput >> 8) : int_to_float(ShifterOutput);
    }
    if (inst.part.adrl)
    {
      const u16 A_SEL_Inputs[2] = { (u16)((INPUTS >> 16) & 0xFFF), (u16)(ShifterOutput >> 12) };

      dsp->adrs_reg = A_SEL_Inputs[inst.part.shift0 & inst.part.shift1];
    }
  }
}


/* ReST-V (ADR-106): the whole program per sample with decoded steps and the registers in locals. Same results as
   ScspDspExec step by step (bakubaku / colmns97 / vfremix spent 20 ms a frame in the DSP on the Vita). A step is
   decoded again whenever its MPRO word differs from the one it was decoded from (writes, state loads) */
typedef struct {
   u8 tra, twt, twa, xsel, ysel, ira, iwt, iwa, table, mwt, mrd, ewt, ewa, adrl, frcl, sh0, sh1, yrl, negb, zero,
      bsel, nofl, coef, masa, adreb, nxadr;
} RstvDspStep;
static RstvDspStep s_dsp_step[128];
static u64 s_dsp_src[128];
static int s_dsp_valid[128];

static void rstv_dsp_decode(int i, u64 w)
{
   union ScspDspInstruction inst;
   RstvDspStep *d = &s_dsp_step[i];
   inst.all = w;
   d->tra = inst.part.tra; d->twt = inst.part.twt; d->twa = inst.part.twa;
   d->xsel = inst.part.xsel; d->ysel = inst.part.ysel; d->ira = inst.part.ira; d->iwt = inst.part.iwt;
   d->iwa = inst.part.iwa; d->table = inst.part.table; d->mwt = inst.part.mwt; d->mrd = inst.part.mrd;
   d->ewt = inst.part.ewt; d->ewa = inst.part.ewa; d->adrl = inst.part.adrl; d->frcl = inst.part.frcl;
   d->sh0 = inst.part.shift0; d->sh1 = inst.part.shift1; d->yrl = inst.part.yrl; d->negb = inst.part.negb;
   d->zero = inst.part.zero; d->bsel = inst.part.bsel; d->nofl = inst.part.nofl; d->coef = inst.part.coef;
   d->masa = inst.part.masa; d->adreb = inst.part.adreb; d->nxadr = inst.part.nxadr;
   s_dsp_src[i] = w;
   s_dsp_valid[i] = 1;
}

void ScspDspRun(ScspDsp* dsp, int nsteps, u8 * sound_ram)
{
   u16* const ram16 = (u16*)sound_ram;
   s32 inputs = dsp->inputs;
   s32 y_reg = dsp->y_reg;
   u16 frc_reg = dsp->frc_reg, adrs_reg = dsp->adrs_reg;
   s64 product = dsp->product;
   u32 read_value = dsp->read_value, write_value = dsp->write_value, io_addr = dsp->io_addr, shift_reg = dsp->shift_reg;
   int read_pending = dsp->read_pending, write_pending = dsp->write_pending;
   const u32 mdec_ct = dsp->mdec_ct;
   const u16 ring_mask = (u16)((0x2000 << dsp->rbl) - 1);
   const u32 rbp = (u32)dsp->rbp << 12;
   int i;

   for (i = 0; i < nsteps; i++) {
      const u64 w = scsp_dsp.mpro[i];
      if (!s_dsp_valid[i] || s_dsp_src[i] != w)
         rstv_dsp_decode(i, w);
      const RstvDspStep *d = &s_dsp_step[i];
      const unsigned twa = (d->twa + mdec_ct) & 0x7F, tra = (d->tra + mdec_ct) & 0x7F;

      if (d->ira & 0x20) {
         if (d->ira & 0x10) {
            if (!(d->ira & 0xE))
               inputs = dsp->exts[d->ira & 0x1] << 8;
         } else
            inputs = dsp->mixs[d->ira & 0xF] << 4;
      } else
         inputs = dsp->mems[d->ira & 0x1F];

      const int INPUTS = sign_x_to_s32(24, inputs);
      const int TEMP = sign_x_to_s32(24, dsp->temp[tra]);
      u16 ysel_in;
      switch (d->ysel) {
      case 0: ysel_in = frc_reg; break;
      case 1: ysel_in = dsp->coef[d->coef]; break;
      case 2: ysel_in = (u16)((y_reg >> 11) & 0x1FFF); break;
      default: ysel_in = (u16)((y_reg >> 4) & 0x0FFF); break;
      }
      const u32 sga_in = d->bsel ? shift_reg : (u32)TEMP;

      if (d->yrl)
         y_reg = INPUTS & 0xFFFFFF;

      int ShifterOutput = (u32)sign_x_to_s32(26, shift_reg) << (d->sh0 ^ d->sh1);
      if (!d->sh1) {
         if (ShifterOutput > 0x7FFFFF)
            ShifterOutput = 0x7FFFFF;
         else if (ShifterOutput < -0x800000)
            ShifterOutput = 0x800000;
      }
      ShifterOutput &= 0xFFFFFF;

      if (d->ewt)
         dsp->efreg[d->ewa] = (ShifterOutput >> 8);
      if (d->twt)
         dsp->temp[twa] = ShifterOutput;
      if (d->frcl)
         frc_reg = (u16)((d->sh0 & d->sh1) ? (unsigned)(ShifterOutput & 0xFFF) : (unsigned)(ShifterOutput >> 11));

      product = ((s64)sign_x_to_s32(13, ysel_in) * (d->xsel ? INPUTS : TEMP)) >> 12;

      u32 SGAOutput = sga_in;
      if (d->negb)
         SGAOutput = -SGAOutput;
      if (d->zero)
         SGAOutput = 0;
      shift_reg = (product + SGAOutput) & 0x3FFFFFF;

      if (d->iwt)
         dsp->mems[d->iwa] = read_value;

      if (read_pending) {
         const u16 tmp = ram16[io_addr];
         read_value = (read_pending == 2) ? (tmp << 8) : float_to_int(tmp);
         read_pending = 0;
      } else if (write_pending) {
         if (!(io_addr & 0x40000))
            ram16[io_addr] = write_value;
         write_pending = 0;
      }
      {
         u16 addr = dsp->madrs[d->masa];
         addr += d->nxadr;
         if (d->adreb)
            addr += sign_x_to_s32(12, adrs_reg);
         if (!d->table) {
            addr += mdec_ct;
            addr &= ring_mask;
         }
         io_addr = (addr + rbp) & 0x3FFFF;
         if (d->mrd)
            read_pending = 1 + d->nofl;
         if (d->mwt) {
            write_pending = 1;
            write_value = d->nofl ? (ShifterOutput >> 8) : int_to_float(ShifterOutput);
         }
         if (d->adrl)
            adrs_reg = (d->sh0 & d->sh1) ? (u16)(ShifterOutput >> 12) : (u16)((INPUTS >> 16) & 0xFFF);
      }
   }
   dsp->inputs = inputs;
   dsp->y_reg = y_reg;
   dsp->frc_reg = frc_reg;
   dsp->adrs_reg = adrs_reg;
   dsp->product = product;
   dsp->read_value = read_value;
   dsp->write_value = write_value;
   dsp->io_addr = io_addr;
   dsp->shift_reg = shift_reg;
   dsp->read_pending = read_pending;
   dsp->write_pending = write_pending;
}


int ScspDspAssembleGetValue(char* instruction)
{
   char temp[512] = { 0 };
   int value = 0;
   sscanf(instruction, "%s %d", temp, &value);
   return value;
}

u64 ScspDspAssembleLine(char* line)
{
   union ScspDspInstruction instruction = { 0 };

   char* temp = NULL;

   if ((temp = strstr(line, "tra")))
   {
      instruction.part.tra = ScspDspAssembleGetValue(temp);
   }

   if (strstr(line, "twt"))
   {
      instruction.part.twt = 1;
   }

   if ((temp = strstr(line, "twa")))
   {
      instruction.part.twa = ScspDspAssembleGetValue(temp);
   }

   if (strstr(line, "xsel"))
   {
      instruction.part.xsel = 1;
   }

   if ((temp = strstr(line, "ysel")))
   {
      instruction.part.ysel = ScspDspAssembleGetValue(temp);
   }

   if ((temp = strstr(line, "ira")))
   {
      instruction.part.ira = ScspDspAssembleGetValue(temp);
   }

   if (strstr(line, "iwt"))
   {
      instruction.part.iwt = 1;
   }

   if ((temp = strstr(line, "iwa")))
   {
      instruction.part.iwa = ScspDspAssembleGetValue(temp);
   }

   if (strstr(line, "table"))
   {
      instruction.part.table = 1;
   }

   if (strstr(line, "mwt"))
   {
      instruction.part.mwt = 1;
   }

   if (strstr(line, "mrd"))
   {
      instruction.part.mrd = 1;
   }

   if (strstr(line, "ewt"))
   {
      instruction.part.ewt = 1;
   }

   if ((temp = strstr(line, "ewa")))
   {
      instruction.part.ewa = ScspDspAssembleGetValue(temp);
   }

   if (strstr(line, "adrl"))
   {
      instruction.part.adrl = 1;
   }

   if (strstr(line, "frcl"))
   {
      instruction.part.frcl = 1;
   }

   if ((temp = strstr(line, "shift")))
   {
      instruction.part.shift1 = ScspDspAssembleGetValue(temp);
   }

   if (strstr(line, "yrl"))
   {
      instruction.part.yrl = 1;
   }

   if (strstr(line, "negb"))
   {
      instruction.part.negb = 1;
   }

   if (strstr(line, "zero"))
   {
      instruction.part.zero = 1;
   }

   if (strstr(line, "bsel"))
   {
      instruction.part.bsel = 1;
   }

   if (strstr(line, "nofl"))
   {
      instruction.part.nofl = 1;
   }

   if ((temp = strstr(line, "coef")))
   {
      instruction.part.coef = ScspDspAssembleGetValue(temp);
   }

   if ((temp = strstr(line, "masa")))
   {
      instruction.part.masa = ScspDspAssembleGetValue(temp);
   }

   if (strstr(line, "adreb"))
   {
      instruction.part.adreb = 1;
   }

   if (strstr(line, "nxadr"))
   {
      instruction.part.adreb = 1;
   }

   if (strstr(line, "nop"))
   {
      instruction.all = 0;
   }

   return instruction.all;
}

void ScspDspAssembleFromFile(char * filename, u64* output)
{
   int i;
   char line[1024] = { 0 };

   FILE * fp = fopen(filename, "r");

   if (!fp)
   {
      return;
   }

   for (i = 0; i < 128; i++)
   {
      char * result = fgets(line, sizeof(line), fp);
      output[i] = ScspDspAssembleLine(line);
   }
   fclose(fp);
}

void ScspDspDisasm(u8 addr, char *outstring)
{
   union ScspDspInstruction instruction;

   instruction.all = scsp_dsp.mpro[addr];

   sprintf(outstring, "%02X: ", addr);
   outstring += strlen(outstring);

   if (instruction.all == 0)
   {
      sprintf(outstring, "nop ");
      outstring += strlen(outstring);
      return;
   }

   if (instruction.part.nofl)
   {
      sprintf(outstring, "nofl ");
      outstring += strlen(outstring);
   }

   if (instruction.part.coef)
   {
      sprintf(outstring, "coef %02X ", (unsigned int)(instruction.part.coef & 0x3F));
      outstring += strlen(outstring);
   }

   if (instruction.part.masa)
   {
      sprintf(outstring, "masa %02X ", (unsigned int)(instruction.part.masa & 0x1F));
      outstring += strlen(outstring);
   }

   if (instruction.part.adreb)
   {
      sprintf(outstring, "adreb ");
      outstring += strlen(outstring);
   }

   if (instruction.part.nxadr)
   {
      sprintf(outstring, "nxadr ");
      outstring += strlen(outstring);
   }

   if (instruction.part.table)
   {
      sprintf(outstring, "table ");
      outstring += strlen(outstring);
   }

   if (instruction.part.mwt)
   {
      sprintf(outstring, "mwt ");
      outstring += strlen(outstring);
   }

   if (instruction.part.mrd)
   {
      sprintf(outstring, "mrd ");
      outstring += strlen(outstring);
   }

   if (instruction.part.ewt)
   {
      sprintf(outstring, "ewt ");
      outstring += strlen(outstring);
   }

   if (instruction.part.ewa)
   {
      sprintf(outstring, "ewa %01X ", (unsigned int)(instruction.part.ewa & 0xf));
      outstring += strlen(outstring);
   }

   if (instruction.part.adrl)
   {
      sprintf(outstring, "adrl ");
      outstring += strlen(outstring);
   }

   if (instruction.part.frcl)
   {
      sprintf(outstring, "frcl ");
      outstring += strlen(outstring);
   }

   if (instruction.part.shift1)
   {
      sprintf(outstring, "shift %d ", (int)(instruction.part.shift1 & 3));
      outstring += strlen(outstring);
   }

   if (instruction.part.yrl)
   {
      sprintf(outstring, "yrl ");
      outstring += strlen(outstring);
   }

   if (instruction.part.negb)
   {
      sprintf(outstring, "negb ");
      outstring += strlen(outstring);
   }

   if (instruction.part.zero)
   {
      sprintf(outstring, "zero ");
      outstring += strlen(outstring);
   }

   if (instruction.part.bsel)
   {
      sprintf(outstring, "bsel ");
      outstring += strlen(outstring);
   }

   if (instruction.part.xsel)
   {
      sprintf(outstring, "xsel ");
      outstring += strlen(outstring);
   }

   if (instruction.part.ysel)
   {
      sprintf(outstring, "ysel %d ", (int)(instruction.part.ysel & 3));
      outstring += strlen(outstring);
   }

   if (instruction.part.ira)
   {
      sprintf(outstring, "ira %02X ", (int)(instruction.part.ira & 0x3F));
      outstring += strlen(outstring);
   }

   if (instruction.part.iwt)
   {
      sprintf(outstring, "iwt ");
      outstring += strlen(outstring);
   }

   if (instruction.part.iwa)
   {
      sprintf(outstring, "iwa %02X ", (unsigned int)(instruction.part.iwa & 0x1F));
      outstring += strlen(outstring);
   }

   if (instruction.part.tra)
   {
      sprintf(outstring, "tra %02X ", (unsigned int)(instruction.part.tra & 0x7F));
      outstring += strlen(outstring);
   }

   if (instruction.part.twt)
   {
      sprintf(outstring, "twt ");
      outstring += strlen(outstring);
   }

   if (instruction.part.twa)
   {
      sprintf(outstring, "twa %02X ", (unsigned int)(instruction.part.twa & 0x7F));
      outstring += strlen(outstring);
   }

   if (instruction.part.unknown)
   {
      sprintf(outstring, "unknown ");
      outstring += strlen(outstring);
   }

   if (instruction.part.unknown2)
   {
      sprintf(outstring, "unknown2 ");
      outstring += strlen(outstring);
   }

//   if (instruction.part.unknown3)
 //  {
 //     sprintf(outstring, "unknown3 %d", (int)(instruction.part.unknown3 & 3));
 //     outstring += strlen(outstring);
 //  }
}

void ScspDspDisassembleToFile(char * filename)
{
   int i;
   FILE * fp = fopen(filename, "w");

   if (!fp)
   {
      return;
   }

   for (i = 0; i < 128; i++)
   {
      char output[1024] = { 0 };
      ScspDspDisasm(i, output);
      fprintf(fp, "%s\n", output);
   }

   fclose(fp);
}
