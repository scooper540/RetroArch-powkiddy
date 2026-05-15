/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2010-2014 - Hans-Kristian Arntzen
 *  Copyright (C) 2011-2017 - Daniel De Matteis
 *  Copyright (C) 2011-2017 - Higor Euripedes
 *  Copyright (C) 2019-2021 - James Leaver
 *
 *  Modified for Powkiddy X39 — ATS3605 DE hardware scaler — 2026
 *
 *  ARCHITECTURE v7 — DE hardware scaling, SDL-free
 *
 *  - Framebuffer accédé directement via mmap(/dev/fb0)
 *  - Display Engine (DE) ATS3605 configuré via mmap(/dev/mem) + sysfs
 *  - Rotation des pixels en C (ROT_0/90/180/270), sans upscaling
 *  - L'upscaling est entièrement délégué au DE (ISIZE/OSIZE/SR/E_COOR)
 *  - Vsync via ioctl OWLFB_WAITFORVSYNC sur /dev/fb0
 *  - Pas de dépendance SDL
 *
 *  Registres DE ATS3605 confirmés :
 *    [0x2A4] ISIZE   - taille source  (w-1)|(h-1)<<16
 *    [0x2A8] OSIZE   - taille output  (w-1)|(h-1)<<16
 *    [0x2AC] SR      - scale ratio    sr_h|(sr_v<<16), 0x2000 = 1.0
 *    [0x2D0] BA0     - adresse buffer physique
 *    [0x2E8] STR     - stride en unités de 8 octets
 *    [0x154] E_COOR  - position output sur l'écran x|(y<<16)
 *
 *  Rotation values (mirrors RetroArch video_rotation setting):
 *    ROT_0   (0) — pas de rotation
 *    ROT_90  (1) — 90° CW  (X39 Pro portrait, défaut)
 *    ROT_180 (2) — 180°
 *    ROT_270 (3) — 90° CCW
 */

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>

#include <retro_assert.h>
#include <gfx/video_frame.h>
#include <string/stdstring.h>
#include <encodings/utf.h>
#include <features/features_cpu.h>

#ifdef HAVE_CONFIG_H
#include "../../config.h"
#endif

#ifdef HAVE_MENU
#include "../../menu/menu_driver.h"
#endif

#include "../../verbosity.h"
#include "../../gfx/drivers_font_renderer/bitmap.h"
#include "../../configuration.h"
#include "../../retroarch.h"
#include "../../config.def.h"

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

/* ==========================================================================
 * Registres DE ATS3605
 * ========================================================================== */
#define DE_BASE           0xB02F0000
#define DE_SIZE           0x00004078

#define DE_PATH_SIZE      0x0150   /* LCD size (r/o) : (w-1)|(h-1)<<16 */
#define DE_PATH_E_COOR    0x0154   /* position output : x|(y<<16) */

#define DE_OVL_CFG        0x02A0
#define DE_OVL_ISIZE      0x02A4   /* src  (w-1)|(h-1)<<16 */
#define DE_OVL_OSIZE      0x02A8   /* out  (w-1)|(h-1)<<16 */
#define DE_OVL_SR         0x02AC   /* scale ratio, 0x2000 = 1.0 */
#define DE_OVL_SCOEF0     0x02B0
#define DE_OVL_SCOEF1     0x02B4
#define DE_OVL_SCOEF2     0x02B8
#define DE_OVL_SCOEF3     0x02BC
#define DE_OVL_SCOEF4     0x02C0
#define DE_OVL_SCOEF5     0x02C4
#define DE_OVL_SCOEF6     0x02C8
#define DE_OVL_SCOEF7     0x02CC
#define DE_OVL_BA0        0x02D0   /* buffer physical address */
#define DE_OVL_STR        0x02E8   /* stride in 8-byte units */

#define DE_SR_FRAC        0x2000   /* 1.0 en virgule fixe */

/* ioctl vsync OWL */
#define OWL_IOW(n,t)        _IOW('O', n, t)
#define OWLFB_WAITFORVSYNC  OWL_IOW(57, long long)
#ifndef FBIO_WAITFORVSYNC
#define FBIO_WAITFORVSYNC _IOW('F', 0x20, __u32)
#endif
/* ==========================================================================
 * Misc
 * ========================================================================== */
#define DE_POWKIDDY_MAX_FB_W  1920
#define DE_POWKIDDY_MAX_FB_H  1920
#define DE_POWKIDDY_NUM_FONT_GLYPHS 256

#define ROT_0    0
#define ROT_90   1
#define ROT_180  2
#define ROT_270  3

/* ==========================================================================
 * Struct principal
 * ========================================================================== */
typedef struct de_powkiddy_video
{
   retro_time_t last_frame_time;
   retro_time_t ff_frame_time_min;

   bitmapfont_lut_t *osd_font;

   /* Framebuffer — accès direct mmap */
   int      fd_fb;
   uint8_t *fb_map;          /* pointeur mmap sur le FB complet */
   uint32_t fb_map_size;     /* taille mmap (finfo.smem_len) */
   uint32_t fb_phys;         /* adresse physique smem_start */
   unsigned fb_pitch;        /* pitch FB en octets */

   /* Display Engine — accès direct /dev/mem */
   int                 fd_mem;
   volatile uint32_t  *de;       /* pointeur mmap sur les registres DE */
   uint32_t            buf_phys; /* adresse physique du buffer source DE */
   uint8_t            *buf_virt; /* pointeur virtuel vers ce buffer dans fb_map */

   /* Dimensions LCD physique (lues depuis DE_PATH_SIZE) */
   unsigned lcd_w;
   unsigned lcd_h;

   /* Dimensions FB */
   unsigned fb_width;
   unsigned fb_height;

   /* Espace logique menu (axes swappés si rotation 90/270) */
   unsigned menu_w;
   unsigned menu_h;

   unsigned frame_width;
   unsigned frame_height;
   unsigned rotation;

   uint32_t font_colour32;
   uint16_t font_colour16;

   float scaling;

   /* Menu texture */
   uint16_t *menu_texture;
   unsigned  menu_texture_width;
   unsigned  menu_texture_height;

   /* Dernier état DE (pour éviter les re-programations inutiles) */
   unsigned de_src_w;
   unsigned de_src_h;
   unsigned de_out_w;
   unsigned de_out_h;
   unsigned de_out_x;
   unsigned de_out_y;

   bool rgb32;
   bool vsync;
   bool keep_aspect;
   bool integer_scaling;
   uint filtertype;
   bool menu_active;
   bool was_in_menu;
   bool quitting;
   bool de_ok;               /* DE initialisé avec succès */

   uint16_t *last_frame_buf;
   size_t    last_frame_buf_size;
   unsigned  last_frame_width;
   unsigned  last_frame_height;
   unsigned  last_frame_pitch;
   char      last_msg[512];
} de_powkiddy_video_t;

/* ==========================================================================
 * DE helpers
 * ========================================================================== */
static void de_apply(void)
{
   FILE *f = fopen("/sys/kernel/debug/de/path1/apply", "w");
   if (f) { fprintf(f, "1\n"); fclose(f); }
}

/* Programme OSIZE + SR + E_COOR dans le DE.
 * src_w/src_h = dimensions source déjà dans ISIZE.
 * out_w/out_h/out_x/out_y = sortie souhaitée à l'écran. */
static void de_set_output(de_powkiddy_video_t *vid,
      unsigned out_w, unsigned out_h,
      unsigned out_x, unsigned out_y)
{
   unsigned src_w = vid->de_src_w;
   unsigned src_h = vid->de_src_h;

   vid->de_out_w = out_w;
   vid->de_out_h = out_h;
   vid->de_out_x = out_x;
   vid->de_out_y = out_y;

   /* OSIZE */
   vid->de[DE_OVL_OSIZE/4] = ((out_w - 1) & 0xFFFF) | ((out_h - 1) << 16);

   /* SR = (src / dst) * 0x2000 — virgule fixe */
   uint32_t sr_h = (src_w > 0) ? ((DE_SR_FRAC * src_w / out_w) & 0xFFFF) : DE_SR_FRAC;
   uint32_t sr_v = (src_h > 0) ? ((DE_SR_FRAC * src_h / out_h) & 0xFFFF) : DE_SR_FRAC;
   vid->de[DE_OVL_SR/4] = sr_h | (sr_v << 16);

   /* E_COOR : position output sur l'écran */
   vid->de[DE_PATH_E_COOR/4] = (out_x & 0xFFFF) | ((out_y & 0xFFFF) << 16);

   de_apply();
}

