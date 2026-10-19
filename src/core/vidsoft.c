/*  Copyright 2003-2004 Guillaume Duhamel
    Copyright 2004-2008 Theo Berkau
    Copyright 2006 Fabien Coulon
    Copyright 2015 R. Danbrook

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

/*! \file vidsoft.c
    \brief Software video renderer interface.
*/

#include "vidsoft.h"
#include "ygl.h"
#include "vidshared.h"
#include "debug.h"
#include "vdp2.h"
#include "titan/titan.h"
#include "rstv_prof.h"
#include "rstv_capture.h"
#include "rstv_vdp1rec.h"
static void rstv_v1_unsupported(unsigned why);
static void rstv_v1pat_reset(void);
#include <stddef.h>

int rstv_capture_mode = 0;
static int rstv_rbg0_erase_pending;
/* ReST-V (ADR-031): in capture mode, a line whose VDP2 registers equal the previous line's repeats that line's
   result (LoadLineParams etc. skipped). 0 = always recompute (harness check) */
int rstv_cap_same = 1;
unsigned char rstv_line_same[512];
/* ReST-V (ADR-079): the layer pass reads these (NULL = the live VDP2 state). The GPU frontend points them at its
   snapshot so the pass can run on the render worker while the main thread emulates the next frame. */
static Vdp2 *rstv_in_lines, *rstv_in_regs;
static u8 *rstv_in_ram, *rstv_in_cram;
static struct CellScrollData *rstv_in_cells;
static const Vdp2 *rstv_line_same_src; /* the lines rstv_line_same was computed from */
void rstv_vidsoft_set_inputs(Vdp2 *lines, Vdp2 *regs, u8 *ram, u8 *cram, struct CellScrollData *cells)
{
   rstv_in_lines = lines; rstv_in_regs = regs; rstv_in_ram = ram; rstv_in_cram = cram; rstv_in_cells = cells;
}
#define IN_LINES (rstv_in_lines ? rstv_in_lines : Vdp2Lines)
#define IN_REGS (rstv_in_regs ? rstv_in_regs : Vdp2Regs)
#define IN_RAM (rstv_in_ram ? rstv_in_ram : Vdp2Ram)
#define IN_CRAM (rstv_in_cram ? rstv_in_cram : Vdp2ColorRam)
#define IN_CELLS (rstv_in_cells ? rstv_in_cells : cell_scroll_data)
RstvLayerCapture rstv_layer_capture[6];
RstvRotCapture rstv_rot_capture;
int rstv_rot_why;
int rstv_rot_dbg[4]; /* diagnosis (harness RSTV_ROTWHY): parameter A dKAx, dKAst, coefficient mode, KAst */
int Rbg0CheckRam(Vdp2* regs);


#ifdef HAVE_LIBGL
#define USE_OPENGL
#endif

#ifdef USE_OPENGL
#include "ygl.h"
#endif

#include "yui.h"
#include "threads.h"

#include <stdlib.h>
#include <limits.h>

#if defined WORDS_BIGENDIAN
static INLINE u32 COLSAT2YAB16(int priority,u32 temp)            { return (priority | (temp & 0x7C00) << 1 | (temp & 0x3E0) << 14 | (temp & 0x1F) << 27); }
static INLINE u32 COLSAT2YAB32(int priority,u32 temp)            { return (((temp & 0xFF) << 24) | ((temp & 0xFF00) << 8) | ((temp & 0xFF0000) >> 8) | priority); }
static INLINE u32 COLSAT2YAB32_2(int priority,u32 temp1,u32 temp2)   { return (((temp2 & 0xFF) << 24) | ((temp2 & 0xFF00) << 8) | ((temp1 & 0xFF) << 8) | priority); }
static INLINE u32 COLSATSTRIPPRIORITY(u32 pixel)              { return (pixel | 0xFF); }
#else
static INLINE u32 COLSAT2YAB16(int priority,u32 temp) { return (priority << 24 | (temp & 0x1F) << 3 | (temp & 0x3E0) << 6 | (temp & 0x7C00) << 9); }
static INLINE u32 COLSAT2YAB32(int priority, u32 temp) { return (priority << 24 | (temp & 0xFF0000) | (temp & 0xFF00) | (temp & 0xFF)); }
static INLINE u32 COLSAT2YAB32_2(int priority,u32 temp1,u32 temp2)   { return (priority << 24 | ((temp1 & 0xFF) << 16) | (temp2 & 0xFF00) | (temp2 & 0xFF)); }
static INLINE u32 COLSATSTRIPPRIORITY(u32 pixel) { return (0xFF000000 | pixel); }
#endif

#define COLOR_ADDt(b)		(b>0xFF?0xFF:(b<0?0:b))
#define COLOR_ADDb(b1,b2)	COLOR_ADDt((signed) (b1) + (b2))
#ifdef WORDS_BIGENDIAN
#define COLOR_ADD(l,r,g,b)      (l & 0xFF) | \
                                (COLOR_ADDb((l >> 8) & 0xFF, b) << 8) | \
                                (COLOR_ADDb((l >> 16) & 0xFF, g) << 16) | \
                                (COLOR_ADDb((l >> 24), r) << 24)
#else
#define COLOR_ADD(l,r,g,b)	COLOR_ADDb((l & 0xFF), r) | \
                                (COLOR_ADDb((l >> 8) & 0xFF, g) << 8) | \
                                (COLOR_ADDb((l >> 16) & 0xFF, b) << 16) | \
				(l & 0xFF000000)
#endif


int VIDSoftInit(void);
void VIDSoftSetupGL(void);
void VIDSoftDeInit(void);
void VIDSoftResize(int,int,unsigned int, unsigned int, int, int);
int VIDSoftIsFullscreen(void);
int VIDSoftVdp1Reset(void);
void VIDSoftVdp1DrawStart(void);
void VIDSoftVdp1DrawEnd(void);
void VIDSoftVdp1NormalSpriteDraw(u8 * ram, Vdp1 * regs, u8* back_framebuffer);
void VIDSoftVdp1ScaledSpriteDraw(u8 * ram, Vdp1 * regs, u8* back_framebuffer);
void VIDSoftVdp1DistortedSpriteDraw(u8 * ram, Vdp1 * regs, u8* back_framebuffer);
void VIDSoftVdp1PolygonDraw(u8 * ram, Vdp1 * regs, u8* back_framebuffer);
void VIDSoftVdp1PolylineDraw(u8 * ram, Vdp1 * regs, u8* back_framebuffer);
void VIDSoftVdp1LineDraw(u8 * ram, Vdp1 * regs, u8* back_framebuffer);
void VIDSoftVdp1UserClipping(u8 * ram, Vdp1 * regs);
void VIDSoftVdp1SystemClipping(u8 * ram, Vdp1 * regs);
void VIDSoftVdp1LocalCoordinate(u8 * ram, Vdp1 * regs);
void VIDSoftVdp1ReadFrameBuffer(u32 type, u32 addr, void * out);
void VIDSoftVdp1WriteFrameBuffer(u32 type, u32 addr, u32 val);
void VIDSoftVdp1EraseWrite(){};
void VIDSoftVdp1FrameChange(void);
int VIDSoftVdp2Reset(void);
void VIDSoftVdp2DrawStart(void);
void VIDSoftVdp2DrawEnd(void);
void VIDSoftVdp2DrawScreens(void);
void VIDSoftVdp2SetResolution(u16 TVMD);
void VIDSoftGetGlSize(int *width, int *height);
void VIDSoftVdp1SwapFrameBuffer(void);
void VIDSoftVdp1EraseFrameBuffer(Vdp1* regs, u8 * back_framebuffer);
void VIDSoftSetSettingValueMode(int type, int value){};
void VIDSoftSync(){};
void VIDSoftVdp2DispOff(void);void VidsoftDrawSprite(Vdp2 * vdp2_regs, u8 * sprite_window_mask, u8* vdp1_front_framebuffer, u8 * vdp2_ram, Vdp1* vdp1_regs, Vdp2* vdp2_lines, u8*color_ram);
void VIDSoftGetNativeResolution(int *width, int *height, int*interlace);
void VIDSoftVdp2DispOff(void);void VIDSoftVdp2DispOff(void);

VideoInterface_struct VIDSoft = {
VIDCORE_SOFT,
"Software Video Interface",
VIDSoftInit,
VIDSoftDeInit,
VIDSoftResize,
VIDSoftIsFullscreen,
VIDSoftVdp1Reset,
VIDSoftVdp1DrawStart,
VIDSoftVdp1DrawEnd,
VIDSoftVdp1NormalSpriteDraw,
VIDSoftVdp1ScaledSpriteDraw,
VIDSoftVdp1DistortedSpriteDraw,
//for the actual hardware, polygons are essentially identical to distorted sprites
//the actual hardware draws using diagonal lines, which is why using half-transparent processing
//on distorted sprites and polygons is not recommended since the hardware overdraws to prevent gaps
//thus, with half-transparent processing some pixels will be processed more than once, producing moire patterns in the drawn shapes
VIDSoftVdp1DistortedSpriteDraw,
VIDSoftVdp1PolylineDraw,
VIDSoftVdp1LineDraw,
VIDSoftVdp1UserClipping,
VIDSoftVdp1SystemClipping,
VIDSoftVdp1LocalCoordinate,
VIDSoftVdp1ReadFrameBuffer,
VIDSoftVdp1WriteFrameBuffer,
VIDSoftVdp1EraseWrite,
VIDSoftVdp1FrameChange,
VIDSoftVdp2Reset,
VIDSoftVdp2DrawStart,
VIDSoftVdp2DrawEnd,
VIDSoftVdp2DrawScreens,
VIDSoftGetGlSize,
VIDSoftSetSettingValueMode,
VIDSoftSync,
VIDSoftGetNativeResolution,
VIDSoftVdp2DispOff
};

pixel_t *dispbuffer=NULL;
u8 *vdp1framebuffer[2]= { NULL, NULL };
u8 *vdp1frontframebuffer;
u8 *vdp1backframebuffer;
u8 sprite_window_mask[704 * 512];

static int vdp1width;
static int vdp1height;
static int vdp1interlace;
static int vdp1pixelsize;

/* ReST-V GPU compose mode (vita/src/video/gvdp2.c, ADR-011): the CPU only draws the VDP2 background
   layers into the Titan buffers, without colour offset (applied on the GPU); the sprite layer and the
   priority/colour-calculation compose are done on the GPU. */
int rstv_gpu_compose = 0;
static void rstv_vdp1_sync_buffer(const u8 *buf);
void rstv_vidsoft_vdp1_info(int *width, int *height, int *pixelsize, u8 **front_framebuffer)
{
   rstv_vdp1_sync_buffer(vdp1frontframebuffer);
   *width = vdp1width;
   *height = vdp1height;
   *pixelsize = vdp1pixelsize;
   *front_framebuffer = vdp1frontframebuffer;
}
int vdp2width;
int rbg0width = 0;
int vdp2height;

#ifdef USE_OPENGL
static int outputwidth;
static int outputheight;
GLuint vao = 0;
GLuint vbo = 0;
GLuint vshader = 0;
GLuint fshader = 0;
GLuint gl_shader_prog = 0;
GLuint gl_texture_id = 0;
#endif
int vdp2_x_hires = 0;
int vdp2_interlace = 0;
/* ReST-V (ADR-100, speed): draw hi-res (640/704) and double-density interlace screens at half width / height.
   rstv_hires_half enables it (frontend); rstv_half_x / rstv_half_y say whether this frame is halved */
int rstv_hires_half = 0;
int rstv_half_x = 0, rstv_half_y = 0;
static int rbg0height = 0;
int bilinear = 0;
int vidsoft_num_layer_threads = 0;
int bad_cycle_setting[6] = { 0 };

struct VidsoftVdp1ThreadContext
{
   volatile int draw_finished;
   volatile int need_draw;
   Vdp1 regs;
   u8 ram[0x80000];
   u8 back_framebuffer[0x40000];
}vidsoft_vdp1_thread_context;

int vidsoft_vdp1_thread_enabled = 0;
#ifdef RSTV
/* ReST-V VDP1 thread: the command list is drawn on a helper thread straight into the back framebuffer
   (VDP1 RAM and registers are snapshotted). The main thread only waits when it touches that buffer:
   the next VDP1 draw, CPU framebuffer access, or the VDP2 sprite layer reading it after the swap. */
static YabEventQueue *rstv_vdp1_req, *rstv_vdp1_done;
static volatile int rstv_vdp1_pending;
static u8 *rstv_vdp1_target;
int rstv_vdp1_thread = 0; /* set before YabauseInit */
static void rstv_vdp1_sync_buffer(const u8 *buf)
{
   if (rstv_vdp1_pending && (buf == NULL || buf == rstv_vdp1_target))
   {
      u64 pt_ = RSTV_PROF_T();
      YabWaitEventQueue(rstv_vdp1_done);
      RSTV_PROF_ADD(RSTV_PROF_VDP1_WAIT, pt_);
      rstv_vdp1_pending = 0;
   }
}
void rstv_vdp1_quiesce(void) { rstv_vdp1_sync_buffer(NULL); } /* save state: no VDP1 drawing in flight */
#endif

typedef struct { s16 x; s16 y; } vdp1vertex;

typedef struct
{
   int pagepixelwh, pagepixelwh_bits, pagepixelwh_mask;
   int planepixelwidth, planepixelwidth_bits, planepixelwidth_mask;
   int planepixelheight, planepixelheight_bits, planepixelheight_mask;
   int screenwidth;
   int screenheight;
   int oldcellx, oldcelly, oldcellcheck;
   int xmask, ymask;
   u32 planetbl[16];
} screeninfo_struct;

//////////////////////////////////////////////////////////////////////////////

static INLINE u32 FASTCALL Vdp2ColorRamGetColorSoft(u32 addr, u8* vdp2_color_ram)
{
   switch(Vdp2Internal.ColorMode)
   {
      case 0:
      {
         u32 tmp;
         addr <<= 1;
         tmp = T2ReadWord(vdp2_color_ram, addr & 0xFFF);
         /* we preserve MSB for special color calculation mode 3 (see Vdp2 user's manual 3.4 and 12.3) */
         return (((tmp & 0x1F) << 3) | ((tmp & 0x03E0) << 6) | ((tmp & 0x7C00) << 9)) | ((tmp & 0x8000) << 16);
      }
      case 1:
      {
         u32 tmp;
         addr <<= 1;
         tmp = T2ReadWord(vdp2_color_ram, addr & 0xFFF);
         /* we preserve MSB for special color calculation mode 3 (see Vdp2 user's manual 3.4 and 12.3) */
         return (((tmp & 0x1F) << 3) | ((tmp & 0x03E0) << 6) | ((tmp & 0x7C00) << 9)) | ((tmp & 0x8000) << 16);
      }
      case 2:
      {
         addr <<= 2;   
         return T2ReadLong(vdp2_color_ram, addr & 0xFFF);
      }
      default: break;
   }

   return 0;
}

//////////////////////////////////////////////////////////////////////////////

static INLINE void Vdp2PatternAddr(vdp2draw_struct *info, Vdp2* regs, u8* ram)
{
   switch(info->patterndatasize)
   {
      case 1:
      {
         u16 tmp = T1ReadWord(ram, info->addr);         

         info->addr += 2;
         info->specialfunction = (info->supplementdata >> 9) & 0x1;
         info->specialcolorfunction = (info->supplementdata >> 8) & 0x1;

         switch(info->colornumber)
         {
            case 0: // in 16 colors
               info->paladdr = ((tmp & 0xF000) >> 8) | ((info->supplementdata & 0xE0) << 3);
               break;
            default: // not in 16 colors
               info->paladdr = (tmp & 0x7000) >> 4;
               break;
         }

         switch(info->auxmode)
         {
            case 0:
               info->flipfunction = (tmp & 0xC00) >> 10;

               switch(info->patternwh)
               {
                  case 1:
                     info->charaddr = (tmp & 0x3FF) | ((info->supplementdata & 0x1F) << 10);
                     break;
                  case 2:
                     info->charaddr = ((tmp & 0x3FF) << 2) | (info->supplementdata & 0x3) | ((info->supplementdata & 0x1C) << 10);
                     break;
               }
               break;
            case 1:
               info->flipfunction = 0;

               switch(info->patternwh)
               {
                  case 1:
                     info->charaddr = (tmp & 0xFFF) | ((info->supplementdata & 0x1C) << 10);
                     break;
                  case 2:
                     info->charaddr = ((tmp & 0xFFF) << 2) | (info->supplementdata & 0x3) | ((info->supplementdata & 0x10) << 10);
                     break;
               }
               break;
         }

         break;
      }
      case 2: {
         u16 tmp1 = T1ReadWord(ram, info->addr);
         u16 tmp2 = T1ReadWord(ram, info->addr+2);
         info->addr += 4;
         info->charaddr = tmp2 & 0x7FFF;
         info->flipfunction = (tmp1 & 0xC000) >> 14;
         switch(info->colornumber) {
            case 0:
               info->paladdr = (tmp1 & 0x7F) << 4;
               break;
            default:
               info->paladdr = ((tmp1 & 0x70) << 4);
               break;
         }
         info->specialfunction = (tmp1 & 0x2000) >> 13;
         info->specialcolorfunction = (tmp1 & 0x1000) >> 12;
         break;
      }
   }

   if (!(regs->VRSIZE & 0x8000))
      info->charaddr &= 0x3FFF;

   info->charaddr *= 0x20; // selon Runik
   if (info->specialprimode == 1) {
      info->priority = (info->priority & 0xE) | (info->specialfunction & 1);
   }
}

//////////////////////////////////////////////////////////////////////////////

static INLINE u32 FASTCALL DoNothing(UNUSED void *info, u32 pixel)
{
   return pixel;
}

//////////////////////////////////////////////////////////////////////////////

static INLINE u32 FASTCALL DoColorOffset(void *info, u32 pixel)
{
    return COLOR_ADD(pixel, ((vdp2draw_struct *)info)->cor,
                     ((vdp2draw_struct *)info)->cog,
                     ((vdp2draw_struct *)info)->cob);
}

//////////////////////////////////////////////////////////////////////////////

static INLINE void ReadVdp2ColorOffset(Vdp2 * regs, vdp2draw_struct *info, int clofmask, int ccmask)
{
   if (regs->CLOFEN & clofmask)
   {
      // color offset enable
      if (regs->CLOFSL & clofmask)
      {
         // color offset B
         info->cor = regs->COBR & 0xFF;
         if (regs->COBR & 0x100)
            info->cor |= 0xFFFFFF00;

         info->cog = regs->COBG & 0xFF;
         if (regs->COBG & 0x100)
            info->cog |= 0xFFFFFF00;

         info->cob = regs->COBB & 0xFF;
         if (regs->COBB & 0x100)
            info->cob |= 0xFFFFFF00;
      }
      else
      {
         // color offset A
         info->cor = regs->COAR & 0xFF;
         if (regs->COAR & 0x100)
            info->cor |= 0xFFFFFF00;

         info->cog = regs->COAG & 0xFF;
         if (regs->COAG & 0x100)
            info->cog |= 0xFFFFFF00;

         info->cob = regs->COAB & 0xFF;
         if (regs->COAB & 0x100)
            info->cob |= 0xFFFFFF00;
      }

      info->PostPixelFetchCalc = &DoColorOffset;
   }
   else // color offset disable
      info->PostPixelFetchCalc = &DoNothing;

   if (rstv_gpu_compose)
      info->PostPixelFetchCalc = &DoNothing; // ReST-V: offset applied by the GPU compose pass
}

//////////////////////////////////////////////////////////////////////////////

static INLINE int Vdp2FetchPixel(vdp2draw_struct *info, int x, int y, u32 *color, u32 *dot, u8 * ram, int charaddr, int paladdr, u8* vdp2_color_ram)
{
   switch(info->colornumber)
   {
      case 0: // 4 BPP
         *dot = T1ReadByte(ram, ((charaddr + ((y * info->cellw) + x) / 2) & 0x7FFFF));
         if (!(x & 0x1)) *dot >>= 4;
         if (!(*dot & 0xF) && info->transparencyenable) return 0;
         else
         {
            *color = Vdp2ColorRamGetColorSoft(info->coloroffset + (paladdr | (*dot & 0xF)),vdp2_color_ram);
            return 1;
         }
      case 1: // 8 BPP
         *dot = T1ReadByte(ram, ((charaddr + (y * info->cellw) + x) & 0x7FFFF));
         if (!(*dot & 0xFF) && info->transparencyenable) return 0;
         else
         {
            *color = Vdp2ColorRamGetColorSoft(info->coloroffset + (paladdr | (*dot & 0xFF)), vdp2_color_ram);
            return 1;
         }
      case 2: // 16 BPP(palette)
         *dot = T1ReadWord(ram, ((charaddr + ((y * info->cellw) + x) * 2) & 0x7FFFF));
         if ((*dot == 0) && info->transparencyenable) return 0;
         else
         {
            *color = Vdp2ColorRamGetColorSoft(info->coloroffset + *dot, vdp2_color_ram);
            return 1;
         }
      case 3: // 16 BPP(RGB)      
         *dot = T1ReadWord(ram, ((charaddr + ((y * info->cellw) + x) * 2) & 0x7FFFF));
         if (!(*dot & 0x8000) && info->transparencyenable) return 0;
         else
         {
            *color = COLSAT2YAB16(0, *dot);
            return 1;
         }
      case 4: // 32 BPP
         *dot = T1ReadLong(ram, ((charaddr + ((y * info->cellw) + x) * 4) & 0x7FFFF));
         if (!(*dot & 0x80000000) && info->transparencyenable) return 0;
         else
         {
            *color = COLSAT2YAB32(0, *dot);
            return 1;
         }
      default:
         return 0;
   }
}

//////////////////////////////////////////////////////////////////////////////

static INLINE int TestWindow(int wctl, int enablemask, int inoutmask, clipping_struct *clip, int x, int y)
{
   if (wctl & enablemask) 
   {
      if (wctl & inoutmask)
      {
         // Draw inside of window
         if (x < clip->xstart || x > clip->xend ||
             y < clip->ystart || y > clip->yend)
            return 0;
      }
      else
      {
         // Draw outside of window
         if (x >= clip->xstart && x <= clip->xend &&
             y >= clip->ystart && y <= clip->yend)
            return 0;

		 //it seems to overflow vertically on hardware
		 if(clip->yend > vdp2height && (x >= clip->xstart && x <= clip->xend ))
			 return 0;
      }
      return 1; // return inactive;
   }
   return 3; // return disabled | inactive;
}

//////////////////////////////////////////////////////////////////////////////

int TestSpriteWindow(int wctl, int x, int y)
{
   int mask;
   int addr = (y*vdp2width) + x;

   if (addr >= (704 * 512))
      return 0;

   mask = sprite_window_mask[addr];

   if (wctl & 0x20)//sprite window enabled on layer
   {
      if (wctl & 0x10)//inside or outside
      {
         if (mask == 0)
            return 0;
      }
      else
      {
         if (mask)
            return 0;
      }

      return 1;
   }
   return 3;
}

//////////////////////////////////////////////////////////////////////////////

int WindowLogic(int wctl, int w0, int w1)
{
   if (((wctl & 0x80) == 0x80))
      /* AND logic, returns 0 only if both the windows are active */
      return w0 || w1;
   else
      /* OR logic, returns 0 if one of the windows is active */
      return w0 && w1;
}

//////////////////////////////////////////////////////////////////////////////

static INLINE int TestBothWindow(int wctl, clipping_struct *clip, int x, int y)
{
    //all windows disabled (ReST-V: checked first, the window tests below are not needed then)
    if ((wctl & 0x2a) == 0)
    {
       if ((wctl & 0x80) == 0x80)
          return 0;
       else
          return 1;
    }

    int w0 = TestWindow(wctl, 0x2, 0x1, &clip[0], x, y);
    int w1 = TestWindow(wctl, 0x8, 0x4, &clip[1], x, y);
    int spr = TestSpriteWindow(wctl, x,y);

    //if only window 0 is enabled
    if ((w1 & 2) && (spr & 2)) return w0 & 1;
    //if only window 1 is enabled
    if ((w0 & 2) && (spr & 2)) return w1 & 1;

    //window 0 and 1, sprite disabled
    if ((spr & 2))
       return WindowLogic(wctl, w0, w1);

    //if only sprite window is enabled
    if ((w1 & 2) && (w0 & 2)) return spr & 1;

    //window 0 and sprite enabled
    if ((wctl & 0x2a) == 0x22)
       return WindowLogic(wctl, w0, spr);

    //window 1 and sprite enabled
    if ((wctl & 0x2a) == 0x28)
       return WindowLogic(wctl, w1, spr);

    //all three windows enabled
    if ((wctl & 0x2a) == 0x2a)
    {
       if ((wctl & 0x80) == 0x80)
          return w0 || w1 || spr;//and logic
       else
          return w0 && w1 && spr;//or logic
    }

    return 1;

}

//////////////////////////////////////////////////////////////////////////////

static INLINE void GeneratePlaneAddrTable(vdp2draw_struct *info, u32 *planetbl, void FASTCALL (* PlaneAddr)(void *, int, Vdp2* ), Vdp2* regs)
{
   int i;

   for (i = 0; i < (info->mapwh*info->mapwh); i++)
   {
      PlaneAddr(info, i, regs);
      planetbl[i] = info->addr;
   }
}

//////////////////////////////////////////////////////////////////////////////

static INLINE void FASTCALL Vdp2MapCalcXY(vdp2draw_struct *info, int *x, int *y,
                                 screeninfo_struct *sinfo, Vdp2* regs, u8 * ram, int bad_cycle)
{
   int planenum;
   int flipfunction;
   const int pagesize_bits=info->pagewh_bits*2;
   const int cellwh=(2 + info->patternwh);

   const int check = ((y[0] >> cellwh) << 16) | (x[0] >> cellwh);
   //if ((x[0] >> cellwh) != sinfo->oldcellx || (y[0] >> cellwh) != sinfo->oldcelly)
   if(check != sinfo->oldcellcheck)
   {
      sinfo->oldcellx = x[0] >> cellwh;
      sinfo->oldcelly = y[0] >> cellwh;
	  sinfo->oldcellcheck = (sinfo->oldcelly << 16) | sinfo->oldcellx;

      // Calculate which plane we're dealing with
      planenum = ((y[0] >> sinfo->planepixelheight_bits) * info->mapwh) + (x[0] >> sinfo->planepixelwidth_bits);
      x[0] = (x[0] & sinfo->planepixelwidth_mask);
      y[0] = (y[0] & sinfo->planepixelheight_mask);

      // Fetch and decode pattern name data
      info->addr = sinfo->planetbl[planenum];

      // Figure out which page it's on(if plane size is not 1x1)
      info->addr += ((  ((y[0] >> sinfo->pagepixelwh_bits) << pagesize_bits) << info->planew_bits) +
                     (   (x[0] >> sinfo->pagepixelwh_bits) << pagesize_bits) +
                     (((y[0] & sinfo->pagepixelwh_mask) >> cellwh) << info->pagewh_bits) +
                     ((x[0] & sinfo->pagepixelwh_mask) >> cellwh)) << (info->patterndatasize_bits+1);

      Vdp2PatternAddr(info, regs, ram); // Heh, this could be optimized

      //pipeline the tiles so that they shift over by 1
      info->pipe[0] = info->pipe[1];

      info->pipe[1].paladdr = info->paladdr;
      info->pipe[1].charaddr = info->charaddr;
      info->pipe[1].flipfunction = info->flipfunction;
   }

   if (bad_cycle)
   {
      flipfunction = info->pipe[0].flipfunction;
   }
   else
   {
      flipfunction = info->flipfunction;
   }

   // Figure out which pixel in the tile we want
   if (info->patternwh == 1)
   {
      x[0] &= 8-1;
      y[0] &= 8-1;

	  switch(flipfunction & 0x3)
	  {
	  case 0: //none
		  break;
	  case 1: //horizontal flip
		  x[0] = 8 - 1 - x[0];
		  break;
	  case 2: // vertical flip
         y[0] = 8 - 1 - y[0];
		 break;
	  case 3: //flip both
         x[0] = 8 - 1 - x[0];
		 y[0] = 8 - 1 - y[0];
		 break;
	  }
   }
   else
   {
      if (flipfunction)
      {
         y[0] &= 16 - 1;
         if (flipfunction & 0x2)
         {
            if (!(y[0] & 8))
               y[0] = 8 - 1 - y[0] + 16;
            else
               y[0] = 16 - 1 - y[0];
         }
         else if (y[0] & 8)
            y[0] += 8;

         if (flipfunction & 0x1)
         {
            if (!(x[0] & 8))
               y[0] += 8;

            x[0] &= 8-1;
            x[0] = 8 - 1 - x[0];
         }
         else if (x[0] & 8)
         {
            y[0] += 8;
            x[0] &= 8-1;
         }
         else
            x[0] &= 8-1;
      }
      else
      {
         y[0] &= 16 - 1;

         if (y[0] & 8)
            y[0] += 8;
         if (x[0] & 8)
            y[0] += 8;
         x[0] &= 8-1;
      }
   }
}

//////////////////////////////////////////////////////////////////////////////

