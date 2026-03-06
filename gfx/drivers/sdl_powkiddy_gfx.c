/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2010-2014 - Hans-Kristian Arntzen
 *  Copyright (C) 2011-2017 - Daniel De Matteis
 *  Copyright (C) 2011-2017 - Higor Euripedes
 *  Copyright (C) 2019-2021 - James Leaver
 *
 *  Modified for Powkiddy X-series (X39 Pro, X45, X51, X70...) - 2026
 *
 *  ARCHITECTURE v6 — runtime configurable rotation + auto screen resolution
 *
 *  - Screen resolution auto-detected at init via SDL_GetVideoInfo() / fbset
 *  - Rotation read from RetroArch settings (video_rotation: 0/1/2/3)
 *  - Fully portable across Powkiddy X-series and other Actions Semi devices
 *  - Pure C software scaler, no NEON dependency
 *  - Nearest neighbor + Bilinear 2D filters
 *
 *  Rotation values (mirrors RetroArch video_rotation setting):
 *    ROT_0   (0) — no rotation, native landscape LCD
 *    ROT_90  (1) — 90° CW  (portrait LCD, X39 Pro default)
 *    ROT_180 (2) — 180°
 *    ROT_270 (3) — 90° CCW (portrait LCD, alternate mount)
 */

#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/fb.h>

#include <SDL/SDL.h>
#include <SDL/SDL_video.h>

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

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

/* Maximum supported framebuffer dimensions.
 * Used for stack-allocated LUT arrays in scalers.
 * Increase if targeting screens larger than 1080p. */
#define SDL_POWKIDDY_MAX_FB_W  1920
#define SDL_POWKIDDY_MAX_FB_H  1920

#define SDL_POWKIDDY_NUM_FONT_GLYPHS 256

/* Rotation identifiers — match RetroArch video_rotation values */
#define ROT_0    0   /* no rotation */
#define ROT_90   1   /* 90° CW  — portrait LCD, X39 Pro */
#define ROT_180  2   /* 180° */
#define ROT_270  3   /* 90° CCW — portrait LCD, alternate */

typedef struct sdl_powkiddy_video
{
   retro_time_t last_frame_time;
   retro_time_t ff_frame_time_min;
   SDL_Surface *screen;
   bitmapfont_lut_t *osd_font;

   /* Physical framebuffer dimensions (auto-detected at init) */
   unsigned fb_width;
   unsigned fb_height;

   /* Logical menu dimensions (landscape space RetroArch uses internally)
    * For rotated screens: menu_w = fb_height, menu_h = fb_width
    * For non-rotated:     menu_w = fb_width,  menu_h = fb_height */
   unsigned menu_w;
   unsigned menu_h;

   unsigned frame_width;
   unsigned frame_height;
   unsigned rotation;           /* current rotation: ROT_0/90/180/270 */
   
   uint32_t font_colour32;
   uint16_t font_colour16;

   /* Menu texture — sized for max possible landscape resolution */
   uint16_t *menu_texture;
   unsigned  menu_texture_width;
   unsigned  menu_texture_height;

   bool rgb32;
   bool vsync;
   bool keep_aspect;
   bool integer_scaling;
   bool menu_active;
   bool was_in_menu;
   bool quitting;

   uint16_t *last_frame_buf;
   size_t    last_frame_buf_size;
   unsigned  last_frame_width;
   unsigned  last_frame_height;
   unsigned  last_frame_pitch;
   char      last_msg[512];
} sdl_powkiddy_video_t;

/* ==========================================================================
 * Screen resolution detection
 *
 * Try in order:
 *   1. SDL_GetVideoInfo() — available after SDL_Init
 *   2. ioctl FBIOGET_VSCREENINFO on /dev/fb0 — always reliable on Linux
 * ========================================================================== */
static bool sdl_powkiddy_detect_resolution(unsigned *w, unsigned *h)
{
   /* Try fbdev first — most reliable on embedded Linux */
   int fd = open("/dev/fb0", O_RDONLY);
   if (fd >= 0)
   {
      struct fb_var_screeninfo vinfo;
      if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) == 0
            && vinfo.xres > 0 && vinfo.yres > 0)
      {
         *w = vinfo.xres;
         *h = vinfo.yres;
         close(fd);
         RARCH_LOG("[Powkiddy]: FB resolution from fbdev: %ux%u\n", *w, *h);
         return true;
      }
      close(fd);
   }

   /* Fallback: SDL video info */
   const SDL_VideoInfo *vi = SDL_GetVideoInfo();
   if (vi && vi->current_w > 0 && vi->current_h > 0)
   {
      *w = (unsigned)vi->current_w;
      *h = (unsigned)vi->current_h;
      RARCH_LOG("[Powkiddy]: FB resolution from SDL: %ux%u\n", *w, *h);
      return true;
   }

   RARCH_WARN("[Powkiddy]: Could not detect screen resolution, defaulting to 480x854\n");
   *w = 480;
   *h = 854;
   return false;
}

