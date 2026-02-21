/*  RetroArch - A frontend for libretro.
 *  Modified for Powkiddy X39 Pro - uses evdev (/dev/input/event*) instead of js*
 *  This version reads raw input events to capture ALL buttons including non-standard ones
 */
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
#include <stdio.h>

#include <sys/types.h>
#include <sys/inotify.h>
#include <linux/input.h>

#include <fcntl.h>
#include <sys/epoll.h>

#include <compat/strl.h>
#include <string/stdstring.h>

#include "../input_driver.h"

#include "../../verbosity.h"
#include "../../tasks/tasks_internal.h"

#define NUM_BUTTONS 32
#define NUM_AXES 32

/* Powkiddy X39 Pro button mapping - customize based on your evtest results */
#define EVDEV_BTN_A      158  /* BTN_SOUTH */
#define EVDEV_BTN_B      139  /* BTN_EAST */
#define EVDEV_BTN_X      308  /* BTN_NORTH */
#define EVDEV_BTN_Y      352  /* BTN_WEST */
#define EVDEV_BTN_L1     407  /* BTN_TL */
#define EVDEV_BTN_R1     412  /* BTN_TR */
#define EVDEV_BTN_L2     313  /* BTN_TL2 */
#define EVDEV_BTN_R2     312  /* BTN_TR2 */
#define EVDEV_BTN_SELECT 314  /* BTN_SELECT */
#define EVDEV_BTN_START  315  /* BTN_START */
#define EVDEV_BTN_MENU   174  /* BTN_THUMBL */
#define EVDEV_BTN_VOLUP  115  /* BTN_THUMBR */
#define EVDEV_BTN_VOLDOWN 114  /* BTN_THUMBR */
#define EVDEV_BTN_ON 116  /* BTN_THUMBR */



/* Map evdev button codes to RetroArch button indices (0-31) */
static int evdev_to_retroarch_button(int evdev_code)
{
   switch (evdev_code)
   {
      case EVDEV_BTN_A:      return RETRO_DEVICE_ID_JOYPAD_A;  /* RetroPad B */
      case EVDEV_BTN_B:      return RETRO_DEVICE_ID_JOYPAD_B;  /* RetroPad A */
      case EVDEV_BTN_X:      return RETRO_DEVICE_ID_JOYPAD_X;  /* RetroPad Y */
      case EVDEV_BTN_Y:      return RETRO_DEVICE_ID_JOYPAD_Y;  /* RetroPad X */
      case EVDEV_BTN_L1:     return RETRO_DEVICE_ID_JOYPAD_L;  /* L1 */
      case EVDEV_BTN_R1:     return RETRO_DEVICE_ID_JOYPAD_R;  /* R1 */
      case EVDEV_BTN_L2:     return RETRO_DEVICE_ID_JOYPAD_L2;  /* L2 */
      case EVDEV_BTN_R2:     return RETRO_DEVICE_ID_JOYPAD_R2;  /* R2 */
      case EVDEV_BTN_SELECT: return RETRO_DEVICE_ID_JOYPAD_SELECT;  /* Select */
      case EVDEV_BTN_START:  return RETRO_DEVICE_ID_JOYPAD_START;  /* Start */
      case EVDEV_BTN_MENU: return 14;  /* Select */
      case EVDEV_BTN_VOLUP:  return 11;  /* Start */
      case EVDEV_BTN_VOLDOWN:  return 12;  /* Start */
      case EVDEV_BTN_ON:  return 13;  /* Start */


      /* Add more mappings as needed */
      default:
         RARCH_WARN("[EVDEV]: Unknown button code %d\n", evdev_code);
         return -1;
   }
}


struct linuxraw_joypad
{
   int fd;
   uint32_t buttons;
   int16_t axes[NUM_AXES];
   char *ident;
      
   /* Axis calibration info */
   int32_t axis_min[NUM_AXES];
   int32_t axis_max[NUM_AXES];
   int32_t axis_center[NUM_AXES];
};

static struct linuxraw_joypad linuxraw_pads[MAX_USERS];
static int linuxraw_epoll                              = 0;
static int linuxraw_inotify                            = 0;
static bool linuxraw_hotplug                           = false;