/* Programme ISIZE + BA0 + STR dans le DE.
 * Appelé quand la résolution source change.
 *
 * IMPORTANT — stride DE vs stride FB :
 *   Les fonctions de rotation écrivent dans buf_virt avec dst_stride = fb_pitch/2
 *   (stride physique du FB en pixels), ce qui adresse correctement chaque pixel
 *   dans le mmap.  Mais le DE doit connaître le stride UTILE du contenu, c'est-à-dire
 *   combien de pixels RGB565 séparent le début de deux lignes consécutives du point
 *   de vue du scaler.
 *
 *   Après rotation :
 *     ROT_0 / ROT_180  → lignes de src_w pixels  → stride utile = src_w
 *     ROT_90 / ROT_270 → le contenu rotaté a src_h colonnes écrites comme lignes
 *                         de src_w pixels chacune dans le FB → stride utile = src_w
 *   Dans tous les cas src_w est déjà la largeur rotatée (rot_w) passée par l'appelant.
 *
 *   DE_OVL_STR est en unités de 8 octets (= 4 pixels RGB565).
 *   stride_bytes = src_w * 2  → STR = src_w * 2 / 8 = src_w / 4
 *   On arrondit au multiple de 4 supérieur pour respecter l'alignement HW.
 */
static void de_set_source(de_powkiddy_video_t *vid,
      unsigned src_w, unsigned src_h)
{
   vid->de_src_w = src_w;
   vid->de_src_h = src_h;

   /* stride en unités de 8 octets, aligné sur 4 pixels (8 octets) */
   unsigned stride_pixels = (src_w + 3) & ~3u;   /* arrondi au multiple de 4 */
   unsigned str_val       = (vid->fb_pitch / 2) / 4;    /* /4 car 1 unité = 8 octets = 4 px RGB565 */

   vid->de[DE_OVL_BA0/4]   = vid->buf_phys;
   vid->de[DE_OVL_STR/4]   = str_val;
   vid->de[DE_OVL_ISIZE/4] = ((src_w - 1) & 0xFFFF) | ((src_h - 1) << 16);
   /* Pas de de_apply() ici — sera fait avec set_output juste après */
}

/* Restore DE à l'état normal (1:1, plein écran) */
static void de_restore(de_powkiddy_video_t *vid)
{
   unsigned w = vid->lcd_w;
   unsigned h = vid->lcd_h;
   vid->de[DE_OVL_ISIZE/4]    = ((w-1) & 0xFFFF) | ((h-1) << 16);
   vid->de[DE_OVL_OSIZE/4]    = ((w-1) & 0xFFFF) | ((h-1) << 16);
   vid->de[DE_OVL_SR/4]       = 0x20002000;
   vid->de[DE_PATH_E_COOR/4]  = 0;
   de_apply();
}

static void de_set_filter(de_powkiddy_video_t *vid)
{
   switch(vid->filtertype) 
   {
      case DINGUX_IPU_FILTER_SOFTWARE_NEAREST:
      //stop DE
         de_restore(vid);
         break;
      case DINGUX_IPU_FILTER_BICUBIC: //DE_SCLCOEF_ZOOMIN: / bicubic
         RARCH_LOG("[Powkiddy DE]: FILTER Changed bicubic\n");
      
         vid->de[DE_OVL_SCOEF0/4] = 0x00400000; 
         vid->de[DE_OVL_SCOEF1/4] = 0x04380400; // Plus de poids aux voisins (4 au lieu de 2)
         vid->de[DE_OVL_SCOEF2/4] = 0x08300800; // 8 + 48 + 8 = 64
         vid->de[DE_OVL_SCOEF3/4] = 0x0C280C00; // 12 + 40 + 12 = 64
         vid->de[DE_OVL_SCOEF4/4] = 0x10201000; // 16 + 32 + 16 = 64 (Mélange parfait au centre)
         vid->de[DE_OVL_SCOEF5/4] = 0x0C280C00; 
         vid->de[DE_OVL_SCOEF6/4] = 0x08300800;
         vid->de[DE_OVL_SCOEF7/4] = 0x04380400;
         break;
      case DINGUX_IPU_FILTER_BILINEAR: //DE_SCLCOEF_HALF_ZOOMOUT: / bilineaire
         RARCH_LOG("[Powkiddy DE]: FILTER Changed bilinear\n");
       vid->de[DE_OVL_SCOEF0/4] = 0x00400000; 
         vid->de[DE_OVL_SCOEF1/4] = 0x013A0500; // Un chouïa de pixels voisins (1 et 5)
         vid->de[DE_OVL_SCOEF2/4] = 0x02340A00; 
         vid->de[DE_OVL_SCOEF3/4] = 0x032E0F00; 
         vid->de[DE_OVL_SCOEF4/4] = 0x041C1C04; // 4 + 28 + 28 + 4 = 64
         vid->de[DE_OVL_SCOEF5/4] = 0x0F2E0300; 
         vid->de[DE_OVL_SCOEF6/4] = 0x0A340200;
         vid->de[DE_OVL_SCOEF7/4] = 0x053A0100;
          break;
     case DINGUX_IPU_FILTER_CATMULL_ROM:
         RARCH_LOG("[Powkiddy DE]: FILTER Catmull-Rom (Stable)\n");
         // On élargit les taps pour éviter les sauts de lignes
         vid->de[DE_OVL_SCOEF0/4] = 0x00400000; 
         vid->de[DE_OVL_SCOEF1/4] = 0x02380600; 
         vid->de[DE_OVL_SCOEF2/4] = 0x04320A00; 
         vid->de[DE_OVL_SCOEF3/4] = 0x062A0E02; // On ajoute un chouïa de 4ème tap (02)
         vid->de[DE_OVL_SCOEF4/4] = 0x08181808; 
         vid->de[DE_OVL_SCOEF5/4] = 0x0E2A0602; 
         vid->de[DE_OVL_SCOEF6/4] = 0x0A320400;
         vid->de[DE_OVL_SCOEF7/4] = 0x06380200;
         break;

      case DINGUX_IPU_FILTER_LANCZOS:
         RARCH_LOG("[Powkiddy DE]: FILTER Lanczos (Fake-Sharp Stable)\n");
         // Le vrai Lanczos (négatif) fait sauter ton hardware. 
         // On simule le piqué en serrant la phase centrale au maximum.
         vid->de[DE_OVL_SCOEF0/4] = 0x00400000; 
         vid->de[DE_OVL_SCOEF1/4] = 0x013E0100; // Ultra serré
         vid->de[DE_OVL_SCOEF2/4] = 0x023C0200; 
         vid->de[DE_OVL_SCOEF3/4] = 0x033A0300; 
         vid->de[DE_OVL_SCOEF4/4] = 0x04380400; // Très peu de mélange
         vid->de[DE_OVL_SCOEF5/4] = 0x033A0300; 
         vid->de[DE_OVL_SCOEF6/4] = 0x023C0200;
         vid->de[DE_OVL_SCOEF7/4] = 0x013E0100;
         break;

      case DINGUX_IPU_FILTER_SHARP_BILINEAR:
    RARCH_LOG("[Powkiddy DE]: FILTER Sharp Bilinear (Ultra-Stable)\n");
         /* Si ça saute encore, c'est que le hardware veut ABSOLUMENT 
          * voir les 3 pixels (A, centre, B) sur les phases intermédiaires.
          */
         vid->de[DE_OVL_SCOEF0/4] = 0x00400000; 
         vid->de[DE_OVL_SCOEF1/4] = 0x023C0200; // 2 + 60 + 2 = 64
         vid->de[DE_OVL_SCOEF2/4] = 0x04380400; // 4 + 56 + 4 = 64
         vid->de[DE_OVL_SCOEF3/4] = 0x06340600; // 6 + 52 + 6 = 64
         vid->de[DE_OVL_SCOEF4/4] = 0x08300800; // 8 + 48 + 8 = 64 (Mélange stable)
         vid->de[DE_OVL_SCOEF5/4] = 0x06340600; 
         vid->de[DE_OVL_SCOEF6/4] = 0x04380400;
         vid->de[DE_OVL_SCOEF7/4] = 0x023C0200;
         break;

      case DINGUX_IPU_FILTER_NEAREST:   //nearest
      default:
         RARCH_LOG("[Powkiddy DE]: FILTER Changed nearest\n");
         vid->de[DE_OVL_SCOEF0/4] = 0x00400000;
         vid->de[DE_OVL_SCOEF1/4] = 0x00400000;
         vid->de[DE_OVL_SCOEF2/4] = 0x00400000;
         vid->de[DE_OVL_SCOEF3/4] = 0x00400000;
         vid->de[DE_OVL_SCOEF4/4] = 0x00400000;
         vid->de[DE_OVL_SCOEF5/4] = 0x00400000;
         vid->de[DE_OVL_SCOEF6/4] = 0x00400000;
         vid->de[DE_OVL_SCOEF7/4] = 0x00400000;
	}   
}

/* ==========================================================================
 * Calcul du rectangle de sortie
 *
 * Retourne la position et taille à passer au DE (OSIZE + E_COOR).
 * Pour les rotations 90/270, les axes source sont déjà swappés avant
 * d'arriver ici (rotation C -> src_w/src_h déjà dans l'espace rotaté).
 * ========================================================================== */
