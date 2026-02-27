/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2010-2014 - Hans-Kristian Arntzen
 *  Copyright (C) 2011-2017 - Daniel De Matteis
 *  Copyright (C) 2011-2017 - Higor Euripedes
 *  Copyright (C) 2019-2021 - James Leaver
 *
 *  Modified for Powkiddy X39 Pro - 2026
 *
 *  ARCHITECTURE v4 — SDL 480x854 portrait direct, pure C scaler
 *  Rotation CW (+90°)
 *
 *  Fixes vs v3 :
 *    - integer_scaling : max hauteur portrait → s = min(sy, sx)
 *    - BICUBIC → bilinear (bicubic trop lent)
 *    - last_msg jamais effacé (évite flickering OSD sur frames NULL)
 */

#include <stdlib.h>
#include <string.h>

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

#include "../../dingux/dingux_utils.h"
#include "../../verbosity.h"
#include "../../gfx/drivers_font_renderer/bitmap.h"
#include "../../configuration.h"
#include "../../retroarch.h"
#if defined(DINGUX_BETA)
#include "../../driver.h"
#endif

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#define SDL_DINGUX_FB_WIDTH   480
#define SDL_DINGUX_FB_HEIGHT  854

#define SDL_DINGUX_MENU_WIDTH  854
#define SDL_DINGUX_MENU_HEIGHT 480

#define SDL_DINGUX_NUM_FONT_GLYPHS 256

typedef struct sdl_dingux_video
{
   retro_time_t last_frame_time;
   retro_time_t ff_frame_time_min;
   SDL_Surface *screen;
   bitmapfont_lut_t *osd_font;
   unsigned frame_width;
   unsigned frame_height;
   enum dingux_ipu_filter_type filter_type;
#if defined(DINGUX_BETA)
   enum dingux_refresh_rate refresh_rate;
#endif
   uint32_t font_colour32;
   uint16_t font_colour16;
   uint16_t menu_texture[SDL_DINGUX_MENU_WIDTH * SDL_DINGUX_MENU_HEIGHT]
         __attribute__((aligned(16)));
   unsigned menu_texture_width;
   unsigned menu_texture_height;
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
} sdl_dingux_video_t;

/* ==========================================================================
 * Calcul du rectangle de destination
 * Rotation CW : src_w → hauteur portrait, src_h → largeur portrait
 * ========================================================================== */