static void linuxraw_poll_pad(struct linuxraw_joypad *pad)
{
   struct input_event event;

   while (read(pad->fd, &event, sizeof(event)) == (ssize_t)sizeof(event))
   {
      /* Handle button events */
      if (event.type == EV_KEY)
      {
         int btn_idx = evdev_to_retroarch_button(event.code);
         if (btn_idx >= 0 && btn_idx < NUM_BUTTONS)
         {
            if (event.value)
               BIT32_SET(linuxraw_pads[0].buttons, btn_idx);
            else
               BIT32_CLEAR(linuxraw_pads[0].buttons, btn_idx);
            
            RARCH_LOG("[EVDEV]: Button %d (evdev code %d) = %d\n", 
                  btn_idx, event.code, event.value);
         }
      }
      /* Handle axis events */
       /* Handle axis events */
      else if (event.type == EV_ABS)
      {
         
         if (event.code < NUM_AXES)
         {
            /* Get raw value */
            int32_t val = event.value;
            int32_t min = linuxraw_pads[0].axis_min[event.code];
            int32_t max = linuxraw_pads[0].axis_max[event.code];
            int32_t center = linuxraw_pads[0].axis_center[event.code];
            //joystick reports between -128 to +128 center 0
            //we scale to -0x7FFF to +0x7FFF
            int16_t scaled = (int16_t)((val * 32767) / 128);
            linuxraw_pads[0].axes[event.code] = scaled;
            
            /* Debug log */
            static int16_t last_scaled[NUM_AXES] = {0};
            if (abs(scaled - last_scaled[event.code]) > 5000)
            {
               RARCH_LOG("[EVDEV]: Axis %d raw=%d (converted from unsigned) -> scaled=%d\n",
                     event.code, val, scaled);
               last_scaled[event.code] = scaled;
            }
      
  //
  //          else
  //          {
  //             
  //             /* Standard calibration-based scaling */
  //             int16_t scaled;
  //             
  //             if (val < center)
  //             {
  //                /* Negative side */
  //                if (center != min)
  //                   scaled = (int16_t)(((int64_t)(val - center) * 32767) / (center - min));
  //                else
  //                   scaled = 0;
  //             }
  //             else
  //             {
  //                /* Positive side */
  //                if (max != center)
  //                   scaled = (int16_t)(((int64_t)(val - center) * 32767) / (max - center));
  //                else
  //                   scaled = 0;
  //             }
  //             
  //             linuxraw_pads[0].axes[event.code] = scaled;
  //             
  //             /* Debug log */
  //             static int16_t last_scaled_std[NUM_AXES] = {0};
  //             if (abs(scaled - last_scaled_std[event.code]) > 5000)
  //             {
  //                RARCH_LOG("[EVDEV]: Axis %d raw=%d (min=%d center=%d max=%d) -> scaled=%d\n",
  //                      event.code, val, min, center, max, scaled);
  //                last_scaled_std[event.code] = scaled;
  //             }
  //          }
         }
      }
   }
}

static bool linuxraw_joypad_init_pad(const char *path,
      struct linuxraw_joypad *pad)
{
   if (access(path, R_OK) < 0)
      return false;
   if (pad->fd >= 0)
      return false;

   pad->fd     = open(path, O_RDONLY | O_NONBLOCK);
   *pad->ident = '\0';

   if (pad->fd >= 0)
   {
      struct epoll_event event;
      char name[256] = {0};

      /* Get device name using evdev ioctl */
      if (ioctl(pad->fd, EVIOCGNAME(sizeof(name)), name) >= 0)
      {
         strlcpy(pad->ident, name, input_config_get_device_name_size(0));
         RARCH_LOG("[EVDEV]: Opened %s (%s)\n", path, name);
      }
   
      /* Read axis calibration info */
    /* Read axis calibration info */
      unsigned i;
      for (i = 0; i < NUM_AXES; i++)
      {
         struct input_absinfo absinfo;
         if (ioctl(pad->fd, EVIOCGABS(i), &absinfo) >= 0)
         {
            pad->axis_min[i] = absinfo.minimum;
            pad->axis_max[i] = absinfo.maximum;
            
            /* Detect center based on range type:
             * - 0-255 range: center is 0 (common on handhelds)
             * - Symmetric range (-32768 to 32767): center is 0
             * - Use flat (deadzone) if reported */
            
            if (absinfo.flat > 0)
            {
               pad->axis_center[i] = absinfo.flat;
            }
            else if (absinfo.minimum == 0 && absinfo.maximum == 255)
            {
               /* 0-255 joystick - center at 0, not 128! */
               pad->axis_center[i] = 0;
               RARCH_LOG("[EVDEV]: Axis %d: 0-255 range detected, center=0\n", i);
            }
            else
            {
               /* Standard symmetric range */
               pad->axis_center[i] = (absinfo.minimum + absinfo.maximum) / 2;
            }
            
            /* Log axis info */
            if (absinfo.maximum > absinfo.minimum)
            {
               RARCH_LOG("[EVDEV]: Axis %d: min=%d max=%d center=%d flat=%d fuzz=%d\n",
                     i, absinfo.minimum, absinfo.maximum, 
                     pad->axis_center[i], absinfo.flat, absinfo.fuzz);
            }
         }
         else
         {
            /* Default calibration if ioctl fails */
            pad->axis_min[i] = 0;
            pad->axis_max[i] = 255;
            pad->axis_center[i] = 0;  /* Changed from 128 to 0! */
         }
      }
      event.events             = EPOLLIN;
      event.data.ptr           = pad;

      if (epoll_ctl(linuxraw_epoll, EPOLL_CTL_ADD, pad->fd, &event) >= 0)
         return true;
      
      close(pad->fd);
      pad->fd = -1;
   }