/* ==========================================================================
 * Compute output rectangle
 *
 * All modes:
 *   dst_w, dst_h = physical FB dimensions
 *
 * For rotated modes (90°/270°), source axes are swapped before scaling:
 *   rotated_w = src_h, rotated_h = src_w
 *
 * integer_scaling && !keep_aspect : fill full physical width (dst_w), height by ratio
 * !integer_scaling && keep_aspect:     1:1 pixel, centered
 * integer_scaling && keep_aspect: max scaling rounded to integer (x2,x3,x4)
 * !integer_scaling && !keep_aspect: full screen stretch
 * ========================================================================== */
static void powkiddy_compute_out_rect(
      unsigned dst_w, unsigned dst_h,
      unsigned rotated_w, unsigned rotated_h,
      bool integer_scaling, bool keep_aspect,
      unsigned *out_x, unsigned *out_y,
      unsigned *out_w, unsigned *out_h)
{
   unsigned ow, oh;

   if (integer_scaling && !keep_aspect)
   {
      float ratio = (float)rotated_w / (float)rotated_h;
      ow = (unsigned)(ratio * dst_h + 0.5f);
      oh = dst_h;
      if (ow > dst_w) {
         ow = dst_w;
         oh = (unsigned)(dst_w / ratio + 0.5f);
      }
      if (ow < 1) ow = 1;
      if (oh < 1) oh = 1;
   }
   else if (integer_scaling && keep_aspect)
   {
      unsigned sx = dst_w / rotated_w;
      unsigned sy = dst_h / rotated_h;
      unsigned s  = (sx < sy) ? sx : sy;
      if (s < 1) s = 1;
      ow = rotated_w * s;
      oh = rotated_h * s;
   }
   else if (keep_aspect)
   {
      ow = (rotated_w < dst_w) ? rotated_w : dst_w;
      oh = (rotated_h < dst_h) ? rotated_h : dst_h;
   }
   else
   {
      ow = dst_w;
      oh = dst_h;
   }

   *out_x = (dst_w - ow) / 2;
   *out_y = (dst_h - oh) / 2;
   *out_w = ow;
   *out_h = oh;
}
 static void sdl_powkiddy_compute_out_rect(
      de_powkiddy_video_t *vid,
      unsigned src_w, unsigned src_h,
      unsigned *out_x, unsigned *out_y,
      unsigned *out_w, unsigned *out_h)
{
   unsigned rotated_w, rotated_h;
   if (vid->rotation == ROT_90 || vid->rotation == ROT_270)
      { rotated_w = src_h; rotated_h = src_w; }
   else
      { rotated_w = src_w; rotated_h = src_h; }

   powkiddy_compute_out_rect(
         vid->fb_width, vid->fb_height,
         rotated_w, rotated_h,
         vid->integer_scaling, vid->keep_aspect,
         out_x, out_y, out_w, out_h);
}

static void de_powkiddy_compute_out_rect(
      de_powkiddy_video_t *vid,
      unsigned rotated_w, unsigned rotated_h,
      unsigned *out_x, unsigned *out_y,
      unsigned *out_w, unsigned *out_h)
{
   powkiddy_compute_out_rect(
         vid->lcd_w, vid->lcd_h,
         rotated_w, rotated_h,
         vid->integer_scaling, vid->keep_aspect,
         out_x, out_y, out_w, out_h);
}

static void scale_nearest_16(
      de_powkiddy_video_t *vid,
      const uint16_t * __restrict__ src,
      unsigned src_w, unsigned src_h, unsigned src_stride,
      uint16_t * __restrict__ fb, unsigned fb_stride,
      unsigned out_x, unsigned out_y, unsigned out_w, unsigned out_h)
{
   unsigned dx, dy;

   switch (vid->rotation)
   {
      case ROT_270:
      {
         /* Column-major write: src_x -> fb_y (inverted), src_y -> fb_x */
         unsigned sx_lut[DE_POWKIDDY_MAX_FB_H];
         unsigned step = ((unsigned long long)src_w << 16) / out_h;
         unsigned fp   = (unsigned long long)(out_h - 1) * step;
         for (dy = 0; dy < out_h; dy++, fp -= step)
         {
            unsigned sx = fp >> 16;
            if (sx >= src_w) sx = src_w - 1;
            sx_lut[dy] = sx;
         }
         unsigned step_y = ((unsigned long long)src_h << 16) / out_w;
         unsigned fy     = 0;
         for (dx = 0; dx < out_w; dx++, fy += step_y)
         {
            unsigned sy = fy >> 16;
            if (sy >= src_h) sy = src_h - 1;
            const uint16_t *src_row = src + sy * src_stride;
            uint16_t *dst_col = fb + out_y * fb_stride + (out_x + dx);
            for (dy = 0; dy < out_h; dy++)
               dst_col[dy * fb_stride] = src_row[sx_lut[dy]];
         }
         break;
      }

      case ROT_90:
      {
         /* Column-major write: src_x -> fb_y (normal), src_y -> fb_x (inverted) */
         unsigned sx_lut[DE_POWKIDDY_MAX_FB_H];
         unsigned step = ((unsigned long long)src_w << 16) / out_h;
         unsigned fp   = 0;
         for (dy = 0; dy < out_h; dy++, fp += step)
         {
            unsigned sx = fp >> 16;
            if (sx >= src_w) sx = src_w - 1;
            sx_lut[dy] = sx;
         }
         unsigned step_y = ((unsigned long long)src_h << 16) / out_w;
         unsigned fy     = 0;
         for (dx = 0; dx < out_w; dx++, fy += step_y)
         {
            unsigned sy = fy >> 16;
            if (sy >= src_h) sy = src_h - 1;
            const uint16_t *src_row = src + sy * src_stride;
            uint16_t *dst_col = fb + out_y * fb_stride + (out_x + out_w - 1 - dx);
            for (dy = 0; dy < out_h; dy++)
               dst_col[dy * fb_stride] = src_row[sx_lut[dy]];
         }
         break;
      }

      case ROT_180:
      {
         /* Row-major write, both axes inverted */
         unsigned sy_lut[DE_POWKIDDY_MAX_FB_W];
         unsigned step = ((unsigned long long)src_h << 16) / out_h;
         unsigned fp   = 0;
         for (dy = 0; dy < out_h; dy++, fp += step)
         {
            unsigned sy = fp >> 16;
            if (sy >= src_h) sy = src_h - 1;
            sy_lut[dy] = sy;
         }
         unsigned step_x = ((unsigned long long)src_w << 16) / out_w;
         unsigned fx     = 0;
         for (dx = 0; dx < out_w; dx++, fx += step_x)
         {
            unsigned sx = fx >> 16;
            if (sx >= src_w) sx = src_w - 1;
            for (dy = 0; dy < out_h; dy++)
               fb[(out_y + out_h - 1 - dy) * fb_stride + (out_x + out_w - 1 - dx)]
                     = src[sy_lut[dy] * src_stride + sx];
         }
         break;
      }

      default: /* ROT_0 — direct copy */
      {
         unsigned sy_lut[DE_POWKIDDY_MAX_FB_H];
         unsigned step = ((unsigned long long)src_h << 16) / out_h;
         unsigned fp   = 0;
         for (dy = 0; dy < out_h; dy++, fp += step)
         {
            unsigned sy = fp >> 16;
            if (sy >= src_h) sy = src_h - 1;
            sy_lut[dy] = sy;
         }
         unsigned step_x = ((unsigned long long)src_w << 16) / out_w;
         unsigned fx     = 0;
         for (dx = 0; dx < out_w; dx++, fx += step_x)
         {
            unsigned sx = fx >> 16;
            if (sx >= src_w) sx = src_w - 1;
            uint16_t *dst_col = fb + out_y * fb_stride + (out_x + dx);
            for (dy = 0; dy < out_h; dy++)
               dst_col[dy * fb_stride] = src[sy_lut[dy] * src_stride + sx];
         }
         break;
      }
   }
}


/* ==========================================================================
 * 32bpp (XRGB8888) -> 16bpp nearest scaler, all rotations
 * ========================================================================== */