static INLINE void SetupScreenVars(vdp2draw_struct *info, screeninfo_struct *sinfo, void FASTCALL (* PlaneAddr)(void *, int, Vdp2*), Vdp2* regs)
{
   if (!info->isbitmap)
   {
      sinfo->pagepixelwh=64*8;
	  sinfo->pagepixelwh_bits = 9;
	  sinfo->pagepixelwh_mask = 511;

      sinfo->planepixelwidth=info->planew*sinfo->pagepixelwh;
	  sinfo->planepixelwidth_bits = 8+info->planew;
	  sinfo->planepixelwidth_mask = (1<<(sinfo->planepixelwidth_bits))-1;

      sinfo->planepixelheight=info->planeh*sinfo->pagepixelwh;
	  sinfo->planepixelheight_bits = 8+info->planeh;
	  sinfo->planepixelheight_mask = (1<<(sinfo->planepixelheight_bits))-1;

      sinfo->screenwidth=info->mapwh*sinfo->planepixelwidth;
      sinfo->screenheight=info->mapwh*sinfo->planepixelheight;
      sinfo->oldcellx=-1;
      sinfo->oldcelly=-1;
      sinfo->oldcellcheck=-1;
      sinfo->xmask = sinfo->screenwidth-1;
      sinfo->ymask = sinfo->screenheight-1;
      GeneratePlaneAddrTable(info, sinfo->planetbl, PlaneAddr, regs);
   }
   else
   {
      sinfo->pagepixelwh = 0;
	  sinfo->pagepixelwh_bits = 0;
	  sinfo->pagepixelwh_mask = 0;
      sinfo->planepixelwidth=0;
	  sinfo->planepixelwidth_bits=0;
	  sinfo->planepixelwidth_mask=0;
      sinfo->planepixelheight=0;
	  sinfo->planepixelheight_bits=0;
	  sinfo->planepixelheight_mask=0;
      sinfo->screenwidth=0;
      sinfo->screenheight=0;
      sinfo->oldcellx=0;
      sinfo->oldcelly=0;
      sinfo->oldcellcheck=0;
      sinfo->xmask = info->cellw-1;
      sinfo->ymask = info->cellh-1;
   }
}

//////////////////////////////////////////////////////////////////////////////

static u8 FASTCALL GetAlpha(vdp2draw_struct * info, u32 color, u32 dot)
{
   if (((info->specialcolormode == 1) || (info->specialcolormode == 2)) && ((info->specialcolorfunction & 1) == 0)) {
      /* special color calculation mode 1 and 2 enables color calculation only when special color function = 1 */
      return 0x3F;
   } else if (info->specialcolormode == 2) {
      /* special color calculation 2 enables color calculation according to lower bits of the color code */
      if ((info->specialcode & (1 << ((dot & 0xF) >> 1))) == 0) {
         return 0x3F;
      }
   } else if ((info->specialcolormode == 3) && ((color & 0x80000000) == 0)) {
      /* special color calculation mode 3 enables color calculation only for dots with MSB = 1 */
      return 0x3F;
   }
   return info->alpha;
}

//////////////////////////////////////////////////////////////////////////////

int PixelIsSpecialPriority(int specialcode, int dot)
{
   dot &= 0xf;

   if (specialcode & 0x01)
   {
      if (dot == 0 || dot == 1)
         return 1;
   }
   if (specialcode & 0x02)
   {
      if (dot == 2 || dot == 3)
         return 1;
   }
   if (specialcode & 0x04)
   {
      if (dot == 4 || dot == 5)
         return 1;
   }
   if (specialcode & 0x08)
   {
      if (dot == 6 || dot == 7)
         return 1;
   }
   if (specialcode & 0x10)
   {
      if (dot == 8 || dot == 9)
         return 1;
   }
   if (specialcode & 0x20)
   {
      if (dot == 0xa || dot == 0xb)
         return 1;
   }
   if (specialcode & 0x40)
   {
      if (dot == 0xc || dot == 0xd)
         return 1;
   }
   if (specialcode & 0x80)
   {
      if (dot == 0xe || dot == 0xf)
         return 1;
   }

   return 0;
}

//////////////////////////////////////////////////////////////////////////////

void Vdp2GetInterlaceInfo(int * start_line, int * line_increment)
{
   if (vdp2_interlace)
   {
      if (vdp2_is_odd_frame)
      {
         *start_line = 1;
      }
      else
      {
         *start_line = 0;
      }

      *line_increment = 2;
   }
   else
   {
      *start_line = 0;
      *line_increment = 1;
   }
}

//////////////////////////////////////////////////////////////////////////////

static void FASTCALL Vdp2DrawScroll(vdp2draw_struct *info, Vdp2* lines, Vdp2* regs, u8* ram, u8* color_ram, struct CellScrollData * cell_data)
{
   int i, j;
   int x, y;
   clipping_struct clip[2];
   u32 linewnd0addr, linewnd1addr;
   u32 line_window_base[2] = { 0 };
   screeninfo_struct sinfo;
   int scrolly;
   int *mosaic_y, *mosaic_x;
   clipping_struct colorcalcwindow[2];
   int start_line = 0, line_increment = 0;
   int bad_cycle = bad_cycle_setting[info->titan_which_layer];
   int charaddr, paladdr;
   int output_y = 0;
   u32 linescrollx_table[512] = { 0 };
   u32 linescrolly_table[512] = { 0 };
   float lineszoom_table[512] = { 0 };
   int num_vertical_cell_scroll_enabled = 0;

   SetupScreenVars(info, &sinfo, info->PlaneAddr, regs);

   scrolly = info->y;

   clip[0].xstart = clip[0].ystart = clip[0].xend = clip[0].yend = 0;
   clip[1].xstart = clip[1].ystart = clip[1].xend = clip[1].yend = 0;
   ReadWindowData(info->wctl, clip, regs);
   linewnd0addr = linewnd1addr = 0;
   ReadLineWindowData(&info->islinewindow, info->wctl, &linewnd0addr, &linewnd1addr,regs);
   line_window_base[0] = linewnd0addr;
   line_window_base[1] = linewnd1addr;
   /* color calculation window: in => no color calc, out => color calc */
   ReadWindowData(regs->WCTLD >> 8, colorcalcwindow, regs);
   {
	   static int tables_initialized = 0;
	   static int mosaic_table[16][1024];
	   if(!tables_initialized)
	   {
		   tables_initialized = 1;
			for(i=0;i<16;i++)
			{
				int m = i+1;
				for(j=0;j<1024;j++)
					mosaic_table[i][j] = j/m*m;
			}
	   }
	   mosaic_x = mosaic_table[info->mosaicxmask-1];
	   mosaic_y = mosaic_table[info->mosaicymask-1];
   }

   Vdp2GetInterlaceInfo(&start_line, &line_increment);

   // ReST-V: fast path setup (see the per-line fast loop below)
   u32 fast_pal[2048];
   int fast_width = 0;
   struct PixelData *fast_buf = NULL;
   int fast_ok = !info->isbitmap && info->colornumber <= 4;
   if (fast_ok)
   {
      fast_buf = TitanGetLayerBuffer(info->titan_which_layer, &fast_width);
      fast_ok = (fast_buf != NULL);
   }
   // ReST-V capture mode (GPU layer renderer): record the layer setup instead of drawing pixels
   RstvLayerCapture *cap = (rstv_capture_mode && info->titan_which_layer >= 0 && info->titan_which_layer < 5) ?
      &rstv_layer_capture[info->titan_which_layer] : NULL;
   if (cap)
   {
      memset(cap, 0, offsetof(RstvLayerCapture, row));
      memset(cap->row, 0, sizeof(cap->row));
      cap->captured = 1;
      cap->why = (info->colornumber > 4 ? 2 : 0) | (!info->isbitmap && info->cellw != 8 ? 4 : 0) |
                 (!info->isbitmap && info->mapwh != 2 ? 8 : 0);
      cap->isbitmap = info->isbitmap != 0;
      cap->bm_charaddr = info->charaddr;
      cap->bm_paladdr = info->paladdr;
      cap->bm_scf = info->specialcolorfunction;
      cap->ok = cap->why == 0;
      cap->colornumber = info->colornumber;
      cap->transparencyenable = info->transparencyenable;
      cap->cellw = info->cellw;
      cap->patterndatasize = info->patterndatasize;
      cap->patternwh = info->patternwh;
      cap->auxmode = info->auxmode;
      cap->supplementdata = info->supplementdata;
      cap->pagewh_bits = info->pagewh_bits;
      cap->planew_bits = info->planew_bits;
      cap->patterndatasize_bits = info->patterndatasize_bits;
      cap->mapwh = info->mapwh;
      cap->planepixelwidth_bits = sinfo.planepixelwidth_bits;
      cap->planepixelheight_bits = sinfo.planepixelheight_bits;
      cap->xmask = sinfo.xmask;
      cap->ymask = sinfo.ymask;
      cap->charaddr_mask_3fff = !(regs->VRSIZE & 0x8000);
      cap->coloroffset = info->coloroffset;
      cap->alpha = info->alpha;
      cap->specialcolormode = info->specialcolormode;
      cap->specialcode = info->specialcode;
      fast_ok = 0;
   }
   if (fast_ok && info->colornumber <= 2)
   {
      for (i = 0; i < 2048; i++)
         fast_pal[i] = Vdp2ColorRamGetColorSoft(i, color_ram);
   }

   if (regs->SCRCTL & 1)
      num_vertical_cell_scroll_enabled++;
   if (regs->SCRCTL & 0x100)
      num_vertical_cell_scroll_enabled++;

   //pre-generate line scroll tables
   for (j = start_line; j < vdp2height; j++)
   {
      if (info->islinescroll)
      {
         //line scroll interval bit
         int need_increment = ((j != 0) && (((j + 1) % info->lineinc) == 0));

         //horizontal line scroll
         if (info->islinescroll & 0x1)
         {
            linescrollx_table[j] = (T1ReadLong(ram, info->linescrolltbl) >> 16) & 0x7FF;
            if (need_increment)
               info->linescrolltbl += 4;
         }

         //vertical line scroll
         if (info->islinescroll & 0x2)
         {
            linescrolly_table[j] = ((T1ReadWord(ram, info->linescrolltbl) & 0x7FF)) + scrolly;
            if (need_increment)
               info->linescrolltbl += 4;
            y = info->y;
         }

         //line zoom
         if (info->islinescroll & 0x4)
         {
            lineszoom_table[j] = (T1ReadLong(ram, info->linescrolltbl) & 0x7FF00) / (float)65536.0;
            if (need_increment)
               info->linescrolltbl += 4;
         }
      }
   }

   /* ReST-V: last capture result kind (-1 none, 0 disabled, 1 clipped, 2 row) for repeating identical lines */
   int cap_last = -1;
   /* ReST-V: rectangle windows have a vertical range, so a line with the same registers may still clip differently
      (thuntk hat game: NBG1's window-0 hole over the star was lost) - no row reuse when a window is on */
   const int cap_rep = cap && rstv_cap_same && !info->islinescroll && !info->isverticalscroll && !info->islinewindow &&
      (info->wctl & 0x2a) == 0 && !vdp2_interlace && lines == rstv_line_same_src;
   for (j = start_line; j < vdp2height; j += line_increment)
   {
      int Y;
      int linescrollx = 0;
      // precalculate the coordinate for the line(it's faster) and do line
      // scroll
      if (info->islinescroll)
      {
         //horizontal line scroll
         if (info->islinescroll & 0x1)
         {
            linescrollx = linescrollx_table[j];
         }

         //vertical line scroll
         if (info->islinescroll & 0x2)
         {
            info->y = linescrolly_table[j];
            y = info->y;
         }
         else
            //y = info->y+((int)(info->coordincy *(float)(info->mosaicymask > 1 ? (j / info->mosaicymask * info->mosaicymask) : j)));
			y = info->y + info->coordincy*mosaic_y[j];

         //line zoom
         if (info->islinescroll & 0x4)
         {
            info->coordincx = lineszoom_table[j];
         }
      }
      else
         //y = info->y+((int)(info->coordincy *(float)(info->mosaicymask > 1 ? (j / info->mosaicymask * info->mosaicymask) : j)));
		 y = info->y + info->coordincy*mosaic_y[j];

      if (vdp2_interlace)
      {
         linewnd0addr = line_window_base[0] + (j * 4);
         linewnd1addr = line_window_base[1] + (j * 4);
      }

      // if line window is enabled, adjust clipping values
      ReadLineWindowClip(info->islinewindow, clip, &linewnd0addr, &linewnd1addr, ram, regs);
      y &= sinfo.ymask;

      if (info->isverticalscroll && (!vdp2_x_hires))//seems to be ignored in hi res
      {
         // this is *wrong*, vertical scroll use a different value per cell
         // info->verticalscrolltbl should be incremented by info->verticalscrollinc
         // each time there's a cell change and reseted at the end of the line...
         // or something like that :)
         u32 scroll_value = 0;
         int y_value = 0;
         
         if (vdp2_interlace)
            y_value = j / 2;
         else
            y_value = j;

         if (num_vertical_cell_scroll_enabled == 1)
         {
            scroll_value = cell_data[y_value].data[0] >> 16;
         }
         else
         {
            if (info->titan_which_layer == TITAN_NBG0)
               scroll_value = cell_data[y_value].data[0] >> 16;//reload cell data per line for sonic 2, 2 player mode
            else if (info->titan_which_layer == TITAN_NBG1)
               scroll_value = cell_data[y_value].data[1] >> 16;
         }

         y += scroll_value;
         y &= 0x1FF;
      }

      Y=y;

      if (cap_rep && cap_last >= 0 && j > start_line && j < 512 && rstv_line_same[j])
      {
         if (cap_last == 0)
            continue;
         if (cap_last == 1)
         {
            output_y++;
            continue;
         }
         if (output_y < RSTV_CAP_ROWS && output_y > 0)
         {
            cap->row[output_y] = cap->row[output_y - 1];
            cap->row[output_y].y = (u16)Y;
         }
         else
         {
            cap->ok = 0;
            cap->why |= 32;
         }
         cap->nrows = ++output_y;
         continue;
      }

      if (vdp2_interlace)
         info->LoadLineParams(info, &sinfo, j / 2, lines);
      else
         info->LoadLineParams(info, &sinfo, j, lines);

      if (!info->enable)
      {
         cap_last = 0;
         continue;
      }

      // ReST-V: hoist per-line invariants out of the pixel loop
      const int unit_x = (info->coordincx == 1.0f && info->mosaicxmask <= 1);
      const int wnd_off = (info->wctl & 0x2a) == 0;
      if (wnd_off && (info->wctl & 0x80))
      {
         cap_last = 1;
         output_y++;
         continue; // every pixel clipped
      }
      const int ccwnd_off = ((regs->WCTLD >> 8) & 0x2a) == 0;
      const int ccwnd_val = ((regs->WCTLD >> 8) & 0x80) ? 0 : 1;

      if (cap)
      {
         /* ReST-V: a constant horizontal zoom (no line zoom) is drawn by the GPU pixel path (ADR-037). Horizontal mosaic
            is screen space (output pixel i shows pixel i / m * m): the layer is drawn without it and the compose shader
            snaps x (ADR-129). Vertical mosaic is already in the row Y */
         const int zoom_ok = info->coordincx >= 0.0f;
         if (info->mosaicxmask > 1)
            cap->mosx = info->mosaicxmask;
         if (zoom_ok && info->coordincx != 1.0f)
            cap->zx = info->coordincx;
         /* ReST-V (ADR-040): window 0/1 (no sprite window) become up to 2 visible runs per line, drawn as quads */
         int win_ok = 0, nrun = 0, run0[2] = { 0, 0 }, run1[2] = { 0, 0 };
         if (!wnd_off && !(info->wctl & 0x20))
         {
            int cand[6], nc = 0, a, b;
            cand[nc++] = 0;
            cand[nc++] = vdp2width;
            cand[nc++] = clip[0].xstart; cand[nc++] = clip[0].xend + 1;
            cand[nc++] = clip[1].xstart; cand[nc++] = clip[1].xend + 1;
            for (a = 0; a < nc; a++)
            {
               if (cand[a] < 0) cand[a] = 0;
               if (cand[a] > vdp2width) cand[a] = vdp2width;
            }
            for (a = 1; a < nc; a++) /* sort */
               for (b = a; b > 0 && cand[b - 1] > cand[b]; b--) { const int t = cand[b]; cand[b] = cand[b - 1]; cand[b - 1] = t; }
            win_ok = 1;
            for (a = 0; a + 1 < nc && win_ok; a++)
            {
               if (cand[a] == cand[a + 1])
                  continue;
               if (TestBothWindow(info->wctl, clip, cand[a], j))
               {
                  if (nrun > 0 && run1[nrun - 1] == cand[a] - 1)
                     run1[nrun - 1] = cand[a + 1] - 1;
                  else if (nrun < 2)
                  {
                     run0[nrun] = cand[a];
                     run1[nrun] = cand[a + 1] - 1;
                     nrun++;
                  }
                  else
                     win_ok = 0;
               }
            }
         }
         /* ReST-V (ADR-112): the colour calculation window is applied by the GPU compose shader (rectangles; the sprite
            window case composes on the CPU), so it no longer keeps the layer off the GPU */
         if (!((wnd_off || win_ok) && (unit_x || zoom_ok) && !bad_cycle && info->specialprimode != 2))
         {
            cap->ok = 0;
            cap->why |= 16 | (!wnd_off ? 0x100 : 0) | (!(unit_x || zoom_ok) ? 0x200 : 0) | (bad_cycle ? 0x400 : 0) |
                        (!(unit_x || zoom_ok) && info->mosaicxmask > 1 ? 0x20000 : 0) |

                        (info->specialprimode == 2 ? 0x800 : 0);
         }
         if (output_y < RSTV_CAP_ROWS)
         {
            RstvCapRow *r = &cap->row[output_y];
            r->enable = 1;
            r->priority = (u8)((info->priority & 0x7) | ((info->specialprimode & 0x3) << 4));
            r->x = (u16)info->x;
            r->lsx = (u16)linescrollx;
            r->y = (u16)Y;
            r->planetbl[0] = sinfo.planetbl[0];
            r->planetbl[1] = sinfo.planetbl[1];
            r->planetbl[2] = sinfo.planetbl[2];
            r->planetbl[3] = sinfo.planetbl[3];
            r->zx = info->coordincx;
            r->nwin = 0;
            if (win_ok)
            {
               if (nrun == 0)
                  r->enable = 0; /* every pixel clipped */
               else
               {
                  int k;
                  r->nwin = (u8)nrun;
                  for (k = 0; k < nrun; k++)
                  {
                     r->wx0[k] = (u16)run0[k];
                     r->wx1[k] = (u16)run1[k];
                  }
                  cap->has_win = 1;
               }
            }
         }
         else
         {
            cap->ok = 0;
            cap->why |= 32;
         }
         output_y++;
         cap->nrows = output_y;
         cap_last = 2;
         continue;
      }

      // ReST-V fast path: tile layer without window / zoom / mosaic / bad-cycle / per-pixel priority.
      // Same results as the generic loop below (palette pre-converted, Titan buffer written directly).
      if (fast_ok && wnd_off && unit_x && !bad_cycle && info->specialprimode != 2 && ccwnd_off)
      {
         const int prio = info->priority;
         if (prio != 0)
         {
            struct PixelData *row = fast_buf + output_y * fast_width;
            const int cn = info->colornumber;
            const int transp = info->transparencyenable;
            const int cellw = info->cellw;
            const u32 cofs = info->coloroffset;
            const int alpha_mode = !ccwnd_val ? 0 : (info->specialcolormode == 0 ? 1 : 2);
            const u8 alpha_const = !ccwnd_val ? 0x3F : info->alpha;
            const int do_offset = (info->PostPixelFetchCalc == &DoColorOffset);
            const u8 linescreen = info->linescreen;
            const u8 sh_type = info->titan_shadow_type;
            const u8 sh_en = info->titan_shadow_enabled;

            for (i = 0; i < vdp2width; i++)
            {
               u32 color, dot;
               x = (info->x + i) & sinfo.xmask;
               if (linescrollx) {
                  x += linescrollx;
                  x &= sinfo.xmask; /* ReST-V (ADR-099): the map width, not 1024 (thuntk pushman train, 2x1 planes) */
               }
               y = Y;
               Vdp2MapCalcXY(info, &x, &y, &sinfo, regs, ram, 0);
               charaddr = info->charaddr;
               paladdr = info->paladdr;

               switch (cn)
               {
               case 0:
                  dot = T1ReadByte(ram, ((charaddr + ((y * cellw) + x) / 2) & 0x7FFFF));
                  if (!(x & 0x1)) dot >>= 4;
                  if (!(dot & 0xF) && transp) continue;
                  color = fast_pal[(cofs + (paladdr | (dot & 0xF))) & 0x7FF];
                  break;
               case 1:
                  dot = T1ReadByte(ram, ((charaddr + (y * cellw) + x) & 0x7FFFF));
                  if (!(dot & 0xFF) && transp) continue;
                  color = fast_pal[(cofs + (paladdr | (dot & 0xFF))) & 0x7FF];
                  break;
               case 2:
                  dot = T1ReadWord(ram, ((charaddr + ((y * cellw) + x) * 2) & 0x7FFFF));
                  if ((dot == 0) && transp) continue;
                  color = fast_pal[(cofs + dot) & 0x7FF];
                  break;
               case 3:
                  dot = T1ReadWord(ram, ((charaddr + ((y * cellw) + x) * 2) & 0x7FFFF));
                  if (!(dot & 0x8000) && transp) continue;
                  color = COLSAT2YAB16(0, dot);
                  break;
               default:
                  dot = T1ReadLong(ram, ((charaddr + ((y * cellw) + x) * 4) & 0x7FFFF));
                  if (!(dot & 0x80000000) && transp) continue;
                  color = COLSAT2YAB32(0, dot);
                  break;
               }

               {
                  const u8 alpha = (alpha_mode == 2) ? GetAlpha(info, color, dot) : alpha_const;
                  u32 pixel = COLSAT2YAB32(alpha, color);
                  if (do_offset)
                     pixel = DoColorOffset(info, pixel);
                  row[i].pixel = pixel;
                  row[i].priority = prio;
                  row[i].linescreen = linescreen;
                  row[i].shadow_enabled = sh_en;
                  row[i].shadow_type = sh_type;
               }
            }
         }
         output_y++;
         continue;
      }

      for (i = 0; i < vdp2width; i++)
      {
         u32 color, dot;
         /* I'm really not sure about this... but I think the way we handle
         high resolution gets in the way with window process. I may be wrong...
         This was added for Cotton Boomerang */
			int priority;

         // See if screen position is clipped, if it isn't, continue
         if (!wnd_off && !TestBothWindow(info->wctl, clip, i, j))
         {
            continue;
         }

         //x = info->x+((int)(info->coordincx*(float)((info->mosaicxmask > 1) ? (i / info->mosaicxmask * info->mosaicxmask) : i)));
		 if (unit_x)
		    x = info->x + i;
		 else
		    x = info->x + mosaic_x[i]*info->coordincx;
         x &= sinfo.xmask;

         if (linescrollx) {
            x += linescrollx;
            x &= sinfo.xmask; /* ReST-V (ADR-099): the map width, not 1024 */
         }

         // Fetch Pixel, if it isn't transparent, continue
         if (!info->isbitmap)
         {
            // Tile
            y=Y;
            Vdp2MapCalcXY(info, &x, &y, &sinfo, regs, ram, bad_cycle);
         }

         if (!bad_cycle)
         {
            charaddr = info->charaddr;
            paladdr = info->paladdr;
         }
         else
         {
            charaddr = info->pipe[0].charaddr;
            paladdr = info->pipe[0].paladdr;
         }

         if (!Vdp2FetchPixel(info, x, y, &color, &dot, ram, charaddr, paladdr,color_ram))
         {
            continue;
         }

         priority = info->priority;

         //per-pixel priority is on
         if (info->specialprimode == 2)
         {
            priority = info->priority & 0xE;

            if (info->specialfunction & 1)
            {
               if (PixelIsSpecialPriority(info->specialcode,dot))
               {
                  priority |= 1;
               }
            }
         }

         // Apply color offset and color calculation/special color calculation
         // and then continue.
         // We almost need to know well ahead of time what the top
         // and second pixel is in order to work this.

         {
            u8 alpha;
            /* if we're in the valid area of the color calculation window, don't do color calculation */
            if (!(ccwnd_off ? ccwnd_val : TestBothWindow(regs->WCTLD >> 8, colorcalcwindow, i, j)))
               alpha = 0x3F;
            else
               alpha = GetAlpha(info, color, dot);

            TitanPutPixel(priority, i, output_y, info->PostPixelFetchCalc(info, COLSAT2YAB32(alpha, color)), info->linescreen, info);
         }
      }
      output_y++;
   }    
}

//////////////////////////////////////////////////////////////////////////////

void Rbg0PutHiresPixel(vdp2draw_struct *info, u32 color, u32 dot, int i, int j)
{
   u32 pixel = info->PostPixelFetchCalc(info, COLSAT2YAB32(GetAlpha(info, color, dot), color));
   int x_pos = i * 2;
   TitanPutPixel(info->priority, x_pos, j, pixel, info->linescreen, info);
   TitanPutPixel(info->priority, x_pos + 1, j, pixel, info->linescreen, info);
}

//////////////////////////////////////////////////////////////////////////////

void Rbg0PutPixel(vdp2draw_struct *info, u32 color, u32 dot, int i, int j)
{
   if (vdp2_x_hires)
   {
      Rbg0PutHiresPixel(info, color, dot, i, j);
   }
   else
      TitanPutPixel(info->priority, i, j, info->PostPixelFetchCalc(info, COLSAT2YAB32(GetAlpha(info, color, dot), color)), info->linescreen, info);
}

//////////////////////////////////////////////////////////////////////////////

int CheckBanks(Vdp2* regs, int compare_value)
{
   if (((regs->RAMCTL >> 0) & 3) == compare_value)//a0
      return 0;
   if (((regs->RAMCTL >> 2) & 3) == compare_value)//a1
      return 0;
   if (((regs->RAMCTL >> 4) & 3) == compare_value)//b0
      return 0;
   if (((regs->RAMCTL >> 6) & 3) == compare_value)//b1
      return 0;

   return 1;//no setting present
}

int Rbg0CheckRam(Vdp2* regs)
{
   if (((regs->RAMCTL >> 8) & 3) == 3)//both banks are divided
   {
      //ignore delta kax if the coefficient table
      //bank is unspecified
      if (CheckBanks(regs, 1))
         return 1;
   }

   return 0;
}

/* ReST-V: record RBG0 for the GPU rotation renderer (ADR-016) instead of drawing it. Returns 1 when recorded.
   Supported: tile mode, parameter A or B only, line coefficients (or none) with modes 0-2, no windows,
   no rotation line colour, 16/256 palette colours or RGB555. Everything else is drawn by the CPU as before. */