static void sdl_dingux_compute_out_rect(
      unsigned src_w, unsigned src_h,
      bool keep_aspect, bool integer_scaling,
      unsigned *out_x, unsigned *out_y,
      unsigned *out_w, unsigned *out_h)
{
   unsigned rotated_w = src_h;
   unsigned rotated_h = src_w;
   unsigned dst_w     = SDL_DINGUX_FB_WIDTH;
   unsigned dst_h     = SDL_DINGUX_FB_HEIGHT;
   unsigned ow, oh;

   if (integer_scaling)
   {
      /* Force oh = dst_w (480) = plein écran largeur landscape.
       * ow calculé par ratio exact pour garder les proportions.
       * Pixels légèrement rectangulaires acceptés pour maximiser la hauteur.
       * NES 256×240 → rot 240×256 : oh=480, ow=240×480/256=450
       * GBA 240×160 → rot 160×240 : oh=480, ow=160×480/240=320
       * GB  160×144 → rot 144×160 : oh=480, ow=144×480/160=432
       * SNES 256×224 → rot 224×256: oh=480, ow=224×480/256=420 */
      ow = SDL_DINGUX_FB_WIDTH;                                  /* 480 = plein écran */
      oh = rotated_w * dst_h / rotated_h;          /* ratio exact */
      if (oh < 1) oh = 1;
      if (oh > dst_h) oh = dst_h;                  /* clamp sécurité */
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

/* ==========================================================================
 * Helpers RGB565
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
 * Nearest-neighbor CW
 * ========================================================================== */
static void scale_rotate_cw_nearest_16(
      const uint16_t * __restrict__ src,
      unsigned src_w, unsigned src_h, unsigned src_stride,
      uint16_t * __restrict__ fb, unsigned fb_stride,
      unsigned out_x, unsigned out_y, unsigned out_w, unsigned out_h)
{
   unsigned dx, dy;
   unsigned sx_lut[SDL_DINGUX_FB_HEIGHT];
   {
      unsigned step = ((unsigned long long)src_w << 16) / out_h;
      unsigned fp   = (unsigned long long)(out_h - 1) * step;
      for (dy = 0; dy < out_h; dy++, fp -= step)
      {
         unsigned sx = fp >> 16;
         if (sx >= src_w) sx = src_w - 1;
         sx_lut[dy] = sx;
      }
   }
   unsigned step_y = ((unsigned long long)src_h << 16) / out_w;
   unsigned fy     = 0;
   for (dx = 0; dx < out_w; dx++, fy += step_y)
   {
      unsigned sy = fy >> 16;
      if (sy >= src_h) sy = src_h - 1;
      const uint16_t *src_row = src + sy * src_stride;
      uint16_t       *dst_col = fb + out_y * fb_stride + (out_x + dx);
      for (dy = 0; dy < out_h; dy++)
         dst_col[dy * fb_stride] = src_row[sx_lut[dy]];
   }
}

/* ==========================================================================
 * Bilinear 2D CW
 * ========================================================================== */
static void scale_rotate_cw_bilinear_16(
      const uint16_t * __restrict__ src,
      unsigned src_w, unsigned src_h, unsigned src_stride,
      uint16_t * __restrict__ fb, unsigned fb_stride,
      unsigned out_x, unsigned out_y, unsigned out_w, unsigned out_h)
{
   unsigned dx, dy;
   unsigned sx_fp_lut[SDL_DINGUX_FB_HEIGHT];
   {
      unsigned step = ((unsigned long long)src_w << 16) / out_h;
      unsigned fp   = (unsigned long long)(out_h - 1) * step;
      for (dy = 0; dy < out_h; dy++, fp -= step)
         sx_fp_lut[dy] = fp;
   }
   unsigned step_y = ((unsigned long long)src_h << 16) / out_w;
   unsigned fy_fp  = 0;
   for (dx = 0; dx < out_w; dx++, fy_fp += step_y)
   {
      unsigned sy0 = fy_fp >> 16;
      unsigned fy8 = (fy_fp >> 8) & 0xFF;
      if (sy0 >= src_h) sy0 = src_h - 1;
      unsigned sy1 = (sy0 + 1 < src_h) ? sy0 + 1 : sy0;
      const uint16_t *row0 = src + sy0 * src_stride;
      const uint16_t *row1 = src + sy1 * src_stride;
      uint16_t       *dst  = fb + out_y * fb_stride + (out_x + dx);
      for (dy = 0; dy < out_h; dy++)
      {
         unsigned sx_fp = sx_fp_lut[dy];
         unsigned sx0   = sx_fp >> 16;
         unsigned fx8   = (sx_fp >> 8) & 0xFF;
         if (sx0 >= src_w) sx0 = src_w - 1;
         unsigned sx1 = (sx0 + 1 < src_w) ? sx0 + 1 : sx0;
         int r00, g00, b00; rgb565_unpack(row0[sx0], &r00, &g00, &b00);
         int r10, g10, b10; rgb565_unpack(row0[sx1], &r10, &g10, &b10);
         int r01, g01, b01; rgb565_unpack(row1[sx0], &r01, &g01, &b01);
         int r11, g11, b11; rgb565_unpack(row1[sx1], &r11, &g11, &b11);
         int ifx = 255 - fx8, ify = 255 - fy8;
         int r = (ifx*ify*r00 + fx8*ify*r10 + ifx*fy8*r01 + fx8*fy8*r11) >> 16;
         int g = (ifx*ify*g00 + fx8*ify*g10 + ifx*fy8*g01 + fx8*fy8*g11) >> 16;
         int b = (ifx*ify*b00 + fx8*ify*b10 + ifx*fy8*b01 + fx8*fy8*b11) >> 16;
         dst[dy * fb_stride] = rgb565_pack(r, g, b);
      }
   }
}

/* ==========================================================================
 * Dispatcher + bandes noires à chaque frame
 * ========================================================================== */
static void sdl_dingux_scale_rotate_cw_16(
      const uint16_t * __restrict__ src,
      unsigned src_w, unsigned src_h, unsigned src_stride,
      uint16_t * __restrict__ fb, unsigned fb_stride,
      unsigned out_x, unsigned out_y, unsigned out_w, unsigned out_h,
      enum dingux_ipu_filter_type filter)
{
   unsigned dy;

   /* Bandes noires effacées à chaque frame.
    * Nécessaire : le scaler n'écrit que dans out_rect,
    * les zones autour doivent être remises à zéro explicitement. */
   if (out_y > 0)
      memset(fb, 0, out_y * fb_stride * sizeof(uint16_t));
   if (out_y + out_h < SDL_DINGUX_FB_HEIGHT)
      memset(fb + (out_y + out_h) * fb_stride, 0,
             (SDL_DINGUX_FB_HEIGHT - out_y - out_h) * fb_stride * sizeof(uint16_t));
   if (out_x > 0 || out_x + out_w < SDL_DINGUX_FB_WIDTH)
   {
      for (dy = out_y; dy < out_y + out_h; dy++)
      {
         uint16_t *row = fb + dy * fb_stride;
         if (out_x > 0)
            memset(row, 0, out_x * sizeof(uint16_t));
         if (out_x + out_w < SDL_DINGUX_FB_WIDTH)
            memset(row + out_x + out_w, 0,
                   (SDL_DINGUX_FB_WIDTH - out_x - out_w) * sizeof(uint16_t));
      }
   }

   switch (filter)
   {
      case DINGUX_IPU_FILTER_BICUBIC:  /* bicubic → bilinear */
      case DINGUX_IPU_FILTER_BILINEAR:
         scale_rotate_cw_bilinear_16(src, src_w, src_h, src_stride,
               fb, fb_stride, out_x, out_y, out_w, out_h);
         break;
      case DINGUX_IPU_FILTER_NEAREST:
      default:
         scale_rotate_cw_nearest_16(src, src_w, src_h, src_stride,
               fb, fb_stride, out_x, out_y, out_w, out_h);
         break;
   }
}

static void sdl_dingux_scale_rotate_cw_32(
      const uint32_t * __restrict__ src,
      unsigned src_w, unsigned src_h, unsigned src_stride,
      uint16_t * __restrict__ fb, unsigned fb_stride,
      unsigned out_x, unsigned out_y, unsigned out_w, unsigned out_h,
      enum dingux_ipu_filter_type filter)
{
   (void)filter;
   unsigned dx, dy;
   if (out_y > 0)
      memset(fb, 0, out_y * fb_stride * sizeof(uint16_t));
   if (out_y + out_h < SDL_DINGUX_FB_HEIGHT)
      memset(fb + (out_y + out_h) * fb_stride, 0,
             (SDL_DINGUX_FB_HEIGHT - out_y - out_h) * fb_stride * sizeof(uint16_t));

   unsigned sx_lut[SDL_DINGUX_FB_HEIGHT];
   {
      unsigned step_x = ((unsigned long long)src_w << 16) / out_h;
      unsigned fx     = (unsigned long long)(out_h - 1) * step_x;
      for (dy = 0; dy < out_h; dy++, fx -= step_x)
      {
         unsigned sx = fx >> 16;
         if (sx >= src_w) sx = src_w - 1;
         sx_lut[dy] = sx;
      }
   }
   unsigned step_y = ((unsigned long long)src_h << 16) / out_w;
   unsigned fy     = 0;
   for (dx = 0; dx < out_w; dx++, fy += step_y)
   {
      unsigned sy = fy >> 16;
      if (sy >= src_h) sy = src_h - 1;
      const uint32_t *src_row = src + sy * src_stride;
      uint16_t       *dst_col = fb + out_y * fb_stride + (out_x + dx);
      for (dy = 0; dy < out_h; dy++)
      {
         uint32_t c = src_row[sx_lut[dy]];
         dst_col[dy * fb_stride] = (uint16_t)(
               ((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F));
      }
   }
   if (out_x > 0 || out_x + out_w < SDL_DINGUX_FB_WIDTH)
   {
      for (dy = out_y; dy < out_y + out_h; dy++)
      {
         uint16_t *row = fb + dy * fb_stride;
         if (out_x > 0)
            memset(row, 0, out_x * sizeof(uint16_t));
         if (out_x + out_w < SDL_DINGUX_FB_WIDTH)
            memset(row + out_x + out_w, 0,
                   (SDL_DINGUX_FB_WIDTH - out_x - out_w) * sizeof(uint16_t));
      }
   }
}

/* ==========================================================================
 * Font / OSD
 * ========================================================================== */
static void sdl_dingux_init_font_color(sdl_dingux_video_t *vid)
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

static void sdl_dingux_blit_text_cw(sdl_dingux_video_t *vid,
      unsigned x, unsigned y, const char *str)
{
   uint16_t *fb        = (uint16_t*)vid->screen->pixels;
   unsigned  fb_stride = vid->screen->pitch >> 1;
   bool    **font_lut  = vid->osd_font->lut;
   uint16_t  col       = vid->font_colour16;
   unsigned  x_pos     = x;
   unsigned  y_pos     = y;
   if (y_pos + FONT_HEIGHT + 1 >= SDL_DINGUX_MENU_HEIGHT) return;
   while (!string_is_empty(str))
   {
      if (x_pos + FONT_WIDTH_STRIDE + 1 >= SDL_DINGUX_MENU_WIDTH) return;
      if (*str == ' ') { str++; x_pos += FONT_WIDTH_STRIDE; continue; }
      uint32_t symbol = utf8_walk(&str);
      if (symbol == 339) symbol = 156;
      if (symbol == 338) symbol = 140;
      if (symbol >= SDL_DINGUX_NUM_FONT_GLYPHS) continue;
      bool *sym_lut = font_lut[symbol];
      unsigned i, j;
      for (j = 0; j < FONT_HEIGHT; j++)
      {
         unsigned ly = y_pos + j;
         for (i = 0; i < FONT_WIDTH; i++)
         {
            if (!sym_lut[i + j * FONT_WIDTH]) continue;
            unsigned lx  = x_pos + i;
            unsigned fpx = ly;
            unsigned fpy = SDL_DINGUX_FB_HEIGHT - 1 - lx;
            if (fpx < SDL_DINGUX_FB_WIDTH && fpy < SDL_DINGUX_FB_HEIGHT)
               fb[fpy * fb_stride + fpx] = col;
         }
      }
      x_pos += FONT_WIDTH_STRIDE;
   }
}

static void sdl_dingux_blit_video_mode_error_msg(sdl_dingux_video_t *vid)
{
   const char *error_msg = msg_hash_to_str(MSG_UNSUPPORTED_VIDEO_MODE);
   char display_mode[64];
   display_mode[0] = '\0';
   memset(vid->screen->pixels, 0, vid->screen->pitch * vid->screen->h);
   snprintf(display_mode, sizeof(display_mode), "> %ux%u, %s",
         vid->frame_width, vid->frame_height,
         vid->rgb32 ? "XRGB8888" : "RGB565");
   sdl_dingux_blit_text_cw(vid, FONT_WIDTH_STRIDE, FONT_WIDTH_STRIDE, error_msg);
   sdl_dingux_blit_text_cw(vid, FONT_WIDTH_STRIDE,
         FONT_WIDTH_STRIDE + FONT_HEIGHT_STRIDE, display_mode);
}

/* ==========================================================================
 * Init / Free
 * ========================================================================== */
static void sdl_dingux_gfx_free(void *data)
{
   sdl_dingux_video_t *vid = (sdl_dingux_video_t*)data;
   if (!vid) return;
#if defined(DINGUX_BETA)
   dingux_ipu_reset();
#else
   if (!vid->keep_aspect || vid->integer_scaling)
      dingux_ipu_set_scaling_mode(true, false);
   if (vid->filter_type != DINGUX_IPU_FILTER_BICUBIC)
      dingux_ipu_set_filter_type(DINGUX_IPU_FILTER_BICUBIC);
#endif
   if (vid->osd_font)      bitmapfont_free_lut(vid->osd_font);
   if (vid->last_frame_buf) free(vid->last_frame_buf);
   free(vid);
}

static void sdl_dingux_input_driver_init(
      const char *input_drv_name, const char *joypad_drv_name,
      input_driver_t **input, void **input_data)
{
   if (!input || !input_data) return;
   *input = NULL; *input_data = NULL;
   if (string_is_empty(input_drv_name)) return;
   if (string_is_equal(input_drv_name, "sdl_dingux"))
   {
      *input_data = input_driver_init_wrap(&input_sdl_dingux, joypad_drv_name);
      if (*input_data) *input = &input_sdl_dingux;
      return;
   }
#if defined(HAVE_SDL) || defined(HAVE_SDL2)
   if (string_is_equal(input_drv_name, "sdl"))
   {
      *input_data = input_driver_init_wrap(&input_sdl, joypad_drv_name);
      if (*input_data) *input = &input_sdl;
      return;
   }
#endif
#if defined(HAVE_UDEV)
   if (string_is_equal(input_drv_name, "udev"))
   {
      *input_data = input_driver_init_wrap(&input_udev, joypad_drv_name);
      if (*input_data) *input = &input_udev;
      return;
   }
#endif
#if defined(__linux__)
   if (string_is_equal(input_drv_name, "linuxraw"))
   {
      *input_data = input_driver_init_wrap(&input_linuxraw, joypad_drv_name);
      if (*input_data) *input = &input_linuxraw;
      return;
   }
#endif
}

static void *sdl_dingux_gfx_init(const video_info_t *video,
      input_driver_t **input, void **input_data)
{
   sdl_dingux_video_t *vid            = NULL;
   uint32_t sdl_subsystem_flags       = SDL_WasInit(0);
   settings_t *settings               = config_get_ptr();
   bool ipu_keep_aspect               = settings->bools.video_dingux_ipu_keep_aspect;
   bool ipu_integer_scaling           = settings->bools.video_scale_integer;
   enum dingux_ipu_filter_type ipu_ft = (enum dingux_ipu_filter_type)
         settings->uints.video_dingux_ipu_filter_type;
   const char *input_drv_name         = settings->arrays.input_driver;
   const char *joypad_drv_name        = settings->arrays.input_joypad_driver;
   uint32_t surface_flags             = video->vsync ?
         (SDL_HWSURFACE | SDL_TRIPLEBUF | SDL_FULLSCREEN) :
         (SDL_HWSURFACE | SDL_FULLSCREEN);
#if defined(DINGUX_BETA)
   enum dingux_refresh_rate cur_rr    = DINGUX_REFRESH_RATE_60HZ;
   enum dingux_refresh_rate tgt_rr    = (enum dingux_refresh_rate)
         settings->uints.video_dingux_refresh_rate;
   bool rr_valid                      = false;
   float hw_rr                        = 0.0f;
#endif

   if (sdl_subsystem_flags == 0)
   { if (SDL_Init(SDL_INIT_VIDEO) < 0) return NULL; }
   else if (!(sdl_subsystem_flags & SDL_INIT_VIDEO))
   { if (SDL_InitSubSystem(SDL_INIT_VIDEO) < 0) return NULL; }

   vid = (sdl_dingux_video_t*)calloc(1, sizeof(*vid));
   if (!vid) return NULL;

   dingux_ipu_set_downscaling_enable(true);
   dingux_ipu_set_scaling_mode(ipu_keep_aspect, ipu_integer_scaling);
   dingux_ipu_set_filter_type(ipu_ft);

#if defined(DINGUX_BETA)
   rr_valid = dingux_get_video_refresh_rate(&cur_rr);
   if (!rr_valid || cur_rr != tgt_rr)
      hw_rr = dingux_set_video_refresh_rate(tgt_rr);
   else
      hw_rr = (cur_rr == DINGUX_REFRESH_RATE_50HZ) ? 50.0f : 60.0f;
   if (hw_rr == 0.0f)
   { RARCH_ERR("[SDL1]: Failed to set video refresh rate\n"); goto error; }
   vid->refresh_rate      = tgt_rr;
   vid->ff_frame_time_min = (tgt_rr == DINGUX_REFRESH_RATE_50HZ) ? 20000 : 16667;
   driver_ctl(RARCH_DRIVER_CTL_SET_REFRESH_RATE, &hw_rr);
#else
   vid->ff_frame_time_min = 16667;
#endif

   vid->screen = SDL_SetVideoMode(SDL_DINGUX_FB_WIDTH, SDL_DINGUX_FB_HEIGHT,
         16, surface_flags);
   if (!vid->screen)
   {
      RARCH_ERR("[SDL1]: Failed to init SDL surface: %s\n", SDL_GetError());
      goto error;
   }
   RARCH_LOG("[SDL1]: Portrait surface %dx%d pitch=%d\n",
             SDL_DINGUX_FB_WIDTH, SDL_DINGUX_FB_HEIGHT, vid->screen->pitch);

   vid->frame_width         = SDL_DINGUX_FB_WIDTH;
   vid->frame_height        = SDL_DINGUX_FB_HEIGHT;
   vid->rgb32               = video->rgb32;
   vid->vsync               = video->vsync;
   vid->keep_aspect         = ipu_keep_aspect;
   vid->integer_scaling     = ipu_integer_scaling;
   vid->filter_type         = ipu_ft;
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
   sdl_dingux_input_driver_init(input_drv_name, joypad_drv_name, input, input_data);
   sdl_dingux_init_font_color(vid);
   vid->osd_font = bitmapfont_get_lut();
   if (!vid->osd_font || vid->osd_font->glyph_max < (SDL_DINGUX_NUM_FONT_GLYPHS - 1))
   { RARCH_ERR("[SDL1]: Failed to init OSD font\n"); goto error; }
   return vid;

error:
   sdl_dingux_gfx_free(vid);
   return NULL;
}

/* ==========================================================================
 * Frame
 * ========================================================================== */
static bool sdl_dingux_gfx_frame(void *data, const void *frame,
      unsigned width, unsigned height, uint64_t frame_count,
      unsigned pitch, const char *msg, video_frame_info_t *video_info)
{
   sdl_dingux_video_t *vid = (sdl_dingux_video_t*)data;
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

   /* Cache dernier frame jeu */
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

   /* Cache OSD — ne jamais effacer last_msg.
    * RetroArch envoie msg=NULL sur frames dupliquées → on garde le dernier. */
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

         static unsigned last_w = 0, last_h = 0;
         static bool last_ka = false, last_is = false;
         if (src_w != last_w || src_h != last_h ||
             vid->keep_aspect != last_ka || vid->integer_scaling != last_is)
         {
            last_w = src_w; last_h = src_h;
            last_ka = vid->keep_aspect; last_is = vid->integer_scaling;
            unsigned dbg_x, dbg_y, dbg_w, dbg_h;
            sdl_dingux_compute_out_rect(src_w, src_h,
                  vid->keep_aspect, vid->integer_scaling, &dbg_x, &dbg_y, &dbg_w, &dbg_h);
            RARCH_LOG("[SDL1]: Game %ux%u keep=%d int=%d → out %ux%u at %u,%u\n",
                      src_w, src_h, vid->keep_aspect, vid->integer_scaling,
                      dbg_w, dbg_h, dbg_x, dbg_y);
         }

         sdl_dingux_compute_out_rect(src_w, src_h,
               vid->keep_aspect, vid->integer_scaling,
               &out_x, &out_y, &out_w, &out_h);

         if (vid->rgb32)
            sdl_dingux_scale_rotate_cw_32(
                  (const uint32_t*)src_frame, src_w, src_h, src_pitch >> 2,
                  fb, fb_stride, out_x, out_y, out_w, out_h, vid->filter_type);
         else
            sdl_dingux_scale_rotate_cw_16(
                  (const uint16_t*)src_frame, src_w, src_h, src_pitch >> 1,
                  fb, fb_stride, out_x, out_y, out_w, out_h, vid->filter_type);
      }
   }
   else
   {
      vid->was_in_menu = true;
      if (vid->menu_texture_width > 0 && vid->menu_texture_height > 0)
      {
         uint16_t *fb        = (uint16_t*)vid->screen->pixels;
         unsigned  fb_stride = vid->screen->pitch >> 1;
         unsigned out_x, out_y, out_w, out_h;
         sdl_dingux_compute_out_rect(
               vid->menu_texture_width, vid->menu_texture_height,
               vid->keep_aspect, vid->integer_scaling,
               &out_x, &out_y, &out_w, &out_h);
         sdl_dingux_scale_rotate_cw_16(
               vid->menu_texture,
               vid->menu_texture_width, vid->menu_texture_height,
               vid->menu_texture_width,
               fb, fb_stride, out_x, out_y, out_w, out_h, vid->filter_type);
                /* OSD menu : dessiné DANS le rect menu (toujours réécrit par le scaler)
          * → pas de flickering triple-buffer, contrairement aux bandes noires.
          * Coords landscape : x=bord gauche menu, y=bas du menu */
         if (vid->last_msg[0] && vid->osd_font)
         {
            unsigned osd_lx = out_x + out_w - (FONT_HEIGHT + FONT_WIDTH_STRIDE);
            unsigned osd_ly = out_y + (FONT_WIDTH_STRIDE);
            sdl_dingux_blit_text_cw(vid, osd_ly, osd_lx, vid->last_msg);
         }
      }
   }

   /* OSD jeu : bas-gauche paysage logique, hors menu */
   if (!vid->menu_active && vid->last_msg[0] && vid->osd_font)
      sdl_dingux_blit_text_cw(vid, FONT_WIDTH_STRIDE,
            SDL_DINGUX_MENU_HEIGHT - (FONT_HEIGHT + FONT_WIDTH_STRIDE),
            vid->last_msg);
   if (SDL_MUSTLOCK(vid->screen)) SDL_UnlockSurface(vid->screen);
   SDL_Flip(vid->screen);
   return true;
}

/* ==========================================================================
 * State changes
 * ========================================================================== */
static void sdl_dingux_set_texture_enable(void *data, bool state, bool full_screen)
{
   sdl_dingux_video_t *vid = (sdl_dingux_video_t*)data;
   if (!vid) return;
   vid->menu_active = state;
   if (!state) vid->was_in_menu = false;
}

static void sdl_dingux_set_texture_frame(void *data, const void *frame, bool rgb32,
      unsigned width, unsigned height, float alpha)
{
   sdl_dingux_video_t *vid = (sdl_dingux_video_t*)data;
   if (!vid || rgb32 || width > SDL_DINGUX_MENU_WIDTH || height > SDL_DINGUX_MENU_HEIGHT)
      return;
   vid->menu_texture_width  = width;
   vid->menu_texture_height = height;
   memcpy(vid->menu_texture, frame, width * height * sizeof(uint16_t));
}

static void sdl_dingux_gfx_set_nonblock_state(void *data, bool toggle,
      bool adaptive_vsync_enabled, unsigned swap_interval)
{
   sdl_dingux_video_t *vid = (sdl_dingux_video_t*)data;
   bool vsync = !toggle;
   uint32_t sf;
   if (!vid || vid->vsync == vsync) return;
   vid->vsync = vsync;
   sf = vsync ? (SDL_HWSURFACE | SDL_TRIPLEBUF | SDL_FULLSCREEN)
              : (SDL_HWSURFACE | SDL_FULLSCREEN);
   SDL_SetVideoMode(SDL_DINGUX_FB_WIDTH, SDL_DINGUX_FB_HEIGHT - 2, 16, sf);
   vid->screen = SDL_SetVideoMode(SDL_DINGUX_FB_WIDTH, SDL_DINGUX_FB_HEIGHT, 16, sf);
}

static void sdl_dingux_apply_state_changes(void *data)
{
   sdl_dingux_video_t *vid = (sdl_dingux_video_t*)data;
   settings_t *settings    = config_get_ptr();
   bool ka, is;
   if (!vid || !settings) return;
   ka = settings->bools.video_dingux_ipu_keep_aspect;
   is = settings->bools.video_scale_integer;
   if (vid->keep_aspect != ka || vid->integer_scaling != is)
   {
      RARCH_LOG("[SDL1]: State change: keep_aspect=%d integer_scaling=%d\n", ka, is);
      dingux_ipu_set_scaling_mode(ka, is);
      vid->keep_aspect     = ka;
      vid->integer_scaling = is;
   }
}

static void sdl_dingux_set_filtering(void *data, unsigned index, bool smooth, bool ctx_scaling)
{
   sdl_dingux_video_t *vid = (sdl_dingux_video_t*)data;
   settings_t *settings    = config_get_ptr();
   enum dingux_ipu_filter_type ft;
   if (!vid || !settings) return;
   ft = (enum dingux_ipu_filter_type)settings->uints.video_dingux_ipu_filter_type;
   if (vid->filter_type != ft) { dingux_ipu_set_filter_type(ft); vid->filter_type = ft; }
}

static void sdl_dingux_gfx_check_window(sdl_dingux_video_t *vid)
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

static bool sdl_dingux_gfx_alive(void *data)
{
   sdl_dingux_video_t *vid = (sdl_dingux_video_t*)data;
   if (!vid) return false;
   sdl_dingux_gfx_check_window(vid);
   return !vid->quitting;
}

static bool sdl_dingux_gfx_focus(void *data)                    { return true; }
static bool sdl_dingux_gfx_suppress_screensaver(void *d, bool e) { return false; }
static bool sdl_dingux_gfx_has_windowed(void *data)              { return false; }
static bool sdl_dingux_gfx_set_shader(void *d,
      enum rarch_shader_type t, const char *p)                   { return false; }

static void sdl_dingux_gfx_viewport_info(void *data, struct video_viewport *vp)
{
   sdl_dingux_video_t *vid = (sdl_dingux_video_t*)data;
   if (!vid) return;
   vp->x = vp->y = 0;
   vp->width  = vp->full_width  = vid->frame_width;
   vp->height = vp->full_height = vid->frame_height;
}

static float sdl_dingux_get_refresh_rate(void *data)
{
#if defined(DINGUX_BETA)
   sdl_dingux_video_t *vid = (sdl_dingux_video_t*)data;
   if (vid && vid->refresh_rate == DINGUX_REFRESH_RATE_50HZ) return 50.0f;
#endif
   return 60.0f;
}

static uint32_t sdl_dingux_get_flags(void *data) { return 0; }

static const video_poke_interface_t sdl_dingux_poke_interface = {
   sdl_dingux_get_flags,
   NULL, NULL, NULL,
   sdl_dingux_get_refresh_rate,
   sdl_dingux_set_filtering,
   NULL, NULL, NULL, NULL, NULL, NULL,
   sdl_dingux_apply_state_changes,
   sdl_dingux_set_texture_frame,
   sdl_dingux_set_texture_enable,
   NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL
};

static void sdl_dingux_get_poke_interface(void *data, const video_poke_interface_t **iface)
{ *iface = &sdl_dingux_poke_interface; }

video_driver_t video_sdl_dingux = {
   sdl_dingux_gfx_init,
   sdl_dingux_gfx_frame,
   sdl_dingux_gfx_set_nonblock_state,
   sdl_dingux_gfx_alive,
   sdl_dingux_gfx_focus,
   sdl_dingux_gfx_suppress_screensaver,
   sdl_dingux_gfx_has_windowed,
   sdl_dingux_gfx_set_shader,
   sdl_dingux_gfx_free,
   "sdl_dingux",
   NULL, NULL,
   sdl_dingux_gfx_viewport_info,
   NULL, NULL,
#ifdef HAVE_OVERLAY
   NULL,
#endif
#ifdef HAVE_VIDEO_LAYOUT
   NULL,
#endif
   sdl_dingux_get_poke_interface
};