static void scale_nearest_32to16(
      de_powkiddy_video_t *vid,
      const uint32_t * __restrict__ src,
      unsigned src_w, unsigned src_h, unsigned src_stride,
      uint16_t * __restrict__ fb, unsigned fb_stride,
      unsigned out_x, unsigned out_y, unsigned out_w, unsigned out_h)
{
   unsigned dx, dy;

   switch (vid->rotation)
   {
      case ROT_270:
      {
         unsigned sx_lut[DE_POWKIDDY_MAX_FB_H];
         unsigned step = ((unsigned long long)src_w << 16) / out_h;
         unsigned fp   = (unsigned long long)(out_h - 1) * step;
         for (dy = 0; dy < out_h; dy++, fp -= step)
         {
            unsigned sx = fp >> 16;
            if (sx >= src_w) sx = src_w - 1;
            sx_lut[dy] = sx;
         }
         unsigned step_y = ((unsigned long long)src_h << 16) / out_w;
         unsigned fy     = 0;
         for (dx = 0; dx < out_w; dx++, fy += step_y)
         {
            unsigned sy = fy >> 16;
            if (sy >= src_h) sy = src_h - 1;
            const uint32_t *src_row = src + sy * src_stride;
            uint16_t *dst_col = fb + out_y * fb_stride + (out_x + dx);
            for (dy = 0; dy < out_h; dy++)
            {
               uint32_t c = src_row[sx_lut[dy]];
               dst_col[dy * fb_stride] = (uint16_t)(
                     ((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F));
            }
         }
         break;
      }

      case ROT_90:
      {
         unsigned sx_lut[DE_POWKIDDY_MAX_FB_H];
         unsigned step = ((unsigned long long)src_w << 16) / out_h;
         unsigned fp   = 0;
         for (dy = 0; dy < out_h; dy++, fp += step)
         {
            unsigned sx = fp >> 16;
            if (sx >= src_w) sx = src_w - 1;
            sx_lut[dy] = sx;
         }
         unsigned step_y = ((unsigned long long)src_h << 16) / out_w;
         unsigned fy     = 0;
         for (dx = 0; dx < out_w; dx++, fy += step_y)
         {
            unsigned sy = fy >> 16;
            if (sy >= src_h) sy = src_h - 1;
            const uint32_t *src_row = src + sy * src_stride;
            uint16_t *dst_col = fb + out_y * fb_stride + (out_x + out_w - 1 - dx);
            for (dy = 0; dy < out_h; dy++)
            {
               uint32_t c = src_row[sx_lut[dy]];
               dst_col[dy * fb_stride] = (uint16_t)(
                     ((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F));
            }
         }
         break;
      }

      default: /* ROT_0 and ROT_180 */
      {
         unsigned sy_lut[DE_POWKIDDY_MAX_FB_H];
         unsigned step = ((unsigned long long)src_h << 16) / out_h;
         unsigned fp   = 0;
         for (dy = 0; dy < out_h; dy++, fp += step)
         {
            unsigned sy = fp >> 16;
            if (sy >= src_h) sy = src_h - 1;
            sy_lut[dy] = sy;
         }
         unsigned step_x = ((unsigned long long)src_w << 16) / out_w;
         unsigned fx     = 0;
         for (dx = 0; dx < out_w; dx++, fx += step_x)
         {
            unsigned sx = fx >> 16;
            if (sx >= src_w) sx = src_w - 1;
            for (dy = 0; dy < out_h; dy++)
            {
               uint32_t c = src[sy_lut[dy] * src_stride + sx];
               uint16_t px = (uint16_t)(
                     ((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F));
               if (vid->rotation == ROT_180)
                  fb[(out_y+out_h-1-dy)*fb_stride + (out_x+out_w-1-dx)] = px;
               else
                  fb[(out_y+dy)*fb_stride + (out_x+dx)] = px;
            }
         }
         break;
      }
   }
}

/* ==========================================================================
 * Nearest-neighbor scaler — 16bpp, all rotations
 * ========================================================================== */
/* ==========================================================================
 * Dispatcher: clear black borders + dispatch to scaler
 * ========================================================================== */
static void sdl_powkiddy_blit_16(
      de_powkiddy_video_t *vid,
      const uint16_t * __restrict__ src,
      unsigned src_w, unsigned src_h, unsigned src_stride,
      uint16_t * __restrict__ fb, unsigned fb_stride,
      unsigned out_x, unsigned out_y, unsigned out_w, unsigned out_h)
{
   unsigned dy;
   unsigned fb_w = vid->fb_width;
   unsigned fb_h = vid->fb_height;

   /* Clear black borders top/bottom */
   if (out_y > 0)
      memset(fb, 0, out_y * fb_stride * sizeof(uint16_t));
   if (out_y + out_h < fb_h)
      memset(fb + (out_y + out_h) * fb_stride, 0,
             (fb_h - out_y - out_h) * fb_stride * sizeof(uint16_t));
   /* Clear black borders left/right */
   if (out_x > 0 || out_x + out_w < fb_w)
   {
      for (dy = out_y; dy < out_y + out_h; dy++)
      {
         uint16_t *row = fb + dy * fb_stride;
         if (out_x > 0)
            memset(row, 0, out_x * sizeof(uint16_t));
         if (out_x + out_w < fb_w)
            memset(row + out_x + out_w, 0,
                   (fb_w - out_x - out_w) * sizeof(uint16_t));
      }
   }

   scale_nearest_16(vid, src, src_w, src_h, src_stride,
               fb, fb_stride, out_x, out_y, out_w, out_h);
}

static void sdl_powkiddy_blit_32(
      de_powkiddy_video_t *vid,
      const uint32_t * __restrict__ src,
      unsigned src_w, unsigned src_h, unsigned src_stride,
      uint16_t * __restrict__ fb, unsigned fb_stride,
      unsigned out_x, unsigned out_y, unsigned out_w, unsigned out_h)
{
   unsigned dy;
   unsigned fb_w = vid->fb_width;
   unsigned fb_h = vid->fb_height;

   if (out_y > 0)
      memset(fb, 0, out_y * fb_stride * sizeof(uint16_t));
   if (out_y + out_h < fb_h)
      memset(fb + (out_y + out_h) * fb_stride, 0,
             (fb_h - out_y - out_h) * fb_stride * sizeof(uint16_t));
   if (out_x > 0 || out_x + out_w < fb_w)
   {
      for (dy = out_y; dy < out_y + out_h; dy++)
      {
         uint16_t *row = fb + dy * fb_stride;
         if (out_x > 0)
            memset(row, 0, out_x * sizeof(uint16_t));
         if (out_x + out_w < fb_w)
            memset(row + out_x + out_w, 0,
                   (fb_w - out_x - out_w) * sizeof(uint16_t));
      }
   }
   scale_nearest_32to16(vid, src, src_w, src_h, src_stride,
         fb, fb_stride, out_x, out_y, out_w, out_h);
}

/* ==========================================================================
 * Rotation-only copy vers le buffer source DE
 *
 * On écrit exactement src_w * src_h pixels dans buf_virt,
 * avec le stride complet du FB (fb_pitch / 2 pixels par ligne).
 * Pas d'upscaling — le DE s'en charge.
 *
 * Pour ROT_90/270 : src (w x h) → buf (h x w) dans l'espace FB
 * Pour ROT_0/180  : src (w x h) → buf (w x h)
 * ========================================================================== */

/* 16bpp → 16bpp, rotation seule */
static void de_rotate_16(
      de_powkiddy_video_t *vid,
      const uint16_t * __restrict__ src,
      unsigned src_w, unsigned src_h, unsigned src_stride,
      uint16_t * __restrict__ dst, unsigned dst_stride)
{
   unsigned x, y;

   switch (vid->rotation)
   {
      case ROT_90: // 90° Clockwise
         for (y = 0; y < src_h; y++) {
            const uint16_t *src_row = src + y * src_stride;
            for (x = 0; x < src_w; x++)
               // Le X devient la ligne, le Y inversé devient la colonne
               dst[x * dst_stride + (src_h - 1 - y)] = src_row[x];
         }
         break;

      case ROT_270: // 90° Counter-Clockwise
         for (y = 0; y < src_h; y++) {
            const uint16_t *src_row = src + y * src_stride;
            for (x = 0; x < src_w; x++)
               // Le X inversé devient la ligne, le Y devient la colonne
               dst[(src_w - 1 - x) * dst_stride + y] = src_row[x];
         }
         break;

      case ROT_180:
         /* src(x,y) → dst(src_h-1-y, src_w-1-x) */
         for (y = 0; y < src_h; y++)
         {
            const uint16_t *src_row = src + y * src_stride;
            uint16_t *dst_row       = dst + (src_h - 1 - y) * dst_stride;
            for (x = 0; x < src_w; x++)
               dst_row[src_w - 1 - x] = src_row[x];
         }
         break;

      default: /* ROT_0 */
         for (y = 0; y < src_h; y++)
            memcpy(dst + y * dst_stride, src + y * src_stride,
                   src_w * sizeof(uint16_t));
         break;
   }
}

/* 32bpp XRGB8888 → 16bpp RGB565, rotation seule */
static void de_rotate_32to16(
      de_powkiddy_video_t *vid,
      const uint32_t * __restrict__ src,
      unsigned src_w, unsigned src_h, unsigned src_stride,
      uint16_t * __restrict__ dst, unsigned dst_stride)
{
   unsigned x, y;

#define XRGB_TO_RGB565(c) ((uint16_t)( \
      (((c) >> 8) & 0xF800) | \
      (((c) >> 5) & 0x07E0) | \
      (((c) >> 3) & 0x001F)))

   switch (vid->rotation)
   {
      case ROT_90:
         for (y = 0; y < src_h; y++)
         {
            const uint32_t *src_row = src + y * src_stride;
            for (x = 0; x < src_w; x++)
               dst[x * dst_stride + (src_h - 1 - y)] = XRGB_TO_RGB565(src_row[x]);
         }
         break;

      case ROT_270:
         for (y = 0; y < src_h; y++)
         {
            const uint32_t *src_row = src + y * src_stride;
            for (x = 0; x < src_w; x++)
               dst[(src_w - 1 - x) * dst_stride + y] = XRGB_TO_RGB565(src_row[x]);
         }
         break;

      case ROT_180:
         for (y = 0; y < src_h; y++)
         {
            const uint32_t *src_row = src + y * src_stride;
            uint16_t *dst_row       = dst + (src_h - 1 - y) * dst_stride;
            for (x = 0; x < src_w; x++)
               dst_row[src_w - 1 - x] = XRGB_TO_RGB565(src_row[x]);
         }
         break;

      default: /* ROT_0 */
         for (y = 0; y < src_h; y++)
         {
            const uint32_t *src_row = src + y * src_stride;
            uint16_t *dst_row       = dst + y * dst_stride;
            for (x = 0; x < src_w; x++)
               dst_row[x] = XRGB_TO_RGB565(src_row[x]);
         }
         break;
   }
#undef XRGB_TO_RGB565
}

/* ==========================================================================
 * Mise à jour DE si la résolution source ou le mode d'affichage a changé
 *
 * rotated_w/h = dimensions après rotation (ce que le DE recevra en ISIZE)
 * ========================================================================== */
static void de_powkiddy_update_de(de_powkiddy_video_t *vid,
      unsigned rotated_w, unsigned rotated_h)
{
   if (!vid->de_ok) return;

   unsigned out_x, out_y, out_w, out_h;
   de_powkiddy_compute_out_rect(vid, rotated_w, rotated_h,
         &out_x, &out_y, &out_w, &out_h);

   bool src_changed = (rotated_w != vid->de_src_w ||
                       rotated_h != vid->de_src_h);
   bool out_changed = (out_w != vid->de_out_w || out_h != vid->de_out_h ||
                       out_x != vid->de_out_x || out_y != vid->de_out_y);

   if (src_changed)
   {
      de_set_source(vid, rotated_w, rotated_h);
      RARCH_LOG("[Powkiddy DE]: ISIZE %ux%u\n", rotated_w, rotated_h);
   }

   if (src_changed || out_changed)
   {
      de_set_output(vid, out_w, out_h, out_x, out_y);
      RARCH_LOG("[Powkiddy DE]: OSIZE %ux%u E_COOR %u,%u\n",
                out_w, out_h, out_x, out_y);
   }
   //check if nearest or bilinear has changed
   settings_t *settings = config_get_ptr();
   if(settings->uints.video_dingux_ipu_filter_type != vid->filtertype)
   {
      RARCH_LOG("[Powkiddy DE]: FILTER Changed: old value %d new value %d\n",
                vid->filtertype, settings->uints.video_dingux_ipu_filter_type);
      vid->filtertype = settings->uints.video_dingux_ipu_filter_type;
      de_set_filter(vid);
   }
   vid->scaling = (float)(vid->de_src_w * vid->de_src_h) / (vid->de_out_w * vid->de_out_h);
   if (vid->filtertype == DINGUX_IPU_FILTER_SOFTWARE_NEAREST) {
       de_set_source(vid, vid->fb_width, vid->fb_height);
       de_set_output(vid, vid->fb_width, vid->fb_height, 0, 0);
       de_restore(vid);
    
   }
   //RARCH_LOG("Scaling %f\n", vid->scaling);
}

/* ==========================================================================
 * Dimensions logiques menu
 * ========================================================================== */
static void de_powkiddy_update_menu_dims(de_powkiddy_video_t *vid)
{
   /* En rotation 90/270 l'espace "paysage" du menu est lcd_h x lcd_w */
   if (vid->rotation == ROT_90 || vid->rotation == ROT_270)
   {
      vid->menu_w = vid->lcd_h;
      vid->menu_h = vid->lcd_w;
   }
   else
   {
      vid->menu_w = vid->lcd_w;
      vid->menu_h = vid->lcd_h;
   }
}

/* ==========================================================================
 * Font / OSD
 *
 * Écrit directement dans fb_map (le même buffer que le DE lit).
 * x, y en espace logique paysage (menu_w x menu_h).
 * ========================================================================== */
static void de_powkiddy_init_font_color(de_powkiddy_video_t *vid)
{
   settings_t *settings = config_get_ptr();
   uint32_t red = 0xFF, green = 0xFF, blue = 0xFF;
   if (settings)
   {
      red   = (uint32_t)((settings->floats.video_msg_color_r * 255.0f) + 0.5f) & 0xFF;
      green = (uint32_t)((settings->floats.video_msg_color_g * 255.0f) + 0.5f) & 0xFF;
      blue  = (uint32_t)((settings->floats.video_msg_color_b * 255.0f) + 0.5f) & 0xFF;
   }
   vid->font_colour32 = (red << 16) | (green << 8) | blue;
   vid->font_colour16 = (uint16_t)(((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3));
}
//
//static void de_powkiddy_blit_osd(de_powkiddy_video_t *vid, const char *str)
//{
//    if (!vid->buf_virt || !vid->osd_font || !str || !str[0]) return;
//
//    uint16_t *fb       = (uint16_t*)vid->buf_virt;
//    unsigned fb_stride = vid->fb_pitch / 2;  // 480px
//    uint16_t col       = vid->font_colour16;
//    bool **font_lut    = vid->osd_font->lut;
//
//    // Ratio scaling : combien de pixels src pour 1 pixel dst
//    // Si ratio < 1.0 (upscaling) : 1 pixel src = 1/ratio pixels dst
//    float ratio_w = (vid->de_out_w > 0) ? (float)vid->de_src_w / vid->de_out_w : 1.0f;
//    float ratio_h = (vid->de_out_h > 0) ? (float)vid->de_src_h / vid->de_out_h : 1.0f;
//
//    // Taille de la police dans l'espace source pour avoir ~8px à l'écran
//    // On veut FONT_HEIGHT pixels à l'écran → FONT_HEIGHT * ratio_h pixels source
//    // Mais on garde la police originale et on la place en bas
//    // Position Y dans buf_virt pour que le texte soit en bas de l'écran affiché
//    // bas écran affiché = out_y + out_h pixels → dans source = de_src_h lignes
//    // On veut OSD à out_h - 8px de l'écran → src_y = src_h - 8*ratio_h
//
//    unsigned osd_h_screen = FONT_HEIGHT + 2;  // pixels écran souhaités
//    unsigned osd_h_src    = (unsigned)(osd_h_screen * ratio_h + 0.5f);
//    if (osd_h_src < 1) osd_h_src = 1;
//
//    // Position source : bas du buffer source
//    //unsigned src_y0 = (vid->de_src_h > osd_h_src) ? vid->de_src_h - osd_h_src : 0;
//    unsigned src_y0 = vid->de_src_h - osd_h_screen;
//    // Dessine pixel par pixel dans l'espace source
//    unsigned x_pos = 2+8;
//    RARCH_LOG("print osd at %d:%d de_src_h %d osd_h_src %d\n", x_pos, src_y0, vid->de_src_h, osd_h_src);
//    while (!string_is_empty(str)) {
//        if (*str == ' ') { str++; x_pos += FONT_WIDTH + 1; continue; }
//        uint32_t symbol = utf8_walk(&str);
//        if (symbol >= DE_POWKIDDY_NUM_FONT_GLYPHS) continue;
//        bool *sym_lut = font_lut[symbol];
//
//        for (unsigned j = 0; j < FONT_HEIGHT; j++) {
//            // Position Y dans source
//            unsigned src_y = src_y0 + (unsigned)(j * ratio_h);
//            if (src_y >= vid->de_src_h) continue;
//
//            for (unsigned i = 0; i < FONT_WIDTH; i++) {
//                if (!sym_lut[i + j * FONT_WIDTH]) continue;
//                unsigned src_x = x_pos + i;
//                if (src_x >= vid->de_src_w) break;
//
//                // Rotation inverse pour écrire au bon endroit dans buf_virt
//                unsigned fpx, fpy;
//                switch (vid->rotation) {
//                    case ROT_90:
//                        fpx = vid->de_src_w - 1 - src_y;
//                        fpy = src_x;
//                        break;
//                    case ROT_270:
//                        fpx = src_y;
//                        fpy = vid->de_src_h - 1 - src_x;
//                        break;
//                    case ROT_180:
//                        fpx = vid->de_src_w - 1 - src_x;
//                        fpy = vid->de_src_h - 1 - src_y;
//                        break;
//                    default:
//                        fpx = src_x;
//                        fpy = src_y;
//                        break;
//                }
//                if (fpx < vid->fb_pitch/2 && fpy < vid->fb_height)
//                    fb[fpy * fb_stride + fpx] = col;
//            }
//        }
//        x_pos += FONT_WIDTH + 1;
//        if (x_pos >= vid->de_src_w) break;
//    }
//}
static void de_powkiddy_blit_text(de_powkiddy_video_t *vid,
      unsigned x, unsigned y, const char *str)
{
   if (!vid->buf_virt) return;
   
   uint16_t *fb        = (uint16_t*)vid->buf_virt;
   /* Le buffer est compact : stride = rot_w arrondi à 4px, exactement comme DE_OVL_STR.
    * On récupère de_src_w qui a été aligné par de_set_source(). */

    

   unsigned  fb_stride = vid->fb_pitch / 2;
   bool    **font_lut  = vid->osd_font->lut;
   uint16_t  col       = vid->font_colour16;
   unsigned  x_pos     = x;
   unsigned  y_pos     = y;
   int font_height = (int)(FONT_HEIGHT);
   int font_width = (int)(FONT_WIDTH);
   int font_width_stride = font_width + 1;
   if (y_pos + font_height + 1 >= vid->menu_h) return;

   while (!string_is_empty(str))
   {
      if (x_pos + font_width_stride + 1 >= vid->menu_w) return;
      if (*str == ' ') { str++; x_pos += font_width_stride; continue; }

      uint32_t symbol = utf8_walk(&str);
      if (symbol == 339) symbol = 156;
      if (symbol == 338) symbol = 140;
      if (symbol >= DE_POWKIDDY_NUM_FONT_GLYPHS) continue;

      bool *sym_lut = font_lut[symbol];
      unsigned i, j;
      for (j = 0; j < font_height; j++)
      {
         unsigned ly = y_pos + j;
         for (i = 0; i < font_width; i++)
         {
            if (!sym_lut[i + j * font_width]) continue;
            unsigned lx = x_pos + i;
            unsigned fpx, fpy;

            /* Coordonnées dans le buffer compact (de_src_w × de_src_h).
             * Le buffer est compact : stride = (de_src_w+3)&~3.
             * Les bornes sont de_src_w / de_src_h, pas fb_width / fb_height. */
            unsigned bw = (vid->de_src_w > 0) ? vid->de_src_w : vid->fb_width;
            unsigned bh = (vid->de_src_h > 0) ? vid->de_src_h : vid->fb_height;

            switch (vid->rotation)
            {
               case ROT_270:
                  fpx = ly;
                  fpy = bh - 1 - lx;
                  break;
               case ROT_90:
                  fpx = bw - 1 - ly;
                  fpy = lx;
                  break;
               case ROT_180:
                  fpx = bw - 1 - lx;
                  fpy = bh - 1 - ly;
                  break;
               default:
                  fpx = lx;
                  fpy = ly;
                  break;
            }

            if (fpx < bw && fpy < bh)
               fb[fpy * fb_stride + fpx] = col;
         }
      }
      x_pos += font_width_stride;
   }
}
static void de_powkiddy_blit_video_mode_error_msg(de_powkiddy_video_t *vid)
{
   const char *error_msg = msg_hash_to_str(MSG_UNSUPPORTED_VIDEO_MODE);
   char display_mode[64];
   display_mode[0] = '\0';
   if (vid->buf_virt)
      memset(vid->buf_virt, 0, vid->fb_pitch * vid->fb_height);
   snprintf(display_mode, sizeof(display_mode), "> %ux%u, %s",
         vid->frame_width, vid->frame_height,
         vid->rgb32 ? "XRGB8888" : "RGB565");
   de_powkiddy_blit_text(vid, FONT_WIDTH_STRIDE, FONT_WIDTH_STRIDE,  error_msg);
   de_powkiddy_blit_text(vid,  FONT_WIDTH_STRIDE,
         FONT_WIDTH_STRIDE + FONT_HEIGHT_STRIDE, display_mode);
}

static void de_powkiddy_apply_state_changes(void *data)
{
   de_powkiddy_video_t *vid = (de_powkiddy_video_t*)data;
   settings_t *settings      = config_get_ptr();
   if (!vid || !settings) return;

   bool ka      = settings->bools.video_dingux_ipu_keep_aspect;
   bool is      = settings->bools.video_scale_integer;
   unsigned rot = settings->uints.video_rotation & 3;
   bool changed = false;
  //check if nearest or bilinear has changed
   
   if (vid->keep_aspect != ka || vid->integer_scaling != is)
   {
      vid->keep_aspect     = ka;
      vid->integer_scaling = is;
      /* Force recalcul DE au prochain frame */
      vid->de_out_w = 0;
      vid->de_out_h = 0;
      changed = true;
   }

   if (vid->rotation != rot)
   {
      vid->rotation = rot;
      de_powkiddy_update_menu_dims(vid);
      /* Force recalcul ISIZE + OSIZE */
      vid->de_src_w = 0;
      vid->de_src_h = 0;
      vid->de_out_w = 0;
      vid->de_out_h = 0;
      changed = true;
   }
   if(settings->uints.video_dingux_ipu_filter_type != vid->filtertype)
   {
      RARCH_LOG("[Powkiddy DE]: FILTER Changed: old value %d new value %d\n",
                vid->filtertype, settings->uints.video_dingux_ipu_filter_type);
      vid->filtertype = settings->uints.video_dingux_ipu_filter_type;
      vid->rotation = rot;
      de_powkiddy_update_menu_dims(vid);
      /* Force recalcul ISIZE + OSIZE */
      vid->de_src_w = 0;
      vid->de_src_h = 0;
      vid->de_out_w = 0;
      vid->de_out_h = 0;
      changed = true;
      de_set_filter(vid);
   }
   if (changed)
      RARCH_LOG("[Powkiddy DE]: State change: keep=%d int=%d rot=%u menu=%ux%u\n",
                vid->keep_aspect, vid->integer_scaling,
                vid->rotation, vid->menu_w, vid->menu_h);
}

/* ==========================================================================
 * Init / Free
 * ========================================================================== */
static void de_powkiddy_gfx_free(void *data)
{
   de_powkiddy_video_t *vid = (de_powkiddy_video_t*)data;
   if (!vid) return;

   /* Restore DE à l'état 1:1 plein écran */
   if (vid->de_ok && vid->de)
   {
      de_restore(vid);
      munmap((void*)vid->de, DE_SIZE);
   }
   if (vid->fd_mem >= 0) close(vid->fd_mem);

   if (vid->fb_map && vid->fb_map != (void*)-1)
      munmap(vid->fb_map, vid->fb_map_size);
   if (vid->fd_fb >= 0) close(vid->fd_fb);

   if (vid->osd_font)       bitmapfont_free_lut(vid->osd_font);
   if (vid->last_frame_buf) free(vid->last_frame_buf);
   if (vid->menu_texture)   free(vid->menu_texture);
   free(vid);
}

static void *de_powkiddy_gfx_init(const video_info_t *video,
      input_driver_t **input, void **input_data)
{
   RARCH_LOG("[Powkiddy DE]: INIT\n");

   de_powkiddy_video_t *vid = NULL;
   settings_t *settings     = config_get_ptr();

   bool ipu_keep_aspect     = settings->bools.video_dingux_ipu_keep_aspect;
   bool ipu_integer_scaling = settings->bools.video_scale_integer;
   uint ipu_filtertype      = settings->uints.video_dingux_ipu_filter_type;
   unsigned rotation        = settings->uints.video_rotation;

   vid = (de_powkiddy_video_t*)calloc(1, sizeof(*vid));
   if (!vid) return NULL;

   vid->fd_fb  = -1;
   vid->fd_mem = -1;
   vid->de_ok  = false;

   /* ------------------------------------------------------------------
    * Ouvre /dev/fb0 et mmap le framebuffer
    * ------------------------------------------------------------------ */
   vid->fd_fb = open("/dev/fb0", O_RDWR);
   if (vid->fd_fb < 0)
   {
      RARCH_ERR("[Powkiddy DE]: Cannot open /dev/fb0\n");
      goto error;
   }

   /* Dimensions physiques depuis fbdev */
   {
      struct fb_var_screeninfo vinfo;
      struct fb_fix_screeninfo finfo;
      if (ioctl(vid->fd_fb, FBIOGET_VSCREENINFO, &vinfo) < 0 ||
          ioctl(vid->fd_fb, FBIOGET_FSCREENINFO, &finfo) < 0)
      {
         RARCH_ERR("[Powkiddy DE]: fbdev ioctl failed\n");
         goto error;
      }
      vid->fb_width    = vinfo.xres;
      vid->fb_height   = vinfo.yres;
      vid->fb_pitch    = finfo.line_length;
      vid->fb_phys     = (uint32_t)finfo.smem_start;
      vid->fb_map_size = finfo.smem_len;

      vid->fb_map = (uint8_t*)mmap(0, finfo.smem_len,
            PROT_READ | PROT_WRITE, MAP_SHARED, vid->fd_fb, 0);
      if (vid->fb_map == (void*)-1)
      {
         RARCH_ERR("[Powkiddy DE]: mmap fb0 failed\n");
         goto error;
      }
      RARCH_LOG("[Powkiddy DE]: FB %ux%u pitch=%u phys=0x%08x size=0x%x\n",
                vid->fb_width, vid->fb_height, vid->fb_pitch,
                vid->fb_phys, vid->fb_map_size);
   }

   /* ------------------------------------------------------------------
    * Ouvre /dev/mem et mmap les registres DE
    * ------------------------------------------------------------------ */
   vid->fd_mem = open("/dev/mem", O_RDWR);
   if (vid->fd_mem < 0)
   {
      RARCH_ERR("[Powkiddy DE]: Cannot open /dev/mem\n");
      goto error;
   }
   vid->de = (volatile uint32_t*)mmap(0, DE_SIZE,
         PROT_READ | PROT_WRITE, MAP_SHARED, vid->fd_mem, DE_BASE);
   if (vid->de == (void*)-1)
   {
      RARCH_ERR("[Powkiddy DE]: mmap DE failed\n");
      vid->de = NULL;
      goto error;
   }

   /* Lecture LCD size depuis DE_PATH_SIZE (registre r/o) */
   {
      uint32_t ps = vid->de[DE_PATH_SIZE/4];
      vid->lcd_w  = (ps & 0xFFFF) + 1;
      vid->lcd_h  = (ps >> 16)    + 1;
      RARCH_LOG("[Powkiddy DE]: LCD %ux%u (from DE_PATH_SIZE)\n",
                vid->lcd_w, vid->lcd_h);
   }

   /* Adresse physique du buffer source (video1/addr0 sysfs) */
   {
      uint32_t addr0 = 0;
      FILE *f = fopen("/sys/kernel/debug/de/video1/addr0", "r");
      if (f) { fscanf(f, "%u", &addr0); fclose(f); }
      if (addr0 == 0)
      {
         /* Fallback : début du FB */
         addr0 = vid->fb_phys;
         RARCH_WARN("[Powkiddy DE]: video1/addr0 not found, using fb_phys\n");
      }
      vid->buf_phys = addr0;
      uint32_t buf_off = addr0 - vid->fb_phys;
      vid->buf_virt    = vid->fb_map + buf_off;
      RARCH_LOG("[Powkiddy DE]: buf_phys=0x%08x buf_off=0x%x\n",
                addr0, buf_off);
   }

   vid->de_ok = true;



   /* ------------------------------------------------------------------
    * Rotation, menu dims, allocation texture menu
    * ------------------------------------------------------------------ */
   vid->rotation = rotation & 3;
   de_powkiddy_update_menu_dims(vid);

   vid->menu_texture = (uint16_t*)malloc(
         vid->menu_w * vid->menu_h * sizeof(uint16_t));
   if (!vid->menu_texture)
      goto error;

   /* ------------------------------------------------------------------
    * Paramètres RetroArch
    * ------------------------------------------------------------------ */
   vid->ff_frame_time_min   = 16667;
   vid->frame_width         = vid->fb_width;
   vid->frame_height        = vid->fb_height;
   vid->rgb32               = video->rgb32;
   vid->vsync               = video->vsync;
   vid->keep_aspect         = ipu_keep_aspect;
   vid->integer_scaling     = ipu_integer_scaling;
   vid->filtertype          = ipu_filtertype;
   vid->menu_active         = false;
   vid->was_in_menu         = false;
   vid->quitting            = false;
   vid->last_frame_time     = 0;
   vid->menu_texture_width  = 0;
   vid->menu_texture_height = 0;
   vid->last_frame_buf      = NULL;
   vid->last_frame_buf_size = 0;
   vid->last_frame_width    = 0;
   vid->last_frame_height   = 0;
   vid->last_frame_pitch    = 0;
   vid->last_msg[0]         = '\0';
   vid->de_src_w = 0;
   vid->de_src_h = 0;
   vid->de_out_w = 0;
   vid->de_out_h = 0;
   vid->de_out_x = 0;
   vid->de_out_y = 0;
   
   /* set filters based on menu */
   de_set_filter(vid);
   /* ------------------------------------------------------------------
    * Input — udev/evdev direct (pas de SDL)
    * ------------------------------------------------------------------ */
   if (input && input_data)
   {
      void *sdl_input  = input_driver_init_wrap(&input_sdl,
            settings->arrays.input_joypad_driver);
      if (sdl_input)
      {
         *input      = &input_sdl;
         *input_data = sdl_input;
      }
      else
      {
         *input      = NULL;
         *input_data = NULL;
      }
   }

   de_powkiddy_init_font_color(vid);
   vid->osd_font = bitmapfont_get_lut();
   if (!vid->osd_font ||
       vid->osd_font->glyph_max < (DE_POWKIDDY_NUM_FONT_GLYPHS - 1))
   {
      RARCH_ERR("[Powkiddy DE]: Failed to init OSD font\n");
      goto error;
   }

   settings->bools.menu_show_restart_retroarch = false;

   RARCH_LOG("[Powkiddy DE]: Init OK — FB=%ux%u LCD=%ux%u rot=%u menu=%ux%u\n",
             vid->fb_width, vid->fb_height,
             vid->lcd_w, vid->lcd_h,
             vid->rotation, vid->menu_w, vid->menu_h);
   return vid;

error:
   de_powkiddy_gfx_free(vid);
   return NULL;
}

/* ==========================================================================
 * Frame
 * ========================================================================== */
static bool de_powkiddy_gfx_frame(void *data, const void *frame,
      unsigned width, unsigned height, uint64_t frame_count,
      unsigned pitch, const char *msg, video_frame_info_t *video_info)
{
   de_powkiddy_video_t *vid = (de_powkiddy_video_t*)data;
   if (unlikely(!vid)) return true;

   if (unlikely(video_info->input_driver_nonblock_state))
   {
      retro_time_t now = cpu_features_get_time_usec();
      if ((now - vid->last_frame_time) < vid->ff_frame_time_min) return true;
      vid->last_frame_time = now;
   }

#ifdef HAVE_MENU
   menu_driver_frame(video_info->menu_is_alive, video_info);
#endif

   /* Cache last game frame pour dup-frame */
   if (frame && !vid->menu_active)
   {
      size_t needed = (size_t)height * pitch;
      if (vid->last_frame_buf_size < needed)
      {
         free(vid->last_frame_buf);
         vid->last_frame_buf      = (uint16_t*)malloc(needed);
         vid->last_frame_buf_size = vid->last_frame_buf ? needed : 0;
      }
      if (vid->last_frame_buf)
      {
         memcpy(vid->last_frame_buf, frame, needed);
         vid->last_frame_width  = width;
         vid->last_frame_height = height;
         vid->last_frame_pitch  = pitch;
      }
   }

   if (msg && msg[0])
      strncpy(vid->last_msg, msg, sizeof(vid->last_msg) - 1);
   else
      vid->last_msg[0] = '\0';
   if (!vid->buf_virt) return true;

   //detect changes
   de_powkiddy_apply_state_changes((void*)vid);
   if (likely(!vid->menu_active))
   {
      vid->was_in_menu = false;

      const void *src_frame = frame ? frame : (const void*)vid->last_frame_buf;
      unsigned    src_w     = frame ? width  : vid->last_frame_width;
      unsigned    src_h     = frame ? height : vid->last_frame_height;
      unsigned    src_pitch = frame ? pitch  : vid->last_frame_pitch;

      if (src_frame && src_w > 0 && src_h > 0)
      {
         if(vid->filtertype == DINGUX_IPU_FILTER_SOFTWARE_NEAREST)
         {
            unsigned out_x, out_y, out_w, out_h;
            unsigned fb_stride = vid->fb_pitch / 2;
            uint16_t *fb       = (uint16_t*)vid->buf_virt;

            static unsigned last_w = 0, last_h = 0, last_rot = 99;
            static bool last_ka = false, last_is = false;
            if (src_w != last_w || src_h != last_h || vid->rotation != last_rot ||
               vid->keep_aspect != last_ka || vid->integer_scaling != last_is)
            {
               last_w = src_w; last_h = src_h; last_rot = vid->rotation;
               last_ka = vid->keep_aspect; last_is = vid->integer_scaling;
               unsigned dbg_x, dbg_y, dbg_w, dbg_h;
               sdl_powkiddy_compute_out_rect(vid, src_w, src_h,
                     &dbg_x, &dbg_y, &dbg_w, &dbg_h);
               RARCH_LOG("[Powkiddy]: Game %ux%u rot=%u keep=%d int=%d"
                        " -> out %ux%u at %u,%u\n",
                        src_w, src_h, vid->rotation,
                        vid->keep_aspect, vid->integer_scaling,
                        dbg_w, dbg_h, dbg_x, dbg_y);
            }

            sdl_powkiddy_compute_out_rect(vid, src_w, src_h,
                  &out_x, &out_y, &out_w, &out_h);

            if (vid->rgb32)
               sdl_powkiddy_blit_32(vid,
                     (const uint32_t*)src_frame, src_w, src_h, src_pitch >> 2,
                     fb, fb_stride, out_x, out_y, out_w, out_h);
            else
               sdl_powkiddy_blit_16(vid,
                     (const uint16_t*)src_frame, src_w, src_h, src_pitch >> 1,
                     fb, fb_stride, out_x, out_y, out_w, out_h);

            /* OSD — bottom-left in logical landscape space */
            
            if (!vid->menu_active && vid->last_msg[0] && vid->osd_font)
            {
             /* Dans l'espace logique source, la frame scalée fait out_w x out_h dans le FB.
               * En ROT_270 : axe X logique = out_h FB, axe Y logique = out_w FB */
               unsigned logical_frame_h = (vid->rotation == ROT_90 || vid->rotation == ROT_270)
                                       ? out_w : out_h;
               unsigned logical_x_off   = (vid->rotation == ROT_90 || vid->rotation == ROT_270)
                                       ? out_y : out_x;
               unsigned logical_y_off   = (vid->rotation == ROT_90 || vid->rotation == ROT_270)
                                       ? out_x : out_y;
               de_powkiddy_blit_text(vid,
                     logical_x_off + FONT_WIDTH_STRIDE,
                     logical_y_off + logical_frame_h - (FONT_HEIGHT + FONT_WIDTH_STRIDE),
                     vid->last_msg);
            }
         }
         else
         { //use DE
            /* Dimensions après rotation (dans l'espace DE/LCD) */
            unsigned rot_w = (vid->rotation == ROT_90 || vid->rotation == ROT_270)
                           ? src_h : src_w;
            unsigned rot_h = (vid->rotation == ROT_90 || vid->rotation == ROT_270)
                           ? src_w : src_h;

            /* Mise à jour DE si nécessaire */
            de_powkiddy_update_de(vid, rot_w, rot_h);

            /* Rotation pixel vers buf_virt — pas d'upscale.
            * dst_stride DOIT correspondre au stride programmé dans DE_OVL_STR,
            * c'est-à-dire rot_w arrondi au multiple de 4 pixels (= 8 octets).
            * Utiliser fb_pitch/2 (480px) décalerait chaque ligne du point de vue
            * du scaler hardware. */
            uint16_t *dst        = (uint16_t*)vid->buf_virt;
            unsigned  dst_stride = vid->fb_pitch / 2;

            if (vid->rgb32)
            {
               de_rotate_32to16(vid, (const uint32_t*)src_frame, src_w, src_h, src_pitch >> 2,
                  dst, dst_stride);      
            }
            else
            {  
               de_rotate_16(vid,
                  (const uint16_t*)src_frame, src_w, src_h, src_pitch >> 1,
                  dst, dst_stride);
            }
            /* OSD en bas à gauche de l'espace logique */
            if (vid->last_msg[0] && vid->osd_font)
               //de_powkiddy_blit_osd(vid, vid->last_msg);
               de_powkiddy_blit_text(vid, FONT_WIDTH_STRIDE,
                     src_h - (FONT_HEIGHT),
                     vid->last_msg);
         }
      }
   }
   else
   {
      vid->was_in_menu = true;

      if (vid->menu_texture_width > 0 && vid->menu_texture_height > 0)
      {
         if(vid->filtertype == DINGUX_IPU_FILTER_SOFTWARE_NEAREST)
         {
            uint16_t *fb        = (uint16_t*)vid->buf_virt;
            unsigned  fb_stride = vid->fb_pitch / 2;
            unsigned out_x, out_y, out_w, out_h;
            
            sdl_powkiddy_compute_out_rect(vid,
                  vid->menu_texture_width, vid->menu_texture_height,
                  &out_x, &out_y, &out_w, &out_h);
            sdl_powkiddy_blit_16(vid,
                  vid->menu_texture,
                  vid->menu_texture_width, vid->menu_texture_height,
                  vid->menu_texture_width,
                  fb, fb_stride, out_x, out_y, out_w, out_h);
            if (vid->last_msg[0] && vid->osd_font)
            {
               unsigned logical_frame_h = (vid->rotation == ROT_90 || vid->rotation == ROT_270)
                                       ? out_w : out_h;
               unsigned logical_x_off   = (vid->rotation == ROT_90 || vid->rotation == ROT_270)
                                       ? out_y : out_x;
               unsigned logical_y_off   = (vid->rotation == ROT_90 || vid->rotation == ROT_270)
                                       ? out_x : out_y;
               de_powkiddy_blit_text(vid,
                     logical_x_off + FONT_WIDTH_STRIDE,
                     logical_y_off + logical_frame_h - (FONT_HEIGHT + FONT_WIDTH_STRIDE),
                     vid->last_msg);
            }
         }
         else
         { //use DE
            /* Pour le menu : on copie la texture après rotation */
            unsigned rot_w = (vid->rotation == ROT_90 || vid->rotation == ROT_270)
                           ? vid->menu_texture_height : vid->menu_texture_width;
            unsigned rot_h = (vid->rotation == ROT_90 || vid->rotation == ROT_270)
                           ? vid->menu_texture_width  : vid->menu_texture_height;

            de_powkiddy_update_de(vid, rot_w, rot_h);

            uint16_t *dst        = (uint16_t*)vid->buf_virt;
            unsigned  dst_stride = vid->fb_pitch / 2;
            
            de_rotate_16(vid,
                  vid->menu_texture,
                  vid->menu_texture_width, vid->menu_texture_height,
                  vid->menu_texture_width,
                  dst, dst_stride);

            if (vid->last_msg[0] && vid->osd_font)
            {
               unsigned osd_lx = 1;//vid->menu_w - (FONT_HEIGHT + FONT_WIDTH_STRIDE);
               unsigned osd_ly = vid->menu_texture_height - (FONT_HEIGHT);
               //de_powkiddy_blit_osd(vid, vid->last_msg);
               de_powkiddy_blit_text(vid, osd_lx, osd_ly,vid->last_msg);
            }
         }
      }
   }

   /* Vsync via ioctl OWL directement sur /dev/fb0 */
   if (vid->vsync)
   {
      long long dmy = 0;
      ioctl(vid->fd_fb, FBIO_WAITFORVSYNC, &dmy);
   }

   return true;
}

/* ==========================================================================
 * State changes
 * ========================================================================== */
static void de_powkiddy_set_texture_enable(void *data, bool state, bool full_screen)
{
   de_powkiddy_video_t *vid = (de_powkiddy_video_t*)data;
   if (!vid) return;
   vid->menu_active = state;
   if (!state) vid->was_in_menu = false;
}

static void de_powkiddy_set_texture_frame(void *data, const void *frame, bool rgb32,
      unsigned width, unsigned height, float alpha)
{
   de_powkiddy_video_t *vid = (de_powkiddy_video_t*)data;
   if (!vid || rgb32 || !vid->menu_texture) return;
   if (width > vid->menu_w || height > vid->menu_h) return;
   vid->menu_texture_width  = width;
   vid->menu_texture_height = height;
   memcpy(vid->menu_texture, frame, width * height * sizeof(uint16_t));
}

static void de_powkiddy_gfx_set_nonblock_state(void *data, bool toggle,
      bool adaptive_vsync_enabled, unsigned swap_interval)
{
   de_powkiddy_video_t *vid = (de_powkiddy_video_t*)data;
   if (!vid) return;
   vid->vsync = !toggle;
}

static bool de_powkiddy_gfx_alive(void *data)
{
   de_powkiddy_video_t *vid = (de_powkiddy_video_t*)data;
   if (!vid) return false;
   return !vid->quitting;
}

static bool de_powkiddy_gfx_focus(void *data)                     { return true; }
static bool de_powkiddy_gfx_suppress_screensaver(void *d, bool e) { return false; }
static bool de_powkiddy_gfx_has_windowed(void *data)              { return false; }
static bool de_powkiddy_gfx_set_shader(void *d,
      enum rarch_shader_type t, const char *p)                     { return false; }

static void de_powkiddy_gfx_viewport_info(void *data, struct video_viewport *vp)
{
   de_powkiddy_video_t *vid = (de_powkiddy_video_t*)data;
   if (!vid) return;
   vp->x = vp->y = 0;
   vp->width  = vp->full_width  = vid->frame_width;
   vp->height = vp->full_height = vid->frame_height;
}

static float de_powkiddy_get_refresh_rate(void *data) { return 60.0f; }
static uint32_t de_powkiddy_get_flags(void *data)     { return 0; }

static const video_poke_interface_t de_powkiddy_poke_interface = {
   de_powkiddy_get_flags,
   NULL, NULL, NULL,
   de_powkiddy_get_refresh_rate,
   NULL,  /* set filtering — géré directement via DE_OVL_SCOEF* si besoin */
   NULL, NULL, NULL, NULL, NULL, NULL,
   de_powkiddy_apply_state_changes,
   de_powkiddy_set_texture_frame,
   de_powkiddy_set_texture_enable,
   NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL
};

static void de_powkiddy_get_poke_interface(void *data,
      const video_poke_interface_t **iface)
{ *iface = &de_powkiddy_poke_interface; }

video_driver_t video_sdl_powkiddy = {
   de_powkiddy_gfx_init,
   de_powkiddy_gfx_frame,
   de_powkiddy_gfx_set_nonblock_state,
   de_powkiddy_gfx_alive,
   de_powkiddy_gfx_focus,
   de_powkiddy_gfx_suppress_screensaver,
   de_powkiddy_gfx_has_windowed,
   de_powkiddy_gfx_set_shader,
   de_powkiddy_gfx_free,
   "sdl_powkiddy",
   NULL, NULL,
   de_powkiddy_gfx_viewport_info,
   NULL, NULL,
#ifdef HAVE_OVERLAY
   NULL,
#endif
#ifdef HAVE_VIDEO_LAYOUT
   NULL,
#endif
   de_powkiddy_get_poke_interface
};