   return false;
}

static const char *linuxraw_joypad_name(unsigned pad)
{
   if (pad >= MAX_USERS || string_is_empty(linuxraw_pads[pad].ident))
      return NULL;

   return linuxraw_pads[pad].ident;
}

static void linuxraw_joypad_poll(void)
{
   int i, ret;
   struct epoll_event events[MAX_USERS + 1];

retry:
   ret = epoll_wait(linuxraw_epoll, events, MAX_USERS + 1, 0);
   if (ret < 0 && errno == EINTR)
      goto retry;

   for (i = 0; i < ret; i++)
   {
      struct linuxraw_joypad *ptr = (struct linuxraw_joypad*)events[i].data.ptr;

      if (ptr)
         linuxraw_poll_pad(ptr);
      else
      {
         /* Handle hotplug events */
         int j, rc;
         size_t event_size  = sizeof(struct inotify_event) + NAME_MAX + 1;
         uint8_t *event_buf = (uint8_t*)calloc(1, event_size);

         while ((rc = read(linuxraw_inotify, event_buf, event_size)) >= 0)
         {
            struct inotify_event *ievent = (struct inotify_event*)&event_buf[0];

            event_buf[rc-1] = '\0';

            for (j = 0; j < rc; j += ievent->len + sizeof(struct inotify_event))
            {
               unsigned idx;

               ievent = (struct inotify_event*)&event_buf[j];

               /* Look for event* devices instead of js* */
               if (strstr(ievent->name, "event") != ievent->name)
                  continue;

               idx = strtoul(ievent->name + 5, NULL, 0);
               if (idx >= MAX_USERS)
                  continue;

               if (ievent->mask & IN_DELETE)
               {
                  if (linuxraw_pads[idx].fd >= 0)
                  {
                     if (linuxraw_hotplug)
                        input_autoconfigure_disconnect(idx,
                              linuxraw_pads[idx].ident);

                     close(linuxraw_pads[idx].fd);
                     linuxraw_pads[idx].buttons = 0;
                     memset(linuxraw_pads[idx].axes, 0,
                           sizeof(linuxraw_pads[idx].axes));
                     linuxraw_pads[idx].fd = -1;
                     *linuxraw_pads[idx].ident = '\0';

                     input_autoconfigure_connect(
                           NULL,
                           NULL,
                           linuxraw_joypad_name(idx),
                           idx,
                           0,
                           0);
                  }
               }
               else if (ievent->mask & (IN_CREATE | IN_ATTRIB))
               {
                  char path[PATH_MAX_LENGTH];

                  path[0] = '\0';

                  snprintf(path, sizeof(path), "/dev/input/%s", ievent->name);

                  if (     !string_is_empty(linuxraw_pads[idx].ident)
                        && linuxraw_joypad_init_pad(path, &linuxraw_pads[idx]))
                     input_autoconfigure_connect(
                           linuxraw_pads[idx].ident,
                           NULL,
                           linuxraw_joypad.ident,
                           idx,
                           0,
                           0);
               }
            }
         }

         free(event_buf);
      }
   }
}

static void *linuxraw_joypad_init(void *data)
{
   unsigned i;
   int fd = epoll_create(32);

   if (fd < 0)
      return NULL;

   linuxraw_epoll = fd;

   for (i = 0; i < MAX_USERS; i++)
   {
      char path[PATH_MAX_LENGTH];
      struct linuxraw_joypad *pad = (struct linuxraw_joypad*)&linuxraw_pads[i];

      path[0]                     = '\0';

      pad->fd                     = -1;
      pad->ident                  = input_config_get_device_name_ptr(i);

      /* Try event devices instead of js devices */
      snprintf(path, sizeof(path), "/dev/input/event%u", i);

      input_autoconfigure_connect(
            pad->ident,
            NULL,
            "linuxraw",
            i,
            0,
            0);

      if (linuxraw_joypad_init_pad(path, pad))
         linuxraw_poll_pad(pad);
   }

   linuxraw_inotify = inotify_init();

   if (linuxraw_inotify >= 0)
   {
      struct epoll_event event;

      fcntl(linuxraw_inotify, F_SETFL, fcntl(linuxraw_inotify, F_GETFL) | O_NONBLOCK);
      inotify_add_watch(linuxraw_inotify, "/dev/input", IN_DELETE | IN_CREATE | IN_ATTRIB);

      event.events             = EPOLLIN;
      event.data.ptr           = NULL;

      if (epoll_ctl(linuxraw_epoll, EPOLL_CTL_ADD, linuxraw_inotify, &event) < 0)
      {
         RARCH_ERR("Failed to add FD (%d) to epoll list (%s).\n",
               linuxraw_inotify, strerror(errno));
      }
   }

   linuxraw_hotplug = true;

   return (void*)-1;
}

