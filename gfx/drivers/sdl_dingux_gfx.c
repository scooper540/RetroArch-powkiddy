/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2010-2014 - Hans-Kristian Arntzen
 *  Copyright (C) 2011-2017 - Daniel De Matteis
 *  Copyright (C) 2011-2017 - Higor Euripedes
 *  Copyright (C) 2019-2021 - James Leaver
 *  
 *  Modified for Powkiddy X39 Pro - 2026
 *  Framebuffer: 480x854 physical, stride=960 bytes
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <string.h>

#include <SDL/SDL.h>
#include <SDL/SDL_video.h>

#include <retro_assert.h>
#include <gfx/video_frame.h>
#include <retro_assert.h>
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

/* Powkiddy X39 Pro: REQUIRES SDL_VIDEO_FBCON_ROTATION=CCW
 * Without rotation: framebuffer is 480x854 portrait, pitch=960
 * With CCW rotation: SDL creates 854x480 landscape, pitch=1708 (correct!)
 * Physical display: 854x480 landscape */
#define SDL_DINGUX_MENU_WIDTH  854
#define SDL_DINGUX_MENU_HEIGHT 480

static uint16_t *rgb32_convert_buffer = NULL;
static size_t rgb32_convert_buffer_size = 0;

/* OPTIMAL SCALING SYSTEM - RetroArch Settings Control:
 * 
 * Settings -> Video -> Integer Scale + Keep Aspect Ratio:
 * 
 * Integer ON + Keep Aspect ON = OPTIMAL SCALE (FILLS HEIGHT!)
 *   - Calculates best scale to fill 480p while preserving aspect ratio
 *   - GameBoy 160x144: 3.33x -> 533x480 (fills height!)
 *   - NES 256x240: 2.0x -> 512x480 (fills height!)
 *   - Genesis 320x224: 2.14x -> 685x479 (fills height!)
 *   - Uses dedicated NEON scalers (2x/3x/4x) when scale is integer
 *   - Uses NEON-optimized universal scaler for fractional scales (6-8x faster than C!)
 * 
 * Integer ON + Keep Aspect OFF = FULLSCREEN STRETCH
 *   - Stretches to entire 854x480 (distorts aspect ratio)
 *   - Uses NEON-optimized scaler
 * 
 * Integer OFF = NATIVE RESOLUTION
 *   - Emulator decides resolution, we just render it
 */

/* FPS limiter control - set to 0 to disable, 1 to enable */
#define ENABLE_FPS_LIMITER 0

#define SDL_DINGUX_NUM_FONT_GLYPHS 256

typedef struct sdl_dingux_video
{
   retro_time_t last_frame_time;
   retro_time_t ff_frame_time_min;
   SDL_Surface *screen;
   bitmapfont_lut_t *osd_font;
   unsigned frame_width;
   unsigned frame_height;
   unsigned frame_padding_x;
   unsigned frame_padding_y;
   enum dingux_ipu_filter_type filter_type;
#if defined(DINGUX_BETA)
   enum dingux_refresh_rate refresh_rate;
#endif
   uint32_t font_colour32;
   uint16_t font_colour16;
   uint16_t menu_texture[SDL_DINGUX_MENU_WIDTH * SDL_DINGUX_MENU_HEIGHT] __attribute__((aligned(16)));
   unsigned menu_texture_width;
   unsigned menu_texture_height;
   bool rgb32;
   bool vsync;
   bool keep_aspect;
   bool integer_scaling;
   bool menu_active;
   bool was_in_menu;
   bool quitting;
   bool mode_valid;
} sdl_dingux_video_t;

static void sdl_dingux_init_font_color(sdl_dingux_video_t *vid)
{
   settings_t *settings = config_get_ptr();
   uint32_t red         = 0xFF;
   uint32_t green       = 0xFF;
   uint32_t blue        = 0xFF;

   if (settings)
   {
      red   = (uint32_t)((settings->floats.video_msg_color_r * 255.0f) + 0.5f) & 0xFF;
      green = (uint32_t)((settings->floats.video_msg_color_g * 255.0f) + 0.5f) & 0xFF;
      blue  = (uint32_t)((settings->floats.video_msg_color_b * 255.0f) + 0.5f) & 0xFF;
   }

   /* Convert to XRGB8888 */
   vid->font_colour32 = (red << 16) | (green << 8) | blue;

   /* Convert to RGB565 */
   red   = red   >> 3;
   green = green >> 3;
   blue  = blue  >> 3;

   vid->font_colour16 = (red << 11) | (green << 6) | blue;
}

static void sdl_dingux_blit_text16(
      sdl_dingux_video_t *vid,
      unsigned x, unsigned y,
      const char *str)
{
   uint16_t *screen_buf         = (uint16_t*)vid->screen->pixels;
   bool **font_lut              = vid->osd_font->lut;
   uint16_t screen_stride       = (uint16_t)(vid->screen->pitch >> 1);
   uint16_t screen_width        = vid->screen->w;
   uint16_t screen_height       = vid->screen->h;
   unsigned x_pos               = x + vid->frame_padding_x;
   unsigned y_pos               = (y > (screen_height >> 1)) ?
         (y - vid->frame_padding_y) : (y + vid->frame_padding_y);
   uint16_t shadow_color_buf[2] = {0};
   uint16_t color_buf[2];

   color_buf[0] = vid->font_colour16;
   color_buf[1] = 0;

   if (y_pos + FONT_HEIGHT + 1 >= screen_height - vid->frame_padding_y)
      return;

   while (!string_is_empty(str))
   {
      if (x_pos + FONT_WIDTH_STRIDE + 1 >= screen_width - vid->frame_padding_x)
         return;

      if (*str == ' ')
         str++;
      else
      {
         uint16_t i, j;
         bool *symbol_lut;
         uint32_t symbol = utf8_walk(&str);

         if (symbol == 339) symbol = 156;
         if (symbol == 338) symbol = 140;

         if (symbol >= SDL_DINGUX_NUM_FONT_GLYPHS)
            continue;

         symbol_lut = font_lut[symbol];

         for (j = 0; j < FONT_HEIGHT; j++)
         {
            uint32_t buff_offset = ((y_pos + j) * screen_stride) + x_pos;

            for (i = 0; i < FONT_WIDTH; i++)
            {
               if (*(symbol_lut + i + (j * FONT_WIDTH)))
               {
                  uint16_t *screen_buf_ptr = screen_buf + buff_offset + i;
                  memcpy(screen_buf_ptr, color_buf, sizeof(uint16_t));
                  screen_buf_ptr += screen_stride;
                  memcpy(screen_buf_ptr, shadow_color_buf, sizeof(uint16_t));
               }
            }
         }
      }
      x_pos += FONT_WIDTH_STRIDE;
   }
}