int rstv_rbg_unrot_on = 1; /* ADR-083; 0: ux0:data/restv/norbgunrot */
volatile int rstv_rbg_unrot_seen; /* ADR-083 diagnostics: frames captured as unrotated (the frontend can take a picture) */
static int rstv_capture_rotation(vdp2draw_struct *info, vdp2rotationparameterfp_struct *parameter, Vdp2 *regs, u8 *ram)
{
   vdp2rotationparameterfp_struct *p = &parameter[info->rotatenum];
   vdp2rotationparameterfp_struct *p2 = NULL; /* ADR-037: parameter B for RPMD 2 (line coefficient MSB selects it) */
   fixed32 xmul2 = 0, ymul2 = 0, C2 = 0, F2 = 0;
   u32 coefy2 = 0, rcoefy2 = 0;
   RstvRotCapture *c = &rstv_rot_capture;
   screeninfo_struct sinfo;
   fixed32 xmul, ymul, C, F;
   u32 coefy = 0, rcoefy = 0;
   int j;

   if (Rbg0CheckRam(regs) && p->coefenab && p->coefmode == 0)
      p->deltaKAx = 0;
   /* ADR-121: a per-dot coefficient step that moves less than one table entry over the whole line (znpwfvt: -64/65536,
      0.34 entries over 352 dots, flickering with 0 from frame to frame) is taken as 0 - the GPU keeps one coefficient
      a line. At most one entry off on part of a line; the CPU path used to take ~86 ms for these frames */
   if (p->coefenab && p->deltaKAx != 0 && (p->deltaKAx < 0 ? -p->deltaKAx : p->deltaKAx) * vdp2width < 65536)
      p->deltaKAx = 0;
   rstv_rot_dbg[0] = p->deltaKAx; rstv_rot_dbg[1] = p->deltaKAst; rstv_rot_dbg[2] = p->coefmode; rstv_rot_dbg[3] = (int)p->KAst;
   rstv_rot_why = (info->isbitmap && !(info->colornumber <= 1 || info->colornumber == 3) ? 1 : 0) |
      ((regs->RPMD & 3) == 3 && (regs->WCTLD & 0x20) ? 0x800 : 0) | ((regs->RPMD & 3) == 2 && !(p->coefenab && p->deltaKAx == 0) ? 2 : 0) |
      ((info->wctl & 0x20) ? 4 : 0) |
      ((regs->KTCTL & 0x1010) ? 8 : 0) | (info->specialprimode == 2 ? 16 : 0) | (vdp2_x_hires ? 32 : 0) |
      (!(info->colornumber <= 1 || info->colornumber == 3) ? 128 : 0) |
      (p->coefenab && p->deltaKAx != 0 ? 256 : 0) | (p->coefenab && p->coefmode > 2 ? 512 : 0);
   if (rstv_rot_why)
      return 0;
   /* ADR-083: no coefficients and no rotation (IsScreenRotatedFP is true when the plane is NOT rotated):
      Vdp2DrawRotationFP draws it as a scaled scroll screen, x = info->x + i * kx, y = info->y + j * ky (truncated, wrapped),
      parameter A only, layer windows only, no screen-over. The GPU rows take exactly that */
   const int unrot = !p->coefenab && IsScreenRotatedFP(p);
   if (unrot && !rstv_rbg_unrot_on)
   {
      rstv_rot_why = 1024;
      return 0;
   }
   if (unrot)
      rstv_rbg_unrot_seen++;
   const u32 unrot_x = unrot ? touint(mulfixed(p->kx, (p->Xst - p->Px)) + p->Px + p->Mx) : 0;
   const u32 unrot_y = unrot ? touint(mulfixed(p->ky, (p->Yst - p->Py)) + p->Py + p->My) : 0;

   GenerateRotatedVarFP(p, &xmul, &ymul, &C, &F);
   CalculateRotationValuesFP(p);
   SetupScreenVars(info, &sinfo, p->PlaneAddr, regs);
   clipping_struct rpwindow[2];
   int isrplinewindow = 0;
   u32 rplinewnd0addr = 0, rplinewnd1addr = 0;
   const int rpwin = (regs->RPMD & 3) == 3 && !unrot;
   if (rpwin)
   {
      ReadWindowData(regs->WCTLD, rpwindow, regs);
      ReadLineWindowData(&isrplinewindow, regs->WCTLD, &rplinewnd0addr, &rplinewnd1addr, regs);
   }
   c->two_maps = 0;
   if ((regs->RPMD & 3) >= 2 && !unrot)
   {
      screeninfo_struct sinfo2;
      p2 = &parameter[1 - info->rotatenum];
      Vdp2ReadRotationTableFP(1 - info->rotatenum, p2, regs, ram);
      if (Rbg0CheckRam(regs) && p2->coefenab && p2->coefmode == 0)
         p2->deltaKAx = 0;
      if (p2->coefenab && p2->deltaKAx != 0 && (p2->deltaKAx < 0 ? -p2->deltaKAx : p2->deltaKAx) * vdp2width < 65536)
         p2->deltaKAx = 0; /* ADR-121 */
      GenerateRotatedVarFP(p2, &xmul2, &ymul2, &C2, &F2);
      CalculateRotationValuesFP(p2);
      SetupScreenVars(info, &sinfo2, p2->PlaneAddr, regs);
      /* the GPU keeps one map: B must use the same map layout and screen-over mode, line coefficients only */
      rstv_rot_why = ((p2->coefenab && (p2->deltaKAx != 0 || p2->coefmode > 2)) ? 0x1000 : 0) |
         (p2->screenover != p->screenover && info->isbitmap ? 0x2000 : 0) |
         (sinfo2.xmask != sinfo.xmask || sinfo2.ymask != sinfo.ymask ? 0x4000 : 0);
      if (rstv_rot_why)
         return 0;
      c->screenover_b = p2->screenover;
      /* ADR-071: B with another screen-over mode goes to the separate B list (drawn with its own mode) */
      if (!info->isbitmap && (p2->screenover != p->screenover || memcmp(sinfo2.planetbl, sinfo.planetbl, sizeof(sinfo.planetbl))))
      {
         /* ADR-046: B has its own map: the GPU draws B pieces with a second page table */
         int k;
         c->two_maps = 1;
         for (k = 0; k < 16; k++)
            c->planetbl_b[k] = sinfo2.planetbl[k];
      }
   }

   c->captured = 1;
   c->colornumber = info->colornumber;
   c->transparencyenable = info->transparencyenable;
   c->patterndatasize = info->patterndatasize;
   c->patternwh = info->patternwh;
   c->auxmode = info->auxmode;
   c->supplementdata = info->supplementdata;
   c->specialprimode = info->specialprimode;
   c->priority = info->priority;
   c->charaddr_mask_3fff = !(regs->VRSIZE & 0x8000);
   c->coloroffset = info->coloroffset;
   c->alpha = info->alpha;
   c->specialcolormode = info->specialcolormode;
   c->specialcode = info->specialcode;
   c->planew = info->planew;
   c->planeh = info->planeh;
   c->mapw = sinfo.xmask + 1;
   c->maph = sinfo.ymask + 1;
   c->screenover = unrot ? 0 : p->screenover;
   c->isbitmap = info->isbitmap != 0;
   c->charaddr = info->charaddr;
   c->paladdr = info->paladdr;
   c->scf = info->specialcolorfunction;
   for (j = 0; j < 16; j++)
      c->planetbl[j] = sinfo.planetbl[j];
   c->nrows = rbg0height < RSTV_CAP_ROWS ? rbg0height : RSTV_CAP_ROWS;
   /* ADR-063: layer window 0/1 (RPMD 0/1, no sprite window) become up to 4 visible runs per line (parameter A) */
   const int lwin = (info->wctl & 0x0A) != 0;
   clipping_struct lclip[2];
   u32 lwnd0 = 0, lwnd1 = 0;
   int islw = 0;
   if (lwin)
   {
      lclip[0].xstart = lclip[0].ystart = lclip[0].xend = lclip[0].yend = 0;
      lclip[1].xstart = lclip[1].ystart = lclip[1].xend = lclip[1].yend = 0;
      ReadWindowData(info->wctl, lclip, regs);
      ReadLineWindowData(&islw, info->wctl, &lwnd0, &lwnd1, regs);
   }
   for (j = 0; j < c->nrows; j++)
   {
      RstvRotLine *r = &c->row[j];
      if (p->coefenab)
         Vdp2ReadCoefficientFP(p, p->coeftbladdr + (coefy + touint(rcoefy)) * p->coefdatasize, ram);
      if (p2 && p2->coefenab)
         Vdp2ReadCoefficientFP(p2, p2->coeftbladdr + (coefy2 + touint(rcoefy2)) * p2->coefdatasize, ram);
      r->enable = !(p->coefenab && p->msb);
      r->nseg = 0;
      if (lwin)
         ReadLineWindowClip(islw, lclip, &lwnd0, &lwnd1, ram, regs);
      if (rpwin)
      {
         /* parameter window: runs of A (window test 1) and B (window test 0), Vdp2DrawRotationFP */
         const fixed32 Xsp = mulfixed(p2->A, xmul2) + mulfixed(p2->B, ymul2) + C2;
         const fixed32 Ysp = mulfixed(p2->D, xmul2) + mulfixed(p2->E, ymul2) + F2;
         int i, prev = -1, n = 0;
         ReadLineWindowClip(isrplinewindow, rpwindow, &rplinewnd0addr, &rplinewnd1addr, ram, regs);
         r->xb0 = (float)((double)p2->kx * (double)Xsp / 4294967296.0 + (double)p2->Xp / 65536.0);
         r->dxb = (float)((double)p2->kx * (double)p2->dX / 4294967296.0);
         r->yb0 = (float)((double)p2->ky * (double)Ysp / 4294967296.0 + (double)p2->Yp / 65536.0);
         r->dyb = (float)((double)p2->ky * (double)p2->dY / 4294967296.0);
         r->enb = !(p2->coefenab && p2->msb);
         {
            /* the window results only change at window edges: test the start of each piece between candidates.
               ADR-063: pieces outside the layer window are not drawn */
            int cand[10], nc = 0, a, bb;
            cand[nc++] = 0;
            cand[nc++] = rbg0width;
            cand[nc++] = rpwindow[0].xstart; cand[nc++] = rpwindow[0].xend + 1;
            cand[nc++] = rpwindow[1].xstart; cand[nc++] = rpwindow[1].xend + 1;
            if (lwin)
            {
               cand[nc++] = lclip[0].xstart; cand[nc++] = lclip[0].xend + 1;
               cand[nc++] = lclip[1].xstart; cand[nc++] = lclip[1].xend + 1;
            }
            for (a = 0; a < nc; a++)
            {
               if (cand[a] < 0) cand[a] = 0;
               if (cand[a] > rbg0width) cand[a] = rbg0width;
            }
            for (a = 1; a < nc; a++)
               for (bb = a; bb > 0 && cand[bb - 1] > cand[bb]; bb--) { const int t = cand[bb]; cand[bb] = cand[bb - 1]; cand[bb - 1] = t; }
            for (a = 0; a + 1 < nc; a++)
            {
               if (cand[a] == cand[a + 1])
                  continue;
               i = cand[a];
               if (lwin && !TestBothWindow(info->wctl, lclip, i, j))
               {
                  prev = -1; /* hidden piece: the next visible one starts a new run */
                  continue;
               }
               const int b = !TestBothWindow(regs->WCTLD, rpwindow, i, j);
               if (b == prev)
                  r->ex[n - 1] = (u16)(cand[a + 1] - 1);
               else
               {
                  if (n == 4)
                  {
                     rstv_rot_why = 0x10000; /* too many runs */
                     c->captured = 0;
                     return 0;
                  }
                  r->sp[n] = (u8)b;
                  r->sx[n] = (u16)i;
                  r->ex[n] = (u16)(cand[a + 1] - 1);
                  n++;
                  prev = b;
               }
            }
         }
         r->nseg = (u8)n;
         if (n == 0)
            r->enable = r->enb = 0; /* every pixel outside the layer window */
      }
      int lineb = 0; /* ADR-100: this line is one B piece on the second map (the window runs keep it) */
      if (!rpwin && !r->enable && p2 && !(p2->coefenab && p2->msb))
      {
         /* RPMD 2: this line uses parameter B (Vdp2DrawRotationFP) */
         const fixed32 Xsp = mulfixed(p2->A, xmul2) + mulfixed(p2->B, ymul2) + C2;
         const fixed32 Ysp = mulfixed(p2->D, xmul2) + mulfixed(p2->E, ymul2) + F2;
         r->enable = 1;
         r->x0 = (float)((double)p2->kx * (double)Xsp / 4294967296.0 + (double)p2->Xp / 65536.0);
         r->dx = (float)((double)p2->kx * (double)p2->dX / 4294967296.0);
         r->y0 = (float)((double)p2->ky * (double)Ysp / 4294967296.0 + (double)p2->Yp / 65536.0);
         r->dy = (float)((double)p2->ky * (double)p2->dY / 4294967296.0);
         if (c->two_maps)
         {
            /* the whole line is one B piece (second map) */
            r->nseg = 1;
            r->sp[0] = 1;
            r->sx[0] = 0;
            r->ex[0] = (u16)(rbg0width - 1);
            r->xb0 = r->x0; r->dxb = r->dx; r->yb0 = r->y0; r->dyb = r->dy;
            r->enb = 1;
            lineb = 1;
         }
      }
      else if (unrot)
      {
         /* ADR-083: Vdp2DrawScroll: y = info->y + coordincy * j, x = info->x + coordincx * i */
         r->x0 = (float)unrot_x;
         r->dx = tofloat(p->kx);
         r->y0 = (float)unrot_y + tofloat(p->ky) * (float)j;
         r->dy = 0.0f;
      }
      else
      {
         const fixed32 Xsp = mulfixed(p->A, xmul) + mulfixed(p->B, ymul) + C;
         const fixed32 Ysp = mulfixed(p->D, xmul) + mulfixed(p->E, ymul) + F;
         /* x(i) = (kx * (Xsp + dX * i) / 2^16 + Xp) / 2^16 (GenerateRotatedXPosFP without the inner rounding) */
         r->x0 = (float)((double)p->kx * (double)Xsp / 4294967296.0 + (double)p->Xp / 65536.0);
         r->dx = (float)((double)p->kx * (double)p->dX / 4294967296.0);
         r->y0 = (float)((double)p->ky * (double)Ysp / 4294967296.0 + (double)p->Yp / 65536.0);
         r->dy = (float)((double)p->ky * (double)p->dY / 4294967296.0);
      }
      if (lwin && !rpwin)
      {
         /* the window result only changes at window edges (Vdp2DrawRotationFP: !TestBothWindow skips the pixel) */
         int cand[6], nc = 0, a, bb, n = 0, prev = 0;
         cand[nc++] = 0;
         cand[nc++] = rbg0width;
         cand[nc++] = lclip[0].xstart; cand[nc++] = lclip[0].xend + 1;
         cand[nc++] = lclip[1].xstart; cand[nc++] = lclip[1].xend + 1;
         for (a = 0; a < nc; a++)
         {
            if (cand[a] < 0) cand[a] = 0;
            if (cand[a] > rbg0width) cand[a] = rbg0width;
         }
         for (a = 1; a < nc; a++)
            for (bb = a; bb > 0 && cand[bb - 1] > cand[bb]; bb--) { const int t = cand[bb]; cand[bb] = cand[bb - 1]; cand[bb - 1] = t; }
         for (a = 0; a + 1 < nc; a++)
         {
            if (cand[a] == cand[a + 1])
               continue;
            const int vis = TestBothWindow(info->wctl, lclip, cand[a], j) != 0;
            if (vis && prev)
               r->ex[n - 1] = (u16)(cand[a + 1] - 1);
            else if (vis)
            {
               if (n == 4)
               {
                  rstv_rot_why = 0x10000; /* too many runs */
                  c->captured = 0;
                  return 0;
               }
               r->sp[n] = (u8)lineb; /* ADR-100: RPMD 2 lines on parameter B keep it inside the window runs */
               r->sx[n] = (u16)cand[a];
               r->ex[n] = (u16)(cand[a + 1] - 1);
               n++;
            }
            prev = vis;
         }
         if (n == 0)
            r->enable = 0;
         r->nseg = (u8)n;
      }
      xmul += p->deltaXst;
      ymul += p->deltaYst;
      if (p->coefenab)
      {
         coefy += toint(p->deltaKAst);
         rcoefy += decipart(p->deltaKAst);
      }
      if (p2)
      {
         xmul2 += p2->deltaXst;
         ymul2 += p2->deltaYst;
         if (p2->coefenab)
         {
            coefy2 += toint(p2->deltaKAst);
            rcoefy2 += decipart(p2->deltaKAst);
         }
      }
   }
   return 1;
}

static void FASTCALL Vdp2DrawRotationFP(vdp2draw_struct *info, vdp2rotationparameterfp_struct *parameter, Vdp2* lines, Vdp2* regs, u8* ram, u8* color_ram, struct CellScrollData * cell_data)
{
   int i, j;
   int x, y;
   screeninfo_struct sinfo;
   vdp2rotationparameterfp_struct *p=&parameter[info->rotatenum];
   clipping_struct clip[2];
   u32 linewnd0addr, linewnd1addr;

   clip[0].xstart = clip[0].ystart = clip[0].xend = clip[0].yend = 0;
   clip[1].xstart = clip[1].ystart = clip[1].xend = clip[1].yend = 0;
   ReadWindowData(info->wctl, clip, regs);
   linewnd0addr = linewnd1addr = 0;
   ReadLineWindowData(&info->islinewindow, info->wctl, &linewnd0addr, &linewnd1addr, regs);

   Vdp2ReadRotationTableFP(info->rotatenum, p, regs, ram);
   if (rstv_capture_mode && info->titan_which_layer == TITAN_RBG0 && rstv_capture_rotation(info, parameter, regs, ram))
   {
      rstv_rbg0_erase_pending = 0; /* the GPU draws it; the Titan buffer is not read */
      return;
   }
   if (info->titan_which_layer == TITAN_RBG0 && rstv_rbg0_erase_pending)
   {
      /* ReST-V: the CPU draws RBG0 this frame - erase its Titan buffer now (deferred, see VIDSoftVdp2DrawScreens) */
      TitanEraseLayer(TITAN_RBG0);
      rstv_rbg0_erase_pending = 0;
   }
#ifdef RSTV_VDP1_STATS
   {
      /* ReST-V harness: which RBG0 features are used (for the GPU rotation renderer) */
      extern unsigned long long rstv_rot_stats[16];
      rstv_rot_stats[0]++;
      if (!p->coefenab) rstv_rot_stats[IsScreenRotatedFP(p) ? 1 : 2]++;
      else {
         rstv_rot_stats[3]++;
         if (p->deltaKAx != 0) rstv_rot_stats[4]++;   /* per-dot coefficients */
         rstv_rot_stats[5 + (p->coefmode & 3)]++;      /* 5..8 coefficient mode */
         if ((regs->RPMD & 3) >= 2) rstv_rot_stats[9]++; /* parameter A/B switching */
      }
      if (info->isbitmap) rstv_rot_stats[10]++;
      if (p->screenover >= 2) rstv_rot_stats[11]++;
      if (info->wctl & 0x2A) rstv_rot_stats[12]++;     /* windows */
      if (info->colornumber > 1) rstv_rot_stats[13]++;
      if (info->linescreen) rstv_rot_stats[14]++;
      {
         static int shown;
         if (shown < 3 && getenv("RSTV_ROT_LOG")) {
            shown++;
            fprintf(stderr, "rbg: cn %d pds %d pwh %d aux %d cellw %d planew %d planeh %d pagewh_bits %d mapwh %d transp %d rotnum %d coefds %d coefmode %d kast %d\n",
               info->colornumber, info->patterndatasize, info->patternwh, info->auxmode, info->cellw, info->planew,
               info->planeh, info->pagewh_bits, info->mapwh, info->transparencyenable, info->rotatenum, p->coefdatasize,
               p->coefmode, (int)p->deltaKAst);
         }
      }
   }
#endif

   if (!p->coefenab)
   {
      fixed32 xmul, ymul, C, F;

      // Since coefficients aren't being used, we can simplify the drawing process
      if (IsScreenRotatedFP(p))
      {
         // No rotation
         info->x = touint(mulfixed(p->kx, (p->Xst - p->Px)) + p->Px + p->Mx);
         info->y = touint(mulfixed(p->ky, (p->Yst - p->Py)) + p->Py + p->My);
         info->coordincx = tofloat(p->kx);
         info->coordincy = tofloat(p->ky);
      }
      else
      {
         GenerateRotatedVarFP(p, &xmul, &ymul, &C, &F);

         // Do simple rotation
         CalculateRotationValuesFP(p);

         SetupScreenVars(info, &sinfo, info->PlaneAddr, regs);

         for (j = 0; j < vdp2height; j++)
         {
            info->LoadLineParams(info, &sinfo, j, lines);
            ReadLineWindowClip(info->islinewindow, clip, &linewnd0addr, &linewnd1addr, ram, regs);

            for (i = 0; i < rbg0width; i++)
            {
               u32 color, dot;

               if (!TestBothWindow(info->wctl, clip, i, j))
                  continue;

               x = GenerateRotatedXPosFP(p, i, xmul, ymul, C) & sinfo.xmask;
               y = GenerateRotatedYPosFP(p, i, xmul, ymul, F) & sinfo.ymask;

               // Convert coordinates into graphics
               if (!info->isbitmap)
               {
                  // Tile
                  Vdp2MapCalcXY(info, &x, &y, &sinfo, regs, ram,0);
               }
 
               // Fetch pixel
               if (!Vdp2FetchPixel(info, x, y, &color, &dot, ram, info->charaddr,info->paladdr, color_ram))
               {
                  continue;
               }

               Rbg0PutPixel(info, color, dot, i, j);
            }
            xmul += p->deltaXst;
            ymul += p->deltaYst;
         }

         return;
      }
   }
   else
   {
      fixed32 xmul, ymul, C, F;
      u32 coefx, coefy;
      u32 rcoefx, rcoefy;
      u32 lineAddr, lineColor, lineInc;
      u16 lineColorAddr;

      fixed32 xmul2, ymul2, C2, F2;
      u32 coefx2, coefy2;
      u32 rcoefx2, rcoefy2;
      screeninfo_struct sinfo2;
      vdp2rotationparameterfp_struct *p2 = NULL;

      clipping_struct rpwindow[2];
      int userpwindow = 0;
      int isrplinewindow = 0;
      u32 rplinewnd0addr, rplinewnd1addr;

      if ((regs->RPMD & 3) == 2)
         p2 = &parameter[1 - info->rotatenum];
      else if ((regs->RPMD & 3) == 3)
      {
         ReadWindowData(regs->WCTLD, rpwindow, regs);
         rplinewnd0addr = rplinewnd1addr = 0;
         ReadLineWindowData(&isrplinewindow, regs->WCTLD, &rplinewnd0addr, &rplinewnd1addr, regs);
         userpwindow = 1;
         p2 = &parameter[1 - info->rotatenum];
      }

      GenerateRotatedVarFP(p, &xmul, &ymul, &C, &F);

      // Rotation using Coefficient Tables(now this stuff just gets wacky. It
      // has to be done in software, no exceptions)
      CalculateRotationValuesFP(p);

      SetupScreenVars(info, &sinfo, p->PlaneAddr, regs);
      coefx = coefy = 0;
      rcoefx = rcoefy = 0;

      if (p2 != NULL)
      {
         Vdp2ReadRotationTableFP(1 - info->rotatenum, p2, regs, ram);
         GenerateRotatedVarFP(p2, &xmul2, &ymul2, &C2, &F2);
         CalculateRotationValuesFP(p2);
         SetupScreenVars(info, &sinfo2, p2->PlaneAddr, regs);
         coefx2 = coefy2 = 0;
         rcoefx2 = rcoefy2 = 0;
      }

      if (Rbg0CheckRam(regs))//sonic r / all star baseball 97
      {
         if (p->coefenab && p->coefmode == 0)
         {
            p->deltaKAx = 0;
         }

         if (p2 && p2->coefenab && p2->coefmode == 0)
         {
            p2->deltaKAx = 0;
         }
      }

      if (info->linescreen)
      {
         if ((info->rotatenum == 0) && (regs->KTCTL & 0x10))
            info->linescreen = 2;
         else if (regs->KTCTL & 0x1000)
            info->linescreen = 3;
         if (regs->VRSIZE & 0x8000)
            lineAddr = (regs->LCTA.all & 0x7FFFF) << 1;
         else
            lineAddr = (regs->LCTA.all & 0x3FFFF) << 1;

         lineInc = regs->LCTA.part.U & 0x8000 ? 2 : 0;
      }

      for (j = 0; j < rbg0height; j++)
      {
         if (p->deltaKAx == 0)
         {
            Vdp2ReadCoefficientFP(p,
                                  p->coeftbladdr +
                                  (coefy + touint(rcoefy)) *
                                  p->coefdatasize, ram);
         }
         if ((p2 != NULL) && p2->coefenab && (p2->deltaKAx == 0))
         {
            Vdp2ReadCoefficientFP(p2,
                                  p2->coeftbladdr +
                                  (coefy2 + touint(rcoefy2)) *
                                  p2->coefdatasize, ram);
         }

         if (info->linescreen > 1)
         {
            lineColorAddr = (T1ReadWord(ram, lineAddr) & 0x780) | p->linescreen;
            lineColor = Vdp2ColorRamGetColor(lineColorAddr, (int)color_ram);
            lineAddr += lineInc;
            TitanPutLineHLine(info->linescreen, j, COLSAT2YAB32(0x3F, lineColor));
         }

         info->LoadLineParams(info, &sinfo, j, lines);
         ReadLineWindowClip(info->islinewindow, clip, &linewnd0addr, &linewnd1addr, ram, regs);

         if (userpwindow)
            ReadLineWindowClip(isrplinewindow, rpwindow, &rplinewnd0addr, &rplinewnd1addr, ram, regs);

         for (i = 0; i < rbg0width; i++)
         {
            u32 color, dot;

            if (p->deltaKAx != 0)
            {
               Vdp2ReadCoefficientFP(p,
                                     p->coeftbladdr +
                                     (coefy + coefx + toint(rcoefx + rcoefy)) *
                                     p->coefdatasize, ram);
               coefx += toint(p->deltaKAx);
               rcoefx += decipart(p->deltaKAx);
            }
            if ((p2 != NULL) && p2->coefenab && (p2->deltaKAx != 0))
            {
               Vdp2ReadCoefficientFP(p2,
                                     p2->coeftbladdr +
                                     (coefy2 + coefx2 + toint(rcoefx2 + rcoefy2)) *
                                     p2->coefdatasize, ram);
               coefx2 += toint(p2->deltaKAx);
               rcoefx2 += decipart(p2->deltaKAx);
            }

            if (!TestBothWindow(info->wctl, clip, i, j))
               continue;

            if (((! userpwindow) && p->msb) || (userpwindow && (! TestBothWindow(regs->WCTLD, rpwindow, i, j))))
            {
               if ((p2 == NULL) || (p2->coefenab && p2->msb)) continue;

               x = GenerateRotatedXPosFP(p2, i, xmul2, ymul2, C2);
               y = GenerateRotatedYPosFP(p2, i, xmul2, ymul2, F2);

               switch(p2->screenover) {
                  case 0:
                     x &= sinfo2.xmask;
                     y &= sinfo2.ymask;
                     break;
                  case 1:
                     VDP2LOG("Screen-over mode 1 not implemented");
                     x &= sinfo2.xmask;
                     y &= sinfo2.ymask;
                     break;
                  case 2:
                     if ((x > sinfo2.xmask) || (y > sinfo2.ymask)) continue;
                     break;
                  case 3:
                     if ((x > 512) || (y > 512)) continue;
               }

               // Convert coordinates into graphics
               if (!info->isbitmap)
               {
                  // Tile
                  Vdp2MapCalcXY(info, &x, &y, &sinfo2, regs, ram, 0);
               }
            }
            else if (p->msb) continue;
            else
            {
               x = GenerateRotatedXPosFP(p, i, xmul, ymul, C);
               y = GenerateRotatedYPosFP(p, i, xmul, ymul, F);

               switch(p->screenover) {
                  case 0:
                     x &= sinfo.xmask;
                     y &= sinfo.ymask;
                     break;
                  case 1:
                     VDP2LOG("Screen-over mode 1 not implemented");
                     x &= sinfo.xmask;
                     y &= sinfo.ymask;
                     break;
                  case 2:
                     if ((x > sinfo.xmask) || (y > sinfo.ymask)) continue;
                     break;
                  case 3:
                     if ((x > 512) || (y > 512)) continue;
               }

               // Convert coordinates into graphics
               if (!info->isbitmap)
               {
                  // Tile
                  Vdp2MapCalcXY(info, &x, &y, &sinfo, regs, ram, 0);
               }
            }

            // Fetch pixel
            if (!Vdp2FetchPixel(info, x, y, &color, &dot, ram, info->charaddr, info->paladdr, color_ram))
            {
               continue;
            }

            Rbg0PutPixel(info, color, dot, i, j);
         }
         xmul += p->deltaXst;
         ymul += p->deltaYst;
         coefx = 0;
         rcoefx = 0;
         coefy += toint(p->deltaKAst);
         rcoefy += decipart(p->deltaKAst);

         if (p2 != NULL)
         {
            xmul2 += p2->deltaXst;
            ymul2 += p2->deltaYst;
            if (p2->coefenab)
            {
               coefx2 = 0;
               rcoefx2 = 0;
               coefy2 += toint(p2->deltaKAst);
               rcoefy2 += decipart(p2->deltaKAst);
            }
         }
      }
      return;
   }

   Vdp2DrawScroll(info, lines, regs, ram, color_ram, cell_data);
}

//////////////////////////////////////////////////////////////////////////////

static void Vdp2DrawBackScreen(void)
{
   int i, j;

   // Only draw black if TVMD's DISP and BDCLMD bits are cleared
   if ((Vdp2Regs->TVMD & 0x8000) == 0 && (Vdp2Regs->TVMD & 0x100) == 0)
   {
      // Draw Black
      for (j = 0; j < vdp2height; j++)
         TitanPutBackHLine(j, COLSAT2YAB32(0x3F, 0));
   }
   else
   {
      // Draw Back Screen
      u32 scrAddr;
      u16 dot;
      vdp2draw_struct info = { 0 };

      ReadVdp2ColorOffset(Vdp2Regs, &info, (1 << 5), 0);

      if (Vdp2Regs->VRSIZE & 0x8000)
         scrAddr = (((Vdp2Regs->BKTAU & 0x7) << 16) | Vdp2Regs->BKTAL) * 2;
      else
         scrAddr = (((Vdp2Regs->BKTAU & 0x3) << 16) | Vdp2Regs->BKTAL) * 2;

      if (Vdp2Regs->BKTAU & 0x8000)
      {
         // Per Line
         for (i = 0; i < vdp2height; i++)
         {
            dot = T1ReadWord(Vdp2Ram, scrAddr);
            scrAddr += 2;

            TitanPutBackHLine(i, info.PostPixelFetchCalc(&info, COLSAT2YAB16(0x3f, dot)));
         }
      }
      else
      {
         // Single Color
         dot = T1ReadWord(Vdp2Ram, scrAddr);

         for (j = 0; j < vdp2height; j++)
            TitanPutBackHLine(j, info.PostPixelFetchCalc(&info, COLSAT2YAB16(0x3f, dot)));
      }
   }
}

//////////////////////////////////////////////////////////////////////////////

static void Vdp2DrawLineScreen(void)
{
   u32 scrAddr;
   u16 color;
   u32 dot;
   int i;
	int alpha;

   /* no need to go further if no screen is using the line screen */
   if (Vdp2Regs->LNCLEN == 0)
      return;

   if (Vdp2Regs->VRSIZE & 0x8000)
      scrAddr = (Vdp2Regs->LCTA.all & 0x7FFFF) << 1;
   else
      scrAddr = (Vdp2Regs->LCTA.all & 0x3FFFF) << 1;

   alpha = (Vdp2Regs->CCRLB & 0x1f) << 1;

   if (Vdp2Regs->LCTA.part.U & 0x8000)
   {
      /* per line */
      for (i = 0; i < vdp2height; i++)
      {
         color = T1ReadWord(Vdp2Ram, scrAddr) & 0x7FF;
         dot = Vdp2ColorRamGetColor(color, (int)Vdp2ColorRam);
         scrAddr += 2;

         TitanPutLineHLine(1, i, COLSAT2YAB32(alpha, dot));
      }
   }
   else
   {
      /* single color, implemented but not tested... */
      color = T1ReadWord(Vdp2Ram, scrAddr) & 0x7FF;
      dot = Vdp2ColorRamGetColor(color, (int)Vdp2ColorRam);
      for (i = 0; i < vdp2height; i++)
         TitanPutLineHLine(1, i, COLSAT2YAB32(alpha, dot));
   }
}

//////////////////////////////////////////////////////////////////////////////

static void LoadLineParamsNBG0(vdp2draw_struct * info, screeninfo_struct * sinfo, int line, Vdp2* lines)
{
   Vdp2 * regs;

   regs = Vdp2RestoreRegs(line, lines);
   if (regs == NULL) return;
   ReadVdp2ColorOffset(regs, info, 0x1, 0x1);
   info->specialprimode = regs->SFPRMD & 0x3;
   info->enable = regs->BGON & 0x1 || regs->BGON & 0x20;//nbg0 or rbg1
   GeneratePlaneAddrTable(info, sinfo->planetbl, info->PlaneAddr, regs);//sonic 2, 2 player mode
}

//////////////////////////////////////////////////////////////////////////////