static void linuxraw_joypad_destroy(void)
{
   unsigned i;

   for (i = 0; i < MAX_USERS; i++)
   {
      if (linuxraw_pads[i].fd >= 0)
         close(linuxraw_pads[i].fd);
   }

   memset(linuxraw_pads, 0, sizeof(linuxraw_pads));

   for (i = 0; i < MAX_USERS; i++)
      linuxraw_pads[i].fd = -1;

   if (linuxraw_inotify >= 0)
      close(linuxraw_inotify);
   linuxraw_inotify = -1;

   if (linuxraw_epoll >= 0)
      close(linuxraw_epoll);
   linuxraw_epoll = -1;

   linuxraw_hotplug = false;
}

static int32_t linuxraw_joypad_button(unsigned port, uint16_t joykey)
{
   const struct linuxraw_joypad    *pad = (const struct linuxraw_joypad*)
      &linuxraw_pads[port];
   if (port >= DEFAULT_MAX_PADS)
      return 0;
   if (joykey < NUM_BUTTONS)
      return (BIT32_GET(pad->buttons, joykey));
   return 0;
}

static void linuxraw_joypad_get_buttons(unsigned port, input_bits_t *state)
{
	const struct linuxraw_joypad *pad = (const struct linuxraw_joypad*)
      &linuxraw_pads[port];

	if (pad)
   {
		BITS_COPY16_PTR(state, pad->buttons);
	}
   else
		BIT256_CLEAR_ALL_PTR(state);
}

static int16_t linuxraw_joypad_axis_state(
      const struct linuxraw_joypad *pad,
      unsigned port, uint32_t joyaxis)
{
   if (AXIS_NEG_GET(joyaxis) < NUM_AXES)
   {
      int16_t val = pad->axes[AXIS_NEG_GET(joyaxis)];
      if (val < 0)
         return val;
   }
   else if (AXIS_POS_GET(joyaxis) < NUM_AXES)
   {
      int16_t val = pad->axes[AXIS_POS_GET(joyaxis)];
      if (val > 0)
         return val;
   }
   return 0;
}

static int16_t linuxraw_joypad_axis(unsigned port, uint32_t joyaxis)
{
   const struct linuxraw_joypad *pad = (const struct linuxraw_joypad*)
      &linuxraw_pads[0];
   return linuxraw_joypad_axis_state(pad, 0, joyaxis);
}

static int16_t linuxraw_joypad_state(
      rarch_joypad_info_t *joypad_info,
      const struct retro_keybind *binds,
      unsigned port)
{
   unsigned i;
   int16_t ret                          = 0;
   uint16_t port_idx                    = joypad_info->joy_idx;
   const struct linuxraw_joypad    *pad = (const struct linuxraw_joypad*)
      &linuxraw_pads[port_idx];

   if (port_idx >= DEFAULT_MAX_PADS)
      return 0;

   for (i = 0; i < RARCH_FIRST_CUSTOM_BIND; i++)
   {
      const uint64_t joykey  = (binds[i].joykey != NO_BTN)
         ? binds[i].joykey  : joypad_info->auto_binds[i].joykey;
      const uint32_t joyaxis = (binds[i].joyaxis != AXIS_NONE)
         ? binds[i].joyaxis : joypad_info->auto_binds[i].joyaxis;
      if ((uint16_t)joykey != NO_BTN && 
            (joykey < NUM_BUTTONS)   &&
            (BIT32_GET(pad->buttons, joykey)))
         ret |= ( 1 << i);
      else if (joyaxis != AXIS_NONE &&
            ((float)abs(linuxraw_joypad_axis_state(pad, port_idx, joyaxis)) 
             / 0x8000) > joypad_info->axis_threshold)
         ret |= (1 << i);
   }

   return ret;
}

static bool linuxraw_joypad_query_pad(unsigned pad)
{
   return pad < MAX_USERS && linuxraw_pads[pad].fd >= 0;
}

input_device_driver_t linuxraw_joypad = {
   linuxraw_joypad_init,
   linuxraw_joypad_query_pad,
   linuxraw_joypad_destroy,
   linuxraw_joypad_button,
   linuxraw_joypad_state,
   linuxraw_joypad_get_buttons,
   linuxraw_joypad_axis,
   linuxraw_joypad_poll,
   NULL,
   NULL,
   linuxraw_joypad_name,
   "linuxraw",
};