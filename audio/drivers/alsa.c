/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2010-2014 - Hans-Kristian Arntzen
 *  Copyright (C) 2011-2017 - Daniel De Matteis
 *
 *  Basé sur le driver ALSA original S16 qui fonctionne
 *  Modifié pour : S32_LE + conversion float→S32 avec soft-clipping
 */

#include <stdlib.h>
#include <string.h>

#include <lists/string_list.h>
#include <string/stdstring.h>

#include <alsa/asoundlib.h>

#include "../audio_driver.h"
#include "../../verbosity.h"

///* ============ CONFIGURATION ============ */
//#define FLOAT_TO_S32_GAIN_PERCENT 85
//#define FLOAT_TO_S32_SCALE ((int32_t)(0x7FFFFFFF * (FLOAT_TO_S32_GAIN_PERCENT / 100.0f)))
//#define SOFT_CLIP_THRESHOLD 0.95f
//
///* Filtre passe-haut pour atténuer les basses */
//#define USE_BASS_FILTER 1
//#define BASS_FILTER_COEF 0.98f  /* Plus proche de 1.0 = coupe plus de basses (0.90-0.98) */
///* ======================================= */

#define FLOAT_TO_S32_GAIN_PERCENT 85
#define FLOAT_TO_S32_SCALE ((int32_t)(0x7FFFFFFF * (FLOAT_TO_S32_GAIN_PERCENT / 100.0f)))

/* Audio DSP settings */
#define ENABLE_HIGHPASS    0    /* Remove low rumble */
#define ENABLE_LOWPASS     0    /* Smooth harsh highs */
#define ENABLE_COMPRESSOR  0    /* Even out volume levels */
#define ENABLE_NORMALIZER  0    /* Boost quiet audio */
#define ENABLE_SOFTCLIP    0    /* Prevent hard clipping */

/* Filter parameters */
#define HIGHPASS_FREQ      300.0f   /* Hz - cut below this */
#define LOWPASS_FREQ       12000.0f /* Hz - cut above this */

/* Compressor settings */
#define COMP_THRESHOLD     0.4f    /* Start compressing above 60% */
#define COMP_RATIO         3.0f    /* 3:1 compression ratio */
#define COMP_ATTACK        0.01f  /* 1ms attack time */
#define COMP_RELEASE       0.1f    /* 100ms release time */

/* Normalizer */
#define NORM_TARGET        0.7f    /* Target 80% of max volume */
#define NORM_SPEED         0.1f   /* Adaptation speed */

#define M_PI            3.14159265358979323846

typedef struct alsa
{
   snd_pcm_t *pcm;
   size_t buffer_size;
   unsigned int frame_bits;
   bool nonblock;
   bool has_float;
   bool can_pause;
   bool is_paused;
   int32_t *conversion_buffer;
   size_t conversion_buffer_size;
      
   /* High-pass filter (1st order) */
   float hp_prev_in[2];
   float hp_prev_out[2];
   float hp_alpha;
    
   /* Low-pass filter (1st order) */
   float lp_prev_out[2];
   float lp_alpha;
    
   /* Compressor state */
   float comp_envelope;
    
   /* Normalizer state */
   float norm_gain;
   float norm_peak;
   unsigned sample_rate;
} alsa_t;


/* Initialize DSP */
static void audio_dsp_init(void *data, unsigned sample_rate)
{
    alsa_t *alsa = (alsa_t*)data;
    alsa->sample_rate = sample_rate;
    /* High-pass filter coefficient (1st order) */
    float hp_rc = 1.0f / (2.0f * M_PI * HIGHPASS_FREQ);
    float hp_dt = 1.0f / sample_rate;
    alsa->hp_alpha = hp_rc / (hp_rc + hp_dt);
    
    /* Low-pass filter coefficient (1st order) */
    float lp_rc = 1.0f / (2.0f * M_PI * LOWPASS_FREQ);
    float lp_dt = 1.0f / sample_rate;
    alsa->lp_alpha = lp_dt / (lp_rc + lp_dt);
    
    /* Compressor init */
    alsa->comp_envelope = 0.0f;
    
    /* Normalizer init */
    alsa->norm_gain = 1.0f;
    alsa->norm_peak = 0.0f;
    
    fprintf(stderr,"Audio DSP initialized:\n");
    fprintf(stderr,"  Activate %d High-pass: %.0f Hz (alpha=%.4f)\n", ENABLE_HIGHPASS, HIGHPASS_FREQ, alsa->hp_alpha);
    fprintf(stderr,"  Activate %d Low-pass: %.0f Hz (alpha=%.4f)\n", ENABLE_LOWPASS, LOWPASS_FREQ, alsa->lp_alpha);
    fprintf(stderr,"  Activate %d Compressor: %.1f:1 ratio, threshold=%.0f%%\n", ENABLE_COMPRESSOR, COMP_RATIO, COMP_THRESHOLD * 100);
    fprintf(stderr,"  Activate %d Normalizer: target=%.0f%%\n", ENABLE_NORMALIZER, NORM_TARGET * 100);
}