int rstv_layer_off; /* ReST-V debug (harness RSTV_LAYEROFF): bit n = do not draw NBGn (bit 4 RBG0); all five gated */
static void Vdp2DrawNBG0(Vdp2* lines, Vdp2* regs, u8* ram, u8* color_ram, struct CellScrollData * cell_data)
{
   if (rstv_layer_off & 1) return;
   vdp2draw_struct info = { 0 };
   vdp2rotationparameterfp_struct parameter[2];

   info.titan_which_layer = TITAN_NBG0;
   info.titan_shadow_enabled = (regs->SDCTL >> 0) & 1;

   parameter[0].PlaneAddr = (void FASTCALL (*)(void *, int, Vdp2*))&Vdp2ParameterAPlaneAddr;
   parameter[1].PlaneAddr = (void FASTCALL(*)(void *, int, Vdp2*))&Vdp2ParameterBPlaneAddr;

   if (regs->BGON & 0x20)
   {
      // RBG1 mode
      info.enable = regs->BGON & 0x20;

      // Read in Parameter B
      Vdp2ReadRotationTableFP(1, &parameter[1], regs, ram);

      if((info.isbitmap = regs->CHCTLA & 0x2) != 0)
      {
         // Bitmap Mode
         ReadBitmapSize(&info, regs->CHCTLA >> 2, 0x3);

         info.charaddr = (regs->MPOFR & 0x70) * 0x2000;
         info.paladdr = (regs->BMPNA & 0x7) << 8;
         info.flipfunction = 0;
         info.specialfunction = 0;
         info.specialcolorfunction = (regs->BMPNA & 0x10) >> 4;
      }
      else
      {
         // Tile Mode
         info.mapwh = 4;
         ReadPlaneSize(&info, regs->PLSZ >> 12);
         ReadPatternData(&info, regs->PNCN0, regs->CHCTLA & 0x1);
      }

      info.rotatenum = 1;
      info.rotatemode = 0;
      info.PlaneAddr = (void FASTCALL(*)(void *, int, Vdp2*))&Vdp2ParameterBPlaneAddr;
   }
   else if (regs->BGON & 0x1)
   {
      // NBG0 mode
      info.enable = regs->BGON & 0x1;

      if((info.isbitmap = regs->CHCTLA & 0x2) != 0)
      {
         // Bitmap Mode
         ReadBitmapSize(&info, regs->CHCTLA >> 2, 0x3);

         info.x = regs->SCXIN0 & 0x7FF;
         info.y = regs->SCYIN0 & 0x7FF;

         info.charaddr = (regs->MPOFN & 0x7) * 0x20000;
         info.paladdr = (regs->BMPNA & 0x7) << 8;
         info.flipfunction = 0;
         info.specialfunction = 0;
         info.specialcolorfunction = (regs->BMPNA & 0x10) >> 4;
      }
      else
      {
         // Tile Mode
         info.mapwh = 2;

         ReadPlaneSize(&info, regs->PLSZ);

         info.x = regs->SCXIN0 & 0x7FF;
         info.y = regs->SCYIN0 & 0x7FF;
         ReadPatternData(&info, regs->PNCN0, regs->CHCTLA & 0x1);
      }

      info.coordincx = (regs->ZMXN0.all & 0x7FF00) / (float) 65536 * (rstv_half_x ? 2 : 1);
      info.coordincy = (regs->ZMYN0.all & 0x7FF00) / (float) 65536 * (rstv_half_y ? 2 : 1);
      info.PlaneAddr = (void FASTCALL(*)(void *, int, Vdp2*))&Vdp2NBG0PlaneAddr;
   }

   info.transparencyenable = !(regs->BGON & 0x100);
   info.specialprimode = regs->SFPRMD & 0x3;

   info.colornumber = (regs->CHCTLA & 0x70) >> 4;

   if (regs->CCCTL & 0x201)
      info.alpha = ((~regs->CCRNA & 0x1F) << 1) + 1;
   else
      info.alpha = 0x3F;
   if ((regs->CCCTL & 0x201) == 0x201) info.alpha |= 0x80;
   else if ((regs->CCCTL & 0x101) == 0x101) info.alpha |= 0x80;
   info.specialcolormode = regs->SFCCMD & 0x3;
   if (regs->SFSEL & 0x1)
      info.specialcode = regs->SFCODE >> 8;
   else
      info.specialcode = regs->SFCODE & 0xFF;
   info.linescreen = 0;
   if (regs->LNCLEN & 0x1)
      info.linescreen = 1;

   info.coloroffset = (regs->CRAOFA & 0x7) << 8;
   ReadVdp2ColorOffset(regs, &info, 0x1, 0x1);
   info.priority = regs->PRINA & 0x7;

   if (!(info.enable & Vdp2External.disptoggle))
      return;

   ReadMosaicData(&info, 0x1, regs);
   ReadLineScrollData(&info, regs->SCRCTL & 0xFF, regs->LSTA0.all);
   if (regs->SCRCTL & 1)
   {
      info.isverticalscroll = 1;
      info.verticalscrolltbl = (regs->VCSTA.all & 0x7FFFE) << 1;
      if (regs->SCRCTL & 0x100)
         info.verticalscrollinc = 8;
      else
         info.verticalscrollinc = 4;
   }
   else
      info.isverticalscroll = 0;
   info.wctl = regs->WCTLA;

   info.LoadLineParams = (void (*)(void *, void *,int ,Vdp2*)) LoadLineParamsNBG0;

   if (info.enable == 1)
   {
      // NBG0 draw
      Vdp2DrawScroll(&info, lines, regs, ram, color_ram, cell_data);
   }
   else
   {
      // RBG1 draw
      Vdp2DrawRotationFP(&info, parameter, lines, regs, ram, color_ram, cell_data);
   }
}

//////////////////////////////////////////////////////////////////////////////

static void LoadLineParamsNBG1(vdp2draw_struct * info, screeninfo_struct * sinfo, int line, Vdp2* lines)
{
   Vdp2 * regs;

   regs = Vdp2RestoreRegs(line, lines);
   if (regs == NULL) return;
   ReadVdp2ColorOffset(regs, info, 0x2, 0x2);
   info->specialprimode = (regs->SFPRMD >> 2) & 0x3;
   info->enable = regs->BGON & 0x2;//f1 challenge map when zoomed out
   GeneratePlaneAddrTable(info, sinfo->planetbl, info->PlaneAddr, regs);
}

//////////////////////////////////////////////////////////////////////////////

static void Vdp2DrawNBG1(Vdp2* lines, Vdp2* regs, u8* ram, u8* color_ram, struct CellScrollData * cell_data)
{
   if (rstv_layer_off & 2) return;
   vdp2draw_struct info = { 0 };

   info.titan_which_layer = TITAN_NBG1;
   info.titan_shadow_enabled = (regs->SDCTL >> 1) & 1;

   info.enable = regs->BGON & 0x2;
   info.transparencyenable = !(regs->BGON & 0x200);
   info.specialprimode = (regs->SFPRMD >> 2) & 0x3;

   info.colornumber = (regs->CHCTLA & 0x3000) >> 12;

   if((info.isbitmap = regs->CHCTLA & 0x200) != 0)
   {
      ReadBitmapSize(&info, regs->CHCTLA >> 10, 0x3);

      info.x = regs->SCXIN1 & 0x7FF;
      info.y = regs->SCYIN1 & 0x7FF;

      info.charaddr = ((regs->MPOFN & 0x70) >> 4) * 0x20000;
      info.paladdr = regs->BMPNA & 0x700;
      info.flipfunction = 0;
      info.specialfunction = 0;
      info.specialcolorfunction = (regs->BMPNA & 0x1000) >> 12;
   }
   else
   {
      info.mapwh = 2;

      ReadPlaneSize(&info, regs->PLSZ >> 2);

      info.x = regs->SCXIN1 & 0x7FF;
      info.y = regs->SCYIN1 & 0x7FF;

      ReadPatternData(&info, regs->PNCN1, regs->CHCTLA & 0x100);
   }

   if (regs->CCCTL & 0x202)
      info.alpha = ((~regs->CCRNA & 0x1F00) >> 7) + 1;
   else
      info.alpha = 0x3F;
   if ((regs->CCCTL & 0x202) == 0x202) info.alpha |= 0x80;
   else if ((regs->CCCTL & 0x102) == 0x102) info.alpha |= 0x80;
   info.specialcolormode = (regs->SFCCMD >> 2) & 0x3;
   if (regs->SFSEL & 0x2)
      info.specialcode = regs->SFCODE >> 8;
   else
      info.specialcode = regs->SFCODE & 0xFF;
   info.linescreen = 0;
   if (regs->LNCLEN & 0x2)
      info.linescreen = 1;

   info.coloroffset = (regs->CRAOFA & 0x70) << 4;
   ReadVdp2ColorOffset(regs, &info, 0x2, 0x2);
   info.coordincx = (regs->ZMXN1.all & 0x7FF00) / (float) 65536 * (rstv_half_x ? 2 : 1);
   info.coordincy = (regs->ZMYN1.all & 0x7FF00) / (float) 65536 * (rstv_half_y ? 2 : 1);

   info.priority = (regs->PRINA >> 8) & 0x7;
   info.PlaneAddr = (void FASTCALL(*)(void *, int, Vdp2*))&Vdp2NBG1PlaneAddr;

   if (!(info.enable & Vdp2External.disptoggle) ||
       (regs->BGON & 0x1 && (regs->CHCTLA & 0x70) >> 4 == 4)) // If NBG0 16M mode is enabled, don't draw
      return;

   ReadMosaicData(&info, 0x2, regs);
   ReadLineScrollData(&info, regs->SCRCTL >> 8, regs->LSTA1.all);
   if (regs->SCRCTL & 0x100)
   {
      info.isverticalscroll = 1;
      if (regs->SCRCTL & 0x1)
      {
         info.verticalscrolltbl = 4 + ((regs->VCSTA.all & 0x7FFFE) << 1);
         info.verticalscrollinc = 8;
      }
      else
      {
         info.verticalscrolltbl = (regs->VCSTA.all & 0x7FFFE) << 1;
         info.verticalscrollinc = 4;
      }
   }
   else
      info.isverticalscroll = 0;
   info.wctl = regs->WCTLA >> 8;

   info.LoadLineParams = (void(*)(void *, void*, int, Vdp2*)) LoadLineParamsNBG1;

   Vdp2DrawScroll(&info, lines, regs, ram, color_ram, cell_data);
}

//////////////////////////////////////////////////////////////////////////////

static void LoadLineParamsNBG2(vdp2draw_struct * info, screeninfo_struct * sinfo, int line, Vdp2* lines)
{
   Vdp2 * regs;

   regs = Vdp2RestoreRegs(line, lines);
   if (regs == NULL) return;
   ReadVdp2ColorOffset(regs, info, 0x4, 0x4);
   info->specialprimode = (regs->SFPRMD >> 4) & 0x3;
   info->enable = regs->BGON & 0x4;
   GeneratePlaneAddrTable(info, sinfo->planetbl, info->PlaneAddr, regs);
}

//////////////////////////////////////////////////////////////////////////////

static void Vdp2DrawNBG2(Vdp2* lines, Vdp2* regs, u8* ram, u8* color_ram, struct CellScrollData * cell_data)
{
   if (rstv_layer_off & 4) return;
   vdp2draw_struct info = { 0 };

   info.titan_which_layer = TITAN_NBG2;
   info.titan_shadow_enabled = (regs->SDCTL >> 2) & 1;

   info.enable = regs->BGON & 0x4;
   info.transparencyenable = !(regs->BGON & 0x400);
   info.specialprimode = (regs->SFPRMD >> 4) & 0x3;

   info.colornumber = (regs->CHCTLB & 0x2) >> 1;	
   info.mapwh = 2;

   ReadPlaneSize(&info, regs->PLSZ >> 4);
   info.x = regs->SCXN2 & 0x7FF;
   info.y = regs->SCYN2 & 0x7FF;
   ReadPatternData(&info, regs->PNCN2, regs->CHCTLB & 0x1);
    
   if (regs->CCCTL & 0x204)
      info.alpha = ((~regs->CCRNB & 0x1F) << 1) + 1;
   else
      info.alpha = 0x3F;
   if ((regs->CCCTL & 0x204) == 0x204) info.alpha |= 0x80;
   else if ((regs->CCCTL & 0x104) == 0x104) info.alpha |= 0x80;
   info.specialcolormode = (regs->SFCCMD >> 4) & 0x3;
   if (regs->SFSEL & 0x4)
      info.specialcode = regs->SFCODE >> 8;
   else
      info.specialcode = regs->SFCODE & 0xFF;
   info.linescreen = 0;
   if (regs->LNCLEN & 0x4)
      info.linescreen = 1;

   info.coloroffset = regs->CRAOFA & 0x700;
   ReadVdp2ColorOffset(regs, &info, 0x4, 0x4);
   info.coordincx = rstv_half_x ? 2 : 1; /* ReST-V (ADR-100) */
   info.coordincy = rstv_half_y ? 2 : 1;

   info.priority = regs->PRINB & 0x7;
   info.PlaneAddr = (void FASTCALL(*)(void *, int, Vdp2*))&Vdp2NBG2PlaneAddr;

   if (!(info.enable & Vdp2External.disptoggle) ||
      (regs->BGON & 0x1 && (regs->CHCTLA & 0x70) >> 4 >= 2)) // If NBG0 2048/32786/16M mode is enabled, don't draw
      return;

   ReadMosaicData(&info, 0x4, regs);
   info.islinescroll = 0;
   info.isverticalscroll = 0;
   info.wctl = regs->WCTLB;
   info.isbitmap = 0;

   info.LoadLineParams = (void(*)(void *,void*, int, Vdp2*)) LoadLineParamsNBG2;

   Vdp2DrawScroll(&info, lines, regs, ram, color_ram, cell_data);
}

//////////////////////////////////////////////////////////////////////////////

static void LoadLineParamsNBG3(vdp2draw_struct * info, screeninfo_struct * sinfo, int line, Vdp2* lines)
{
   Vdp2 * regs;

   regs = Vdp2RestoreRegs(line, lines);
   if (regs == NULL) return;
   ReadVdp2ColorOffset(regs, info, 0x8, 0x8);
   info->specialprimode = (regs->SFPRMD >> 6) & 0x3;
   info->enable = regs->BGON & 0x8;
   GeneratePlaneAddrTable(info, sinfo->planetbl, info->PlaneAddr, regs);
}

//////////////////////////////////////////////////////////////////////////////

static void Vdp2DrawNBG3(Vdp2* lines, Vdp2* regs, u8* ram, u8* color_ram, struct CellScrollData * cell_data)
{
   if (rstv_layer_off & 8) return;
   vdp2draw_struct info = { 0 };

   info.titan_which_layer = TITAN_NBG3;
   info.titan_shadow_enabled = (regs->SDCTL >> 3) & 1;

   info.enable = regs->BGON & 0x8;
   info.transparencyenable = !(regs->BGON & 0x800);
   info.specialprimode = (regs->SFPRMD >> 6) & 0x3;

   info.colornumber = (regs->CHCTLB & 0x20) >> 5;
	
   info.mapwh = 2;

   ReadPlaneSize(&info, regs->PLSZ >> 6);
   info.x = regs->SCXN3 & 0x7FF;
   info.y = regs->SCYN3 & 0x7FF;
   ReadPatternData(&info, regs->PNCN3, regs->CHCTLB & 0x10);

   if (regs->CCCTL & 0x208)
      info.alpha = ((~regs->CCRNB & 0x1F00) >> 7) + 1;
   else
      info.alpha = 0x3F;
   if ((regs->CCCTL & 0x208) == 0x208) info.alpha |= 0x80;
   else if ((regs->CCCTL & 0x108) == 0x108) info.alpha |= 0x80;
   info.specialcolormode = (regs->SFCCMD >> 6) & 0x3;
   if (regs->SFSEL & 0x8)
      info.specialcode = regs->SFCODE >> 8;
   else
      info.specialcode = regs->SFCODE & 0xFF;
   info.linescreen = 0;
   if (regs->LNCLEN & 0x8)
      info.linescreen = 1;

   info.coloroffset = (regs->CRAOFA & 0x7000) >> 4;
   ReadVdp2ColorOffset(regs, &info, 0x8, 0x8);
   info.coordincx = rstv_half_x ? 2 : 1; /* ReST-V (ADR-100) */
   info.coordincy = rstv_half_y ? 2 : 1;

   info.priority = (regs->PRINB >> 8) & 0x7;
   info.PlaneAddr = (void FASTCALL(*)(void *, int, Vdp2*))&Vdp2NBG3PlaneAddr;

   if (!(info.enable & Vdp2External.disptoggle) ||
      (regs->BGON & 0x1 && (regs->CHCTLA & 0x70) >> 4 == 4) || // If NBG0 16M mode is enabled, don't draw
      (regs->BGON & 0x2 && (regs->CHCTLA & 0x3000) >> 12 >= 2)) // If NBG1 2048/32786 is enabled, don't draw
      return;

   ReadMosaicData(&info, 0x8, regs);
   info.islinescroll = 0;
   info.isverticalscroll = 0;
   info.wctl = regs->WCTLB >> 8;
   info.isbitmap = 0;

   info.LoadLineParams = (void(*)(void *, void*, int, Vdp2*)) LoadLineParamsNBG3;

   Vdp2DrawScroll(&info, lines, regs, ram, color_ram, cell_data);
}

//////////////////////////////////////////////////////////////////////////////

static void LoadLineParamsRBG0(vdp2draw_struct * info, screeninfo_struct * sinfo, int line, Vdp2* lines)
{
   Vdp2 * regs;

   regs = Vdp2RestoreRegs(line, lines);
   if (regs == NULL) return;
   ReadVdp2ColorOffset(regs, info, 0x10, 0x10);
   info->specialprimode = (regs->SFPRMD >> 8) & 0x3;
}

//////////////////////////////////////////////////////////////////////////////

static void Vdp2DrawRBG0(Vdp2* lines, Vdp2* regs, u8* ram, u8* color_ram, struct CellScrollData * cell_data)
{
   if (rstv_layer_off & 16) return;
   vdp2draw_struct info = { 0 };
   vdp2rotationparameterfp_struct parameter[2];

   info.titan_which_layer = TITAN_RBG0;
   info.titan_shadow_enabled = (regs->SDCTL >> 4) & 1;

   parameter[0].PlaneAddr = (void FASTCALL(*)(void *, int, Vdp2*))&Vdp2ParameterAPlaneAddr;
   parameter[1].PlaneAddr = (void FASTCALL(*)(void *, int, Vdp2*))&Vdp2ParameterBPlaneAddr;

   info.enable = regs->BGON & 0x10;
   info.priority = regs->PRIR & 0x7;
   if (!(info.enable & Vdp2External.disptoggle))
      return;
   info.transparencyenable = !(regs->BGON & 0x1000);
   info.specialprimode = (regs->SFPRMD >> 8) & 0x3;

   info.colornumber = (regs->CHCTLB & 0x7000) >> 12;

   // Figure out which Rotation Parameter we're using
   switch (regs->RPMD & 0x3)
   {
      case 0:
         // Parameter A
         info.rotatenum = 0;
         info.rotatemode = 0;
         info.PlaneAddr = (void FASTCALL(*)(void *, int, Vdp2*))&Vdp2ParameterAPlaneAddr;
         break;
      case 1:
         // Parameter B
         info.rotatenum = 1;
         info.rotatemode = 0;
         info.PlaneAddr = (void FASTCALL(*)(void *, int, Vdp2*))&Vdp2ParameterBPlaneAddr;
         break;
      case 2:
         // Parameter A+B switched via coefficients
      case 3:
         // Parameter A+B switched via rotation parameter window
      default:
         info.rotatenum = 0;
         info.rotatemode = 1 + (regs->RPMD & 0x1);
         info.PlaneAddr = (void FASTCALL(*)(void *, int, Vdp2*))&Vdp2ParameterAPlaneAddr;
         break;
   }

   Vdp2ReadRotationTableFP(info.rotatenum, &parameter[info.rotatenum], regs, ram);

   if((info.isbitmap = regs->CHCTLB & 0x200) != 0)
   {
      // Bitmap Mode
      ReadBitmapSize(&info, regs->CHCTLB >> 10, 0x1);

      if (info.rotatenum == 0)
         // Parameter A
         info.charaddr = (regs->MPOFR & 0x7) * 0x20000;
      else
         // Parameter B
         info.charaddr = (regs->MPOFR & 0x70) * 0x2000;

      info.paladdr = (regs->BMPNB & 0x7) << 8;
      info.flipfunction = 0;
      info.specialfunction = 0;
      info.specialcolorfunction = (regs->BMPNB & 0x10) >> 4;
   }
   else
   {
      // Tile Mode
      info.mapwh = 4;

      if (info.rotatenum == 0)
         // Parameter A
         ReadPlaneSize(&info, regs->PLSZ >> 8);
      else
         // Parameter B
         ReadPlaneSize(&info, regs->PLSZ >> 12);

      ReadPatternData(&info, regs->PNCR, regs->CHCTLB & 0x100);
   }

   if (regs->CCCTL & 0x210)
      info.alpha = ((~regs->CCRR & 0x1F) << 1) + 1;
   else
      info.alpha = 0x3F;
   if ((regs->CCCTL & 0x210) == 0x210) info.alpha |= 0x80;
   else if ((regs->CCCTL & 0x110) == 0x110) info.alpha |= 0x80;
   info.specialcolormode = (regs->SFCCMD >> 8) & 0x3;
   if (regs->SFSEL & 0x10)
      info.specialcode = regs->SFCODE >> 8;
   else
      info.specialcode = regs->SFCODE & 0xFF;
   info.linescreen = 0;
   if (regs->LNCLEN & 0x10)
      info.linescreen = 1;

   info.coloroffset = (regs->CRAOFB & 0x7) << 8;

   ReadVdp2ColorOffset(regs, &info, 0x10, 0x10);
   info.coordincx = info.coordincy = 1;

   ReadMosaicData(&info, 0x10, regs);
   info.islinescroll = 0;
   info.isverticalscroll = 0;
   info.wctl = regs->WCTLC;

   info.LoadLineParams = (void(*)(void *, void*, int, Vdp2*)) LoadLineParamsRBG0;

   Vdp2DrawRotationFP(&info, parameter, lines, regs, ram, color_ram, cell_data);
}

//////////////////////////////////////////////////////////////////////////////

static void LoadLineParamsSprite(vdp2draw_struct * info, int line, Vdp2* lines)
{
   Vdp2 * regs;

   regs = Vdp2RestoreRegs(line, lines);
   if (regs == NULL) return;
   ReadVdp2ColorOffset(regs, info, 0x40, 0x40);
}

//////////////////////////////////////////////////////////////////////////////

struct {
   volatile int need_draw[6];
   volatile int draw_finished[6];
   Vdp2 lines[270];
   Vdp2 regs;
   u8 ram[0x80000];
   u8 color_ram[0x1000];
   struct CellScrollData cell_scroll_data[270];
}vidsoft_thread_context;

#define DECLARE_THREAD(NAME, LAYER, FUNC) \
void NAME(void * data) \
{ \
   for (;;) \
   { \
      if (vidsoft_thread_context.need_draw[LAYER]) \
      { \
         vidsoft_thread_context.need_draw[LAYER] = 0; \
         FUNC(vidsoft_thread_context.lines, &vidsoft_thread_context.regs, vidsoft_thread_context.ram, vidsoft_thread_context.color_ram, vidsoft_thread_context.cell_scroll_data); \
         vidsoft_thread_context.draw_finished[LAYER] = 1; \
      } \
      YabThreadSleep(); \
   } \
}

DECLARE_THREAD(VidsoftRbg0Thread, TITAN_RBG0, Vdp2DrawRBG0)
DECLARE_THREAD(VidsoftNbg0Thread, TITAN_NBG0, Vdp2DrawNBG0)
DECLARE_THREAD(VidsoftNbg1Thread, TITAN_NBG1, Vdp2DrawNBG1)
DECLARE_THREAD(VidsoftNbg2Thread, TITAN_NBG2, Vdp2DrawNBG2)
DECLARE_THREAD(VidsoftNbg3Thread, TITAN_NBG3, Vdp2DrawNBG3)

//////////////////////////////////////////////////////////////////////////////

void VIDSoftSetNumLayerThreads(int num)
{
   vidsoft_num_layer_threads = num;
}

//////////////////////////////////////////////////////////////////////////////

void VidsoftVdp1Thread(void* data)
{
#ifdef RSTV
   for (;;)
   {
      if (YabWaitEventQueue(rstv_vdp1_req) < 0)
         break;
      u64 pt_ = RSTV_PROF_T();
      Vdp1DrawCommands(vidsoft_vdp1_thread_context.ram, &vidsoft_vdp1_thread_context.regs, rstv_vdp1_target);
      RSTV_PROF_ADD(RSTV_PROF_VDP1_THREAD, pt_);
      YabAddEventQueue(rstv_vdp1_done, 1);
   }
   return;
#endif
   for (;;)
   {
      if (vidsoft_vdp1_thread_context.need_draw)
      {
         vidsoft_vdp1_thread_context.need_draw = 0;
         Vdp1DrawCommands(vidsoft_vdp1_thread_context.ram, &vidsoft_vdp1_thread_context.regs, vidsoft_vdp1_thread_context.back_framebuffer);
         memcpy(vdp1backframebuffer, vidsoft_vdp1_thread_context.back_framebuffer, 0x40000);
         vidsoft_vdp1_thread_context.draw_finished = 1;
      }

      YabThreadSleep();
   }
}

//////////////////////////////////////////////////////////////////////////////

void VidsoftWaitForVdp1Thread()
{
#ifdef RSTV
   rstv_vdp1_sync_buffer(NULL);
   return;
#endif
   if (vidsoft_vdp1_thread_enabled)
   {
      while (!vidsoft_vdp1_thread_context.draw_finished){}
   }
}

//////////////////////////////////////////////////////////////////////////////

void VIDSoftSetVdp1ThreadEnable(int b)
{
#ifdef RSTV
   vidsoft_vdp1_thread_enabled = rstv_vdp1_req != NULL; /* ReST-V: decided by rstv_vdp1_thread at init */
#else
   vidsoft_vdp1_thread_enabled = b;
#endif

}

void VidsoftSpriteThread(void * data)
{
   for (;;)
   {
      if (vidsoft_thread_context.need_draw[TITAN_SPRITE])
      {
         vidsoft_thread_context.need_draw[TITAN_SPRITE] = 0;
         VidsoftDrawSprite(&vidsoft_thread_context.regs, sprite_window_mask, vdp1frontframebuffer, vidsoft_thread_context.ram, Vdp1Regs,vidsoft_thread_context.lines, vidsoft_thread_context.color_ram);
         vidsoft_thread_context.draw_finished[TITAN_SPRITE] = 1;
      }
      YabThreadSleep();
   }
}

//////////////////////////////////////////////////////////////////////////////

int VIDSoftInit(void)
{
   int i;

   if (TitanInit() == -1)
      return -1;

   if ((dispbuffer = (pixel_t *)calloc(sizeof(pixel_t), 704 * 512)) == NULL)
      return -1;

   // Initialize VDP1 framebuffer 1
   if ((vdp1framebuffer[0] = (u8 *)calloc(sizeof(u8), 0x40000)) == NULL)
      return -1;

   // Initialize VDP1 framebuffer 2
   if ((vdp1framebuffer[1] = (u8 *)calloc(sizeof(u8), 0x40000)) == NULL)
      return -1;

   vdp1backframebuffer = vdp1framebuffer[0];
   vdp1frontframebuffer = vdp1framebuffer[1];
   rbg0width = vdp2width = 320;
   vdp2height = 224;

#ifdef USE_OPENGL
   VIDSoftSetupGL();
#endif

   for (i = 0; i < 6; i++)
   {
      vidsoft_thread_context.draw_finished[i] = 1;
      vidsoft_thread_context.need_draw[i] = 0;
   }

   vidsoft_vdp1_thread_context.need_draw = 0;
   vidsoft_vdp1_thread_context.draw_finished = 1;
#ifdef RSTV
   if (rstv_vdp1_thread && !rstv_vdp1_req)
   {
      rstv_vdp1_req = YabThreadCreateQueue(4);
      rstv_vdp1_done = YabThreadCreateQueue(4);
      if (YabThreadStart(YAB_THREAD_VIDSOFT_VDP1, (void * (*)(void *))VidsoftVdp1Thread, 0) == 0)
         vidsoft_vdp1_thread_enabled = 1;
   }
#else
   /* ReST-V: these helper threads are never used (layer/VDP1 threading off) and only cost CPU time */
   YabThreadStart(YAB_THREAD_VIDSOFT_VDP1, (void * (*)(void *))VidsoftVdp1Thread, 0);

   YabThreadStart(YAB_THREAD_VIDSOFT_LAYER_RBG0, (void * (*)(void *))VidsoftRbg0Thread, 0);
   YabThreadStart(YAB_THREAD_VIDSOFT_LAYER_NBG0, (void * (*)(void *))VidsoftNbg0Thread, 0);
   YabThreadStart(YAB_THREAD_VIDSOFT_LAYER_NBG1, (void * (*)(void *))VidsoftNbg1Thread, 0);
   YabThreadStart(YAB_THREAD_VIDSOFT_LAYER_NBG2, (void * (*)(void *))VidsoftNbg2Thread, 0);
   YabThreadStart(YAB_THREAD_VIDSOFT_LAYER_NBG3, (void * (*)(void *))VidsoftNbg3Thread, 0);
   YabThreadStart(YAB_THREAD_VIDSOFT_LAYER_SPRITE, (void * (*)(void *))VidsoftSpriteThread, 0);
#endif

   return 0;
}

//////////////////////////////////////////////////////////////////////////////

void VIDSoftSetBilinear(int b)
{
   bilinear = b;
}

//////////////////////////////////////////////////////////////////////////////