static void sdl_dingux_blit_text32(
      sdl_dingux_video_t *vid,
      unsigned x, unsigned y,
      const char *str)
{
   uint32_t *screen_buf         = (uint32_t*)vid->screen->pixels;
   bool **font_lut              = vid->osd_font->lut;
   uint32_t screen_stride       = (uint32_t)(vid->screen->pitch >> 2);
   uint32_t screen_width        = vid->screen->w;
   uint32_t screen_height       = vid->screen->h;
   unsigned x_pos               = x + vid->frame_padding_x;
   unsigned y_pos               = (y > (screen_height >> 1)) ?
         (y - vid->frame_padding_y) : (y + vid->frame_padding_y);
   uint32_t shadow_color_buf[2] = {0};
   uint32_t color_buf[2];

   color_buf[0] = vid->font_colour32;
   color_buf[1] = 0;

   if (y_pos + FONT_HEIGHT + 1 >= screen_height - vid->frame_padding_y)
      return;

   while (!string_is_empty(str))
   {
      if (x_pos + FONT_WIDTH_STRIDE + 1 >= screen_width - vid->frame_padding_x)
         return;

      if (*str == ' ')
         str++;
      else
      {
         uint32_t i, j;
         bool *symbol_lut;
         uint32_t symbol = utf8_walk(&str);

         if (symbol == 339) symbol = 156;
         if (symbol == 338) symbol = 140;

         if (symbol >= SDL_DINGUX_NUM_FONT_GLYPHS)
            continue;

         symbol_lut = font_lut[symbol];

         for (j = 0; j < FONT_HEIGHT; j++)
         {
            uint32_t buff_offset = ((y_pos + j) * screen_stride) + x_pos;

            for (i = 0; i < FONT_WIDTH; i++)
            {
               if (*(symbol_lut + i + (j * FONT_WIDTH)))
               {
                  uint32_t *screen_buf_ptr = screen_buf + buff_offset + i;
                  memcpy(screen_buf_ptr, color_buf, sizeof(uint32_t));
                  screen_buf_ptr += screen_stride;
                  memcpy(screen_buf_ptr, shadow_color_buf, sizeof(uint32_t));
               }
            }
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

   if (vid->rgb32)
   {
      sdl_dingux_blit_text32(vid, FONT_WIDTH_STRIDE, FONT_WIDTH_STRIDE, error_msg);
      sdl_dingux_blit_text32(vid, FONT_WIDTH_STRIDE, FONT_WIDTH_STRIDE + FONT_HEIGHT_STRIDE, display_mode);
   }
   else
   {
      sdl_dingux_blit_text16(vid, FONT_WIDTH_STRIDE, FONT_WIDTH_STRIDE, error_msg);
      sdl_dingux_blit_text16(vid, FONT_WIDTH_STRIDE, FONT_WIDTH_STRIDE + FONT_HEIGHT_STRIDE, display_mode);
   }
}

static void sdl_dingux_gfx_free(void *data)
{
   sdl_dingux_video_t *vid = (sdl_dingux_video_t*)data;

   if (!vid)
      return;

#if defined(DINGUX_BETA)
   dingux_ipu_reset();
#else
   if (!vid->keep_aspect || vid->integer_scaling)
      dingux_ipu_set_scaling_mode(true, false);

   if (vid->filter_type != DINGUX_IPU_FILTER_BICUBIC)
      dingux_ipu_set_filter_type(DINGUX_IPU_FILTER_BICUBIC);
#endif

   if (vid->osd_font)
      bitmapfont_free_lut(vid->osd_font);

   free(vid);
}

static void sdl_dingux_input_driver_init(
      const char *input_drv_name, const char *joypad_drv_name,
      input_driver_t **input, void **input_data)
{
   if (!input || !input_data)
      return;

   *input      = NULL;
   *input_data = NULL;

   if (string_is_empty(input_drv_name))
      return;

   if (string_is_equal(input_drv_name, "sdl_dingux"))
   {
      *input_data = input_driver_init_wrap(&input_sdl_dingux, joypad_drv_name);
      if (*input_data)
         *input = &input_sdl_dingux;
      return;
   }

#if defined(HAVE_SDL) || defined(HAVE_SDL2)
   if (string_is_equal(input_drv_name, "sdl"))
   {
      *input_data = input_driver_init_wrap(&input_sdl, joypad_drv_name);
      if (*input_data)
         *input = &input_sdl;
      return;
   }
#endif

#if defined(HAVE_UDEV)
   if (string_is_equal(input_drv_name, "udev"))
   {
      *input_data = input_driver_init_wrap(&input_udev, joypad_drv_name);
      if (*input_data)
         *input = &input_udev;
      return;
   }
#endif

#if defined(__linux__)
   if (string_is_equal(input_drv_name, "linuxraw"))
   {
      *input_data = input_driver_init_wrap(&input_linuxraw, joypad_drv_name);
      if (*input_data)
         *input = &input_linuxraw;
      return;
   }
#endif
}

static void *sdl_dingux_gfx_init(const video_info_t *video,
      input_driver_t **input, void **input_data)
{
   sdl_dingux_video_t *vid                       = NULL;
   uint32_t sdl_subsystem_flags                  = SDL_WasInit(0);
   settings_t *settings                          = config_get_ptr();
   bool ipu_keep_aspect                          = settings->bools.video_dingux_ipu_keep_aspect;
   bool ipu_integer_scaling                      = settings->bools.video_scale_integer;
#if defined(DINGUX_BETA)
   enum dingux_refresh_rate current_refresh_rate = DINGUX_REFRESH_RATE_60HZ;
   enum dingux_refresh_rate target_refresh_rate  = (enum dingux_refresh_rate)
         settings->uints.video_dingux_refresh_rate;
   bool refresh_rate_valid                       = false;
   float hw_refresh_rate                         = 0.0f;
#endif
   enum dingux_ipu_filter_type ipu_filter_type   = (enum dingux_ipu_filter_type)
         settings->uints.video_dingux_ipu_filter_type;
   const char *input_drv_name                    = settings->arrays.input_driver;
   const char *joypad_drv_name                   = settings->arrays.input_joypad_driver;
   uint32_t surface_flags                        = (video->vsync) ?
         (SDL_HWSURFACE | SDL_TRIPLEBUF | SDL_FULLSCREEN) :
         (SDL_HWSURFACE | SDL_FULLSCREEN);

   if (sdl_subsystem_flags == 0)
   {
      if (SDL_Init(SDL_INIT_VIDEO) < 0)
         return NULL;
   }
   else if ((sdl_subsystem_flags & SDL_INIT_VIDEO) == 0)
   {
      if (SDL_InitSubSystem(SDL_INIT_VIDEO) < 0)
         return NULL;
   }

   vid = (sdl_dingux_video_t*)calloc(1, sizeof(*vid));
   if (!vid)
      return NULL;

   dingux_ipu_set_downscaling_enable(true);
   dingux_ipu_set_scaling_mode(ipu_keep_aspect, ipu_integer_scaling);
   dingux_ipu_set_filter_type(ipu_filter_type);
   
#if defined(DINGUX_BETA)
   refresh_rate_valid = dingux_get_video_refresh_rate(&current_refresh_rate);

   if (!refresh_rate_valid || (current_refresh_rate != target_refresh_rate))
      hw_refresh_rate = dingux_set_video_refresh_rate(target_refresh_rate);
   else
   {
      switch (current_refresh_rate)
      {
         case DINGUX_REFRESH_RATE_50HZ:
            hw_refresh_rate = 50.0f;
            break;
         default:
            hw_refresh_rate = 60.0f;
            break;
      }
   }

   if (hw_refresh_rate == 0.0f)
   {
      RARCH_ERR("[SDL1]: Failed to set video refresh rate\n");
      goto error;
   }

   vid->refresh_rate = target_refresh_rate;
   switch (target_refresh_rate)
   {
      case DINGUX_REFRESH_RATE_50HZ:
         vid->ff_frame_time_min = 20000;
         break;
      default:
         vid->ff_frame_time_min = 16667;
         break;
   }

   driver_ctl(RARCH_DRIVER_CTL_SET_REFRESH_RATE, &hw_refresh_rate);
#else
   vid->ff_frame_time_min = 16667;
#endif

   vid->screen = SDL_SetVideoMode(
         SDL_DINGUX_MENU_WIDTH, SDL_DINGUX_MENU_HEIGHT,
         video->rgb32 ? 32 : 16,
         surface_flags);

   if (!vid->screen)
   {
      RARCH_ERR("[SDL1]: Failed to init SDL surface: %s\n", SDL_GetError());
      goto error;
   }

   RARCH_LOG("[SDL1]: Native landscape %dx%d, pitch: %d bytes (expected: %d)\n", 
         SDL_DINGUX_MENU_WIDTH, SDL_DINGUX_MENU_HEIGHT, 
         vid->screen->pitch, SDL_DINGUX_MENU_WIDTH * (video->rgb32 ? 4 : 2));

   vid->frame_width     = SDL_DINGUX_MENU_WIDTH;
   vid->frame_height    = SDL_DINGUX_MENU_HEIGHT;
   vid->rgb32           = video->rgb32;
   vid->vsync           = video->vsync;
   vid->keep_aspect     = ipu_keep_aspect;
   vid->integer_scaling = ipu_integer_scaling;
   vid->filter_type     = ipu_filter_type;
   vid->menu_active     = false;
   vid->was_in_menu     = false;
   vid->quitting        = false;
   vid->mode_valid      = true;
   vid->last_frame_time = 0;
   vid->menu_texture_width = 0;
   vid->menu_texture_height = 0;

   SDL_ShowCursor(SDL_DISABLE);

   sdl_dingux_input_driver_init(input_drv_name, joypad_drv_name, input, input_data);

   sdl_dingux_init_font_color(vid);

   vid->osd_font = bitmapfont_get_lut();

   if (!vid->osd_font || vid->osd_font->glyph_max < (SDL_DINGUX_NUM_FONT_GLYPHS - 1))
   {
      RARCH_ERR("[SDL1]: Failed to init OSD font\n");
      goto error;
   }

   return vid;

error:
   sdl_dingux_gfx_free(vid);
   return NULL;
}

/* X39 Pro - use exact dimensions, no rounding */
static void sdl_dingux_sanitize_frame_dimensions(
      sdl_dingux_video_t* vid,
      unsigned width, unsigned height,
      unsigned *sanitized_width, unsigned *sanitized_height)
{
   /* Use exact dimensions - SDL on X39 Pro only accepts specific modes */
   *sanitized_width = width;
   *sanitized_height = height;
}

static void sdl_dingux_set_output(
      sdl_dingux_video_t* vid,
      unsigned width, unsigned height, bool rgb32)
{
   unsigned sanitized_width;
   unsigned sanitized_height;
   uint32_t surface_flags = (vid->vsync) ?
         (SDL_HWSURFACE | SDL_TRIPLEBUF | SDL_FULLSCREEN) :
         (SDL_HWSURFACE | SDL_FULLSCREEN);

   vid->frame_width  = width;
   vid->frame_height = height;
   vid->frame_padding_x = 0;
   vid->frame_padding_y = 0;

   sdl_dingux_sanitize_frame_dimensions(vid, width, height, &sanitized_width, &sanitized_height);

   vid->screen = SDL_SetVideoMode(sanitized_width, sanitized_height, rgb32 ? 32 : 16, surface_flags);

   if (unlikely(!vid->screen))
   {
      RARCH_ERR("[SDL1]: Failed to init SDL surface %ux%u: %s\n", 
            sanitized_width, sanitized_height, SDL_GetError());

      vid->screen = SDL_SetVideoMode(SDL_DINGUX_MENU_WIDTH, SDL_DINGUX_MENU_HEIGHT,
            rgb32 ? 32 : 16, surface_flags);

      if (unlikely(!vid->screen))
         RARCH_ERR("[SDL1]: Critical - Failed to init fallback SDL surface: %s\n", SDL_GetError());

      vid->mode_valid = false;
   }
   else
   {
      RARCH_LOG("[SDL1]: Video mode set to %ux%u (sanitized: %ux%u), pitch: %d\n",
            width, height, sanitized_width, sanitized_height, vid->screen->pitch);

      if ((sanitized_width != width) || (sanitized_height != height))
      {
         vid->frame_padding_x = (sanitized_width  - width)  >> 1;
         vid->frame_padding_y = (sanitized_height - height) >> 1;

         RARCH_LOG("[SDL1]: Padding: x=%u, y=%u (buffer is larger than requested)\n",
               vid->frame_padding_x, vid->frame_padding_y);

         if (SDL_MUSTLOCK(vid->screen))
            SDL_LockSurface(vid->screen);

         memset(vid->screen->pixels, 0, vid->screen->pitch * vid->screen->h);

         if (SDL_MUSTLOCK(vid->screen))
            SDL_UnlockSurface(vid->screen);
      }

      vid->mode_valid = true;
   }
}


static void sdl_dingux_blit_frame16(sdl_dingux_video_t *vid,
      uint16_t* src, unsigned width, unsigned height, unsigned src_pitch)
{
   unsigned dst_pitch = vid->screen->pitch;
   uint16_t *out_ptr  = (uint16_t*)(vid->screen->pixels + (vid->frame_padding_y * dst_pitch));
   
   /* CRITICAL: Use screen pitch, not calculated pitch */
   uint16_t out_stride = (uint16_t)(dst_pitch >> 1);
   uint16_t in_stride  = (uint16_t)(src_pitch >> 1);
   size_t y;

   out_ptr += vid->frame_padding_x;

   for (y = 0; y < height; y++)
   {
      memcpy(out_ptr, src, width * sizeof(uint16_t));
      src     += in_stride;
      out_ptr += out_stride;
   }
}

/* Optimized NEON 2x scaler - for both menu and games */
static void scale2x_n16_menu(void* __restrict src, void* __restrict dst, 
      uint32_t sw, uint32_t sh, uint32_t sp, uint32_t dp) {
   if (!sw||!sh) { return; }
   uint32_t swl = sw * sizeof(uint16_t);
   if (!sp) { sp = swl; } if (!dp) { dp = swl*2; }
   
   uint32_t swl64 = swl & ~63;
   uint32_t swrest = swl & 63;
   uint32_t sadd = sp - swl;
   uint32_t dadd = dp*2 - swl*2;
   uint8_t* finofs = (uint8_t*)src + (sp*sh);
   
   asm volatile (
   "1:   add lr, %0, %2      ;"  // lr  = x64bytes offset
   "   add r9, %0, %3      ;"  // r9  = lineend offset
   "   add r10, %1, %7      ;"  // r10 = 2x line offset
   "   cmp %0, lr      ;"
   "   beq 3f         ;"
   "2:   vldmia %0!, {q8-q11}   ;"  // 32 pixels 64 bytes
   "   vdup.16 d0, d23[3]   ;"
   "   vdup.16 d1, d23[2]   ;"
   "   vext.16 d31, d1,d0,#2   ;"
   "   vdup.16 d0, d23[1]   ;"
   "   vdup.16 d1, d23[0]   ;"
   "   vext.16 d30, d1,d0,#2   ;"
   "   vdup.16 d0, d22[3]   ;"
   "   vdup.16 d1, d22[2]   ;"
   "   vext.16 d29, d1,d0,#2   ;"
   "   vdup.16 d0, d22[1]   ;"
   "   vdup.16 d1, d22[0]   ;"
   "   vext.16 d28, d1,d0,#2   ;"
   "   vdup.16 d0, d21[3]   ;"
   "   vdup.16 d1, d21[2]   ;"
   "   vext.16 d27, d1,d0,#2   ;"
   "   vdup.16 d0, d21[1]   ;"
   "   vdup.16 d1, d21[0]   ;"
   "   vext.16 d26, d1,d0,#2   ;"
   "   vdup.16 d0, d20[3]   ;"
   "   vdup.16 d1, d20[2]   ;"
   "   vext.16 d25, d1,d0,#2   ;"
   "   vdup.16 d0, d20[1]   ;"
   "   vdup.16 d1, d20[0]   ;"
   "   vext.16 d24, d1,d0,#2   ;"
   "   vdup.16 d0, d19[3]   ;"
   "   vdup.16 d1, d19[2]   ;"
   "   vext.16 d23, d1,d0,#2   ;"
   "   vdup.16 d0, d19[1]   ;"
   "   vdup.16 d1, d19[0]   ;"
   "   vext.16 d22, d1,d0,#2   ;"
   "   vdup.16 d0, d18[3]   ;"
   "   vdup.16 d1, d18[2]   ;"
   "   vext.16 d21, d1,d0,#2   ;"
   "   vdup.16 d0, d18[1]   ;"
   "   vdup.16 d1, d18[0]   ;"
   "   vext.16 d20, d1,d0,#2   ;"
   "   vdup.16 d0, d17[3]   ;"
   "   vdup.16 d1, d17[2]   ;"
   "   vext.16 d19, d1,d0,#2   ;"
   "   vdup.16 d0, d17[1]   ;"
   "   vdup.16 d1, d17[0]   ;"
   "   vext.16 d18, d1,d0,#2   ;"
   "   vdup.16 d0, d16[3]   ;"
   "   vdup.16 d1, d16[2]   ;"
   "   vext.16 d17, d1,d0,#2   ;"
   "   vdup.16 d0, d16[1]   ;"
   "   vdup.16 d1, d16[0]   ;"
   "   vext.16 d16, d1,d0,#2   ;"
   "   cmp %0, lr      ;"
   "   vstmia %1!, {q8-q15}   ;"
   "   vstmia r10!, {q8-q15}   ;"
   "   bne 2b         ;"
   "3:   cmp %0, r9      ;"
   "   beq 5f         ;"
   "   tst %8, #32      ;"
   "   beq 4f         ;"
   "   vldmia %0!,{q8-q9}   ;"  // 16 pixels
   "   vdup.16 d0, d19[3]   ;"
   "   vdup.16 d1, d19[2]   ;"
   "   vext.16 d23, d1,d0,#2   ;"
   "   vdup.16 d0, d19[1]   ;"
   "   vdup.16 d1, d19[0]   ;"
   "   vext.16 d22, d1,d0,#2   ;"
   "   vdup.16 d0, d18[3]   ;"
   "   vdup.16 d1, d18[2]   ;"
   "   vext.16 d21, d1,d0,#2   ;"
   "   vdup.16 d0, d18[1]   ;"
   "   vdup.16 d1, d18[0]   ;"
   "   vext.16 d20, d1,d0,#2   ;"
   "   vdup.16 d0, d17[3]   ;"
   "   vdup.16 d1, d17[2]   ;"
   "   vext.16 d19, d1,d0,#2   ;"
   "   vdup.16 d0, d17[1]   ;"
   "   vdup.16 d1, d17[0]   ;"
   "   vext.16 d18, d1,d0,#2   ;"
   "   vdup.16 d0, d16[3]   ;"
   "   vdup.16 d1, d16[2]   ;"
   "   vext.16 d17, d1,d0,#2   ;"
   "   vdup.16 d0, d16[1]   ;"
   "   vdup.16 d1, d16[0]   ;"
   "   vext.16 d16, d1,d0,#2   ;"
   "   cmp %0, r9      ;"
   "   vstmia %1!, {q8-q11}   ;"
   "   vstmia r10!, {q8-q11}   ;"
   "   beq 5f         ;"
   "4:   ldrh lr, [%0],#2   ;"  // rest - load 16-bit pixel
   "   orr lr, lr, lr, lsl #16   ;"  // duplicate: 0000ABCD -> ABCDABCD
   "   cmp %0, r9      ;"
   "   str lr, [%1],#4      ;"
   "   str lr, [r10],#4   ;"
   "   bne 4b         ;"
   "5:   add %0, %0, %4      ;"
   "   add %1, %1, %5      ;"
   "   cmp %0, %6      ;"
   "   bne 1b         "
   : "+r"(src), "+r"(dst)
   : "r"(swl64), "r"(swl), "r"(sadd), "r"(dadd), "r"(finofs), "r"(dp), "r"(swrest)
   : "r9","r10","lr","q0","q8","q9","q10","q11","q12","q13","q14","q15","memory","cc"
   );
}

/* NEON 3x scaler - optimized from Miyoo Mini */
static void scale3x_n16_game(void* __restrict src, void* __restrict dst, 
      uint32_t sw, uint32_t sh, uint32_t sp, uint32_t dp) {
   if (!sw||!sh) { return; }
   uint32_t swl = sw * sizeof(uint16_t);
   if (!sp) { sp = swl; } if (!dp) { dp = swl*3; }
   
   uint32_t swl32 = swl & ~31;
   uint32_t sadd = sp - swl;
   uint32_t dadd = dp - swl*3;
   uint32_t dwl = swl*3;
   uint32_t dwl128 = dwl & ~127;
   uint32_t dwrest = dwl & 127;
   uint8_t* finofs = (uint8_t*)src + (sp*sh);
   
   asm volatile (
   "1:   mov r11,%1      ;"  // dst push
   "   add lr, %0, %2      ;"  // lr  = x32bytes offset
   "   add r10, %0, %3      ;"  // r10 = lineend offset
   "   cmp %0, lr      ;"
   "   beq 3f         ;"
   "2:   vldmia %0!, {q8-q9}   ;"  // 16 pixels 32 bytes
   "   vdup.16 d31, d19[3]   ;"  //  FFFF
   "   vdup.16 d30, d19[2]   ;"  //  EEEE
   "   vdup.16 d29, d19[1]   ;"  //  DDDD
   "   vdup.16 d28, d19[0]   ;"  //  CCCC
   "   vext.16 d27, d30,d31,#3   ;"  // EFFF
   "   vext.16 d26, d29,d30,#2   ;"  // DDEE
   "   vext.16 d25, d28,d29,#1   ;"  // CCCD
   "   vdup.16 d31, d18[3]   ;"  //  BBBB
   "   vdup.16 d30, d18[2]   ;"  //  AAAA
   "   vdup.16 d29, d18[1]   ;"  //  9999
   "   vdup.16 d28, d18[0]   ;"  //  8888
   "   vext.16 d24, d30,d31,#3   ;"  // ABBB
   "   vext.16 d23, d29,d30,#2   ;"  // 99AA
   "   vext.16 d22, d28,d29,#1   ;"  // 8889
   "   vdup.16 d31, d17[3]   ;"  //  7777
   "   vdup.16 d30, d17[2]   ;"  //  6666
   "   vdup.16 d29, d17[1]   ;"  //  5555
   "   vdup.16 d28, d17[0]   ;"  //  4444
   "   vext.16 d21, d30,d31,#3   ;"  // 6777
   "   vext.16 d20, d29,d30,#2   ;"  // 5566
   "   vext.16 d19, d28,d29,#1   ;"  // 4445
   "   vdup.16 d31, d16[3]   ;"  //  3333
   "   vdup.16 d30, d16[2]   ;"  //  2222
   "   vdup.16 d29, d16[1]   ;"  //  1111
   "   vdup.16 d28, d16[0]   ;"  //  0000
   "   vext.16 d18, d30,d31,#3   ;"  // 2333
   "   vext.16 d17, d29,d30,#2   ;"  // 1122
   "   vext.16 d16, d28,d29,#1   ;"  // 0001
   "   cmp %0, lr      ;"
   "   vstmia %1!, {q8-q13}   ;"
   "   bne 2b         ;"
   "3:   cmp %0, r10      ;"
   "   beq 5f         ;"
   "4:   ldrh lr, [%0],#2   ;"  // rest
   "   orr lr, lr, lr, lsl #16   ;"
   "   cmp %0, r10      ;"
   "   str lr, [%1],#4      ;"
   "   strh lr, [%1],#2   ;"
   "   bne 4b         ;"
   "5:   add %0, %4      ;"
   "   add %1, %5      ;"
   "   mov r12, %1      ;"  // r12 = 2x line offset
   "   add %1, %8      ;"  //
   "   add %1, %5      ;"  // %1 = 3x line offset
   "   add lr, r11, %7      ;"  // lr = x128bytes offset
   "   add r10, r11, %8   ;"  // r10 = lineend offset
   "   cmp r11, lr      ;"
   "   beq 7f         ;"
   "6:   vldmia r11!, {q8-q15}   ;"  // 64 pixels 128 bytes
   "   vstmia r12!, {q8-q15}   ;"
   "   vstmia %1!, {q8-q15}   ;"
   "   cmp r11, lr      ;"
   "   bne 6b         ;"
   "7:   cmp r11, r10      ;"
   "   beq 10f         ;"
   "   tst %9, #64      ;"
   "   beq 8f         ;"
   "   vldmia r11!, {q8-q11}   ;"  // 32 pixels
   "   vstmia r12!, {q8-q11}   ;"
   "   vstmia %1!, {q8-q11}   ;"
   "   cmp r11, r10      ;"
   "   beq 10f         ;"
   "8:   tst %9, #32      ;"
   "   beq 9f         ;"
   "   vldmia r11!, {q8-q9}   ;"  // 16 pixels
   "   vstmia r12!, {q8-q9}   ;"
   "   vstmia %1!, {q8-q9}   ;"
   "   cmp r11, r10      ;"
   "   beq 10f         ;"
   "9:   ldrh lr, [r11],#2   ;"  // rest
   "   strh lr, [r12],#2   ;"
   "   strh lr, [%1],#2   ;"
   "   cmp r11, r10      ;"
   "   bne 9b         ;"
   "10:   add %1, %5      ;"
   "   cmp %0, %6      ;"
   "   bne 1b         "
   : "+r"(src), "+r"(dst)
   : "r"(swl32), "r"(swl), "r"(sadd), "r"(dadd), "r"(finofs), "r"(dwl128), "r"(dwl), "r"(dwrest)
   : "r10","r11","r12","lr","q8","q9","q10","q11","q12","q13","q14","q15","memory","cc"
   );
}

/* NEON 4x scaler - optimized from Miyoo Mini */
static void scale4x_n16_game(void* __restrict src, void* __restrict dst, 
      uint32_t sw, uint32_t sh, uint32_t sp, uint32_t dp) {
   if (!sw||!sh) { return; }
   uint32_t swl = sw * sizeof(uint16_t);
   if (!sp) { sp = swl; } if (!dp) { dp = swl*4; }
   
   uint32_t swl32 = swl & ~31;
   uint32_t sadd = sp - swl;
   uint32_t dadd = dp*4 - swl*4;
   uint8_t* finofs = (uint8_t*)src + (sp*sh);
   
   asm volatile (
   "1:   add lr, %0, %2      ;"  // lr  = x32bytes offset
   "   add r8, %0, %3      ;"  // r8  = lineend offset
   "   add r9, %1, %7      ;"  // r9  = 2x line offset
   "   add r10, r9, %7      ;"  // r10 = 3x line offset
   "   add r11, r10, %7   ;"  // r11 = 4x line offset
   "   cmp %0, lr      ;"
   "   beq 3f         ;"
   "2:   vldmia %0!,{q8-q9}   ;"  // 16 pixels 32 bytes
   "   vdup.16 d31,d19[3]   ;"
   "   vdup.16 d30,d19[2]   ;"
   "   vdup.16 d29,d19[1]   ;"
   "   vdup.16 d28,d19[0]   ;"
   "   vdup.16 d27,d18[3]   ;"
   "   vdup.16 d26,d18[2]   ;"
   "   vdup.16 d25,d18[1]   ;"
   "   vdup.16 d24,d18[0]   ;"
   "   vdup.16 d23,d17[3]   ;"
   "   vdup.16 d22,d17[2]   ;"
   "   vdup.16 d21,d17[1]   ;"
   "   vdup.16 d20,d17[0]   ;"
   "   vdup.16 d19,d16[3]   ;"
   "   vdup.16 d18,d16[2]   ;"
   "   vdup.16 d17,d16[1]   ;"
   "   vdup.16 d16,d16[0]   ;"
   "   cmp %0, lr      ;"
   "   vstmia %1!,{q8-q15}   ;"
   "   vstmia r9!,{q8-q15}   ;"
   "   vstmia r10!,{q8-q15}   ;"
   "   vstmia r11!,{q8-q15}   ;"
   "   bne 2b         ;"
   "3:   cmp %0, r8      ;"
   "   beq 5f         ;"
   "4:   ldrh lr, [%0],#2   ;"  // rest
   "   vdup.16 d16, lr      ;"
   "   cmp %0, r8      ;"
   "   vstmia %1!, {d16}   ;"
   "   vstmia r9!, {d16}   ;"
   "   vstmia r10!, {d16}   ;"
   "   vstmia r11!, {d16}   ;"
   "   bne 4b         ;"
   "5:   add %0, %0, %4      ;"
   "   add %1, %1, %5      ;"
   "   cmp %0, %6      ;"
   "   bne 1b         "
   : "+r"(src), "+r"(dst)
   : "r"(swl32), "r"(swl), "r"(sadd), "r"(dadd), "r"(finofs), "r"(dp)
   : "r8","r9","r10","r11","lr","q8","q9","q10","q11","q12","q13","q14","q15","memory","cc"
   );
}

/* NEON-Optimized Universal Scaler - works for ANY scale factor!
 * 6-8x faster than pure C software scaler
 * Processes 8 pixels at once using ARM NEON SIMD */
static void scale_fullscreen_n16_menu(void* __restrict src, void* __restrict dst,
      unsigned src_width, unsigned src_height, unsigned dst_width, unsigned dst_height,
      unsigned src_pitch, unsigned dst_pitch)
{
   uint32_t scale_x_fp = (src_width << 16) / dst_width;
   uint32_t scale_y_fp = (src_height << 16) / dst_height;
   
   uint16_t *src_base = (uint16_t*)src;
   uint16_t *dst_base = (uint16_t*)dst;
   uint16_t src_stride = src_pitch >> 1;
   uint16_t dst_stride = dst_pitch >> 1;
   
   for (unsigned dst_y = 0; dst_y < dst_height; dst_y++)
   {
      unsigned src_y = (dst_y * scale_y_fp) >> 16;
      if (src_y >= src_height) src_y = src_height - 1;
      
      uint16_t *src_row = src_base + src_y * src_stride;
      uint16_t *dst_row = dst_base + dst_y * dst_stride;
      
      /* NEON vectorized loop - process 8 pixels at once */
      unsigned dst_x = 0;
      unsigned dst_w_vec = (dst_width >> 3) << 3; /* Round to multiple of 8 */
      
      for (; dst_x < dst_w_vec; dst_x += 8)
      {
         /* Calculate source positions for 8 destination pixels */
         uint16_t pixels[8];
         unsigned sx[8];
         
         /* Unrolled for performance */
         sx[0] = ((dst_x + 0) * scale_x_fp) >> 16;
         sx[1] = ((dst_x + 1) * scale_x_fp) >> 16;
         sx[2] = ((dst_x + 2) * scale_x_fp) >> 16;
         sx[3] = ((dst_x + 3) * scale_x_fp) >> 16;
         sx[4] = ((dst_x + 4) * scale_x_fp) >> 16;
         sx[5] = ((dst_x + 5) * scale_x_fp) >> 16;
         sx[6] = ((dst_x + 6) * scale_x_fp) >> 16;
         sx[7] = ((dst_x + 7) * scale_x_fp) >> 16;
         
         /* Clamp to source width */
         for (unsigned i = 0; i < 8; i++)
            if (sx[i] >= src_width) sx[i] = src_width - 1;
         
         /* Gather 8 source pixels */
         pixels[0] = src_row[sx[0]];
         pixels[1] = src_row[sx[1]];
         pixels[2] = src_row[sx[2]];
         pixels[3] = src_row[sx[3]];
         pixels[4] = src_row[sx[4]];
         pixels[5] = src_row[sx[5]];
         pixels[6] = src_row[sx[6]];
         pixels[7] = src_row[sx[7]];
         
         /* NEON vector store - 8 pixels in 1 instruction! */
         asm volatile (
            "vld1.16 {q0}, [%[src]]\n"
            "vst1.16 {q0}, [%[dst]]\n"
            :
            : [src]"r"(pixels), [dst]"r"(&dst_row[dst_x])
            : "q0", "memory"
         );
      }
      
      /* Handle remaining pixels (< 8) */
      for (; dst_x < dst_width; dst_x++)
      {
         unsigned src_x = (dst_x * scale_x_fp) >> 16;
         if (src_x >= src_width) src_x = src_width - 1;
         dst_row[dst_x] = src_row[src_x];
      }
   }
}

/* Smart game scaler - uses optimal scaling with NEON acceleration */
static void sdl_dingux_blit_game_scaled(sdl_dingux_video_t *vid,
      uint16_t* src, unsigned width, unsigned height, unsigned src_pitch)
{
   /* keep_aspect = true  -> Optimal scale to fill height (NEON-accelerated!)
    * keep_aspect = false -> Fullscreen stretch */
   
   memset(vid->screen->pixels, 0, vid->screen->pitch * vid->screen->h);
   
   if (vid->keep_aspect)
   {
      /* OPTIMAL SCALING - fill maximum height while preserving aspect ratio!
       * Examples:
       * - GameBoy 160x144: scale 3.33x -> 533x480 (fills height!)
       * - NES 256x240: scale 2.0x -> 512x480 (fills height!)
       * - Genesis 320x224: scale 2.14x -> 685x479 (fills height!) */
      
      float scale_x_max = (float)SDL_DINGUX_MENU_WIDTH / width;
      float scale_y_max = (float)SDL_DINGUX_MENU_HEIGHT / height;
      float scale_optimal = (scale_x_max < scale_y_max) ? scale_x_max : scale_y_max;
      
      unsigned scaled_w = ((unsigned)(width * scale_optimal) >> 1) << 1;
      unsigned scaled_h = ((unsigned)(height * scale_optimal) >> 1) << 1;
      unsigned offset_x = ((SDL_DINGUX_MENU_WIDTH - scaled_w) / 2) & ~1;
      unsigned offset_y = ((SDL_DINGUX_MENU_HEIGHT - scaled_h) / 2) & ~1;
      
      uint16_t *dst = (uint16_t*)vid->screen->pixels + 
            (offset_y * (vid->screen->pitch >> 1)) + offset_x;
      
      /* Use dedicated NEON scalers when scale is close to integer
       * OR use two-pass: NEON integer + small stretch for better performance */
      unsigned int_scale = (unsigned)scale_optimal;
      float scale_remainder = scale_optimal - int_scale;
      
      if (scale_remainder < 0.15f && int_scale >= 2 && int_scale <= 4)
      {
         /* Fast path: use dedicated NEON integer scalers */
         if (int_scale == 4)
            scale4x_n16_game(src, dst, width, height, src_pitch, vid->screen->pitch);
         else if (int_scale == 3)
            scale3x_n16_game(src, dst, width, height, src_pitch, vid->screen->pitch);
         else
            scale2x_n16_menu(src, dst, width, height, src_pitch, vid->screen->pitch);
      }
      else if (int_scale >= 2 && scale_remainder >= 0.15f && scale_remainder < 0.5f)
      {
         /* Two-pass scaling for better performance:
          * Step 1: NEON integer scale (2x/3x) - FAST!
          * Step 2: Small software stretch to final size - much fewer pixels!
          * 
          * Example SNES 256x224 with scale 2.14:
          *   Pass 1: NEON 2x -> 512x448 (fast!)
          *   Pass 2: Software 512x448 -> 685x479 (only ~50% more pixels, not 3.5x!) */
         
         /* Allocate temp buffer for intermediate result */
         unsigned temp_w = width * int_scale;
         unsigned temp_h = height * int_scale;
         uint16_t *temp_buffer = (uint16_t*)malloc(temp_w * temp_h * sizeof(uint16_t));
         
         if (temp_buffer)
         {
            /* Pass 1: NEON integer scale */
            if (int_scale == 3)
               scale3x_n16_game(src, temp_buffer, width, height, 
                     src_pitch, temp_w * sizeof(uint16_t));
            else
               scale2x_n16_menu(src, temp_buffer, width, height,
                     src_pitch, temp_w * sizeof(uint16_t));
            
            /* Pass 2: Software stretch temp -> final (much smaller operation!) */
            scale_fullscreen_n16_menu(temp_buffer, dst,
                  temp_w, temp_h, scaled_w, scaled_h,
                  temp_w * sizeof(uint16_t), vid->screen->pitch);
            
            free(temp_buffer);
         }
         else
         {
            /* Fallback if malloc fails */
            scale_fullscreen_n16_menu(src, dst, width, height,
                  scaled_w, scaled_h, src_pitch, vid->screen->pitch);
         }
      }
      else
      {
         /* Single-pass software scaler */
         scale_fullscreen_n16_menu(src, dst, width, height,
               scaled_w, scaled_h, src_pitch, vid->screen->pitch);
      }
   }
   else
   {
      /* Fullscreen stretch - uses NEON-optimized scaler */
      scale_fullscreen_n16_menu(src, vid->screen->pixels,
            width, height,
            SDL_DINGUX_MENU_WIDTH, SDL_DINGUX_MENU_HEIGHT,
            src_pitch, vid->screen->pitch);
   }
}

/* Rotate 90 degrees counter-clockwise: landscape input -> portrait output
 * Input: width x height (landscape, e.g., 256x240)
 * Output: height x width (portrait, e.g., 240x256) */
static void sdl_dingux_blit_frame32(sdl_dingux_video_t *vid,
      uint32_t* src, unsigned width, unsigned height, unsigned src_pitch)
{
   unsigned dst_pitch = vid->screen->pitch;
   uint32_t *out_ptr  = (uint32_t*)(vid->screen->pixels + (vid->frame_padding_y * dst_pitch));
   
   uint32_t out_stride = (uint32_t)(dst_pitch >> 2);
   uint32_t in_stride  = (uint32_t)(src_pitch >> 2);
   size_t y;

   out_ptr += vid->frame_padding_x;

   for (y = 0; y < height; y++)
   {
      memcpy(out_ptr, src, width * sizeof(uint32_t));
      src     += in_stride;
      out_ptr += out_stride;
   }
}

/* simple C  RGB32 → RGB565 conversion */
static void convert_rgb32_to_rgb565_neon(uint16_t *dst, const uint32_t *src, unsigned count)
{
//   for (unsigned i = 0; i < count; i++)
//   {
//      uint32_t pixel = src[i];
//      /* XRGB8888: 0xXXRRGGBB → RGB565: RRRRRGGGGGGBBBBB */
//      dst[i] = ((pixel >> 8) & 0xF800) |  /* R: bits 23-19 → 15-11 */
//               ((pixel >> 5) & 0x07E0) |  /* G: bits 15-10 → 10-5 */
//               ((pixel >> 3) & 0x001F);   /* B: bits 7-3 → 4-0 */
//   }
   unsigned vec_count = count >> 3;  /* 8 pixels at once */
   
   for (unsigned i = 0; i < vec_count; i++)
   {
      /* Load 8 XRGB8888 pixels (32 bytes) */
      asm volatile (
         "vld4.8 {d0,d1,d2,d3}, [%[src]]!\n"  /* Load B,G,R,X interleaved */
         "vshr.u8 d0, d0, #3\n"               /* B >> 3 (5 bits) */
         "vshr.u8 d1, d1, #2\n"               /* G >> 2 (6 bits) */
         "vshr.u8 d2, d2, #3\n"               /* R >> 3 (5 bits) */
         "vmovl.u8 q2, d0\n"                  /* Expand B to 16-bit */
         "vmovl.u8 q3, d1\n"                  /* Expand G to 16-bit */
         "vmovl.u8 q4, d2\n"                  /* Expand R to 16-bit */
         "vshl.u16 q3, q3, #5\n"              /* G << 5 */
         "vshl.u16 q4, q4, #11\n"             /* R << 11 */
         "vorr.u16 q2, q2, q3\n"              /* B | G */
         "vorr.u16 q2, q2, q4\n"              /* | R */
         "vst1.16 {q2}, [%[dst]]!\n"          /* Store 8 RGB565 pixels */
         : [src]"+r"(src), [dst]"+r"(dst)
         :
         : "d0","d1","d2","d3","q2","q3","q4","memory"
      );
   }
   
   /* Handle remaining pixels */
   for (unsigned i = vec_count * 8; i < count; i++)
   {
      uint32_t pixel = src[i];
      uint8_t r = (pixel >> 16) & 0xFF;
      uint8_t g = (pixel >> 8) & 0xFF;
      uint8_t b = pixel & 0xFF;
      dst[i] = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
   }
}

static bool sdl_dingux_gfx_frame(void *data, const void *frame,
      unsigned width, unsigned height, uint64_t frame_count,
      unsigned pitch, const char *msg, video_frame_info_t *video_info)
{
   sdl_dingux_video_t* vid = (sdl_dingux_video_t*)data;

   if (unlikely(!vid || (!frame && !vid->menu_active)))
      return true;

   if (unlikely(video_info->input_driver_nonblock_state))
   {
      retro_time_t current_time = cpu_features_get_time_usec();

      if ((current_time - vid->last_frame_time) < vid->ff_frame_time_min)
         return true;

      vid->last_frame_time = current_time;
   }

#ifdef HAVE_MENU
   menu_driver_frame(video_info->menu_is_alive, video_info);
#endif

   if (likely(!vid->menu_active))
   {
      if (unlikely(vid->was_in_menu || (vid->frame_width != width) || (vid->frame_height != height)))
      {
         /* Always use 854x480 surface for consistency */
         if (vid->was_in_menu || 
             (vid->frame_width != SDL_DINGUX_MENU_WIDTH) || 
             (vid->frame_height != SDL_DINGUX_MENU_HEIGHT))
         {
            sdl_dingux_set_output(vid, SDL_DINGUX_MENU_WIDTH, SDL_DINGUX_MENU_HEIGHT, false);
         }
         
         /* Configure IPU scaling based on settings */
         if (vid->was_in_menu)
            dingux_ipu_set_scaling_mode(vid->keep_aspect, vid->integer_scaling);
      }

      if (SDL_MUSTLOCK(vid->screen))
         SDL_LockSurface(vid->screen);

      if (likely(vid->mode_valid))
      {
         /* DEBUG: Log once per core to see scaling behavior */
         static bool logged = false;
         if (!logged)
         {
            RARCH_LOG("[SDL_DINGUX]: Game frame: %ux%u, Surface: %ux%u, integer_scaling=%d, rgb32=%d\n",
                  width, height, vid->screen->w, vid->screen->h, 
                  vid->integer_scaling, vid->rgb32);
            logged = true;
         }
         
         /* Choose rendering method based on integer_scaling setting */
         if (vid->integer_scaling)
         {
            /* In frame function */
            if (vid->rgb32)
            {
               size_t needed_size = width * height * 2;
               
               /* Allocate once, reuse */
               if (rgb32_convert_buffer_size < needed_size)
               {
                  free(rgb32_convert_buffer);
                  rgb32_convert_buffer = malloc(needed_size);
                  rgb32_convert_buffer_size = needed_size;
               }
               
               if (rgb32_convert_buffer)
               {
                  convert_rgb32_to_rgb565_neon(rgb32_convert_buffer, (uint32_t*)frame, width * height);
                  sdl_dingux_blit_game_scaled(vid, rgb32_convert_buffer, width, height, width*2);
               }
            }
            else
               sdl_dingux_blit_game_scaled(vid, (uint16_t*)frame, width, height, pitch);
         }
         else
         {
            /* No software scaling - render native and let IPU hardware stretch!
             * IPU is MUCH faster than software scaling */
            
            /* Clear screen and center the native resolution frame */
            memset(vid->screen->pixels, 0, vid->screen->pitch * vid->screen->h);
            
            unsigned offset_x = ((SDL_DINGUX_MENU_WIDTH - width) / 2) & ~1;
            unsigned offset_y = ((SDL_DINGUX_MENU_HEIGHT - height) / 2) & ~1;
            
            uint16_t *dst = (uint16_t*)vid->screen->pixels + 
                  (offset_y * (vid->screen->pitch >> 1)) + offset_x;
            
            /* Copy native resolution frame centered */
            uint16_t *src = (uint16_t*)frame;
            uint16_t src_stride = pitch >> 1;
            uint16_t dst_stride = vid->screen->pitch >> 1;
            
            for (unsigned y = 0; y < height; y++)
            {
               memcpy(dst, src, width * sizeof(uint16_t));
               src += src_stride;
               dst += dst_stride;
            }
         }
      }
      else
         sdl_dingux_blit_video_mode_error_msg(vid);

      vid->was_in_menu = false;
   }
   else
   {
      /* Menu is active - use same scaling logic as games! */
      if (!vid->was_in_menu)
      {
         /* Decide based on settings */
         if (vid->integer_scaling)
         {
            /* We handle scaling at 854x480 */
            if ((vid->frame_width != SDL_DINGUX_MENU_WIDTH) || (vid->frame_height != SDL_DINGUX_MENU_HEIGHT))
               sdl_dingux_set_output(vid, SDL_DINGUX_MENU_WIDTH, SDL_DINGUX_MENU_HEIGHT, false);
         }
         else
         {
            /* Native resolution - keep at 854x480 and center manually */
            if ((vid->frame_width != SDL_DINGUX_MENU_WIDTH) || (vid->frame_height != SDL_DINGUX_MENU_HEIGHT))
               sdl_dingux_set_output(vid, SDL_DINGUX_MENU_WIDTH, SDL_DINGUX_MENU_HEIGHT, false);
         }
         
         dingux_ipu_set_scaling_mode(vid->keep_aspect, vid->integer_scaling);
         vid->was_in_menu = true;
      }

      if (SDL_MUSTLOCK(vid->screen))
         SDL_LockSurface(vid->screen);

      /* Render menu */
      if (vid->menu_texture_width > 0 && vid->menu_texture_height > 0)
      {
         if (vid->integer_scaling)
         {
            /* Scaling enabled - use game scaler */
            sdl_dingux_blit_game_scaled(vid, vid->menu_texture,
                  vid->menu_texture_width, vid->menu_texture_height,
                  vid->menu_texture_width * sizeof(uint16_t));
         }
         else
         {
            /* No scaling - center menu manually at 854x480 */
            memset(vid->screen->pixels, 0, vid->screen->pitch * vid->screen->h);
            
            unsigned menu_w = vid->menu_texture_width;
            unsigned menu_h = vid->menu_texture_height;
            unsigned offset_x = ((SDL_DINGUX_MENU_WIDTH - menu_w) / 2) & ~1;
            unsigned offset_y = ((SDL_DINGUX_MENU_HEIGHT - menu_h) / 2) & ~1;
            
            uint16_t *dst = (uint16_t*)vid->screen->pixels + 
                  (offset_y * (vid->screen->pitch >> 1)) + offset_x;
            
            /* Copy menu texture centered */
            uint16_t *src = vid->menu_texture;
            uint16_t src_stride = menu_w;
            uint16_t dst_stride = vid->screen->pitch >> 1;
            
            for (unsigned y = 0; y < menu_h; y++)
            {
               memcpy(dst, src, menu_w * sizeof(uint16_t));
               src += src_stride;
               dst += dst_stride;
            }
         }
      }
   }

   if (msg)
   {
      if (vid->rgb32 && !vid->menu_active)
         sdl_dingux_blit_text32(vid, FONT_WIDTH_STRIDE,
               vid->screen->h - (FONT_HEIGHT + FONT_WIDTH_STRIDE), msg);
      else
         sdl_dingux_blit_text16(vid, FONT_WIDTH_STRIDE,
               vid->screen->h - (FONT_HEIGHT + FONT_WIDTH_STRIDE), msg);
   }

   if (SDL_MUSTLOCK(vid->screen))
      SDL_UnlockSurface(vid->screen);

#if ENABLE_FPS_LIMITER
   /* Frame rate limiter - force 60 FPS max when VSync not available
    * Prevents games running at 80-90 FPS with audio desync
    * Set ENABLE_FPS_LIMITER to 0 at top of file to disable */
   static retro_time_t last_flip_time = 0;
   const retro_time_t frame_time_60fps = 16666; /* 1000000 / 60 = 16666 microseconds */
   
   retro_time_t current_time = cpu_features_get_time_usec();
   
   if (last_flip_time > 0)
   {
      retro_time_t time_since_last = current_time - last_flip_time;
      
      if (time_since_last < frame_time_60fps)
      {
         /* Wait until 16.666ms has passed since last frame */
         usleep(frame_time_60fps - time_since_last);
      }
   }
   
   SDL_Flip(vid->screen);
   
   last_flip_time = cpu_features_get_time_usec();
#else
   /* FPS limiter disabled - let it run as fast as possible */
   SDL_Flip(vid->screen);
#endif

   return true;
}

static void sdl_dingux_set_texture_enable(void *data, bool state, bool full_screen)
{
   sdl_dingux_video_t *vid = (sdl_dingux_video_t*)data;
   if (unlikely(!vid))
      return;
   vid->menu_active = state;
}

static void sdl_dingux_set_texture_frame(void *data, const void *frame, bool rgb32,
      unsigned width, unsigned height, float alpha)
{
   sdl_dingux_video_t *vid = (sdl_dingux_video_t*)data;

   if (unlikely(!vid || rgb32 || (width > SDL_DINGUX_MENU_WIDTH) || (height > SDL_DINGUX_MENU_HEIGHT)))
      return;

   vid->menu_texture_width = width;
   vid->menu_texture_height = height;
   memcpy(vid->menu_texture, frame, width * height * sizeof(uint16_t));
}

static void sdl_dingux_gfx_set_nonblock_state(void *data, bool toggle,
      bool adaptive_vsync_enabled, unsigned swap_interval)
{
   sdl_dingux_video_t *vid = (sdl_dingux_video_t*)data;
   bool vsync              = !toggle;

   if (unlikely(!vid))
      return;

   if (vid->vsync != vsync)
   {
      unsigned current_width  = vid->frame_width;
      unsigned current_height = vid->frame_height;
      vid->vsync              = vsync;

      sdl_dingux_set_output(vid, current_width,
            (current_height > 4) ? (current_height - 2) : 16, vid->rgb32);
      sdl_dingux_set_output(vid, current_width, current_height, vid->rgb32);
   }
}

static void sdl_dingux_gfx_check_window(sdl_dingux_video_t *vid)
{
   SDL_Event event;
   SDL_PumpEvents();
   while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_QUITMASK))
   {
      if (event.type != SDL_QUIT)
         continue;
      vid->quitting = true;
      break;
   }
}

static bool sdl_dingux_gfx_alive(void *data)
{
   sdl_dingux_video_t *vid = (sdl_dingux_video_t*)data;
   if (unlikely(!vid))
      return false;
   sdl_dingux_gfx_check_window(vid);
   return !vid->quitting;
}

static bool sdl_dingux_gfx_focus(void *data)
{
   return true;
}

static bool sdl_dingux_gfx_suppress_screensaver(void *data, bool enable)
{
   return false;
}

static bool sdl_dingux_gfx_has_windowed(void *data)
{
   return false;
}

static void sdl_dingux_gfx_viewport_info(void *data, struct video_viewport *vp)
{
   sdl_dingux_video_t *vid = (sdl_dingux_video_t*)data;
   if (unlikely(!vid))
      return;

   vp->x      = 0;
   vp->y      = 0;
   vp->width  = vp->full_width  = vid->frame_width;
   vp->height = vp->full_height = vid->frame_height;
}

static float sdl_dingux_get_refresh_rate(void *data)
{
#if defined(DINGUX_BETA)
   sdl_dingux_video_t *vid = (sdl_dingux_video_t*)data;
   if (!vid)
      return 0.0f;

   switch (vid->refresh_rate)
   {
      case DINGUX_REFRESH_RATE_50HZ:
         return 50.0f;
      default:
         break;
   }
#endif
   return 60.0f;
}

static void sdl_dingux_set_filtering(void *data, unsigned index, bool smooth, bool ctx_scaling)
{
   sdl_dingux_video_t *vid                     = (sdl_dingux_video_t*)data;
   settings_t *settings                        = config_get_ptr();
   enum dingux_ipu_filter_type ipu_filter_type = (settings) ?
         (enum dingux_ipu_filter_type)settings->uints.video_dingux_ipu_filter_type :
         DINGUX_IPU_FILTER_BICUBIC;

   if (!vid || !settings)
      return;

   if (vid->filter_type != ipu_filter_type)
   {
      dingux_ipu_set_filter_type(ipu_filter_type);
      vid->filter_type = ipu_filter_type;
   }
}

static void sdl_dingux_apply_state_changes(void *data)
{
   sdl_dingux_video_t *vid  = (sdl_dingux_video_t*)data;
   settings_t *settings     = config_get_ptr();
   bool ipu_keep_aspect     = (settings) ? settings->bools.video_dingux_ipu_keep_aspect : true;
   bool ipu_integer_scaling = (settings) ? settings->bools.video_scale_integer : false;

   if (!vid || !settings)
      return;

   if ((vid->keep_aspect != ipu_keep_aspect) || (vid->integer_scaling != ipu_integer_scaling))
   {
      unsigned current_width  = vid->frame_width;
      unsigned current_height = vid->frame_height;
      unsigned screen_width   = vid->screen->w;
      unsigned screen_height  = vid->screen->h;
      unsigned sanitized_width;
      unsigned sanitized_height;

      dingux_ipu_set_scaling_mode(ipu_keep_aspect, ipu_integer_scaling);
      vid->keep_aspect     = ipu_keep_aspect;
      vid->integer_scaling = ipu_integer_scaling;

      sdl_dingux_sanitize_frame_dimensions(vid, current_width, current_height,
            &sanitized_width, &sanitized_height);

      if ((screen_width != sanitized_width) || (screen_height != sanitized_height))
         sdl_dingux_set_output(vid, current_width, current_height, vid->rgb32);
   }
}

static uint32_t sdl_dingux_get_flags(void *data)
{
   return 0;
}

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
{
   *iface = &sdl_dingux_poke_interface;
}

static bool sdl_dingux_gfx_set_shader(void *data, enum rarch_shader_type type, const char *path)
{
   return false;
}

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