/* ==========================================================================
 * Update logical menu dimensions from current rotation + fb size
 *
 * For 90°/270° rotations the axes are swapped:
 *   menu_w = fb_height (e.g. 854 for X39 Pro)
 *   menu_h = fb_width  (e.g. 480 for X39 Pro)
 *
 * For 0°/180°:
 *   menu_w = fb_width
 *   menu_h = fb_height
 * ========================================================================== */
static void sdl_powkiddy_update_menu_dims(sdl_powkiddy_video_t *vid)
{
   vid->menu_w = vid->fb_height;
   vid->menu_h = vid->fb_width;
}

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
static void sdl_powkiddy_compute_out_rect(
      sdl_powkiddy_video_t *vid,
      unsigned src_w, unsigned src_h,
      unsigned *out_x, unsigned *out_y,
      unsigned *out_w, unsigned *out_h)
{
   unsigned rotated_w, rotated_h;
   unsigned dst_w = vid->fb_width;
   unsigned dst_h = vid->fb_height;
   unsigned ow, oh;

   /* Swap axes for 90°/270° rotations */
   if (vid->rotation == ROT_90 || vid->rotation == ROT_270)
   {
      rotated_w = src_h;
      rotated_h = src_w;
   }
   else
   {
      rotated_w = src_w;
      rotated_h = src_h;
   }

   if (vid->integer_scaling)
   {
      /* Fill full physical width, height by exact ratio.
       * Maximizes the screen height in landscape view.
       *
       * ROT_90 examples (X39 Pro 480x854):
       *   GB  160x144 -> rot 144x160 : ow=480, oh=144*854/160=768
       *   NES 256x240 -> rot 240x256 : ow=480, oh=240*854/256=800
       *   GBA 240x160 -> rot 160x240 : ow=480, oh=160*854/240=569
       *
       * ROT_0 examples (landscape 640x480):
       *   NES 256x240 : ow=640, oh=256*480/240=512
       */
      if(!vid->keep_aspect)
      {  
         ow = dst_w;
         oh = (rotated_h > 0) ? (rotated_w * dst_h / rotated_h) : dst_h;
         if (oh < 1)   oh = 1;
         if(oh > dst_h) oh = dst_h;
      }
      else
      {
         unsigned sx = dst_w / rotated_w;
         unsigned sy = dst_h / rotated_h;
         unsigned s  = (sx < sy) ? sx : sy;
         if (s < 1) s = 1;
         ow = rotated_w * s;
         oh = rotated_h * s;
      }
   }
   else if (vid->keep_aspect)
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

/* ==========================================================================
 * RGB565 helpers
 * ========================================================================== */
static inline void rgb565_unpack(uint16_t c, int *r, int *g, int *b)
{
   *r = ((c >> 11) & 0x1F) << 3;
   *g = ((c >>  5) & 0x3F) << 2;
   *b = ( c        & 0x1F) << 3;
}

static inline uint16_t rgb565_pack(int r, int g, int b)
{
   if (r < 0) r = 0; else if (r > 255) r = 255;
   if (g < 0) g = 0; else if (g > 255) g = 255;
   if (b < 0) b = 0; else if (b > 255) b = 255;
   return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

/* ==========================================================================
 * Nearest-neighbor scaler — 16bpp, all rotations
 * ========================================================================== */
static void scale_nearest_16(
      sdl_powkiddy_video_t *vid,
      const uint16_t * __restrict__ src,
      unsigned src_w, unsigned src_h, unsigned src_stride,
      uint16_t * __restrict__ fb, unsigned fb_stride,
      unsigned out_x, unsigned out_y, unsigned out_w, unsigned out_h)
{
   unsigned dx, dy;

   switch (vid->rotation)
   {
      case ROT_90:
      {
         /* Column-major write: src_x -> fb_y (inverted), src_y -> fb_x */
         unsigned sx_lut[SDL_POWKIDDY_MAX_FB_H];
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

      case ROT_270:
      {
         /* Column-major write: src_x -> fb_y (normal), src_y -> fb_x (inverted) */
         unsigned sx_lut[SDL_POWKIDDY_MAX_FB_H];
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
         unsigned sy_lut[SDL_POWKIDDY_MAX_FB_W];
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
         unsigned sy_lut[SDL_POWKIDDY_MAX_FB_H];
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
      sdl_powkiddy_video_t *vid,
      const uint32_t * __restrict__ src,
      unsigned src_w, unsigned src_h, unsigned src_stride,
      uint16_t * __restrict__ fb, unsigned fb_stride,
      unsigned out_x, unsigned out_y, unsigned out_w, unsigned out_h)
{
   unsigned dx, dy;

   switch (vid->rotation)
   {
      case ROT_90:
      {
         unsigned sx_lut[SDL_POWKIDDY_MAX_FB_H];
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

      case ROT_270:
      {
         unsigned sx_lut[SDL_POWKIDDY_MAX_FB_H];
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
         unsigned sy_lut[SDL_POWKIDDY_MAX_FB_H];
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
 * Dispatcher: clear black borders + dispatch to scaler
 * ========================================================================== */
static void sdl_powkiddy_blit_16(
      sdl_powkiddy_video_t *vid,
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
      sdl_powkiddy_video_t *vid,
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
 * Font / OSD
 *
 * x, y in logical landscape space (menu_w x menu_h).
 * Transformed to physical fb coordinates based on current rotation.
 * ========================================================================== */
static void sdl_powkiddy_init_font_color(sdl_powkiddy_video_t *vid)
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
   red >>= 3; green >>= 3; blue >>= 3;
   vid->font_colour16 = (uint16_t)((red << 11) | (green << 6) | blue);
}

static void sdl_powkiddy_blit_text(sdl_powkiddy_video_t *vid,
      unsigned x, unsigned y, const char *str)
{
   uint16_t *fb        = (uint16_t*)vid->screen->pixels;
   unsigned  fb_stride = vid->screen->pitch >> 1;
   bool    **font_lut  = vid->osd_font->lut;
   uint16_t  col       = vid->font_colour16;
   unsigned  x_pos     = x;
   unsigned  y_pos     = y;

   if (y_pos + FONT_HEIGHT + 1 >= vid->menu_h) return;

   while (!string_is_empty(str))
   {
      if (x_pos + FONT_WIDTH_STRIDE + 1 >= vid->menu_w) return;
      if (*str == ' ') { str++; x_pos += FONT_WIDTH_STRIDE; continue; }

      uint32_t symbol = utf8_walk(&str);
      if (symbol == 339) symbol = 156;
      if (symbol == 338) symbol = 140;
      if (symbol >= SDL_POWKIDDY_NUM_FONT_GLYPHS) continue;

      bool *sym_lut = font_lut[symbol];
      unsigned i, j;
      for (j = 0; j < FONT_HEIGHT; j++)
      {
         unsigned ly = y_pos + j;
         for (i = 0; i < FONT_WIDTH; i++)
         {
            if (!sym_lut[i + j * FONT_WIDTH]) continue;
            unsigned lx = x_pos + i;
            unsigned fpx, fpy;

            /* Transform logical (lx, ly) -> physical fb (fpx, fpy) */
            switch (vid->rotation)
            {
               case ROT_90:
                  fpx = ly;
                  fpy = vid->fb_height - 1 - lx;
                  break;
               case ROT_270:
                  fpx = vid->fb_width - 1 - ly;
                  fpy = lx;
                  break;
               case ROT_180:
                  fpx = vid->fb_width  - 1 - lx;
                  fpy = vid->fb_height - 1 - ly;
                  break;
               default: /* ROT_0 */
                  fpx = lx;
                  fpy = ly;
                  break;
            }

            if (fpx < vid->fb_width && fpy < vid->fb_height)
               fb[fpy * fb_stride + fpx] = col;
         }
      }
      x_pos += FONT_WIDTH_STRIDE;
   }
}

static void sdl_powkiddy_blit_video_mode_error_msg(sdl_powkiddy_video_t *vid)
{
   const char *error_msg = msg_hash_to_str(MSG_UNSUPPORTED_VIDEO_MODE);
   char display_mode[64];
   display_mode[0] = '\0';
   memset(vid->screen->pixels, 0, vid->screen->pitch * vid->screen->h);
   snprintf(display_mode, sizeof(display_mode), "> %ux%u, %s",
         vid->frame_width, vid->frame_height,
         vid->rgb32 ? "XRGB8888" : "RGB565");
   sdl_powkiddy_blit_text(vid, FONT_WIDTH_STRIDE, FONT_WIDTH_STRIDE, error_msg);
   sdl_powkiddy_blit_text(vid, FONT_WIDTH_STRIDE,
         FONT_WIDTH_STRIDE + FONT_HEIGHT_STRIDE, display_mode);
}

/* ==========================================================================
 * Init / Free
 * ========================================================================== */
static void sdl_powkiddy_gfx_free(void *data)
{
   sdl_powkiddy_video_t *vid = (sdl_powkiddy_video_t*)data;
   if (!vid) return;
   if (vid->osd_font)       bitmapfont_free_lut(vid->osd_font);
   if (vid->last_frame_buf) free(vid->last_frame_buf);
   if (vid->menu_texture)   free(vid->menu_texture);
   free(vid);
}


static void *sdl_powkiddy_gfx_init(const video_info_t *video,
      input_driver_t **input, void **input_data)
{
   RARCH_ERR("[Powkiddy]: INIT\n");
   sdl_powkiddy_video_t *vid            = NULL;
   uint32_t sdl_subsystem_flags         = SDL_WasInit(0);
   settings_t *settings                 = config_get_ptr();
   bool ipu_keep_aspect                 = settings->bools.video_dingux_ipu_keep_aspect;
   bool ipu_integer_scaling             = settings->bools.video_scale_integer;
   unsigned rotation                    = settings->uints.video_rotation;
   const char *input_drv_name           = settings->arrays.input_driver;
   const char *joypad_drv_name          = settings->arrays.input_joypad_driver;
   uint32_t surface_flags               = video->vsync ?
         (SDL_HWSURFACE | SDL_TRIPLEBUF | SDL_FULLSCREEN) :
         (SDL_HWSURFACE | SDL_FULLSCREEN);

   if (sdl_subsystem_flags == 0)
   { if (SDL_Init(SDL_INIT_VIDEO) < 0) return NULL; }
   else if (!(sdl_subsystem_flags & SDL_INIT_VIDEO))
   { if (SDL_InitSubSystem(SDL_INIT_VIDEO) < 0) return NULL; }

   vid = (sdl_powkiddy_video_t*)calloc(1, sizeof(*vid));
   if (!vid) return NULL;

   /* Detect physical screen resolution */
   sdl_powkiddy_detect_resolution(&vid->fb_width, &vid->fb_height);

   /* Set rotation from RetroArch settings */
   vid->rotation = rotation & 3;  /* clamp to 0-3 */
   sdl_powkiddy_update_menu_dims(vid);

   /* Allocate menu texture for full landscape resolution */
   vid->menu_texture = (uint16_t*)malloc(
         vid->menu_w * vid->menu_h * sizeof(uint16_t));
   if (!vid->menu_texture)
      goto error;

   vid->ff_frame_time_min = 16667;
   vid->screen = SDL_SetVideoMode(
         (int)vid->fb_width, (int)vid->fb_height, 16, surface_flags);
   if (!vid->screen)
   {
      RARCH_ERR("[Powkiddy]: Failed to init SDL surface: %s\n", SDL_GetError());
      goto error;
   }

   RARCH_LOG("[Powkiddy]: FB=%ux%u rotation=%u menu=%ux%u pitch=%d\n",
             vid->fb_width, vid->fb_height, vid->rotation,
             vid->menu_w, vid->menu_h, vid->screen->pitch);

   vid->frame_width         = vid->fb_width;
   vid->frame_height        = vid->fb_height;
   vid->rgb32               = video->rgb32;
   vid->vsync               = video->vsync;
   vid->keep_aspect         = ipu_keep_aspect;
   vid->integer_scaling     = ipu_integer_scaling;
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

   SDL_ShowCursor(SDL_DISABLE);
   
   if (input && input_data)
   {
      void *sdl_input = input_driver_init_wrap(&input_sdl,
            settings->arrays.input_joypad_driver);

      if (sdl_input)
      {
         *input = &input_sdl;
         *input_data = sdl_input;
      }
      else
      {
         *input = NULL;
         *input_data = NULL;
      }
   }

   sdl_powkiddy_init_font_color(vid);
   vid->osd_font = bitmapfont_get_lut();
   if (!vid->osd_font || vid->osd_font->glyph_max < (SDL_POWKIDDY_NUM_FONT_GLYPHS - 1))
   { RARCH_ERR("[Powkiddy]: Failed to init OSD font\n"); goto error; }

      //disable restart retroarch button
   settings->bools.menu_show_restart_retroarch = false;

   return vid;

error:
   sdl_powkiddy_gfx_free(vid);
   return NULL;
}

/* ==========================================================================
 * Frame
 * ========================================================================== */
static bool sdl_powkiddy_gfx_frame(void *data, const void *frame,
      unsigned width, unsigned height, uint64_t frame_count,
      unsigned pitch, const char *msg, video_frame_info_t *video_info)
{
   sdl_powkiddy_video_t *vid = (sdl_powkiddy_video_t*)data;
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

   /* Cache last game frame for dup-frame handling */
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

   /* OSD cache — never clear last_msg.
    * RetroArch sends msg=NULL on duplicated frames, keep last one. */
   if (msg && msg[0])
      strncpy(vid->last_msg, msg, sizeof(vid->last_msg) - 1);

   if (SDL_MUSTLOCK(vid->screen)) SDL_LockSurface(vid->screen);

   if (likely(!vid->menu_active))
   {
      vid->was_in_menu = false;
      const void *src_frame = frame ? frame : (const void*)vid->last_frame_buf;
      unsigned    src_w     = frame ? width  : vid->last_frame_width;
      unsigned    src_h     = frame ? height : vid->last_frame_height;
      unsigned    src_pitch = frame ? pitch  : vid->last_frame_pitch;

      if (src_frame && src_w > 0 && src_h > 0)
      {
         unsigned out_x, out_y, out_w, out_h;
         unsigned fb_stride = vid->screen->pitch >> 1;
         uint16_t *fb       = (uint16_t*)vid->screen->pixels;

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
      }
   }
   else
   {
      vid->was_in_menu = true;
      if (vid->menu_texture_width > 0 && vid->menu_texture_height > 0)
      {
         //RARCH_LOG("[Powkiddy]: menu tex %ux%u first pixels: %04X %04X %04X %04X\n",
         // width, height,
         // ((uint16_t*)frame)[0], ((uint16_t*)frame)[1],
         // ((uint16_t*)frame)[width], ((uint16_t*)frame)[width+1]);

         uint16_t *fb        = (uint16_t*)vid->screen->pixels;
         unsigned  fb_stride = vid->screen->pitch >> 1;
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
            unsigned osd_lx = out_x + out_w - (FONT_HEIGHT + FONT_WIDTH_STRIDE);
            unsigned osd_ly = out_y + (FONT_WIDTH_STRIDE);
            sdl_powkiddy_blit_text(vid, osd_ly, osd_lx, vid->last_msg);
         }
      }
   }

   /* OSD — bottom-left in logical landscape space */
   if (!vid->menu_active && vid->last_msg[0] && vid->osd_font)
      sdl_powkiddy_blit_text(vid, FONT_WIDTH_STRIDE,
            vid->menu_h - (FONT_HEIGHT + FONT_WIDTH_STRIDE),
            vid->last_msg);

   if (SDL_MUSTLOCK(vid->screen)) SDL_UnlockSurface(vid->screen);
   SDL_Flip(vid->screen);
   return true;
}

/* ==========================================================================
 * State changes
 * ========================================================================== */
static void sdl_powkiddy_set_texture_enable(void *data, bool state, bool full_screen)
{
   sdl_powkiddy_video_t *vid = (sdl_powkiddy_video_t*)data;
   if (!vid) return;
   vid->menu_active = state;
   if (!state) vid->was_in_menu = false;
}

static void sdl_powkiddy_set_texture_frame(void *data, const void *frame, bool rgb32,
      unsigned width, unsigned height, float alpha)
{
   sdl_powkiddy_video_t *vid = (sdl_powkiddy_video_t*)data;
   if (!vid || rgb32 || !vid->menu_texture) return;
   if (width > vid->menu_w || height > vid->menu_h)  return;
   vid->menu_texture_width  = width;
   vid->menu_texture_height = height;
   memcpy(vid->menu_texture, frame, width * height * sizeof(uint16_t));
}

static void sdl_powkiddy_gfx_set_nonblock_state(void *data, bool toggle,
      bool adaptive_vsync_enabled, unsigned swap_interval)
{
   sdl_powkiddy_video_t *vid = (sdl_powkiddy_video_t*)data;
   bool vsync = !toggle;
   uint32_t sf;
   if (!vid || vid->vsync == vsync) return;
   vid->vsync = vsync;
   sf = vsync ? (SDL_HWSURFACE | SDL_TRIPLEBUF | SDL_FULLSCREEN)
              : (SDL_HWSURFACE | SDL_FULLSCREEN);
   /* Double SDL_SetVideoMode trick to force driver reset */
   SDL_SetVideoMode((int)vid->fb_width, (int)vid->fb_height - 2, 16, sf);
   vid->screen = SDL_SetVideoMode((int)vid->fb_width, (int)vid->fb_height, 16, sf);
}

static void sdl_powkiddy_apply_state_changes(void *data)
{
   sdl_powkiddy_video_t *vid = (sdl_powkiddy_video_t*)data;
   settings_t *settings      = config_get_ptr();
   if (!vid || !settings) return;

   bool ka       = settings->bools.video_dingux_ipu_keep_aspect;
   bool is       = settings->bools.video_scale_integer;
   unsigned rot  = settings->uints.video_rotation & 3;

   bool changed = false;

   if (vid->keep_aspect != ka || vid->integer_scaling != is)
   {
      vid->keep_aspect     = ka;
      vid->integer_scaling = is;
      changed = true;
   }

   if (vid->rotation != rot)
   {
      vid->rotation = rot;
      sdl_powkiddy_update_menu_dims(vid);
      changed = true;
   }

   if (changed)
      RARCH_LOG("[Powkiddy]: State change: keep=%d int=%d rot=%u"
                " menu=%ux%u\n",
                vid->keep_aspect, vid->integer_scaling,
                vid->rotation, vid->menu_w, vid->menu_h);
}


static void sdl_powkiddy_gfx_check_window(sdl_powkiddy_video_t *vid)
{
   SDL_Event event;
   SDL_PumpEvents();
   while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_QUITMASK))
   {
      if (event.type != SDL_QUIT) continue;
      vid->quitting = true;
      break;
   }
}

static bool sdl_powkiddy_gfx_alive(void *data)
{
   sdl_powkiddy_video_t *vid = (sdl_powkiddy_video_t*)data;
   if (!vid) return false;
   sdl_powkiddy_gfx_check_window(vid);
   return !vid->quitting;
}

static bool sdl_powkiddy_gfx_focus(void *data)                     { return true; }
static bool sdl_powkiddy_gfx_suppress_screensaver(void *d, bool e) { return false; }
static bool sdl_powkiddy_gfx_has_windowed(void *data)              { return false; }
static bool sdl_powkiddy_gfx_set_shader(void *d,
      enum rarch_shader_type t, const char *p)                      { return false; }

static void sdl_powkiddy_gfx_viewport_info(void *data, struct video_viewport *vp)
{
   sdl_powkiddy_video_t *vid = (sdl_powkiddy_video_t*)data;
   if (!vid) return;
   vp->x = vp->y = 0;
   vp->width  = vp->full_width  = vid->frame_width;
   vp->height = vp->full_height = vid->frame_height;
}

static float sdl_powkiddy_get_refresh_rate(void *data)
{
   return 60.0f;
}

static uint32_t sdl_powkiddy_get_flags(void *data) { return 0; }

static const video_poke_interface_t sdl_powkiddy_poke_interface = {
   sdl_powkiddy_get_flags,
   NULL, NULL, NULL,
   sdl_powkiddy_get_refresh_rate,
   NULL, //set filtering
   NULL, NULL, NULL, NULL, NULL, NULL,
   sdl_powkiddy_apply_state_changes,
   sdl_powkiddy_set_texture_frame,
   sdl_powkiddy_set_texture_enable,
   NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL
};

static void sdl_powkiddy_get_poke_interface(void *data,
      const video_poke_interface_t **iface)
{ *iface = &sdl_powkiddy_poke_interface; }

video_driver_t video_sdl_powkiddy = {
   sdl_powkiddy_gfx_init,
   sdl_powkiddy_gfx_frame,
   sdl_powkiddy_gfx_set_nonblock_state,
   sdl_powkiddy_gfx_alive,
   sdl_powkiddy_gfx_focus,
   sdl_powkiddy_gfx_suppress_screensaver,
   sdl_powkiddy_gfx_has_windowed,
   sdl_powkiddy_gfx_set_shader,
   sdl_powkiddy_gfx_free,
   "sdl_powkiddy",
   NULL, NULL,
   sdl_powkiddy_gfx_viewport_info,
   NULL, NULL,
#ifdef HAVE_OVERLAY
   NULL,
#endif
#ifdef HAVE_VIDEO_LAYOUT
   NULL,
#endif
   sdl_powkiddy_get_poke_interface
};