void VIDSoftSetupGL(void)
{
#if !defined(ANDROID) && !defined(IOS)
#ifdef USE_OPENGL
   GLint status;
   GLint texAttrib;
   GLint posAttrib;

   // Shader sources
   const GLchar* vshader_src =
      "#version 330 core\n"
      "in vec2 position;"
      "in vec2 texcoord;"
      "out vec2 outcoord;"
      "void main() {"
      "   outcoord = texcoord;"
      "   gl_Position = vec4(position, 0.0, 1.0);"
      "}";

   const GLchar* fshader_src =
      "#version 330 core\n"
      "in vec2 outcoord;"
      "out vec4 fragcolor;"
      "uniform sampler2D sattex;"
      "void main() {"
      "   fragcolor = texture(sattex, outcoord);"
      "}";

   const float vertices[16] = {
      -1.0f, -1.0f, // Vertex 1 (X, Y)
      -1.0f, 1.0f,  // Vertex 2 (X, Y)
      1.0f, -1.0f,  // Vertex 3 (X, Y)
      1.0f, 1.0f,   // Vertex 4 (X, Y)
      0.0, 1.0,     // Texture 1 (X, Y)
      0.0, 0.0,     // Texture 2 (X, Y)
      1.0, 1.0,     // Texture 3 (X, Y)
      1.0, 0.0      // Texture 4 (X, Y)
   };

   outputwidth = vdp2width;
   outputheight = vdp2height;
#if defined(_USEGLEW_)
   glewInit();
#endif

   glGenVertexArrays(1, &vao);
   glBindVertexArray(vao);

   glGenBuffers(1, &vbo);
   glBindBuffer(GL_ARRAY_BUFFER, vbo);
   glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

   vshader = glCreateShader(GL_VERTEX_SHADER);
   glShaderSource(vshader, 1, &vshader_src, NULL);
   glCompileShader(vshader);

   glGetShaderiv(vshader, GL_COMPILE_STATUS, &status);
   if (status == GL_FALSE) { YGLLOG("Failed to compile vertex shader\n"); }

   fshader = glCreateShader(GL_FRAGMENT_SHADER);
   glShaderSource(fshader, 1, &fshader_src, NULL);
   glCompileShader(fshader);

   glGetShaderiv(fshader, GL_COMPILE_STATUS, &status);
   if (status == GL_FALSE) { YGLLOG("Failed to compile fragment shader\n"); }
	
   gl_shader_prog = glCreateProgram();
   glAttachShader(gl_shader_prog, vshader);
   glAttachShader(gl_shader_prog, fshader);

   glLinkProgram(gl_shader_prog);

   glValidateProgram(gl_shader_prog);
   glGetProgramiv(gl_shader_prog, GL_LINK_STATUS, &status);
   if (status == GL_FALSE) { YGLLOG("Failed to link shader program\n"); }

   glUseProgram(gl_shader_prog);
	
   posAttrib = glGetAttribLocation(gl_shader_prog, "position");
   glEnableVertexAttribArray(posAttrib);
   glVertexAttribPointer(posAttrib, 2, GL_FLOAT, GL_FALSE, 0, 0);

   texAttrib = glGetAttribLocation(gl_shader_prog, "texcoord");
   glEnableVertexAttribArray(texAttrib);
   glVertexAttribPointer(texAttrib, 2, GL_FLOAT, GL_FALSE, 0, (void*)(8 * sizeof(GLfloat)));

   glGenTextures(1, &gl_texture_id);
   glActiveTexture(GL_TEXTURE0);
   glBindTexture(GL_TEXTURE_2D, gl_texture_id);

   if (bilinear) { glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR); }
   else { glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST); }
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

   glViewport(0, 0, outputwidth, outputheight);

   glUniform1i(glGetUniformLocation(gl_shader_prog, "sattex"), 0);
#endif
#endif
}

//////////////////////////////////////////////////////////////////////////////

void VIDSoftDeInit(void)
{
   if (dispbuffer)
   {
      free(dispbuffer);
      dispbuffer = NULL;
   }

   if (vdp1framebuffer[0])
      free(vdp1framebuffer[0]);

   if (vdp1framebuffer[1])
      free(vdp1framebuffer[1]);
  
#if !defined(ANDROID)  
#ifdef USE_OPENGL
   if (gl_texture_id) { glDeleteTextures(1, &gl_texture_id); }
   if (gl_shader_prog) { glDeleteProgram(gl_shader_prog); }
   if (vshader) { glDeleteShader(vshader); }
   if (fshader) { glDeleteShader(fshader); }
   if (vao) { glDeleteVertexArrays(1, &vao); }
   if (vbo) { glDeleteBuffers(1, &vbo); }
#endif
#endif
}

//////////////////////////////////////////////////////////////////////////////

static int IsFullscreen = 0;

void VIDSoftResize(int x, int y, unsigned int w, unsigned int h, int on, int keep)
{
#ifdef USE_OPENGL
   IsFullscreen = on;
   glClear(GL_COLOR_BUFFER_BIT);
   glViewport(0, 0, w, h);
   outputwidth = w;
   outputheight = h;
#endif
}

//////////////////////////////////////////////////////////////////////////////

int VIDSoftIsFullscreen(void) {
   return IsFullscreen;
}

//////////////////////////////////////////////////////////////////////////////

int VIDSoftVdp1Reset(void)
{
   Vdp1Regs->userclipX1 = Vdp1Regs->systemclipX1 = 0;
   Vdp1Regs->userclipY1 = Vdp1Regs->systemclipY1 = 0;
   Vdp1Regs->userclipX2 = Vdp1Regs->systemclipX2 = 512;
   Vdp1Regs->userclipY2 = Vdp1Regs->systemclipY2 = 256;
   
   return 0;
}

//////////////////////////////////////////////////////////////////////////////

void VIDSoftVdp1DrawStartBody(Vdp1* regs, u8 * back_framebuffer)
{
   if (regs->FBCR & 8)
      vdp1interlace = 2;
   else
      vdp1interlace = 1;
   if (regs->TVMR & 0x1)
   {
      if (regs->TVMR & 0x2)
      {
         // Rotation 8-bit
         vdp1width = 512;
         vdp1height = 512;
      }
      else
      {
         // Normal 8-bit
         vdp1width = 1024;
         vdp1height = 256;
      }

      vdp1pixelsize = 1;
   }
   else
   {
      // Rotation/Normal 16-bit
      vdp1width = 512;
      vdp1height = 256;
      vdp1pixelsize = 2;
   }

   VIDSoftVdp1EraseFrameBuffer(regs, back_framebuffer);

   //night warriors doesn't set clipping most frames and uses
   //the last part of the vdp1 framebuffer as scratch ram
   //the previously set clipping values need to be reused
}
//////////////////////////////////////////////////////////////////////////////

void VIDSoftVdp1DrawStart()
{
#ifdef RSTV
   if (rstv_vdp1_gpu_on && vidsoft_vdp1_thread_enabled)
   {
      /* ADR-023: record the command list for the GPU renderer on a register copy */
      rstv_vdp1_sync_buffer(NULL);
      Vdp1 tmp;
      memcpy(&tmp, Vdp1Regs, sizeof(Vdp1));
      if (tmp.FBCR & 8) vdp1interlace = 2; else vdp1interlace = 1;
      if (tmp.TVMR & 1) { vdp1pixelsize = 1; vdp1width = (tmp.TVMR & 2) ? 512 : 1024; vdp1height = (tmp.TVMR & 2) ? 512 : 256; }
      else { vdp1pixelsize = 2; vdp1width = 512; vdp1height = 256; }
      rstv_v1n = 0;
      rstv_v1unsup = 0;
      rstv_v1_ccn = 0;
      rstv_v1pat_reset();
      /* ADR-100: hi-res 8-bit / double interlace lists are drawn by the GPU at half size (rstv_hires_half) */
      rstv_v1list_fmt = 0;
      if (rstv_hires_half && vdp1pixelsize == 1 && vdp1width == 1024)
         rstv_v1list_fmt |= 1 | 4;
      if (rstv_hires_half && vdp1interlace == 2)
         rstv_v1list_fmt |= 2;
      {
         /* sprite window used by a layer: gvdp2 builds its mask from the CPU framebuffer (same test as gvdp2.c) */
         const Vdp2 *rr = Vdp2Regs;
         const int uses = ((rr->WCTLA | (rr->WCTLA >> 8) | rr->WCTLB | (rr->WCTLB >> 8) | rr->WCTLC) & 0x20) != 0;
         if ((rr->SPCTL & 0x10) && uses)
            rstv_v1_unsupported(10);
      }
      if (!rstv_v1unsup && (((tmp.FBCR & 2) == 0) || Vdp1External.manualerase || (tmp.TVMR & 8))) {
         /* erase (VIDSoftVdp1EraseFrameBuffer, 16-bit) as the first item */
         RstvV1Item *it = &rstv_v1list[rstv_v1n++];
         memset(it, 0, sizeof(*it));
         it->kind = RSTV_V1_ERASE;
         it->x[0] = (s16)((tmp.EWLR >> 6) & 0x1F8); it->y[0] = (s16)(tmp.EWLR & 0x1FF);
         it->x[3] = (s16)(((tmp.EWRR >> 6) & 0x3F8) + 8); it->y[3] = (s16)((tmp.EWRR & 0x1FF) + 1);
         it->colr = tmp.EWDR;
      }
      rstv_v1rec = 1;
      if (!rstv_v1unsup)
         Vdp1DrawCommands(Vdp1Ram, &tmp, NULL);
      rstv_v1rec = 0;
      if (!rstv_v1unsup) {
         rstv_v1ready = 1;
         rstv_v1ready_target = rstv_vdp1_back_idx;
         rstv_vdp1_gpu_drawn[rstv_vdp1_back_idx] = 1;
         rstv_v1_gpu_frames++;
         Vdp1FakeDrawCommands(Vdp1Ram, Vdp1Regs);
         Vdp1External.status = VDP1_STATUS_IDLE;
         return;
      }
      rstv_v1_cpu_frames++;
   }
   rstv_vdp1_gpu_drawn[rstv_vdp1_back_idx] = 0;
   if (vidsoft_vdp1_thread_enabled)
   {
      rstv_vdp1_sync_buffer(NULL);
      {
         /* copy only the 4KB pages written since the last snapshot (vdp1.cpp write stamps) */
         extern u32 rstv_vdp1_serial, rstv_vdp1_page_stamp[128];
         static u32 copied = 0;
         int p;
         for (p = 0; p < 128; p++)
            if (!copied || rstv_vdp1_page_stamp[p] > copied)
               memcpy(vidsoft_vdp1_thread_context.ram + p * 4096, Vdp1Ram + p * 4096, 4096);
         copied = rstv_vdp1_serial++;
      }
      VIDSoftVdp1DrawStartBody(Vdp1Regs, vdp1backframebuffer); // erase on this thread (sets vdp1width etc.)
      memcpy(&vidsoft_vdp1_thread_context.regs, Vdp1Regs, sizeof(Vdp1));
      rstv_vdp1_target = vdp1backframebuffer;
      rstv_vdp1_pending = 1;
      YabAddEventQueue(rstv_vdp1_req, 1);
      Vdp1FakeDrawCommands(Vdp1Ram, Vdp1Regs);
      Vdp1External.status = VDP1_STATUS_IDLE;
      return;
   }
#endif
   if (vidsoft_vdp1_thread_enabled)
   {
      VidsoftWaitForVdp1Thread();

      //take a snapshot of the vdp1 state, to be used by the thread
      memcpy(vidsoft_vdp1_thread_context.ram, Vdp1Ram, 0x80000);
      memcpy(&vidsoft_vdp1_thread_context.regs, Vdp1Regs, sizeof(Vdp1));
      memcpy(vidsoft_vdp1_thread_context.back_framebuffer, vdp1backframebuffer, 0x40000);

      VIDSoftVdp1DrawStartBody(&vidsoft_vdp1_thread_context.regs, vidsoft_vdp1_thread_context.back_framebuffer);

      //start thread
      vidsoft_vdp1_thread_context.draw_finished = 0;
      vidsoft_vdp1_thread_context.need_draw = 1;
      YabThreadWake(YAB_THREAD_VIDSOFT_VDP1);

      Vdp1FakeDrawCommands(Vdp1Ram, Vdp1Regs);
   }
   else
   {
      VIDSoftVdp1DrawStartBody(Vdp1Regs, vdp1backframebuffer);
      Vdp1DrawCommands(Vdp1Ram, Vdp1Regs, vdp1backframebuffer);
   }
}

//////////////////////////////////////////////////////////////////////////////

void VIDSoftVdp1DrawEnd(void)
{
}

//////////////////////////////////////////////////////////////////////////////

static INLINE u16  Vdp1ReadPattern16( u32 base, u32 offset , u8 * ram) {

   u16 dot = T1ReadByte(ram, (base + (offset >> 1)) & 0x7FFFF);
  if ((offset & 0x1) == 0) dot >>= 4; // Even pixel
  else dot &= 0xF; // Odd pixel
  return dot;
}

static INLINE u16  Vdp1ReadPattern64(u32 base, u32 offset, u8 * ram) {

   return T1ReadByte(ram, (base + offset) & 0x7FFFF) & 0x3F;
}

static INLINE u16  Vdp1ReadPattern128(u32 base, u32 offset, u8 * ram) {

   return T1ReadByte(ram, (base + offset) & 0x7FFFF) & 0x7F;
}

static INLINE u16  Vdp1ReadPattern256(u32 base, u32 offset, u8 * ram) {

   return T1ReadByte(ram, (base + offset) & 0x7FFFF) & 0xFF;
}

static INLINE u16  Vdp1ReadPattern64k(u32 base, u32 offset, u8 * ram) {

  return T1ReadWord(ram, ( base + 2*offset) & 0x7FFFF);
}

////////////////////////////////////////////////////////////////////////////////

static INLINE u32 alphablend16(u32 d, u32 s, u32 level)
{
	int r,g,b,sr,sg,sb,dr,dg,db;

	int invlevel = 256-level;
	sr = s & 0x001f; dr = d & 0x001f; 
	r = (sr*level + dr*invlevel)>>8; r&= 0x1f;
	sg = s & 0x03e0; dg = d & 0x03e0;
	g = (sg*level + dg*invlevel)>>8; g&= 0x03e0;
	sb = s & 0x7c00; db = d & 0x7c00;
	b = (sb*level + db*invlevel)>>8; b&= 0x7c00;
	return r|g|b;
}

typedef struct _COLOR_PARAMS
{
	double r,g,b;
} COLOR_PARAMS;

COLOR_PARAMS leftColumnColor;



int currentPixel;
int currentPixelIsVisible;
int characterWidth;
int characterHeight;

static int getpixel(int linenumber, int currentlineindex, vdp1cmd_struct *cmd, u8 * ram) {

	u32 characterAddress;
	u32 colorlut;
	u16 colorbank;
	u8 SPD;
	int endcode;
	int endcodesEnabled;
	int untexturedColor = 0;
	int isTextured = 1;
	int currentShape = cmd->CMDCTRL & 0x7;
	int flip;

   characterAddress = cmd->CMDSRCA << 3;
   colorbank = cmd->CMDCOLR;
	colorlut = (u32)colorbank << 3;
   SPD = ((cmd->CMDPMOD & 0x40) != 0);//show the actual color of transparent pixels if 1 (they won't be drawn transparent)
   endcodesEnabled = ((cmd->CMDPMOD & 0x80) == 0) ? 1 : 0;
   flip = (cmd->CMDCTRL & 0x30) >> 4;

	//4 polygon, 5 polyline or 6 line
	if(currentShape == 4 || currentShape == 5 || currentShape == 6) {
		isTextured = 0;
      untexturedColor = cmd->CMDCOLR;
	}

	switch( flip ) {
		case 1:
			// Horizontal flipping
			currentlineindex = characterWidth - currentlineindex-1;
			break;
		case 2:
			// Vertical flipping
			linenumber = characterHeight - linenumber-1;

			break;
		case 3:
			// Horizontal/Vertical flipping
			linenumber = characterHeight - linenumber-1;
			currentlineindex = characterWidth - currentlineindex-1;
			break;
	}

   switch ((cmd->CMDPMOD >> 3) & 0x7)
	{
		case 0x0: //4bpp bank
			endcode = 0xf;
			currentPixel = Vdp1ReadPattern16( characterAddress + (linenumber*(characterWidth>>1)), currentlineindex , ram);
			if(isTextured && endcodesEnabled && currentPixel == endcode)
				return 1;
			if (!((currentPixel == 0) && !SPD)) 
				currentPixel = (colorbank &0xfff0)| currentPixel;
			currentPixelIsVisible = 0xf;
			break;

		case 0x1://4bpp lut
			endcode = 0xf;
         currentPixel = Vdp1ReadPattern16(characterAddress + (linenumber*(characterWidth >> 1)), currentlineindex, ram);
			if(isTextured && endcodesEnabled && currentPixel == endcode)
				return 1;
			if (!(currentPixel == 0 && !SPD))
				currentPixel = T1ReadWord(ram, (currentPixel * 2 + colorlut) & 0x7FFFF);
			currentPixelIsVisible = 0xffff;
			break;
		case 0x2://8pp bank (64 color)
			//is there a hardware bug with endcodes in this color mode?
			//there are white lines around some characters in scud
			//using an endcode of 63 eliminates the white lines
			//but also causes some dropout due to endcodes being triggered that aren't triggered on hardware
			//the closest thing i can do to match the hardware is make all pixels with color index 63 transparent
			//this needs more hardware testing

			endcode = 63;
         currentPixel = Vdp1ReadPattern64(characterAddress + (linenumber*(characterWidth)), currentlineindex, ram);
			if(isTextured && endcodesEnabled && currentPixel == endcode)
				currentPixel = 0;
		//		return 1;
			if (!((currentPixel == 0) && !SPD)) 
				currentPixel = (colorbank&0xffc0) | currentPixel;
			currentPixelIsVisible = 0x3f;
			break;
		case 0x3://128 color
			endcode = 0xff;
         currentPixel = Vdp1ReadPattern128(characterAddress + (linenumber*characterWidth), currentlineindex, ram);
			if(isTextured && endcodesEnabled && currentPixel == endcode)
				return 1;
			if (!((currentPixel == 0) && !SPD)) 
				currentPixel = (colorbank&0xff80) | currentPixel;//dead or alive needs colorbank to be masked
			currentPixelIsVisible = 0x7f;
			break;
		case 0x4://256 color
			endcode = 0xff;
         currentPixel = Vdp1ReadPattern256(characterAddress + (linenumber*characterWidth), currentlineindex, ram);
			if(isTextured && endcodesEnabled && currentPixel == endcode)
				return 1;
			currentPixelIsVisible = 0xff;
			if (!((currentPixel == 0) && !SPD)) 
				currentPixel = (colorbank&0xff00) | currentPixel;
			break;
		case 0x5://16bpp bank
			endcode = 0x7fff;
         currentPixel = Vdp1ReadPattern64k(characterAddress + (linenumber*characterWidth * 2), currentlineindex, ram);
			if(isTextured && endcodesEnabled && currentPixel == endcode)
				return 1;

			/* the transparent pixel in 16bpp is supposed to be 0x0000
			but some games use pixels with invalid values and expect
			them to be transparent (see vdp1 doc p. 92) */
			if (!(currentPixel & 0x8000) && !SPD)
				currentPixel = 0;

			currentPixelIsVisible = 0xffff;
			break;
	}

	if(!isTextured)
		currentPixel = untexturedColor;

	//force the MSB to be on if MSBON is set
	//currentPixel |= cmd.CMDPMOD & (1 << 15);

	return 0;
}

static int gouraudAdjust( int color, int tableValue )
{
	color += (tableValue - 0x10);

	if ( color < 0 ) color = 0;
	if ( color > 0x1f ) color = 0x1f;

	return color;
}


static int CheckDil(int y, Vdp1 * regs)
{
   int dil = (regs->FBCR >> 2) & 1;

   if (vdp1interlace == 2)
   {
      if (dil)
      {
         if ((y & 1) == 0)
            return 1;
      }
      else
      {
         if ((y & 1))
            return 1;
      }
   }

   return 0;
}

static INLINE int IsUserClipped(int x, int y, Vdp1* regs)
{
   return !(x >= regs->userclipX1 &&
      x <= regs->userclipX2 &&
      y >= regs->userclipY1 &&
      y <= regs->userclipY2);
}

static INLINE int IsSystemClipped(int x, int y, Vdp1* regs)
{
   return !(x >= 0 &&
      x <= regs->systemclipX2 &&
      y >= 0 &&
      y <= regs->systemclipY2);
}

int IsClipped(int x, int y, Vdp1* regs, vdp1cmd_struct * cmd)
{
   if (cmd->CMDPMOD & 0x0400)//user clipping enabled
   {
      int is_user_clipped = IsUserClipped(x, y, regs);

      if (((cmd->CMDPMOD >> 9) & 0x3) == 0x3)//outside clipping mode
         is_user_clipped = !is_user_clipped;

      return is_user_clipped || IsSystemClipped(x, y, regs);
   }
   else
   {
      return IsSystemClipped(x, y, regs);
   }
}

static void putpixel8(int x, int y, Vdp1 * regs, vdp1cmd_struct *cmd, u8 * back_framebuffer) {

    int y2 = y / vdp1interlace;
    u8 * iPix = &back_framebuffer[(y2 * vdp1width) + x];
    int mesh = cmd->CMDPMOD & 0x0100;
    int SPD = ((cmd->CMDPMOD & 0x40) != 0);//show the actual color of transparent pixels if 1 (they won't be drawn transparent)

    if (iPix >= (back_framebuffer + 0x40000))
        return;

    if (CheckDil(y, regs))
       return;

    currentPixel &= 0xFF;

    if (mesh && ((x ^ y2) & 1)) {
       return;
    }

    if (IsClipped(x, y, regs, cmd))
       return;

    if ( SPD || (currentPixel & currentPixelIsVisible))
    {
        switch( cmd->CMDPMOD & 0x7 )//we want bits 0,1,2
        {
        default:
        case 0:	// replace
            if (!((currentPixel == 0) && !SPD))
                *(iPix) = currentPixel;
            break;
        }
    }
}

static void putpixel(int x, int y, Vdp1* regs, vdp1cmd_struct * cmd, u8 * back_framebuffer) {

	u16* iPix;
	int mesh = cmd->CMDPMOD & 0x0100;
	int SPD = ((cmd->CMDPMOD & 0x40) != 0);//show the actual color of transparent pixels if 1 (they won't be drawn transparent)
   int original_y = y;

   if (CheckDil(y, regs))
      return;

	y /= vdp1interlace;
   iPix = &((u16 *)back_framebuffer)[(y * vdp1width) + x];

   if (iPix >= (u16*)(back_framebuffer + 0x40000))
		return;

	if(mesh && (x^y)&1)
		return;

   if (IsClipped(x, original_y, regs, cmd))
      return;

	if (cmd->CMDPMOD & (1 << 15))
	{
		if (currentPixel) {
			*iPix |= 0x8000;
			return;
		}
	}

	if ( SPD || (currentPixel & currentPixelIsVisible))
	{
		switch( cmd->CMDPMOD & 0x7 )//we want bits 0,1,2
		{
		case 0:	// replace
			if (!((currentPixel == 0) && !SPD)) 
				*(iPix) = currentPixel;
			break;
		case 1: // shadow
			if (*(iPix) & (1 << 15)) // only if MSB of framebuffer data is set
				*(iPix) = alphablend16(*(iPix), 0, (1 << 7)) | (1 << 15);
			break;
		case 2: // half luminance
			*(iPix) = ((currentPixel & ~0x8421) >> 1) | (1 << 15);
			break;
		case 3: // half transparent
			if ( *(iPix) & (1 << 15) )//only if MSB of framebuffer data is set 
				*(iPix) = alphablend16( *(iPix), currentPixel, (1 << 7) ) | (1 << 15);
			else
				*(iPix) = currentPixel;
			break;
		case 4: //gouraud
			#define COLOR(r,g,b)    (((r)&0x1F)|(((g)&0x1F)<<5)|(((b)&0x1F)<<10) |0x8000 )

			//handle the special case demonstrated in the sgl chrome demo
			//if we are in a paletted bank mode and the other two colors are unused, adjust the index value instead of rgb
			if(
				(((cmd->CMDPMOD >> 3) & 0x7) != 5) &&
				(((cmd->CMDPMOD >> 3) & 0x7) != 1) && 
				(int)leftColumnColor.g == 16 && 
				(int)leftColumnColor.b == 16) 
			{
				int c = (int)(leftColumnColor.r-0x10);
				if(c < 0) c = 0;
				currentPixel = currentPixel+c;
				*(iPix) = currentPixel;
				break;
			}
			*(iPix) = COLOR(
				gouraudAdjust(
				currentPixel&0x001F,
				(int)leftColumnColor.r),

				gouraudAdjust(
				(currentPixel&0x03e0) >> 5,
				(int)leftColumnColor.g),

				gouraudAdjust(
				(currentPixel&0x7c00) >> 10,
				(int)leftColumnColor.b)
				);
			break;
		default:
			*(iPix) = alphablend16( COLOR((int)leftColumnColor.r,(int)leftColumnColor.g, (int)leftColumnColor.b), currentPixel, (1 << 7) ) | (1 << 15);
			break;
		}
	}
}

static int iterateOverLine(int x1, int y1, int x2, int y2, int greedy, void *data,
   int(*line_callback)(int x, int y, int i, void *data, Vdp1* regs, vdp1cmd_struct * cmd, u8* ram, u8* back_framebuffer), Vdp1* regs, vdp1cmd_struct * cmd, u8 * ram, u8* back_framebuffer) {
	int i, a, ax, ay, dx, dy;

	a = i = 0;
	dx = x2 - x1;
	dy = y2 - y1;
	ax = (dx >= 0) ? 1 : -1;
	ay = (dy >= 0) ? 1 : -1;

	//burning rangers tries to draw huge shapes
	//this will at least let it run
	if(abs(dx) > 999 || abs(dy) > 999)
		return INT_MAX;

	if (abs(dx) > abs(dy)) {
		if (ax != ay) dx = -dx;

		for (; x1 != x2; x1 += ax, i++) {
         if (line_callback && line_callback(x1, y1, i, data, regs, cmd, ram, back_framebuffer) != 0) return i + 1;

			a += dy;
			if (abs(a) >= abs(dx)) {
				a -= dx;
				y1 += ay;

				// Make sure we 'fill holes' the same as the Saturn
				if (greedy) {
					i ++;
					if (ax == ay) {
						if (line_callback &&
                     line_callback(x1 + ax, y1 - ay, i, data, regs, cmd, ram, back_framebuffer) != 0)
							return i + 1;
					} else {
						if (line_callback &&
                     line_callback(x1, y1, i, data, regs, cmd, ram, back_framebuffer) != 0)
							return i + 1;
					}
				}
			}
		}

		// If the line isn't greedy here, we end up with gaps that don't occur on the Saturn
		if (/*(i == 0) || (y1 != y2)*/1) {
         if (line_callback) line_callback(x2, y2, i, data, regs, cmd, ram, back_framebuffer);
			i ++;
		}
	} else {
		if (ax != ay) dy = -dy;

		for (; y1 != y2; y1 += ay, i++) {
         if (line_callback && line_callback(x1, y1, i, data, regs, cmd, ram, back_framebuffer) != 0) return i + 1;

			a += dx;
			if (abs(a) >= abs(dy)) {
				a -= dy;
				x1 += ax;

				if (greedy) {
					i ++;
					if (ay == ax) {
						if (line_callback &&
                     line_callback(x1, y1, i, data, regs, cmd, ram, back_framebuffer) != 0)
							return i + 1;
					} else {
						if (line_callback &&
                     line_callback(x1 - ax, y1 + ay, i, data, regs, cmd, ram, back_framebuffer) != 0)
							return i + 1;
					}
				}
			}
		}

		if (/*(i == 0) || (y1 != y2)*/1) {
         if (line_callback) line_callback(x2, y2, i, data, regs, cmd, ram, back_framebuffer);
			i ++;
		}
	}

	return i;
}

typedef struct {
	double linenumber;
	double texturestep;
	double xredstep;
	double xgreenstep;
	double xbluestep;
	int endcodesdetected;
	int previousStep;
} DrawLineData;

static int DrawLineCallback(int x, int y, int i, void *data, Vdp1* regs, vdp1cmd_struct * cmd, u8* ram, u8* back_framebuffer)
{
	int currentStep;
	DrawLineData *linedata = data;

	leftColumnColor.r += linedata->xredstep;
	leftColumnColor.g += linedata->xgreenstep;
	leftColumnColor.b += linedata->xbluestep;

	currentStep = (int)i * linedata->texturestep;
	if (getpixel(linedata->linenumber, currentStep, cmd, ram)) {
		if (currentStep != linedata->previousStep) {
			linedata->previousStep = currentStep;
			linedata->endcodesdetected ++;
		}
	} else if (vdp1pixelsize == 2) {
		putpixel(x, y, regs, cmd, back_framebuffer);
	} else {
      putpixel8(x, y, regs, cmd, back_framebuffer);
    }

	if (linedata->endcodesdetected == 2) return -1;

	return 0;
}

static int DrawLine(int x1, int y1, int x2, int y2, int greedy, double linenumber, double texturestep, double xredstep, double xgreenstep, double xbluestep, Vdp1* regs, vdp1cmd_struct *cmd, u8 * ram, u8* back_framebuffer)
{
	DrawLineData data;

	data.linenumber = linenumber;
	data.texturestep = texturestep;
	data.xredstep = xredstep;
	data.xgreenstep = xgreenstep;
	data.xbluestep = xbluestep;
	data.endcodesdetected = 0;
	data.previousStep = 123456789;

   return iterateOverLine(x1, y1, x2, y2, greedy, &data, DrawLineCallback, regs, cmd, ram, back_framebuffer);
}


typedef struct {
   int cmode, SPD, ends, endcodesEnabled, flip, cw, ch, mesh, dil_skip; /* dil_skip: -1 none, else skip y with (y & 1) == dil_skip */
   int cx1, cx2, cy1, cy2;
   u32 charaddr;
   u16 bank;
   u16 lut[16];
   u8 *ram;
   u16 *fb;
} RstvLineTex;