/* Soft clipping (tanh approximation for speed) */
static inline float soft_clip(float x)
{
    if (x > 1.0f) return 0.995f;
    if (x < -1.0f) return -0.995f;
    
    /* Fast tanh approximation */
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

/* Dynamic range compressor */
static float compress(float sample, float *envelope, unsigned sample_rate)
{
    /* Get absolute value */
    float abs_sample = fabsf(sample);
    
    /* Envelope follower (peak detector) */
    float attack_coef = expf(-1.0f / (COMP_ATTACK * sample_rate));
    float release_coef = expf(-1.0f / (COMP_RELEASE * sample_rate));
    
    if (abs_sample > *envelope)
        *envelope = attack_coef * (*envelope) + (1.0f - attack_coef) * abs_sample;
    else
        *envelope = release_coef * (*envelope) + (1.0f - release_coef) * abs_sample;
    
    /* Calculate gain reduction */
    float gain = 1.0f;
    if (*envelope > COMP_THRESHOLD)
    {
        /* Compress above threshold */
        float overshoot = *envelope - COMP_THRESHOLD;
        gain = COMP_THRESHOLD + overshoot / COMP_RATIO;
        gain = gain / *envelope;
    }
    
    return sample * gain;
}


static bool alsa_use_float(void *data)
{
   alsa_t *alsa = (alsa_t*)data;
   return alsa->has_float;
}

//static inline float soft_clip(float x)
//{
//   if (x > 1.0f) return 1.0f;
//   if (x < -1.0f) return -1.0f;
//   
//   if (x > SOFT_CLIP_THRESHOLD || x < -SOFT_CLIP_THRESHOLD)
//   {
//      float threshold = SOFT_CLIP_THRESHOLD;
//      
//      if (x > threshold)
//         return threshold + (x - threshold) * 0.5f;
//      else
//         return -threshold + (x + threshold) * 0.5f;
//   }
//   
//   return x;
//}
//
///* Filtre passe-haut simple (DC blocking + atténuation basses)
// * y[n] = coef * (y[n-1] + x[n] - x[n-1])
// * Plus coef proche de 1.0 = plus de basses coupées
// */
//static inline float high_pass_filter(float input, float *prev_in, float *prev_out, float coef)
//{
//   float output = coef * (*prev_out + input - *prev_in);
//   *prev_in = input;
//   *prev_out = output;
//   return output;
//}

static void *alsa_init(const char *device, unsigned rate, unsigned latency,
      unsigned block_frames,
      unsigned *new_rate)
{
   snd_pcm_format_t format;
   snd_pcm_uframes_t buffer_size;
   snd_pcm_hw_params_t *params    = NULL;
   snd_pcm_sw_params_t *sw_params = NULL;
   unsigned channels              = 2;
   unsigned orig_rate             = rate;
   const char *alsa_dev           = "default";
   alsa_t *alsa                   = (alsa_t*)calloc(1, sizeof(alsa_t));

   if (!alsa)
      return NULL;

   if (device)
      alsa_dev = device;

   RARCH_LOG("[ALSA S32]: Opening device: %s\n", alsa_dev);
   RARCH_LOG("[ALSA S32]: Float→S32 gain: %d%%\n", FLOAT_TO_S32_GAIN_PERCENT);

   if (snd_pcm_open(&alsa->pcm, alsa_dev, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK) < 0)
      goto error;

   if (snd_pcm_hw_params_malloc(&params) < 0)
      goto error;

   if (snd_pcm_hw_params_any(alsa->pcm, params) < 0)
      goto error;

   /* Force S32_LE - on accepte float en entrée */
   alsa->has_float = true;
   format = SND_PCM_FORMAT_S32_LE;

   if (snd_pcm_hw_params_set_access(alsa->pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED) < 0)
      goto error;

   /* S32 stereo = 64 bits per frame */
   alsa->frame_bits = snd_pcm_format_physical_width(format) * 2;

   if (snd_pcm_hw_params_set_format(alsa->pcm, params, format) < 0)
      goto error;

   if (snd_pcm_hw_params_set_channels(alsa->pcm, params, channels) < 0)
      goto error;

   snd_pcm_hw_params_set_rate_resample(alsa->pcm, params, 0);
   if (snd_pcm_hw_params_set_rate_near(alsa->pcm, params, &rate, 0) < 0)
      goto error;

   if (rate != orig_rate)
      *new_rate = rate;

   /* Force buffer comme dans ton driver qui marche */
   if (snd_pcm_hw_params_set_period_size(alsa->pcm, params, 768, NULL) < 0)
      goto error;
   if (snd_pcm_hw_params_set_buffer_size(alsa->pcm, params, 7680) < 0)
      goto error;

   if (snd_pcm_hw_params(alsa->pcm, params) < 0)
      goto error;
   snd_pcm_uframes_t period_size = 0;
   if (snd_pcm_hw_params_get_period_size(params, &period_size, NULL))
      snd_pcm_hw_params_get_period_size_min(params, &period_size, NULL);

   RARCH_LOG("[ALSA S32]: Period size: %d frames\n", (int)period_size);

   if (snd_pcm_hw_params_get_buffer_size(params, &buffer_size))
      snd_pcm_hw_params_get_buffer_size_max(params, &buffer_size);

   RARCH_LOG("[ALSA S32]: Buffer size: %d frames\n", (int)buffer_size);

   alsa->buffer_size = snd_pcm_frames_to_bytes(alsa->pcm, buffer_size);
   alsa->can_pause = snd_pcm_hw_params_can_pause(params);

   /* Alloue buffer conversion */
   alsa->conversion_buffer_size = alsa->buffer_size * 2;
   alsa->conversion_buffer = (int32_t*)calloc(1, alsa->conversion_buffer_size);
   
   if (!alsa->conversion_buffer)
   {
      RARCH_ERR("[ALSA S32]: Failed to allocate conversion buffer\n");
      goto error;
   }
   if (snd_pcm_sw_params_malloc(&sw_params) < 0)
      goto error;

   if (snd_pcm_sw_params_current(alsa->pcm, sw_params) < 0)
      goto error;

   if (snd_pcm_sw_params_set_start_threshold(alsa->pcm, sw_params, 768) < 0)
      goto error;
   if (snd_pcm_sw_params_set_avail_min(alsa->pcm, sw_params, 1) < 0)
      goto error;
   if (snd_pcm_sw_params(alsa->pcm, sw_params) < 0)
      goto error;

   /* Init filtre */
   audio_dsp_init(alsa, rate);
   snd_pcm_hw_params_free(params);
   snd_pcm_sw_params_free(sw_params);

   RARCH_LOG("[ALSA S32]: Initialization successful\n");

   return alsa;

error:
   RARCH_ERR("[ALSA S32]: Failed to initialize\n");
   if (params)
      snd_pcm_hw_params_free(params);
   if (sw_params)
      snd_pcm_sw_params_free(sw_params);
   if (alsa)
   {
      if (alsa->conversion_buffer)
         free(alsa->conversion_buffer);
      if (alsa->pcm)
      {
         snd_pcm_close(alsa->pcm);
         snd_config_update_free_global();
      }
      free(alsa);
   }
   return NULL;
}

#define BYTES_TO_FRAMES(bytes, frame_bits)  ((bytes) * 8 / frame_bits)
#define FRAMES_TO_BYTES(frames, frame_bits) ((frames) * frame_bits / 8)

static bool alsa_start(void *data, bool is_shutdown);

static ssize_t alsa_write(void *data, const void *buf_, size_t size_)
{
   alsa_t *alsa              = (alsa_t*)data;
   const uint8_t *buf        = (const uint8_t*)buf_;
   snd_pcm_sframes_t written = 0;
   snd_pcm_sframes_t size    = BYTES_TO_FRAMES(size_, alsa->frame_bits);
   size_t frames_size        = sizeof(int32_t); /* S32 */

   if (alsa->is_paused)
      if (!alsa_start(alsa, false))
         return -1;

#if 0         
//   /* Conversion FLOAT → S32 */
//   if (alsa->has_float)
//   {
//      size_t samples = size_ / sizeof(float);
//      const float *buf_float = (const float*)buf_;
//      size_t i;
//
//      if (samples * 4 > alsa->conversion_buffer_size)
//         return -1;
//
//      for (i = 0; i < samples; i++)
//      {
//         float sample = buf_float[i];
//         
//#if USE_BASS_FILTER
//         /* Applique filtre passe-haut (stéréo: i%2 = canal) */
//         int channel = i & 1;
//         sample = high_pass_filter(sample, 
//                                   &alsa->hp_prev_in[channel],
//                                   &alsa->hp_prev_out[channel],
//                                   BASS_FILTER_COEF);
//#endif
//         
//         /* Soft-clip */
//         sample = soft_clip(sample);
//         
//         /* Convertis en S32 */
//         alsa->conversion_buffer[i] = (int32_t)(sample * (float)FLOAT_TO_S32_SCALE);
//      }
#endif
/* Conversion FLOAT → S32 */
   if (alsa->has_float)
   {
      size_t samples = size_ / sizeof(float);
      const float *buf_float = (const float*)buf_;
      size_t i;

      if (samples * 4 > alsa->conversion_buffer_size)
         return -1;

      for (i = 0; i < samples; i++)
      {
        float sample = buf_float[i];
        int channel = i & 1;
#if ENABLE_HIGHPASS
        /* High-pass filter (DC removal + rumble cut) */
        float hp_out = alsa->hp_alpha * (alsa->hp_prev_out[channel] + sample - alsa->hp_prev_in[channel]);
        alsa->hp_prev_in[channel] = sample;
        alsa->hp_prev_out[channel] = hp_out;
        sample = hp_out;
#endif

#if ENABLE_LOWPASS
        /* Low-pass filter (smooth harsh highs) */
        alsa->lp_prev_out[channel] = alsa->lp_prev_out[channel] + alsa->lp_alpha * (sample - alsa->lp_prev_out[channel]);
        sample = alsa->lp_prev_out[channel];
#endif

#if ENABLE_COMPRESSOR
        /* Dynamic range compression */
        sample = compress(sample, &alsa->comp_envelope, alsa->sample_rate/2);
#endif

#if ENABLE_NORMALIZER
        /* Automatic gain control / normalization */
        float abs_sample = fabsf(sample);
        
        /* Track peak with decay */
        if (abs_sample > alsa->norm_peak)
            alsa->norm_peak = abs_sample;
        else
            alsa->norm_peak *= 0.9999f;  /* Slow decay */
        
        /* Adjust gain towards target */
        if (alsa->norm_peak > 0.01f)  /* Avoid division by zero */
        {
            float target_gain = NORM_TARGET / alsa->norm_peak;
            alsa->norm_gain += (target_gain - alsa->norm_gain) * NORM_SPEED;
            
            /* Limit gain range */
            if (alsa->norm_gain < 0.5f) alsa->norm_gain = 0.5f;
            if (alsa->norm_gain > 2.0f) alsa->norm_gain = 2.0f;
        }
        /* Après tous les filtres, avant conversion: */
         static int debug_counter = 0;
         if (++debug_counter % 48000 == 0)  // Toutes les secondes
         {
            fprintf(stderr, "DSP: gain=%.2f, peak=%.2f, envelope=%.2f\n",
                     alsa->norm_gain, alsa->norm_peak, alsa->comp_envelope);
         }
        sample *= alsa->norm_gain;
#endif

#if ENABLE_SOFTCLIP
        /* Soft clipping (prevent harsh distortion) */
        sample = soft_clip(sample);
#endif
        
        /* Convert to int32 */
        alsa->conversion_buffer[i] = (int32_t)(sample * (float)FLOAT_TO_S32_SCALE);
      }
      buf = (const uint8_t*)alsa->conversion_buffer;
      size_ = samples * 4;
      size = BYTES_TO_FRAMES(size_, alsa->frame_bits);

      static bool logged = false;
      if (!logged)
      {
         RARCH_LOG("[ALSA S32]: Float→S32 active, gain=%d%%", FLOAT_TO_S32_GAIN_PERCENT);
         RARCH_LOG("\n");
         logged = true;
      }
   }
 


   if (alsa->nonblock)
   {
      while (size)
      {
         snd_pcm_sframes_t frames = snd_pcm_writei(alsa->pcm, buf, size);

         if (frames == -EPIPE || frames == -EINTR || frames == -ESTRPIPE)
         {
            if (snd_pcm_recover(alsa->pcm, frames, 1) < 0)
               return -1;
            break;
         }
         else if (frames == -EAGAIN)
            break;
         else if (frames < 0)
            return -1;

         written += frames;
         buf     += (frames << 1) * frames_size;
         size    -= frames;
      }
   }
   else
   {
      bool eagain_retry = true;

      while (size)
      {
         snd_pcm_sframes_t frames;
         int rc = snd_pcm_wait(alsa->pcm, -1);

         if (rc == -EPIPE || rc == -ESTRPIPE || rc == -EINTR)
         {
            if (snd_pcm_recover(alsa->pcm, rc, 1) < 0)
               return -1;
            continue;
         }

         frames = snd_pcm_writei(alsa->pcm, buf, size);

         if (frames == -EPIPE || frames == -EINTR || frames == -ESTRPIPE)
         {
            if (snd_pcm_recover(alsa->pcm, frames, 1) < 0)
               return -1;
            break;
         }
         else if (frames == -EAGAIN)
         {
            if (eagain_retry)
            {
               eagain_retry = false;
               continue;
            }
            break;
         }
         else if (frames < 0)
            return -1;

         written += frames;
         buf     += (frames << 1) * frames_size;
         size    -= frames;
      }
   }

   return written;
}

static bool alsa_alive(void *data)
{
   alsa_t *alsa = (alsa_t*)data;
   if (!alsa)
      return false;
   return !alsa->is_paused;
}

static bool alsa_stop(void *data)
{
   alsa_t *alsa = (alsa_t*)data;
   if (alsa->is_paused)
      return true;

   if (alsa->can_pause && !alsa->is_paused)
   {
      int ret = snd_pcm_pause(alsa->pcm, 1);
      if (ret < 0)
         return false;
      alsa->is_paused = true;
   }
   return true;
}

static void alsa_set_nonblock_state(void *data, bool state)
{
   alsa_t *alsa = (alsa_t*)data;
   alsa->nonblock = state;
}

static bool alsa_start(void *data, bool is_shutdown)
{
   alsa_t *alsa = (alsa_t*)data;
   if (!alsa->is_paused)
      return true;

   if (alsa->can_pause && alsa->is_paused)
   {
      int ret = snd_pcm_pause(alsa->pcm, 0);
      if (ret < 0)
      {
         RARCH_ERR("[ALSA S32]: Failed to unpause\n");
         return false;
      }
      alsa->is_paused = false;
   }
   return true;
}

static void alsa_free(void *data)
{
   alsa_t *alsa = (alsa_t*)data;

   if (alsa)
   {
      if (alsa->conversion_buffer)
         free(alsa->conversion_buffer);
      
      if (alsa->pcm)
      {
         snd_pcm_drop(alsa->pcm);
         snd_pcm_close(alsa->pcm);
         snd_config_update_free_global();
      }
      free(alsa);
   }
}

static size_t alsa_write_avail(void *data)
{
   alsa_t *alsa            = (alsa_t*)data;
   snd_pcm_sframes_t avail = snd_pcm_avail(alsa->pcm);

   if (avail < 0)
      return alsa->buffer_size;

   return FRAMES_TO_BYTES(avail, alsa->frame_bits);
}

static size_t alsa_buffer_size(void *data)
{
   alsa_t *alsa = (alsa_t*)data;
   return alsa->buffer_size;
}

static void *alsa_device_list_new(void *data)
{
   void **hints, **n;
   union string_list_elem_attr attr;
   struct string_list *s = string_list_new();

   if (!s)
      return NULL;

   attr.i = 0;

   if (snd_device_name_hint(-1, "pcm", &hints) != 0)
      goto error;

   n = hints;

   while (*n)
   {
      char *name = snd_device_name_get_hint(*n, "NAME");
      char *io   = snd_device_name_get_hint(*n, "IOID");
      char *desc = snd_device_name_get_hint(*n, "DESC");

      if (!io || (string_is_equal(io, "Output")))
         string_list_append(s, name, attr);

      if (name)
         free(name);
      if (io)
         free(io);
      if (desc)
         free(desc);

      n++;
   }

   snd_device_name_free_hint(hints);
   return s;

error:
   string_list_free(s);
   return NULL;
}

static void alsa_device_list_free(void *data, void *array_list_data)
{
   struct string_list *s = (struct string_list*)array_list_data;

   if (!s)
      return;

   string_list_free(s);
}

audio_driver_t audio_alsa = {
   alsa_init,
   alsa_write,
   alsa_stop,
   alsa_start,
   alsa_alive,
   alsa_set_nonblock_state,
   alsa_free,
   alsa_use_float,
   "alsa",
   alsa_device_list_new,
   alsa_device_list_free,
   alsa_write_avail,
   alsa_buffer_size,
};