static INLINE int rstv_plot_fast(const RstvLineTex *t, u8 *ram, u32 rowaddr, double texturestep, int *endcodes, int *previousStep,
   int x, int y, int i)
{
   const int step = (int)i * texturestep;
   const int col = (t->flip & 1) ? (t->cw - step - 1) : step;
   u32 dot, pix;
   int wr;
   switch (t->cmode) {
   case 0:
   case 1:
      dot = T1ReadByte(ram, (rowaddr + ((u32)col >> 1)) & 0x7FFFF);
      dot = (col & 1) ? (dot & 0xF) : (dot >> 4);
      if (t->ends && dot == 0xf) goto endcode;
      if (t->cmode == 0) { wr = dot != 0; pix = (t->bank & 0xfff0) | dot; }
      else { pix = t->lut[dot]; wr = dot != 0 && pix != 0; }
      break;
   case 2:
      dot = T1ReadByte(ram, (rowaddr + col) & 0x7FFFF) & 0x3F;
      if (t->endcodesEnabled && dot == 63) dot = 0;
      wr = dot != 0;
      pix = (t->bank & 0xffc0) | dot;
      break;
   case 3:
      dot = T1ReadByte(ram, (rowaddr + col) & 0x7FFFF) & 0x7F;
      wr = dot != 0;
      pix = (t->bank & 0xff80) | dot;
      break;
   case 4:
      dot = T1ReadByte(ram, (rowaddr + col) & 0x7FFFF);
      if (t->ends && dot == 0xff) goto endcode;
      wr = dot != 0;
      pix = (t->bank & 0xff00) | dot;
      break;
   default:
      dot = T1ReadWord(ram, (rowaddr + 2 * col) & 0x7FFFF);
      if (t->ends && dot == 0x7fff) goto endcode;
      wr = (dot & 0x8000) != 0;
      pix = dot;
      break;
   }
   if ((t->SPD || wr) && x >= t->cx1 && x <= t->cx2 && y >= t->cy1 && y <= t->cy2 && (y & 1) != t->dil_skip) {
      const int y2 = y / vdp1interlace;
      const int idx = y2 * vdp1width + x;
      if (idx < 0x40000 / 2 && !(t->mesh && ((x ^ y2) & 1)))
         t->fb[idx] = (u16)pix;
   }
   return 0;
endcode:
   if (step != *previousStep) {
      *previousStep = step;
      if (++*endcodes == 2)
         return 1;
   }
   return 0;
}

/* ReST-V: DrawLine for the common textured case (16bpp framebuffer, replace mode, no MSB-on, no gouraud,
   colour modes 0-5, no "outside" user clipping). Same line walk as iterateOverLine and the same pixels as
   DrawLineCallback + getpixel + putpixel, with the per-command work hoisted out of the pixel loop. */

static int rstv_line_tex_setup(RstvLineTex *t, Vdp1 *regs, vdp1cmd_struct *cmd, u8 *ram, u8 *back_framebuffer)
{
   const int pmod = cmd->CMDPMOD;
   const int cmode = (pmod >> 3) & 7;
   int k;
   if (vdp1pixelsize != 2 || (pmod & 7) != 0 || (pmod & 0x8000) || (cmd->CMDCTRL & 7) > 3 || cmode > 5)
      return 0;
   if ((pmod & 0x0400) && ((pmod >> 9) & 3) == 3)
      return 0;
   t->cmode = cmode;
   t->SPD = (pmod & 0x40) != 0;
   t->endcodesEnabled = (pmod & 0x80) == 0;
   t->ends = t->endcodesEnabled && (cmode == 0 || cmode == 1 || cmode == 4 || cmode == 5);
   t->flip = (cmd->CMDCTRL & 0x30) >> 4;
   t->cw = characterWidth;
   t->ch = characterHeight;
   t->mesh = pmod & 0x0100;
   t->dil_skip = (vdp1interlace == 2) ? (((regs->FBCR >> 2) & 1) ? 0 : 1) : -1;
   t->cx1 = 0; t->cx2 = regs->systemclipX2; t->cy1 = 0; t->cy2 = regs->systemclipY2;
   if (pmod & 0x0400) {
      if (regs->userclipX1 > t->cx1) t->cx1 = regs->userclipX1;
      if (regs->userclipX2 < t->cx2) t->cx2 = regs->userclipX2;
      if (regs->userclipY1 > t->cy1) t->cy1 = regs->userclipY1;
      if (regs->userclipY2 < t->cy2) t->cy2 = regs->userclipY2;
   }
   t->charaddr = (u32)cmd->CMDSRCA << 3;
   t->bank = cmd->CMDCOLR;
   if (cmode == 1)
      for (k = 0; k < 16; k++)
         t->lut[k] = T1ReadWord(ram, (k * 2 + ((u32)t->bank << 3)) & 0x7FFFF);
   t->ram = ram;
   t->fb = (u16 *)back_framebuffer;
   return 1;
}

static void DrawLineFast(int x1, int y1, int x2, int y2, double linenumber, double texturestep, const RstvLineTex *t)
{
   int i, a, ax, ay, dx, dy;
   int endcodes = 0, previousStep = 123456789;
   u8 *const ram = t->ram;
   const int cw = t->cw;
   int ln = (int)linenumber;
   if (t->flip & 2)
      ln = t->ch - ln - 1;
   u32 rowaddr;
   switch (t->cmode) {
   case 0: case 1: rowaddr = t->charaddr + (ln * (cw >> 1)); break;
   case 5: rowaddr = t->charaddr + (ln * cw * 2); break;
   default: rowaddr = t->charaddr + (ln * cw); break;
   }

   /* plot one pixel; evaluates to 1 when the line must stop (second end code) */
#define RSTV_PLOT(PX, PY, PI) rstv_plot_fast(t, ram, rowaddr, texturestep, &endcodes, &previousStep, (PX), (PY), (PI))
   a = i = 0;
   dx = x2 - x1;
   dy = y2 - y1;
   ax = (dx >= 0) ? 1 : -1;
   ay = (dy >= 0) ? 1 : -1;
   if (abs(dx) > 999 || abs(dy) > 999)
      return;
   if (abs(dx) > abs(dy)) {
      if (ax != ay) dx = -dx;
      for (; x1 != x2; x1 += ax, i++) {
         if (RSTV_PLOT(x1, y1, i)) return;
         a += dy;
         if (abs(a) >= abs(dx)) {
            a -= dx;
            y1 += ay;
            i++;
            if (ax == ay) { if (RSTV_PLOT(x1 + ax, y1 - ay, i)) return; }
            else { if (RSTV_PLOT(x1, y1, i)) return; }
         }
      }
      RSTV_PLOT(x2, y2, i);
   } else {
      if (ax != ay) dy = -dy;
      for (; y1 != y2; y1 += ay, i++) {
         if (RSTV_PLOT(x1, y1, i)) return;
         a += dx;
         if (abs(a) >= abs(dy)) {
            a -= dy;
            x1 += ax;
            i++;
            if (ay == ax) { if (RSTV_PLOT(x1, y1, i)) return; }
            else { if (RSTV_PLOT(x1 - ax, y1 + ay, i)) return; }
         }
      }
      RSTV_PLOT(x2, y2, i);
   }
#undef RSTV_PLOT
}

static INLINE double interpolate(double start, double end, int numberofsteps) {

	double stepvalue = 0;

	if(numberofsteps == 0)
		return 1;

	stepvalue = (end - start) / numberofsteps;

	return stepvalue;
}

typedef union _COLOR { // xbgr x555
	struct {
#ifdef WORDS_BIGENDIAN
	u16 x:1;
	u16 b:5;
	u16 g:5;
	u16 r:5;
#else
     u16 r:5;
     u16 g:5;
     u16 b:5;
     u16 x:1;
#endif
	};
	u16 value;
} COLOR;

COLOR gouraudA;
COLOR gouraudB;
COLOR gouraudC;
COLOR gouraudD;

static void gouraudTable(u8* ram, Vdp1* regs, vdp1cmd_struct * cmd)
{
	int gouraudTableAddress;



	gouraudTableAddress = (((unsigned int)cmd->CMDGRDA) << 3);

   gouraudA.value = T1ReadWord(ram, gouraudTableAddress);
   gouraudB.value = T1ReadWord(ram, gouraudTableAddress + 2);
   gouraudC.value = T1ReadWord(ram, gouraudTableAddress + 4);
   gouraudD.value = T1ReadWord(ram, gouraudTableAddress + 6);
}

int xleft[1000];
int yleft[1000];
int xright[1000];
int yright[1000];

static int
storeLineCoords(int x, int y, int i, void *arrays, Vdp1* regs, vdp1cmd_struct * cmd, u8* ram, u8* back_framebuffer) {
	int **intArrays = arrays;

	intArrays[0][i] = x;
	intArrays[1][i] = y;

	return 0;
}

//skip objects that are completely outside of system clipping
int is_pre_clipped(s16 tl_x, s16 tl_y, s16 bl_x, s16 bl_y, s16 tr_x, s16 tr_y, s16 br_x, s16 br_y, Vdp1* regs)
{
   int y_val = regs->systemclipY2;

   if (vdp1interlace)
      y_val *= 2;

   //if all x values are to the left of the screen
   if ((tl_x < 0) &&
      (bl_x < 0) &&
      (tr_x < 0) &&
      (br_x < 0))
      return 1;

   //to the right
   if ((tl_x > regs->systemclipX2) &&
      (bl_x > regs->systemclipX2) &&
      (tr_x > regs->systemclipX2) &&
      (br_x > regs->systemclipX2))
      return 1;

   //above
   if ((tl_y < 0) &&
      (bl_y < 0) &&
      (tr_y < 0) &&
      (br_y < 0))
      return 1;

   //below
   if ((tl_y > y_val) &&
      (bl_y > y_val) &&
      (tr_y > y_val) &&
      (br_y > y_val))
      return 1;

   return 0;
}

// ReST-V: specialised axis-aligned sprite drawer for the common case (16bpp framebuffer, replace mode,
// no MSB-on, textured, colour modes 0-5, no "outside" user clipping). Same results as
// getpixel()+putpixel() per pixel, with clipping/interlace/mesh hoisted out of the pixel loop.
static int drawAxisAlignedFast(int tl_x, int tl_y, int ax, int ay, int rows, int cols,
   double xtexturestep, double ytexturestep, u8 * ram, Vdp1* regs, vdp1cmd_struct * cmd, u8* back_framebuffer)
{
   const int pmod = cmd->CMDPMOD;
   const int cmode = (pmod >> 3) & 0x7;
   const int shape = cmd->CMDCTRL & 0x7;
   int i, k;

   if (vdp1pixelsize != 2 || (pmod & 0x7) != 0 || (pmod & 0x8000) || (shape > 2 && shape != 4) || cmode > 5)
      return 0;
   if ((pmod & 0x0400) && ((pmod >> 9) & 0x3) == 0x3)
      return 0; // outside user clipping
   if (shape == 4)
   {
      // untextured polygon: every pixel is CMDCOLR (getpixel), visibility mask by colour mode
      static const u32 vis_tab[6] = { 0xf, 0xffff, 0x3f, 0x7f, 0xff, 0xffff };
      const u32 pix = cmd->CMDCOLR;
      const int SPD = ((pmod & 0x40) != 0);
      const int mesh = pmod & 0x0100;
      if (!((SPD || (pix & vis_tab[cmode])) && !(pix == 0 && !SPD)))
         return 1; // nothing visible
      int cx1 = 0, cx2 = regs->systemclipX2, cy1 = 0, cy2 = regs->systemclipY2;
      if (pmod & 0x0400) {
         if (regs->userclipX1 > cx1) cx1 = regs->userclipX1;
         if (regs->userclipX2 < cx2) cx2 = regs->userclipX2;
         if (regs->userclipY1 > cy1) cy1 = regs->userclipY1;
         if (regs->userclipY2 < cy2) cy2 = regs->userclipY2;
      }
      int x0 = (ax > 0) ? tl_x : tl_x - (cols - 1), x1 = x0 + cols - 1;
      if (x0 < cx1) x0 = cx1;
      if (x1 > cx2) x1 = cx2;
      if (x0 > x1)
         return 1;
      u16 * const fb = (u16 *)back_framebuffer;
      const int fb_limit = 0x40000 / 2;
      for (i = 0; i < rows; i++) {
         const int y = tl_y + i * ay;
         if (y < cy1 || y > cy2 || CheckDil(y, regs))
            continue;
         const int y2 = y / vdp1interlace;
         for (k = x0; k <= x1; k++) {
            const int idx = (y2 * vdp1width) + k;
            if (idx >= fb_limit)
               break;
            if (!(mesh && ((k ^ y2) & 1)))
               fb[idx] = (u16)pix;
         }
      }
      return 1;
   }

   const u32 characterAddress = (u32)cmd->CMDSRCA << 3;
   const u16 colorbank = cmd->CMDCOLR;
   const u32 colorlut = (u32)colorbank << 3;
   const int SPD = ((pmod & 0x40) != 0);
   const int endcodesEnabled = ((pmod & 0x80) == 0);
   const int flip = (cmd->CMDCTRL & 0x30) >> 4;
   const int mesh = pmod & 0x0100;
   const int cw = characterWidth, ch = characterHeight;

   // clip rectangle (inclusive)
   int cx1 = 0, cx2 = regs->systemclipX2, cy1 = 0, cy2 = regs->systemclipY2;
   if (pmod & 0x0400) {
      if (regs->userclipX1 > cx1) cx1 = regs->userclipX1;
      if (regs->userclipX2 < cx2) cx2 = regs->userclipX2;
      if (regs->userclipY1 > cy1) cy1 = regs->userclipY1;
      if (regs->userclipY2 < cy2) cy2 = regs->userclipY2;
   }
   if (cols > 1000)
      return 0;

   u16 * const fb = (u16 *)back_framebuffer;
   const int fb_limit = 0x40000 / 2;

   // ReST-V: per-command tables instead of per-pixel work (same results as the per-pixel version):
   // texel column of each screen column (same double expression), visible column range, 4bpp lookup table
   static int colmap[1000];
   for (k = 0; k < cols; k++) {
      const int currentStep = (int)k * xtexturestep;
      colmap[k] = currentStep;
   }
   int k0, k1; // columns k0..k1 are inside cx1..cx2
   if (ax > 0) { k0 = cx1 - tl_x; k1 = cx2 - tl_x; }
   else { k0 = tl_x - cx2; k1 = tl_x - cx1; }
   if (k0 < 0) k0 = 0;
   if (k1 > cols - 1) k1 = cols - 1;
   if (k0 > k1)
      return 1;
   u16 lut[16];
   if (cmode == 1)
      for (k = 0; k < 16; k++)
         lut[k] = T1ReadWord(ram, (k * 2 + colorlut) & 0x7FFFF);
   // end codes only matter for these modes; without them the columns before k0 can be skipped
   const int ends = endcodesEnabled && (cmode == 0 || cmode == 1 || cmode == 4 || cmode == 5);

   for (i = 0; i < rows; i++) {
      const int y = tl_y + i * ay;
      if (y < cy1 || y > cy2)
         continue;
      if (CheckDil(y, regs))
         continue;
      const int y2 = y / vdp1interlace;
      int ln = (int)(ytexturestep * i);
      if (flip & 2)
         ln = ch - ln - 1;

      u32 rowaddr;
      switch (cmode) {
      case 0: case 1: rowaddr = characterAddress + (ln * (cw >> 1)); break;
      case 5: rowaddr = characterAddress + (ln * cw * 2); break;
      default: rowaddr = characterAddress + (ln * cw); break;
      }
      const int rowbase = y2 * vdp1width;
      if (rowbase >= fb_limit)
         continue;
      // last column whose framebuffer index is below fb_limit
      int kend = k1;
      if (ax > 0) { const int lim = fb_limit - 1 - rowbase - tl_x; if (lim < kend) kend = lim; }
      int kk0 = k0;
      if (ax < 0) { const int lo = tl_x - (fb_limit - 1 - rowbase); if (lo > kk0) kk0 = lo; }
      if (kend < kk0)
         continue;

      int endcodes = 0, previousStep = 123456789;
      u16 *const frow = fb + rowbase;
#define RSTV_V1_COLS(READ, ISEND, WRITE_IF, PIX)                                            \
      for (k = ends ? 0 : kk0; k <= kend; k++) {                                             \
         const int step = colmap[k];                                                       \
         const int col = (flip & 1) ? (cw - step - 1) : step;                              \
         u32 dot = READ;                                                                   \
         if (ISEND) {                                                                      \
            if (step != previousStep) {                                                    \
               previousStep = step;                                                        \
               if (++endcodes == 2)                                                        \
                  break;                                                                   \
            }                                                                              \
            continue;                                                                      \
         }                                                                                 \
         if (k < kk0)                                                                      \
            continue;                                                                      \
         const int x = tl_x + k * ax;                                                      \
         if ((SPD || (WRITE_IF)) && !(mesh && ((x ^ y2) & 1)))                             \
            frow[x] = (u16)(PIX);                                                          \
      }
      switch (cmode) {
      case 0:
         RSTV_V1_COLS(((T1ReadByte(ram, (rowaddr + ((u32)col >> 1)) & 0x7FFFF) >> ((col & 1) ? 0 : 4)) & 0xF),
                      ends && dot == 0xf, dot != 0, (colorbank & 0xfff0) | dot)
         break;
      case 1:
         RSTV_V1_COLS(((T1ReadByte(ram, (rowaddr + ((u32)col >> 1)) & 0x7FFFF) >> ((col & 1) ? 0 : 4)) & 0xF),
                      ends && dot == 0xf, dot != 0 && lut[dot] != 0, lut[dot])
         break;
      case 2:
         RSTV_V1_COLS((T1ReadByte(ram, (rowaddr + col) & 0x7FFFF) & 0x3F), 0,
                      dot != 0 && !(endcodesEnabled && dot == 63),
                      (endcodesEnabled && dot == 63) ? (colorbank & 0xffc0) : ((colorbank & 0xffc0) | dot))
         break;
      case 3:
         RSTV_V1_COLS((T1ReadByte(ram, (rowaddr + col) & 0x7FFFF) & 0x7F), 0, dot != 0, (colorbank & 0xff80) | dot)
         break;
      case 4:
         RSTV_V1_COLS(T1ReadByte(ram, (rowaddr + col) & 0x7FFFF), ends && dot == 0xff, dot != 0, (colorbank & 0xff00) | dot)
         break;
      default: // 5
         RSTV_V1_COLS(T1ReadWord(ram, (rowaddr + 2 * col) & 0x7FFFF), ends && dot == 0x7fff, (dot & 0x8000) != 0, dot)
         break;
      }
#undef RSTV_V1_COLS
   }
   return 1;
}


/* ---- ReST-V: VDP1 recording for the GPU sprite renderer (ADR-023, rstv_vdp1rec.h) ---- */
int rstv_v1rec, rstv_v1unsup, rstv_v1n;
int rstv_v1_cc_ok, rstv_v1_ccn;
RstvV1Item rstv_v1list[RSTV_V1_MAX];
int rstv_vdp1_gpu_on, rstv_v1ready, rstv_v1ready_target, rstv_vdp1_back_idx, rstv_vdp1_gpu_drawn[2];
int rstv_v1list_fmt;
unsigned rstv_v1_gpu_frames, rstv_v1_cpu_frames, rstv_v1_why;

static void rstv_v1_unsupported(unsigned why)
{
   rstv_v1unsup = 1;
   rstv_v1_why = why;
}

/* Clip rectangle and commit of the item at rstv_v1list[rstv_v1n]: system clip, intersected with the user clip
   (mode 2), or (ADR-063, mode 3 "outside") the system clip minus the user rectangle as up to 4 bands, one copy of the
   item per band */
static void rstv_v1_commit(int pmod, Vdp1 *regs)
{
   RstvV1Item *it = &rstv_v1list[rstv_v1n];
   const int sx2 = regs->systemclipX2, sy2 = regs->systemclipY2;
   if ((pmod & 0x0400) && ((pmod >> 9) & 3) == 3)
   {
      const int ux1 = regs->userclipX1, uy1 = regs->userclipY1, ux2 = regs->userclipX2, uy2 = regs->userclipY2;
      int band[4][4], nb = 0, k;
      if (ux1 > ux2 || uy1 > uy2 || ux2 < 0 || uy2 < 0 || ux1 > sx2 || uy1 > sy2)
      {
         band[nb][0] = 0; band[nb][1] = 0; band[nb][2] = sx2; band[nb][3] = sy2; nb++; /* empty user rect: all */
      }
      else
      {
         const int ry1 = uy1 > 0 ? uy1 : 0, ry2 = uy2 < sy2 ? uy2 : sy2;
         if (uy1 > 0) { band[nb][0] = 0; band[nb][1] = 0; band[nb][2] = sx2; band[nb][3] = uy1 - 1; nb++; }
         if (uy2 < sy2) { band[nb][0] = 0; band[nb][1] = uy2 + 1; band[nb][2] = sx2; band[nb][3] = sy2; nb++; }
         if (ux1 > 0) { band[nb][0] = 0; band[nb][1] = ry1; band[nb][2] = ux1 - 1; band[nb][3] = ry2; nb++; }
         if (ux2 < sx2) { band[nb][0] = ux2 + 1; band[nb][1] = ry1; band[nb][2] = sx2; band[nb][3] = ry2; nb++; }
      }
      if (rstv_v1n + nb > RSTV_V1_MAX)
      {
         rstv_v1_unsupported(7);
         return;
      }
      for (k = 0; k < nb; k++)
      {
         RstvV1Item *o = &rstv_v1list[rstv_v1n];
         if (o != it)
            *o = *it;
         o->clip[0] = (s16)band[k][0]; o->clip[1] = (s16)band[k][1]; o->clip[2] = (s16)band[k][2]; o->clip[3] = (s16)band[k][3];
         rstv_v1n++;
      }
      return;
   }
   int cx1 = 0, cx2 = sx2, cy1 = 0, cy2 = sy2;
   if (pmod & 0x0400) {
      if (regs->userclipX1 > cx1) cx1 = regs->userclipX1;
      if (regs->userclipX2 < cx2) cx2 = regs->userclipX2;
      if (regs->userclipY1 > cy1) cy1 = regs->userclipY1;
      if (regs->userclipY2 < cy2) cy2 = regs->userclipY2;
   }
   it->clip[0] = (s16)cx1; it->clip[1] = (s16)cy1; it->clip[2] = (s16)cx2; it->clip[3] = (s16)cy2;
   rstv_v1n++;
}

/* Distinct sprite patterns of this frame and their texel area: the GPU renderer keeps one frame's patterns in a
   1024 x 1024 shelf atlas (v1gpu.c). A frame that would not fit is drawn by the CPU instead of losing sprites
   (thuntk pushman train, enemy line-up) */
#define RSTV_V1PAT_N 2048
static u32 rstv_v1pat[RSTV_V1PAT_N];
static u32 rstv_v1pat_gen = 1, rstv_v1pat_tag[RSTV_V1PAT_N], rstv_v1pat_area;
static void rstv_v1pat_reset(void) { rstv_v1pat_gen++; rstv_v1pat_area = 0; }
static void rstv_v1pat_add(u32 srca, u32 size, u32 colr, int cmode)
{
   if (cmode != 1) /* same masking as the atlas cache (v1gpu.c lookup) */
      colr &= cmode == 0 ? 0xfff0 : cmode == 2 ? 0xffc0 : cmode == 3 ? 0xff80 : cmode == 4 ? 0xff00 : 0;
   const u32 key = (srca << 16) ^ (size << 3) ^ (colr * 2654435761u) ^ (u32)cmode;
   u32 h = (key * 2654435761u) >> 21; /* 11 bits */
   for (int k = 0; k < 64; k++, h = (h + 1) & (RSTV_V1PAT_N - 1)) {
      if (rstv_v1pat_tag[h] == rstv_v1pat_gen && rstv_v1pat[h] == key)
         return;
      if (rstv_v1pat_tag[h] != rstv_v1pat_gen) {
         rstv_v1pat_tag[h] = rstv_v1pat_gen;
         rstv_v1pat[h] = key;
         rstv_v1pat_area += (((size >> 8) & 0x3F) * 8 + 2) * ((size & 0xFF) + 2);
         if (rstv_v1pat_area > 1024 * 1024 * 6 / 10)
            rstv_v1_unsupported(11);
         return;
      }
   }
   rstv_v1_unsupported(11);
}

static void rstv_v1rec_quad(s16 tl_x, s16 tl_y, s16 bl_x, s16 bl_y, s16 tr_x, s16 tr_y, s16 br_x, s16 br_y,
   u8 *ram, Vdp1 *regs, vdp1cmd_struct *cmd)
{
   const int pmod = cmd->CMDPMOD, shape = cmd->CMDCTRL & 7, cmode = (pmod >> 3) & 7;
   if (rstv_v1unsup)
      return;
   if (!(vdp1pixelsize == 2 && vdp1interlace == 1) && !(rstv_v1list_fmt & 3)) { rstv_v1_unsupported(1); return; } /* ADR-100: half-size formats */
   if (pmod & 0x8000) {
      /* ADR-118: MSB on sets bit 15 of the framebuffer pixel: the GPU renderer reads the framebuffer copy too (ADR-115).
         A run of MSB-on commands needs one copy (setting a bit twice is the same), so only a run's first one counts */
      const int run = rstv_v1n > 0 && (rstv_v1list[rstv_v1n - 1].pmod & 0x8000);
      if (!rstv_v1_cc_ok || (rstv_v1list_fmt & 4) || (!run && ++rstv_v1_ccn > RSTV_V1_CC_MAX)) { rstv_v1_unsupported(2); return; }
   }
   else if ((pmod & 7) != 0 && (pmod & 7) != 4) { /* shadow / half luminance / half transparency */
      /* ADR-115: the GPU renderer reads a copy of the framebuffer for these (one copy per run), so only a few a frame */
      if ((pmod & 7) > 3 || !rstv_v1_cc_ok || (rstv_v1list_fmt & 4) || ++rstv_v1_ccn > RSTV_V1_CC_MAX) { rstv_v1_unsupported(3); return; }
   }
   if (cmode > 5) { rstv_v1_unsupported(5); return; }
   if ((pmod & 7) == 4 && shape <= 3 && cmode != 5 && cmode != 1) { rstv_v1_unsupported(6); return; } /* palette gouraud */
   if (rstv_v1n >= RSTV_V1_MAX) { rstv_v1_unsupported(7); return; }
   if (shape <= 3) {
      rstv_v1pat_add(cmd->CMDSRCA, cmd->CMDSIZE, cmd->CMDCOLR, cmode);
      if (rstv_v1unsup)
         return;
   }
   RstvV1Item *it = &rstv_v1list[rstv_v1n];
   it->kind = shape <= 3 ? RSTV_V1_TEX : RSTV_V1_POLY;
   if (it->kind == RSTV_V1_POLY) {
      /* untextured polygon: CMDCOLR, visible by colour mode as in drawAxisAlignedFast */
      static const u32 vis_tab[6] = { 0xf, 0xffff, 0x3f, 0x7f, 0xff, 0xffff };
      const u32 pix = cmd->CMDCOLR;
      const int SPD = (pmod & 0x40) != 0;
      if (!((SPD || (pix & vis_tab[cmode])) && !(pix == 0 && !SPD)))
         return;
   }
   it->axis = tl_x == bl_x && tr_x == br_x && tl_y == tr_y && bl_y == br_y;
   it->x[0] = tl_x; it->y[0] = tl_y; it->x[1] = tr_x; it->y[1] = tr_y;
   it->x[2] = bl_x; it->y[2] = bl_y; it->x[3] = br_x; it->y[3] = br_y;
   it->pmod = (u16)pmod; it->colr = cmd->CMDCOLR; it->srca = cmd->CMDSRCA; it->size = cmd->CMDSIZE; it->ctrl = cmd->CMDCTRL;
   if ((pmod & 7) == 4) {
      const u32 ga = ((u32)cmd->CMDGRDA) << 3;
      it->grd[0] = T1ReadWord(ram, ga);     /* A = tl */
      it->grd[1] = T1ReadWord(ram, ga + 2); /* B = tr */
      it->grd[3] = T1ReadWord(ram, ga + 4); /* C = br */
      it->grd[2] = T1ReadWord(ram, ga + 6); /* D = bl */
   } else {
      it->grd[0] = it->grd[1] = it->grd[2] = it->grd[3] = 0;
   }
   rstv_v1_commit(pmod, regs);
}

/* ReST-V (ADR-041): line / polyline segment, drawn by the GPU renderer as a one pixel wide quad */
static void rstv_v1rec_line(int x1, int y1, int x2, int y2, int ga, int gb, u8 *ram, Vdp1 *regs, vdp1cmd_struct *cmd)
{
   const int pmod = cmd->CMDPMOD, cmode = (pmod >> 3) & 7;
   if (rstv_v1unsup)
      return;
   if (!(vdp1pixelsize == 2 && vdp1interlace == 1) && !(rstv_v1list_fmt & 3)) { rstv_v1_unsupported(1); return; } /* ADR-100: half-size formats */
   if (pmod & 0x8000) { rstv_v1_unsupported(2); return; }
   if ((pmod & 7) != 0 && (pmod & 7) != 4) { rstv_v1_unsupported(3); return; }
   if (cmode > 5) { rstv_v1_unsupported(5); return; }
   if (rstv_v1n >= RSTV_V1_MAX) { rstv_v1_unsupported(7); return; }
   {
      static const u32 vis_tab[6] = { 0xf, 0xffff, 0x3f, 0x7f, 0xff, 0xffff };
      const u32 pix = cmd->CMDCOLR;
      const int SPD = (pmod & 0x40) != 0;
      if (!((SPD || (pix & vis_tab[cmode])) && !(pix == 0 && !SPD)))
         return;
   }
   RstvV1Item *it = &rstv_v1list[rstv_v1n];
   it->kind = RSTV_V1_LINE;
   it->axis = 0;
   it->x[0] = (s16)x1; it->y[0] = (s16)y1; it->x[1] = (s16)x2; it->y[1] = (s16)y2;
   it->x[2] = it->x[3] = it->y[2] = it->y[3] = 0;
   it->pmod = (u16)pmod; it->colr = cmd->CMDCOLR; it->srca = cmd->CMDSRCA; it->size = cmd->CMDSIZE; it->ctrl = cmd->CMDCTRL;
   if ((pmod & 7) == 4) {
      const u32 g = ((u32)cmd->CMDGRDA) << 3;
      it->grd[0] = T1ReadWord(ram, g + ga * 2);
      it->grd[1] = T1ReadWord(ram, g + gb * 2);
   } else
      it->grd[0] = it->grd[1] = 0;
   it->grd[2] = it->grd[3] = 0;
   rstv_v1_commit(pmod, regs);
}

//a real vdp1 draws with arbitrary lines
//this is why endcodes are possible
//this is also the reason why half-transparent shading causes moire patterns
//and the reason why gouraud shading can be applied to a single line draw command
#ifdef RSTV_VDP1_STATS
/* ReST-V harness: VDP1 quad statistics (count, area) by path */
unsigned long long rstv_v1s_n[8], rstv_v1s_px[8], rstv_v1s_ns[8];
static int rstv_v1s_last;
unsigned long long rstv_rot_stats[16];
static const char *rstv_v1s_name[8] = { "fast", "axis-ccalc", "axis-other", "nonaxis-tex", "nonaxis-poly", "axis-gouraud", "nonaxis-gouraud", "axis-ccalc-shadow" };
void rstv_vdp1_stats_print(void)
{
   int i;
   for (i = 0; i < 8; i++)
      if (rstv_v1s_n[i])
         printf("vdp1 %-18s n %10llu px %12llu ms %8.1f\n", rstv_v1s_name[i], rstv_v1s_n[i], rstv_v1s_px[i], rstv_v1s_ns[i] / 1e6);
}
static void rstv_v1s_add(s16 tl_x, s16 tl_y, s16 bl_x, s16 bl_y, s16 tr_x, s16 tr_y, s16 br_x, s16 br_y, vdp1cmd_struct * cmd)
{
   const int pmod = cmd->CMDPMOD;
   const int axis = tl_x == bl_x && tr_x == br_x && tl_y == tr_y && bl_y == br_y;
   const int textured = (cmd->CMDCTRL & 0xF) <= 3;
   int minx = tl_x, maxx = tl_x, miny = tl_y, maxy = tl_y, b;
   const int xs[4] = { tl_x, bl_x, tr_x, br_x }, ys[4] = { tl_y, bl_y, tr_y, br_y };
   for (b = 0; b < 4; b++) { if (xs[b] < minx) minx = xs[b]; if (xs[b] > maxx) maxx = xs[b]; if (ys[b] < miny) miny = ys[b]; if (ys[b] > maxy) maxy = ys[b]; }
   const unsigned long long area = (unsigned long long)(maxx - minx + 1) * (maxy - miny + 1);
   int k;
   if (pmod & 4) k = axis ? 5 : 6;
   else if (!axis) k = textured ? 3 : 4;
   else if (pmod & 7) k = ((pmod & 7) == 1) ? 7 : 1;
   else if (vdp1pixelsize != 2 || (pmod & 0x8000) || (cmd->CMDCTRL & 7) > 2 || ((pmod >> 3) & 7) > 5 ||
            ((pmod & 0x0400) && ((pmod >> 9) & 0x3) == 0x3)) k = 2;
   else k = 0;
   rstv_v1s_n[k]++;
   rstv_v1s_px[k] += area;
   rstv_v1s_last = k;
}
#endif
#ifdef RSTV_VDP1_STATS
#include <time.h>
static void drawQuadImpl(s16 tl_x, s16 tl_y, s16 bl_x, s16 bl_y, s16 tr_x, s16 tr_y, s16 br_x, s16 br_y, u8 * ram, Vdp1* regs, vdp1cmd_struct * cmd, u8* back_framebuffer);
static void drawQuad(s16 tl_x, s16 tl_y, s16 bl_x, s16 bl_y, s16 tr_x, s16 tr_y, s16 br_x, s16 br_y, u8 * ram, Vdp1* regs, vdp1cmd_struct * cmd, u8* back_framebuffer)
{
   rstv_v1s_last = -1;
#ifdef __vita__
   const u64 a = RSTV_PROF_TF(); /* per quad: fine only (ADR-075) */
   drawQuadImpl(tl_x, tl_y, bl_x, bl_y, tr_x, tr_y, br_x, br_y, ram, regs, cmd, back_framebuffer);
   if (rstv_v1s_last >= 0)
      rstv_v1s_ns[rstv_v1s_last] += (RSTV_PROF_TF() - a) * 1000ULL;
#else
   struct timespec a, b;
   clock_gettime(CLOCK_MONOTONIC, &a);
   drawQuadImpl(tl_x, tl_y, bl_x, bl_y, tr_x, tr_y, br_x, br_y, ram, regs, cmd, back_framebuffer);
   clock_gettime(CLOCK_MONOTONIC, &b);
   if (rstv_v1s_last >= 0)
      rstv_v1s_ns[rstv_v1s_last] += (unsigned long long)((b.tv_sec - a.tv_sec) * 1000000000LL + (b.tv_nsec - a.tv_nsec));
#endif
}
#define drawQuad drawQuadImpl
#endif
static void drawQuad(s16 tl_x, s16 tl_y, s16 bl_x, s16 bl_y, s16 tr_x, s16 tr_y, s16 br_x, s16 br_y, u8 * ram, Vdp1* regs, vdp1cmd_struct * cmd, u8* back_framebuffer){

	int totalleft;
	int totalright;
	int total;
	int i;
	int *intarrays[2];

	COLOR_PARAMS topLeftToBottomLeftColorStep = {0,0,0}, topRightToBottomRightColorStep = {0,0,0};
		
	//how quickly we step through the line arrays
	double leftLineStep = 1;
	double rightLineStep = 1; 

	//a lookup table for the gouraud colors
	COLOR colors[4];

   if (is_pre_clipped(tl_x, tl_y, bl_x, bl_y, tr_x, tr_y, br_x, br_y, regs))
      return;
   if (rstv_v1rec) {
      rstv_v1rec_quad(tl_x, tl_y, bl_x, bl_y, tr_x, tr_y, br_x, br_y, ram, regs, cmd);
      return;
   }
#ifdef RSTV_VDP1_STATS
   rstv_v1s_add(tl_x, tl_y, bl_x, bl_y, tr_x, tr_y, br_x, br_y, cmd);
#endif

	characterWidth = ((cmd->CMDSIZE >> 8) & 0x3F) * 8;
   characterHeight = cmd->CMDSIZE & 0xFF;

   // ReST-V fast path: axis-aligned quads (normal / scaled sprites, rectangles) without gouraud.
   // Produces the same pixels as the generic line walker below (same double texture stepping,
   // same "stop the line after the 2nd end code" rule), without per-pixel callbacks.
   if (tl_x == bl_x && tr_x == br_x && tl_y == tr_y && bl_y == br_y &&
       !(cmd->CMDPMOD & (1 << 2)) && (cmd->CMDPMOD & 0x7) < 4 &&
       abs(tr_x - tl_x) <= 999 && abs(bl_y - tl_y) <= 999)
   {
      const int ay = (bl_y >= tl_y) ? 1 : -1;
      const int ax = (tr_x >= tl_x) ? 1 : -1;
      const int rows = abs(bl_y - tl_y) + 1;
      const int cols = abs(tr_x - tl_x) + 1;
      const double ytexturestep = interpolate(0, characterHeight, rows);
      const double xtexturestep = interpolate(0, characterWidth, cols);
      const int is8 = (vdp1pixelsize != 2);
#ifdef RSTV_VDP1_CHECK
      static u8 check_fb[0x40000];
      memcpy(check_fb, back_framebuffer, 0x40000);
      int fast_done = drawAxisAlignedFast(tl_x, tl_y, ax, ay, rows, cols, xtexturestep, ytexturestep, ram, regs, cmd, check_fb);
#else
      if (drawAxisAlignedFast(tl_x, tl_y, ax, ay, rows, cols, xtexturestep, ytexturestep, ram, regs, cmd, back_framebuffer))
         return;
#endif
      for (i = 0; i < rows; i++) {
         const int y = tl_y + i * ay;
         const double linenumber = ytexturestep * i;
         int endcodes = 0, previousStep = 123456789;
         int x = tl_x, k;
         for (k = 0; k < cols; k++, x += ax) {
            int currentStep = (int)k * xtexturestep;
            if (getpixel(linenumber, currentStep, cmd, ram)) {
               if (currentStep != previousStep) {
                  previousStep = currentStep;
                  endcodes++;
               }
            } else if (!is8) {
               putpixel(x, y, regs, cmd, back_framebuffer);
            } else {
               putpixel8(x, y, regs, cmd, back_framebuffer);
            }
            if (endcodes == 2)
               break;
         }
      }
#ifdef RSTV_VDP1_CHECK
      if (fast_done && memcmp(check_fb, back_framebuffer, 0x40000) != 0) {
         int d;
         for (d = 0; d < 0x20000; d++)
            if (((u16 *)check_fb)[d] != ((u16 *)back_framebuffer)[d])
               break;
         fprintf(stderr, "vdp1 mismatch: ctrl %04x pmod %04x size %04x srca %04x colr %04x tl %d,%d rows %d cols %d xs %f ys %f idx %d (x %d y %d) fast %04x ref %04x int %d\n",
            cmd->CMDCTRL, cmd->CMDPMOD, cmd->CMDSIZE, cmd->CMDSRCA, cmd->CMDCOLR, tl_x, tl_y, rows, cols, xtexturestep,
            ytexturestep, d, d % vdp1width, d / vdp1width, ((u16 *)check_fb)[d], ((u16 *)back_framebuffer)[d], vdp1interlace);
      }
#endif
      return;
   }

	intarrays[0] = xleft; intarrays[1] = yleft;
   totalleft = iterateOverLine(tl_x, tl_y, bl_x, bl_y, 0, intarrays, storeLineCoords, regs, cmd, ram, back_framebuffer);
	intarrays[0] = xright; intarrays[1] = yright;
   totalright = iterateOverLine(tr_x, tr_y, br_x, br_y, 0, intarrays, storeLineCoords, regs, cmd, ram, back_framebuffer);

	//just for now since burning rangers will freeze up trying to draw huge shapes
	if(totalleft == INT_MAX || totalright == INT_MAX)
		return;

	total = totalleft > totalright ? totalleft : totalright;


   if (cmd->CMDPMOD & (1 << 2)) {

		gouraudTable(ram, regs, cmd);

		{ colors[0] = gouraudA; colors[1] = gouraudD; colors[2] = gouraudB; colors[3] = gouraudC; }

		topLeftToBottomLeftColorStep.r = interpolate(colors[0].r,colors[1].r,total);
		topLeftToBottomLeftColorStep.g = interpolate(colors[0].g,colors[1].g,total);
		topLeftToBottomLeftColorStep.b = interpolate(colors[0].b,colors[1].b,total);

		topRightToBottomRightColorStep.r = interpolate(colors[2].r,colors[3].r,total);
		topRightToBottomRightColorStep.g = interpolate(colors[2].g,colors[3].g,total);
		topRightToBottomRightColorStep.b = interpolate(colors[2].b,colors[3].b,total);
	}

	//we have to step the equivalent of less than one pixel on the shorter side
	//to make sure textures stretch properly and the shape is correct
	if(total == totalleft && totalleft != totalright) {
		//left side is larger
		leftLineStep = 1;
		rightLineStep = (double)totalright / totalleft;
	}
	else if(totalleft != totalright){
		//right side is larger
		rightLineStep = 1;
		leftLineStep = (double)totalleft / totalright;
	}

	RstvLineTex rstv_lt;
	const int rstv_fast_lines = rstv_line_tex_setup(&rstv_lt, regs, cmd, ram, back_framebuffer);
	for(i = 0; i < total; i++) {

		int xlinelength;

		double xtexturestep;
		double ytexturestep;

		COLOR_PARAMS rightColumnColor;

		COLOR_PARAMS leftToRightStep = {0,0,0};

		//get the length of the line we are about to draw
		xlinelength = iterateOverLine(
			xleft[(int)(i*leftLineStep)],
			yleft[(int)(i*leftLineStep)],
			xright[(int)(i*rightLineStep)],
			yright[(int)(i*rightLineStep)],
         1, NULL, NULL, regs, cmd, ram, back_framebuffer);

		//so from 0 to the width of the texture / the length of the line is how far we need to step
		xtexturestep=interpolate(0,characterWidth,xlinelength);

		//now we need to interpolate the y texture coordinate across multiple lines
		ytexturestep=interpolate(0,characterHeight,total);

		//gouraud interpolation
		if(cmd->CMDPMOD & (1 << 2)) {

			//for each new line we need to step once more through each column
			//and add the orignal color + the number of steps taken times the step value to the bottom of the shape
			//to get the current colors to use to interpolate across the line

			leftColumnColor.r = colors[0].r +(topLeftToBottomLeftColorStep.r*i);
			leftColumnColor.g = colors[0].g +(topLeftToBottomLeftColorStep.g*i);
			leftColumnColor.b = colors[0].b +(topLeftToBottomLeftColorStep.b*i);

			rightColumnColor.r = colors[2].r +(topRightToBottomRightColorStep.r*i);
			rightColumnColor.g = colors[2].g +(topRightToBottomRightColorStep.g*i);
			rightColumnColor.b = colors[2].b +(topRightToBottomRightColorStep.b*i);

			//interpolate colors across to get the right step values
			leftToRightStep.r = interpolate(leftColumnColor.r,rightColumnColor.r,xlinelength);
			leftToRightStep.g = interpolate(leftColumnColor.g,rightColumnColor.g,xlinelength);
			leftToRightStep.b = interpolate(leftColumnColor.b,rightColumnColor.b,xlinelength);
		}

		if (rstv_fast_lines) {
			DrawLineFast(xleft[(int)(i*leftLineStep)], yleft[(int)(i*leftLineStep)],
				xright[(int)(i*rightLineStep)], yright[(int)(i*rightLineStep)], ytexturestep*i, xtexturestep, &rstv_lt);
			continue;
		}
		DrawLine(
			xleft[(int)(i*leftLineStep)],
			yleft[(int)(i*leftLineStep)],
			xright[(int)(i*rightLineStep)],
			yright[(int)(i*rightLineStep)],
			1,
			ytexturestep*i, 
			xtexturestep,
			leftToRightStep.r,
			leftToRightStep.g,
			leftToRightStep.b,
         regs,
         cmd,
         ram, back_framebuffer
			);
	}
}
#ifdef RSTV_VDP1_STATS
#undef drawQuad
#endif

void VIDSoftVdp1NormalSpriteDraw(u8 * ram, Vdp1 * regs, u8 * back_framebuffer) {

	s16 topLeftx,topLefty,topRightx,topRighty,bottomRightx,bottomRighty,bottomLeftx,bottomLefty;
	int spriteWidth;
	int spriteHeight;
   vdp1cmd_struct cmd;
	Vdp1ReadCommand(&cmd, regs->addr, ram);

	topLeftx = cmd.CMDXA + regs->localX;
	topLefty = cmd.CMDYA + regs->localY;
	spriteWidth = ((cmd.CMDSIZE >> 8) & 0x3F) * 8;
	spriteHeight = cmd.CMDSIZE & 0xFF;

	topRightx = topLeftx + (spriteWidth - 1);
	topRighty = topLefty;
	bottomRightx = topLeftx + (spriteWidth - 1);
	bottomRighty = topLefty + (spriteHeight - 1);
	bottomLeftx = topLeftx;
	bottomLefty = topLefty + (spriteHeight - 1);

   drawQuad(topLeftx, topLefty, bottomLeftx, bottomLefty, topRightx, topRighty, bottomRightx, bottomRighty, ram, regs, &cmd, back_framebuffer);
}

void VIDSoftVdp1ScaledSpriteDraw(u8* ram, Vdp1*regs, u8 * back_framebuffer){

	s32 topLeftx,topLefty,topRightx,topRighty,bottomRightx,bottomRighty,bottomLeftx,bottomLefty;
	int x0,y0,x1,y1;
   vdp1cmd_struct cmd;
   Vdp1ReadCommand(&cmd, regs->addr, ram);

	x0 = cmd.CMDXA + regs->localX;
	y0 = cmd.CMDYA + regs->localY;

	switch ((cmd.CMDCTRL >> 8) & 0xF)
	{
	case 0x0: // Only two coordinates
	default:
		x1 = ((int)cmd.CMDXC) - x0 + regs->localX + 1;
		y1 = ((int)cmd.CMDYC) - y0 + regs->localY + 1;
		break;
	case 0x5: // Upper-left
		x1 = ((int)cmd.CMDXB) + 1;
		y1 = ((int)cmd.CMDYB) + 1;
		break;
	case 0x6: // Upper-Center
		x1 = ((int)cmd.CMDXB);
		y1 = ((int)cmd.CMDYB);
		x0 = x0 - x1/2;
		x1++;
		y1++;
		break;
	case 0x7: // Upper-Right
		x1 = ((int)cmd.CMDXB);
		y1 = ((int)cmd.CMDYB);
		x0 = x0 - x1;
		x1++;
		y1++;
		break;
	case 0x9: // Center-left
		x1 = ((int)cmd.CMDXB);
		y1 = ((int)cmd.CMDYB);
		y0 = y0 - y1/2;
		x1++;
		y1++;
		break;
	case 0xA: // Center-center
		x1 = ((int)cmd.CMDXB);
		y1 = ((int)cmd.CMDYB);
		x0 = x0 - x1/2;
		y0 = y0 - y1/2;
		x1++;
		y1++;
		break;
	case 0xB: // Center-right
		x1 = ((int)cmd.CMDXB);
		y1 = ((int)cmd.CMDYB);
		x0 = x0 - x1;
		y0 = y0 - y1/2;
		x1++;
		y1++;
		break;
	case 0xD: // Lower-left
		x1 = ((int)cmd.CMDXB);
		y1 = ((int)cmd.CMDYB);
		y0 = y0 - y1;
		x1++;
		y1++;
		break;
	case 0xE: // Lower-center
		x1 = ((int)cmd.CMDXB);
		y1 = ((int)cmd.CMDYB);
		x0 = x0 - x1/2;
		y0 = y0 - y1;
		x1++;
		y1++;
		break;
	case 0xF: // Lower-right
		x1 = ((int)cmd.CMDXB);
		y1 = ((int)cmd.CMDYB);
		x0 = x0 - x1;
		y0 = y0 - y1;
		x1++;
		y1++;
		break;
	}

	topLeftx = x0;
	topLefty = y0;

	topRightx = x1+x0 - 1;
	topRighty = topLefty;

	bottomRightx = x1+x0 - 1;
	bottomRighty = y1+y0 - 1;

	bottomLeftx = topLeftx;
	bottomLefty = y1+y0 - 1;

   drawQuad(topLeftx, topLefty, bottomLeftx, bottomLefty, topRightx, topRighty, bottomRightx, bottomRighty, ram, regs, &cmd, back_framebuffer);
}

void VIDSoftVdp1DistortedSpriteDraw(u8* ram, Vdp1*regs, u8 * back_framebuffer) {

	s32 xa,ya,xb,yb,xc,yc,xd,yd;
   vdp1cmd_struct cmd;

   Vdp1ReadCommand(&cmd, regs->addr, ram);

    xa = (s32)(cmd.CMDXA + regs->localX);
    ya = (s32)(cmd.CMDYA + regs->localY);

    xb = (s32)(cmd.CMDXB + regs->localX);
    yb = (s32)(cmd.CMDYB + regs->localY);

    xc = (s32)(cmd.CMDXC + regs->localX);
    yc = (s32)(cmd.CMDYC + regs->localY);

    xd = (s32)(cmd.CMDXD + regs->localX);
    yd = (s32)(cmd.CMDYD + regs->localY);

    drawQuad(xa, ya, xd, yd, xb, yb, xc, yc, ram, regs, &cmd, back_framebuffer);
}

static void gouraudLineSetup(double * redstep, double * greenstep, double * bluestep, int length, COLOR table1, COLOR table2, u8* ram, Vdp1* regs, vdp1cmd_struct * cmd, u8 * back_framebuffer) {

	gouraudTable(ram ,regs, cmd);

	*redstep =interpolate(table1.r,table2.r,length);
	*greenstep =interpolate(table1.g,table2.g,length);
	*bluestep =interpolate(table1.b,table2.b,length);

	leftColumnColor.r = table1.r;
	leftColumnColor.g = table1.g;
	leftColumnColor.b = table1.b;
}

void VIDSoftVdp1PolylineDraw(u8* ram, Vdp1*regs, u8 * back_framebuffer)
{
	int X[4];
	int Y[4];
	double redstep = 0, greenstep = 0, bluestep = 0;
	int length;
   vdp1cmd_struct cmd;

   Vdp1ReadCommand(&cmd, regs->addr, ram);

	X[0] = (int)regs->localX + (int)((s16)T1ReadWord(ram, regs->addr + 0x0C));
	Y[0] = (int)regs->localY + (int)((s16)T1ReadWord(ram, regs->addr + 0x0E));
	X[1] = (int)regs->localX + (int)((s16)T1ReadWord(ram, regs->addr + 0x10));
	Y[1] = (int)regs->localY + (int)((s16)T1ReadWord(ram, regs->addr + 0x12));
	X[2] = (int)regs->localX + (int)((s16)T1ReadWord(ram, regs->addr + 0x14));
	Y[2] = (int)regs->localY + (int)((s16)T1ReadWord(ram, regs->addr + 0x16));
	X[3] = (int)regs->localX + (int)((s16)T1ReadWord(ram, regs->addr + 0x18));
	Y[3] = (int)regs->localY + (int)((s16)T1ReadWord(ram, regs->addr + 0x1A));
   if (rstv_v1rec) {
      /* gouraud table: A 0, B 1, C 2, D 3 */
      rstv_v1rec_line(X[0], Y[0], X[1], Y[1], 0, 1, ram, regs, &cmd);
      rstv_v1rec_line(X[1], Y[1], X[2], Y[2], 1, 2, ram, regs, &cmd);
      rstv_v1rec_line(X[3], Y[3], X[2], Y[2], 3, 2, ram, regs, &cmd);
      rstv_v1rec_line(X[0], Y[0], X[3], Y[3], 0, 3, ram, regs, &cmd);
      return;
   }

   length = iterateOverLine(X[0], Y[0], X[1], Y[1], 1, NULL, NULL, regs, &cmd, ram, back_framebuffer);
   gouraudLineSetup(&redstep, &greenstep, &bluestep, length, gouraudA, gouraudB, ram, regs, &cmd, back_framebuffer);
   DrawLine(X[0], Y[0], X[1], Y[1], 0, 0, 0, redstep, greenstep, bluestep, regs, &cmd, ram, back_framebuffer);

   length = iterateOverLine(X[1], Y[1], X[2], Y[2], 1, NULL, NULL, regs, &cmd, ram, back_framebuffer);
   gouraudLineSetup(&redstep, &greenstep, &bluestep, length, gouraudB, gouraudC, ram, regs, &cmd, back_framebuffer);
   DrawLine(X[1], Y[1], X[2], Y[2], 0, 0, 0, redstep, greenstep, bluestep, regs, &cmd, ram, back_framebuffer);

   length = iterateOverLine(X[2], Y[2], X[3], Y[3], 1, NULL, NULL, regs, &cmd, ram, back_framebuffer);
   gouraudLineSetup(&redstep, &greenstep, &bluestep, length, gouraudD, gouraudC, ram, regs, &cmd, back_framebuffer);
   DrawLine(X[3], Y[3], X[2], Y[2], 0, 0, 0, redstep, greenstep, bluestep, regs, &cmd, ram, back_framebuffer);

   length = iterateOverLine(X[3], Y[3], X[0], Y[0], 1, NULL, NULL, regs, &cmd, ram, back_framebuffer);
   gouraudLineSetup(&redstep, &greenstep, &bluestep, length, gouraudA, gouraudD, ram, regs, &cmd, back_framebuffer);
   DrawLine(X[0], Y[0], X[3], Y[3], 0, 0, 0, redstep, greenstep, bluestep, regs, &cmd, ram, back_framebuffer);
}

void VIDSoftVdp1LineDraw(u8* ram, Vdp1*regs, u8* back_framebuffer)
{
	int x1, y1, x2, y2;
	double redstep = 0, greenstep = 0, bluestep = 0;
	int length;
   vdp1cmd_struct cmd;

   Vdp1ReadCommand(&cmd, regs->addr, ram);

	x1 = (int)regs->localX + (int)((s16)T1ReadWord(ram, regs->addr + 0x0C));
	y1 = (int)regs->localY + (int)((s16)T1ReadWord(ram, regs->addr + 0x0E));
	x2 = (int)regs->localX + (int)((s16)T1ReadWord(ram, regs->addr + 0x10));
	y2 = (int)regs->localY + (int)((s16)T1ReadWord(ram, regs->addr + 0x12));
   if (rstv_v1rec) {
      rstv_v1rec_line(x1, y1, x2, y2, 0, 1, ram, regs, &cmd);
      return;
   }

   length = iterateOverLine(x1, y1, x2, y2, 1, NULL, NULL, regs, &cmd, ram, back_framebuffer);
   gouraudLineSetup(&redstep, &bluestep, &greenstep, length, gouraudA, gouraudB, ram, regs, &cmd, back_framebuffer);
   DrawLine(x1, y1, x2, y2, 0, 0, 0, redstep, greenstep, bluestep, regs, &cmd, ram, back_framebuffer);
}

//////////////////////////////////////////////////////////////////////////////

void VIDSoftVdp1UserClipping(u8* ram, Vdp1*regs)
{
   regs->userclipX1 = T1ReadWord(ram, regs->addr + 0xC);
   regs->userclipY1 = T1ReadWord(ram, regs->addr + 0xE);
   regs->userclipX2 = T1ReadWord(ram, regs->addr + 0x14);
   regs->userclipY2 = T1ReadWord(ram, regs->addr + 0x16);
}

//////////////////////////////////////////////////////////////////////////////

void VIDSoftVdp1SystemClipping(u8* ram, Vdp1*regs)
{
   regs->systemclipX1 = 0;
   regs->systemclipY1 = 0;
   regs->systemclipX2 = T1ReadWord(ram, regs->addr + 0x14);
   regs->systemclipY2 = T1ReadWord(ram, regs->addr + 0x16);
}

//////////////////////////////////////////////////////////////////////////////

void VIDSoftVdp1LocalCoordinate(u8* ram, Vdp1*regs)
{
   regs->localX = T1ReadWord(ram, regs->addr + 0xC);
   regs->localY = T1ReadWord(ram, regs->addr + 0xE);
}

//////////////////////////////////////////////////////////////////////////////

void VIDSoftVdp1ReadFrameBuffer(u32 type, u32 addr, void * out)
{
   u32 val;

   VidsoftWaitForVdp1Thread();

   switch (type)
   {
   case 0:
      val = T1ReadByte(vdp1backframebuffer, addr);
      *(u8*)out = val;
      break;
   case 1:
      val = T1ReadWord(vdp1backframebuffer, addr);
#ifndef WORDS_BIGENDIAN
      val = BSWAP16L(val);
#endif
      *(u16*)out = val;
      break;
   case 2:
#if 0 //enable when burning rangers is fixed
      val = T1ReadLong(vdp1backframebuffer, addr);
#ifndef WORDS_BIGENDIAN
      val = BSWAP32(val);
#endif
      val = (val & 0xffff) << 16 | (val & 0xffff0000) >> 16;
      *(u32*)out = val;
#else
      *(u32*)out = 0;
#endif
      break;
   default:
      break;
   }
}

//////////////////////////////////////////////////////////////////////////////

void VIDSoftVdp1WriteFrameBuffer(u32 type, u32 addr, u32 val)
{
   VidsoftWaitForVdp1Thread();

   switch (type)
   {
   case 0:
      T1WriteByte(vdp1backframebuffer, addr, val);
      break;
   case 1:
#ifndef WORDS_BIGENDIAN
      val = BSWAP16L(val);
#endif
      T1WriteWord(vdp1backframebuffer, addr, val);
      break;
   case 2:
#ifndef WORDS_BIGENDIAN
      val = BSWAP32(val);
#endif
      val = (val & 0xffff) << 16 | (val & 0xffff0000) >> 16;
      T1WriteLong(vdp1backframebuffer, addr, val);
      break;
   default:
      break;
   }
}

//////////////////////////////////////////////////////////////////////////////

int VIDSoftVdp2Reset(void)
{
   return 0;
}

//////////////////////////////////////////////////////////////////////////////

int rstv_skip_back_line = 0;
void rstv_vidsoft_draw_back_line(void)
{
   Vdp2DrawBackScreen();
   Vdp2DrawLineScreen();
}

void VIDSoftVdp2DrawStart(void)
{
   int titanblendmode = TITAN_BLEND_TOP;
   if (Vdp2Regs->CCCTL & 0x100) titanblendmode = TITAN_BLEND_ADD;
   else if (Vdp2Regs->CCCTL & 0x200) titanblendmode = TITAN_BLEND_BOTTOM;
   TitanSetBlendingMode(titanblendmode);

   /* ReST-V: the GPU compositor computes back/line screens itself; it calls rstv_vidsoft_draw_back_line()
      only for frames that the CPU composes */
   if (!rstv_skip_back_line)
      rstv_vidsoft_draw_back_line();

   //dracula x bad cycle setting
   if (Vdp2Regs->CYCA0L == 0x5566 &&
      Vdp2Regs->CYCA0U == 0x47ff &&
      Vdp2Regs->CYCA1L == 0xffff &&
      Vdp2Regs->CYCA1U == 0xffff &&
      Vdp2Regs->CYCB0L == 0x12ff &&
      Vdp2Regs->CYCB0U == 0x03ff &&
      Vdp2Regs->CYCB1L == 0xffff &&
      Vdp2Regs->CYCB1U == 0xffff)
   {
      bad_cycle_setting[TITAN_NBG3] = 1;
   }
   else
      bad_cycle_setting[TITAN_NBG3] = 0;
}

//////////////////////////////////////////////////////////////////////////////


void VidsoftDrawSprite(Vdp2 * vdp2_regs, u8 * spr_window_mask, u8* vdp1_front_framebuffer, u8 * vdp2_ram, Vdp1* vdp1_regs, Vdp2* vdp2_lines, u8*color_ram)
{
   int i, i2;
   u16 pixel;
   u8 prioritytable[8];
   u32 vdp1coloroffset;
   int colormode = vdp2_regs->SPCTL & 0x20;
   vdp2draw_struct info = { 0 };
   int islinewindow;
   clipping_struct clip[2];
   u32 linewnd0addr, linewnd1addr;
   int wctl;
   clipping_struct colorcalcwindow[2];
   int framebuffer_readout_y = 0;
   int start_line = 0, line_increment = 0;
   int sprite_window_enabled = vdp2_regs->SPCTL & 0x10;
   int vdp1spritetype = 0;

   if (sprite_window_enabled)
   {
      memset(spr_window_mask, 0, 704 * 512);
   }

   // Figure out whether to draw vdp1 framebuffer or vdp2 framebuffer pixels
   // based on priority
   if (Vdp1External.disptoggle && (vdp2_regs->TVMD & 0x8000))
   {
      int SPCCCS = (vdp2_regs->SPCTL >> 12) & 0x3;
      int SPCCN = (vdp2_regs->SPCTL >> 8) & 0x7;
      u8 colorcalctable[8];
      vdp2rotationparameterfp_struct p;
      int x, y;
      int output_y = 0;

      prioritytable[0] = vdp2_regs->PRISA & 0x7;
      prioritytable[1] = (vdp2_regs->PRISA >> 8) & 0x7;
      prioritytable[2] = vdp2_regs->PRISB & 0x7;
      prioritytable[3] = (vdp2_regs->PRISB >> 8) & 0x7;
      prioritytable[4] = vdp2_regs->PRISC & 0x7;
      prioritytable[5] = (vdp2_regs->PRISC >> 8) & 0x7;
      prioritytable[6] = vdp2_regs->PRISD & 0x7;
      prioritytable[7] = (vdp2_regs->PRISD >> 8) & 0x7;
      colorcalctable[0] = ((~vdp2_regs->CCRSA & 0x1F) << 1) + 1;
      colorcalctable[1] = ((~vdp2_regs->CCRSA >> 7) & 0x3E) + 1;
      colorcalctable[2] = ((~vdp2_regs->CCRSB & 0x1F) << 1) + 1;
      colorcalctable[3] = ((~vdp2_regs->CCRSB >> 7) & 0x3E) + 1;
      colorcalctable[4] = ((~vdp2_regs->CCRSC & 0x1F) << 1) + 1;
      colorcalctable[5] = ((~vdp2_regs->CCRSC >> 7) & 0x3E) + 1;
      colorcalctable[6] = ((~vdp2_regs->CCRSD & 0x1F) << 1) + 1;
      colorcalctable[7] = ((~vdp2_regs->CCRSD >> 7) & 0x3E) + 1;

      vdp1coloroffset = (vdp2_regs->CRAOFB & 0x70) << 4;
      vdp1spritetype = vdp2_regs->SPCTL & 0xF;

      ReadVdp2ColorOffset(vdp2_regs, &info, 0x40, 0x40);

      wctl = vdp2_regs->WCTLC >> 8;
      clip[0].xstart = clip[0].ystart = clip[0].xend = clip[0].yend = 0;
      clip[1].xstart = clip[1].ystart = clip[1].xend = clip[1].yend = 0;
      ReadWindowData(wctl, clip, vdp2_regs);
      linewnd0addr = linewnd1addr = 0;
      ReadLineWindowData(&islinewindow, wctl, &linewnd0addr, &linewnd1addr, vdp2_regs);

      /* color calculation window: in => no color calc, out => color calc */
      ReadWindowData(vdp2_regs->WCTLD >> 8, colorcalcwindow, vdp2_regs);

      if (vdp1_regs->TVMR & 2)
         Vdp2ReadRotationTableFP(0, &p, vdp2_regs, vdp2_ram);

      info.titan_which_layer = TITAN_SPRITE;

      info.linescreen = (vdp2_regs->LNCLEN >> 5) & 1;

      Vdp2GetInterlaceInfo(&start_line, &line_increment);

      for (i2 = start_line; i2 < vdp2height; i2 += line_increment)
      {
         float framebuffer_readout_pos = 0;

         ReadLineWindowClip(islinewindow, clip, &linewnd0addr, &linewnd1addr, vdp2_ram, vdp2_regs);

         if (vdp2_interlace)
            LoadLineParamsSprite(&info, i2 / 2, vdp2_lines);
         else
            LoadLineParamsSprite(&info, i2, vdp2_lines);

         if (vdp2_interlace)
         {
            y = framebuffer_readout_y;
            framebuffer_readout_y += 1;
         }
         else
         {
            y = i2;
         }

         for (i = 0; i < vdp2width; i++)
         {

            info.titan_shadow_type = 0;

            // See if screen position is clipped, if it isn't, continue
            if (!(vdp2_regs->SPCTL & 0x10))
            {
               if (!TestBothWindow(wctl, clip, i, i2))
               {
                  continue;
               }
            }

            if (vdp1_regs->TVMR & 2) {
               x = (touint(p.Xst + i * p.deltaX + i2 * p.deltaXst)) & (vdp1width - 1);
               y = (touint(p.Yst + i * p.deltaY + i2 * p.deltaYst)) & (vdp1height - 1);
            }
            else
            {
               if (vdp1width == 1024 && vdp2_x_hires)
               {
                  //hi res vdp1 and hi res vdp2
                  //pixels 1:1
                  x = (int)framebuffer_readout_pos;
                  framebuffer_readout_pos += 1;
               }
               else if (vdp1width == 512 && vdp2_x_hires)
               {
                  //low res vdp1,hi res vdp2
                  //vdp1 pixel doubling
                  x = (int)framebuffer_readout_pos;
                  framebuffer_readout_pos += .5;
               }
               else if (vdp1width == 1024 && (!vdp2_x_hires))
               {
                  //hi res vdp1, low res vdp2
                  //the vdp1 framebuffer is read out at half-res
                  x = (int)framebuffer_readout_pos;
                  framebuffer_readout_pos += 2;
               }
               else
                  x = i;
            }

            if (vdp1pixelsize == 2)
            {
               // 16-bit pixel size
               pixel = ((u16 *)vdp1_front_framebuffer)[(y * vdp1width) + x];

               if (pixel == 0)
                  ;
               else if (pixel & 0x8000 && colormode)
               {
                  // 16 BPP               
                  u8 alpha = 0x3F;
                  if ((SPCCCS == 3) && TestBothWindow(vdp2_regs->WCTLD >> 8, colorcalcwindow, i, i2) && (vdp2_regs->CCCTL & 0x40))
                  {
                     alpha = colorcalctable[0];
                     if (vdp2_regs->CCCTL & 0x300) alpha |= 0x80;
                  }
                  // if pixel is 0x8000, only draw pixel if sprite window
                  // is disabled/sprite type 2-7. sprite types 0 and 1 are
                  // -always- drawn and sprite types 8-F are always
                  // transparent.
                  if (pixel != 0x8000 || vdp1spritetype < 2 || (vdp1spritetype < 8 && !(vdp2_regs->SPCTL & 0x10)))
                     TitanPutPixel(prioritytable[0], i, output_y, info.PostPixelFetchCalc(&info, COLSAT2YAB16(alpha, pixel)), info.linescreen, &info);
               }
               else
               {
                  // Color bank
                  spritepixelinfo_struct spi;
                  u8 alpha = 0x3F;
                  u32 dot;

                  Vdp1GetSpritePixelInfo(vdp1spritetype, &pixel, &spi);
                  if (spi.normalshadow)
                  {
                     info.titan_shadow_type = TITAN_NORMAL_SHADOW;
                     TitanPutPixel(prioritytable[spi.priority], i, output_y, COLSAT2YAB16(0x3f, 0), info.linescreen, &info);
                     continue;
                  }

                  dot = Vdp2ColorRamGetColor(vdp1coloroffset + pixel,(int)color_ram);

                  if (TestBothWindow(vdp2_regs->WCTLD >> 8, colorcalcwindow, i, i2) && (vdp2_regs->CCCTL & 0x40))
                  {
                     int transparent = 0;

                     /* Sprite color calculation */
                     switch (SPCCCS) {
                     case 0:
                        if (prioritytable[spi.priority] <= SPCCN)
                           transparent = 1;
                        break;
                     case 1:
                        if (prioritytable[spi.priority] == SPCCN)
                           transparent = 1;
                        break;
                     case 2:
                        if (prioritytable[spi.priority] >= SPCCN)
                           transparent = 1;
                        break;
                     case 3:
                        if (dot & 0x80000000)
                           transparent = 1;
                        break;
                     }

                     if (vdp2_regs->CCCTL & 0x200) {
                        /* "bottom" mode, the alpha channel will be used by another layer,
                        so we set it regardless of whether sprites are transparent or not.
                        The highest priority bit is only set if the sprite is transparent
                        (in this case, it's the alpha channel of the lower priority layer
                        that will be used. */
                        alpha = colorcalctable[spi.colorcalc];
                        if (transparent) alpha |= 0x80;
                     }
                     else if (transparent) {
                        alpha = colorcalctable[spi.colorcalc];
                        if (vdp2_regs->CCCTL & 0x100) alpha |= 0x80;
                     }
                  }
                  if (spi.msbshadow)
                  {
                     if (sprite_window_enabled) {
                        spr_window_mask[(y*vdp2width) + x] = 1;
                        info.titan_shadow_type = TITAN_MSB_SHADOW;
                     }
                     else
                     {
                        info.titan_shadow_type = TITAN_MSB_SHADOW;
                     }

                     if (pixel == 0)
                     {
                        TitanPutPixel(prioritytable[spi.priority], i, output_y, info.PostPixelFetchCalc(&info, COLSAT2YAB32(alpha, 0)), info.linescreen, &info);
                        continue;
                     }
                  }

                  if ((sprite_window_enabled))
                  {
                     if (!TestBothWindow(wctl, clip, i, i2))
                     {
                        continue;
                     }
                  }

                  TitanPutPixel(prioritytable[spi.priority], i, output_y, info.PostPixelFetchCalc(&info, COLSAT2YAB32(alpha, dot)), info.linescreen, &info);
               }
            }
            else
            {
               // 8-bit pixel size
               pixel = vdp1_front_framebuffer[(y * vdp1width) + x];

               if (pixel != 0)
               {
                  // Color bank(fix me)
                  spritepixelinfo_struct spi;
                  u8 alpha = 0x3F;
                  u32 dot;

                  Vdp1GetSpritePixelInfo(vdp1spritetype, &pixel, &spi);
                  if (spi.normalshadow)
                  {
                     info.titan_shadow_type = TITAN_NORMAL_SHADOW;
                     TitanPutPixel(prioritytable[spi.priority], i, output_y, COLSAT2YAB16(0x3f, 0), info.linescreen, &info);
                     continue;
                  }

                  dot = Vdp2ColorRamGetColor(vdp1coloroffset + pixel, (int)color_ram);

                  if (TestBothWindow(vdp2_regs->WCTLD >> 8, colorcalcwindow, i, i2) && (vdp2_regs->CCCTL & 0x40))
                  {
                     int transparent = 0;

                     /* Sprite color calculation */
                     switch (SPCCCS) {
                     case 0:
                        if (prioritytable[spi.priority] <= SPCCN)
                           transparent = 1;
                        break;
                     case 1:
                        if (prioritytable[spi.priority] == SPCCN)
                           transparent = 1;
                        break;
                     case 2:
                        if (prioritytable[spi.priority] >= SPCCN)
                           transparent = 1;
                        break;
                     case 3:
                        if (dot & 0x80000000)
                           transparent = 1;
                        break;
                     }

                     if (vdp2_regs->CCCTL & 0x200) {
                        /* "bottom" mode, the alpha channel will be used by another layer,
                        so we set it regardless of whether sprites are transparent or not.
                        The highest priority bit is only set if the sprite is transparent
                        (in this case, it's the alpha channel of the lower priority layer
                        that will be used. */
                        alpha = colorcalctable[spi.colorcalc];
                        if (transparent) alpha |= 0x80;
                     }
                     else if (transparent) {
                        alpha = colorcalctable[spi.colorcalc];
                        if (vdp2_regs->CCCTL & 0x100) alpha |= 0x80;
                     }
                  }

                  TitanPutPixel(prioritytable[spi.priority], i, output_y, info.PostPixelFetchCalc(&info, COLSAT2YAB32(alpha, dot)), info.linescreen, &info);
               }
            }
         }

         output_y++;
      }
   }
}

void VIDSoftVdp2DrawEnd(void)
{
   if (vidsoft_num_layer_threads > 0)
   {
      while (!vidsoft_thread_context.draw_finished[TITAN_NBG0]){}
      while (!vidsoft_thread_context.draw_finished[TITAN_NBG1]){}
      while (!vidsoft_thread_context.draw_finished[TITAN_NBG2]){}
      while (!vidsoft_thread_context.draw_finished[TITAN_NBG3]){}
      while (!vidsoft_thread_context.draw_finished[TITAN_RBG0]){}
      while (!vidsoft_thread_context.draw_finished[TITAN_SPRITE]){}
   }

   TitanRender(dispbuffer);

   VIDSoftVdp1SwapFrameBuffer();

   if (OSDUseBuffer())
      OSDDisplayMessages(dispbuffer, vdp2width, vdp2height);
#if !defined(ANDROID)
#ifdef USE_OPENGL	
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, vdp2width, vdp2height, 0, GL_RGBA, GL_UNSIGNED_BYTE, dispbuffer);
   glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
   glClear(GL_COLOR_BUFFER_BIT);
   glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

#endif
#endif
   if (! OSDUseBuffer())
      OSDDisplayMessages(NULL, -1, -1);
  
   YuiSwapBuffers();
}

//////////////////////////////////////////////////////////////////////////////

void VidsoftStartLayerThread(int * layer_priority, int * draw_priority_0, int * num_threads_dispatched, int which_layer, void(*layer_func) (Vdp2* lines, Vdp2* regs, u8* ram, u8* color_ram, struct CellScrollData * cell_data))
{
   if (layer_priority[which_layer] > 0 || draw_priority_0[which_layer])
   {
      if (*num_threads_dispatched < vidsoft_num_layer_threads)
      {
         vidsoft_thread_context.need_draw[which_layer] = 1;
         vidsoft_thread_context.draw_finished[which_layer] = 0;
         YabThreadWake(YAB_THREAD_VIDSOFT_LAYER_NBG3 + which_layer);
         *num_threads_dispatched = *num_threads_dispatched + 1;
      }
      else
      {
        (*layer_func) (Vdp2Lines, Vdp2Regs, Vdp2Ram, Vdp2ColorRam, cell_scroll_data);
      }
   }
}

//////////////////////////////////////////////////////////////////////////////

int IsSpriteWindowEnabled(u16 wtcl)
{
   if (((wtcl& (1 << 13)) == 0) &&
      ((wtcl & (1 << 5)) == 0))
      return 0;

   return 1;
}

int CanUseSpriteThread()
{
   //check if sprite window is enabled
   if ((Vdp2Regs->SPCTL & (1 << 4)) == 0)
      return 1;

   //check if any layers are using it
   if (IsSpriteWindowEnabled(Vdp2Regs->WCTLA) ||
      IsSpriteWindowEnabled(Vdp2Regs->WCTLB) ||
      IsSpriteWindowEnabled(Vdp2Regs->WCTLC) ||
      IsSpriteWindowEnabled(Vdp2Regs->WCTLD))
   {
      //thread cannot be used
      return 0;
   }
   
   return 1;
}

/* ReST-V: draw one background layer on the CPU (used when the GPU cannot draw a captured layer) */
void rstv_vidsoft_draw_layer(int titan_layer)
{
   const int saved = rstv_capture_mode;
   rstv_capture_mode = 0;
   TitanEraseLayer(titan_layer);
   switch (titan_layer)
   {
   case TITAN_NBG0: Vdp2DrawNBG0(IN_LINES, IN_REGS, IN_RAM, IN_CRAM, IN_CELLS); break;
   case TITAN_NBG1: Vdp2DrawNBG1(IN_LINES, IN_REGS, IN_RAM, IN_CRAM, IN_CELLS); break;
   case TITAN_NBG2: Vdp2DrawNBG2(IN_LINES, IN_REGS, IN_RAM, IN_CRAM, IN_CELLS); break;
   case TITAN_NBG3: Vdp2DrawNBG3(IN_LINES, IN_REGS, IN_RAM, IN_CRAM, IN_CELLS); break;
   case TITAN_RBG0: Vdp2DrawRBG0(IN_LINES, IN_REGS, IN_RAM, IN_CRAM, IN_CELLS); break;
   default: break;
   }
   rstv_capture_mode = saved;
}

/* ReST-V (ADR-079): main-thread part (VDP1 front buffer, resolution) */
void rstv_vidsoft_screens_begin(void)
{
#ifdef RSTV
   rstv_vdp1_sync_buffer(vdp1frontframebuffer);
#endif
   VIDSoftVdp2SetResolution(Vdp2Regs->TVMD);
}

/* ReST-V (ADR-079): the layer pass (capture or CPU drawing), reading rstv_vidsoft_set_inputs */
void rstv_vidsoft_screens_layers(void)
{
   int draw_priority_0[6] = { 0 };
   int layer_priority[6] = { 0 };
   int num_threads_dispatched = 0;
   if (rstv_capture_mode && rstv_cap_same)
   {
      /* ReST-V (ADR-031): which lines have the same registers as the line above (once per frame, for every layer) */
      int y;
      rstv_line_same[0] = 0;
      rstv_line_same_src = IN_LINES;
      for (y = 1; y < 512 && y < vdp2height; y++)
         rstv_line_same[y] = !memcmp(IN_LINES + y, IN_LINES + y - 1, sizeof(Vdp2));
   }
   layer_priority[TITAN_NBG0] = IN_REGS->PRINA & 0x7;
   layer_priority[TITAN_NBG1] = ((IN_REGS->PRINA >> 8) & 0x7);
   layer_priority[TITAN_NBG2] = (IN_REGS->PRINB & 0x7);
   layer_priority[TITAN_NBG3] = ((IN_REGS->PRINB >> 8) & 0x7);
   layer_priority[TITAN_RBG0] = (IN_REGS->PRIR & 0x7);

   if (rstv_capture_mode)
   {
      /* ReST-V: scroll NBG layers are only recorded (and erased by rstv_vidsoft_draw_layer if the CPU
         has to draw them); only the rotation layers are drawn straight into Titan buffers here */
      /* ReST-V: RBG0 is usually drawn by the GPU; erase the CPU buffer only if the CPU draws it (Vdp2DrawRotation) */
      rstv_rbg0_erase_pending = (IN_REGS->BGON & 0x10) != 0;
      if (IN_REGS->BGON & 0x20)
         TitanEraseLayer(TITAN_NBG0);
   }
   else
      TitanErase();

   if (IN_REGS->SFPRMD & 0x3FF)
   {
      draw_priority_0[TITAN_NBG0] = (IN_REGS->SFPRMD >> 0) & 0x3;
      draw_priority_0[TITAN_NBG1] = (IN_REGS->SFPRMD >> 2) & 0x3;
      draw_priority_0[TITAN_NBG2] = (IN_REGS->SFPRMD >> 4) & 0x3;
      draw_priority_0[TITAN_NBG3] = (IN_REGS->SFPRMD >> 6) & 0x3;
      draw_priority_0[TITAN_RBG0] = (IN_REGS->SFPRMD >> 8) & 0x3;
   }

   if (vidsoft_num_layer_threads > 0)
   {
      memcpy(vidsoft_thread_context.lines, Vdp2Lines, sizeof(Vdp2) * 270);
      memcpy(&vidsoft_thread_context.regs, Vdp2Regs, sizeof(Vdp2));
      memcpy(vidsoft_thread_context.ram, Vdp2Ram, 0x80000);
      memcpy(vidsoft_thread_context.color_ram, Vdp2ColorRam, 0x1000);
      memcpy(vidsoft_thread_context.cell_scroll_data, cell_scroll_data, sizeof(struct CellScrollData) * 270);
   }

   //draw vdp2 sprite layer on a thread if sprite window is not enabled
   if (CanUseSpriteThread() && vidsoft_num_layer_threads > 0)
   {
      vidsoft_thread_context.need_draw[TITAN_SPRITE] = 1;
      vidsoft_thread_context.draw_finished[TITAN_SPRITE] = 0;
      YabThreadWake(YAB_THREAD_VIDSOFT_LAYER_SPRITE);
      num_threads_dispatched++;
   }
   else if (!rstv_gpu_compose) // ReST-V: the GPU decodes the sprite layer itself
   {
      u64 pt_ = RSTV_PROF_T();
      VidsoftDrawSprite(IN_REGS, sprite_window_mask, vdp1frontframebuffer, IN_RAM, Vdp1Regs, IN_LINES, IN_CRAM);
      RSTV_PROF_ADD(RSTV_PROF_L_SPR, pt_);
   }

   if (vidsoft_num_layer_threads > 0)
   {
      VidsoftStartLayerThread(layer_priority, draw_priority_0, &num_threads_dispatched, TITAN_NBG0, Vdp2DrawNBG0);
      VidsoftStartLayerThread(layer_priority, draw_priority_0, &num_threads_dispatched, TITAN_RBG0, Vdp2DrawRBG0);
      VidsoftStartLayerThread(layer_priority, draw_priority_0, &num_threads_dispatched, TITAN_NBG1, Vdp2DrawNBG1);
      VidsoftStartLayerThread(layer_priority, draw_priority_0, &num_threads_dispatched, TITAN_NBG2, Vdp2DrawNBG2);
      VidsoftStartLayerThread(layer_priority, draw_priority_0, &num_threads_dispatched, TITAN_NBG3, Vdp2DrawNBG3);
   }
   else
   {
      u64 pt_ = RSTV_PROF_T();
      Vdp2DrawNBG0(IN_LINES, IN_REGS, IN_RAM, IN_CRAM, IN_CELLS);
      RSTV_PROF_ADD(RSTV_PROF_L_N0, pt_); pt_ = RSTV_PROF_T();
      Vdp2DrawNBG1(IN_LINES, IN_REGS, IN_RAM, IN_CRAM, IN_CELLS);
      RSTV_PROF_ADD(RSTV_PROF_L_N1, pt_); pt_ = RSTV_PROF_T();
      Vdp2DrawNBG2(IN_LINES, IN_REGS, IN_RAM, IN_CRAM, IN_CELLS);
      RSTV_PROF_ADD(RSTV_PROF_L_N2, pt_); pt_ = RSTV_PROF_T();
      Vdp2DrawNBG3(IN_LINES, IN_REGS, IN_RAM, IN_CRAM, IN_CELLS);
      RSTV_PROF_ADD(RSTV_PROF_L_N3, pt_); pt_ = RSTV_PROF_T();
      Vdp2DrawRBG0(IN_LINES, IN_REGS, IN_RAM, IN_CRAM, IN_CELLS);
      RSTV_PROF_ADD(RSTV_PROF_L_R0, pt_);
   }
   if (rstv_rbg0_erase_pending)
   {
      /* RBG0 neither captured nor drawn: keep the old behaviour (empty buffer) */
      TitanEraseLayer(TITAN_RBG0);
      rstv_rbg0_erase_pending = 0;
   }
}

void VIDSoftVdp2DrawScreens(void)
{
   rstv_vidsoft_screens_begin();
   rstv_vidsoft_screens_layers();
}

//////////////////////////////////////////////////////////////////////////////

void VIDSoftVdp2DrawScreen(int screen)
{
   VIDSoftVdp2SetResolution(Vdp2Regs->TVMD);

   switch(screen)
   {
      case 0:
         Vdp2DrawNBG0(Vdp2Lines, Vdp2Regs, Vdp2Ram, Vdp2ColorRam, cell_scroll_data);
         break;
      case 1:
         Vdp2DrawNBG1(Vdp2Lines, Vdp2Regs, Vdp2Ram, Vdp2ColorRam, cell_scroll_data);
         break;
      case 2:
         Vdp2DrawNBG2(Vdp2Lines, Vdp2Regs, Vdp2Ram, Vdp2ColorRam, cell_scroll_data);
         break;
      case 3:
         Vdp2DrawNBG3(Vdp2Lines, Vdp2Regs, Vdp2Ram, Vdp2ColorRam, cell_scroll_data);
         break;
      case 4:
         Vdp2DrawRBG0(Vdp2Lines, Vdp2Regs, Vdp2Ram, Vdp2ColorRam, cell_scroll_data);
         break;
   }
}

//////////////////////////////////////////////////////////////////////////////

void VIDSoftVdp2SetResolution(u16 TVMD)
{
   // This needs some work

   // Horizontal Resolution
   switch (TVMD & 0x7)
   {
      case 0:
         rbg0width = vdp2width = 320;
         break;
      case 1:
         rbg0width = vdp2width = 352;
         break;
      case 2:
         vdp2width = 640;
         rbg0width = 320;
         break;
      case 3:
         vdp2width = 704;
         rbg0width = 352;
         break;
      case 4:
         rbg0width = vdp2width = 320;
         break;
      case 5:
         rbg0width = vdp2width = 352;
         break;
      case 6:
         vdp2width = 640;
         rbg0width = 320;
         break;
      case 7:
         vdp2width = 704;
         rbg0width = 352;
         break;
   }

   if ((vdp2width == 704) || (vdp2width == 640))
      vdp2_x_hires = 1;
   else
      vdp2_x_hires = 0;

   // Vertical Resolution
   switch ((TVMD >> 4) & 0x3)
   {
      case 0:
         rbg0height = vdp2height = 224;
         break;
      case 1:
         rbg0height = vdp2height = 240;
         break;
      case 2:
         rbg0height = vdp2height = 256;
         break;
      default: break;
   }

   // Check for interlace
   switch ((TVMD >> 6) & 0x3)
   {
      case 3: // Double-density Interlace
         vdp2height *= 2;
         vdp2_interlace=1;
         break;
      case 2: // Single-density Interlace
      case 0: // Non-interlace
      default: 
         vdp2_interlace = 0;
         break;
   }

   rstv_half_x = rstv_half_y = 0;
   if (rstv_hires_half)
   {
      if (vdp2_x_hires)
      {
         vdp2width /= 2;
         vdp2_x_hires = 0;
         rstv_half_x = 1;
      }
      if (vdp2_interlace)
      {
         vdp2height /= 2;
         vdp2_interlace = 0;
         rstv_half_y = 1;
      }
   }
   TitanSetResolution(vdp2width, vdp2height);
}

//////////////////////////////////////////////////////////////////////////////

/* ReST-V: the frame change is decided by vdp2.cpp (one-cycle mode, or a manual change request with FBCR=3)
   and signalled through VIDCore->Vdp1FrameChange() right before the next VDP1 draw. vidsoft used to swap in
   Vdp2DrawEnd instead and checked Vdp1External.manualchange, which vdp2.cpp had already cleared, so games
   using manual frame change (sandor, thunt, thuntk, ...) never showed their sprites. Swapping before the draw
   is equivalent for one-cycle mode (the drawn buffer is displayed on the next frame either way). */
void VIDSoftVdp1FrameChange(void)
{
   rstv_vdp1_back_idx ^= 1;
   u8 *temp = vdp1frontframebuffer;
   vdp1frontframebuffer = vdp1backframebuffer;
   vdp1backframebuffer = temp;
}

void VIDSoftVdp1SwapFrameBuffer(void)
{
#ifdef RSTV
   return; /* see VIDSoftVdp1FrameChange */
#endif
   if (((Vdp1Regs->FBCR & 2) == 0) || Vdp1External.manualchange)
   {
		u8 *temp;
#ifndef RSTV
      /* ReST-V: no wait, the VDP1 thread keeps its own target pointer */
      if (vidsoft_vdp1_thread_enabled)
      {
         VidsoftWaitForVdp1Thread();
      }
#endif

      temp = vdp1frontframebuffer;
      vdp1frontframebuffer = vdp1backframebuffer;
      vdp1backframebuffer = temp;
      Vdp1External.manualchange = 0;
   }
}

//////////////////////////////////////////////////////////////////////////////

void VIDSoftVdp1EraseFrameBuffer(Vdp1* regs, u8 * back_framebuffer)
{   
   int i,i2;
   int w,h;

   /* ReST-V (ADR-114): VBE (TVMR bit 3) erases the displayed buffer in the V-blank before the change (vdp2.cpp calls
      Vdp1EraseWrite, empty here): the same as erasing the drawing buffer when the draw starts (batmanfr) */
   if (((regs->FBCR & 2) == 0) || Vdp1External.manualerase || (regs->TVMR & 8))
   {
      h = (regs->EWRR & 0x1FF) + 1;
      if (h > vdp1height) h = vdp1height;
      w = ((regs->EWRR >> 6) & 0x3F8) + 8;
      if (w > vdp1width) w = vdp1width;

      if (vdp1pixelsize == 2)
      {
         for (i2 = (regs->EWLR & 0x1FF); i2 < h; i2++)
         {
            for (i = ((regs->EWLR >> 6) & 0x1F8); i < w; i++)
               ((u16 *)back_framebuffer)[(i2 * vdp1width) + i] = regs->EWDR;
         }
      }
      else
      {
         w = regs->EWRR >> 9;
         w *= 16;

         for (i2 = (regs->EWLR & 0x1FF); i2 < h; i2++)
         {
            for (i = ((regs->EWLR >> 6) & 0x1F8); i < w; i++)
            {
               int pos = (i2 * vdp1width) + i;

               if (pos < 0x3FFFF)
                  back_framebuffer[pos] = regs->EWDR & 0xFF;
            }
         }
      }
      Vdp1External.manualerase = 0;
   }
}

//////////////////////////////////////////////////////////////////////////////

void VIDSoftGetGlSize(int *width, int *height)
{
#ifdef USE_OPENGL
   *width = outputwidth;
   *height = outputheight;
#else
   *width = vdp2width;
   *height = vdp2height;
#endif
}

void VIDSoftGetNativeResolution(int *width, int *height, int* interlace)
{
   *width = vdp2width;
   *height = vdp2height;
   *interlace = vdp2_interlace;
}

void VIDSoftVdp2DispOff()
{
   TitanErase();
}
