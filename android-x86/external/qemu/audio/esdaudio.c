/*
 * QEMU ESD audio driver
 *
 * Copyright (c) 2008 The Android Open Source Project
 * Copyright (c) 2006 Frederick Reeve (brushed up by malc)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <esd.h>
#include "audio.h"
#include <signal.h>

#define AUDIO_CAP "esd"
#include "audio_int.h"
#include "audio_pt_int.h"
#include <dlfcn.h>

#include "qemu_debug.h"

#define  DEBUG  1

#if DEBUG
#  include <stdio.h>
#  define D(...)  VERBOSE_PRINT(audio,__VA_ARGS__)
#  define D_ACTIVE  VERBOSE_CHECK(audio)
#  define O(...)  VERBOSE_PRINT(audioout,__VA_ARGS__)
#  define I(...)  VERBOSE_PRINT(audioin,__VA_ARGS__)
#else
#  define D(...)  ((void)0)
#  define D_ACTIVE 0
#  define O(...)  ((void)0)
#  define I(...)  ((void)0)
#endif

#define  STRINGIFY_(x)  #x
#define  STRINGIFY(x)   STRINGIFY_(x)

typedef struct {
    HWVoiceOut hw;
    int done;
    int live;
    int decr;
    int rpos;
    void *pcm_buf;
    int fd;
    struct audio_pt pt;
} ESDVoiceOut;

typedef struct {
    HWVoiceIn hw;
    int done;
    int dead;
    int incr;
    int wpos;
    void *pcm_buf;
    int fd;
    struct audio_pt pt;
} ESDVoiceIn;

static struct {
    int samples;
    int divisor;
    char *dac_host;
    char *adc_host;
} conf = {
    1024,
    2,
    NULL,
    NULL
};

/* link dynamically to the libesd.so */

#define  DYNLINK_FUNCTIONS   \
    DYNLINK_FUNC(int,esd_play_stream,(esd_format_t,int,const char*,const char*))   \
    DYNLINK_FUNC(int,esd_record_stream,(esd_format_t,int,const char*,const char*)) \
    DYNLINK_FUNC(int,esd_open_sound,( const char *host )) \
    DYNLINK_FUNC(int,esd_close,(int)) \

#define  DYNLINK_FUNCTIONS_INIT \
    esd_dynlink_init

#include "dynlink.h"

static void*    esd_lib;

static void GCC_FMT_ATTR (2, 3) qesd_logerr (int err, const char *fmt, ...)
{
    va_list ap;

    va_start (ap, fmt);
    AUD_vlog (AUDIO_CAP, fmt, ap);
    va_end (ap);

    AUD_log (AUDIO_CAP, "Reason: %s\n", strerror (err));
}

/* playback */
static void *qesd_thread_out (void *arg)
{
    ESDVoiceOut* esd = arg;
    HWVoiceOut*  hw  = &esd->hw;
    int threshold;
    sigset_t  set;

    threshold = conf.divisor ? hw->samples / conf.divisor : 0;

    if (audio_pt_lock (&esd->pt, AUDIO_FUNC)) {
        return NULL;
    }

    /* ignore SIGALRM in this thread */
    sigemptyset(&set);
    sigaddset(&set, SIGALRM);

    pthread_sigmask( SIG_BLOCK, &set, NULL);

    O("EsounD output thread starting, threshold is %d samples", threshold);
    for (;;) {
        int decr, to_mix, rpos;

        for (;;) {
            if (esd->done) {
                goto exit;
            }

            if (esd->live > threshold) {
                break;
            }

            if (audio_pt_wait (&esd->pt, AUDIO_FUNC)) {
                O("EsounD output thread aborting on wait error");
                goto exit;
            }
        }

        decr = to_mix = esd->live;
        rpos = hw->rpos;

        if (audio_pt_unlock (&esd->pt, AUDIO_FUNC)) {
            O("EsounD output thread aborting on unlock error");
            return NULL;
        }

        while (to_mix) {
            ssize_t written;
            int chunk = audio_MIN (to_mix, hw->samples - rpos);
            st_sample_t *src = hw->mix_buf + rpos;

            hw->clip (esd->pcm_buf, src, chunk);

        again:
            written = write (esd->fd, esd->pcm_buf, chunk << hw->info.shift);
            if (written == -1) {
                if (errno == EINTR || errno == EAGAIN) {
                    goto again;
                }
                qesd_logerr (errno, "write failed\n");
                O("EsounD output thread aborting on write error: %s", strerror(errno));
                return NULL;
            }

            if (written != chunk << hw->info.shift) {
                int wsamples = written >> hw->info.shift;
                int wbytes = wsamples << hw->info.shift;
                if (wbytes != written) {
                    dolog ("warning: Misaligned write %d (requested %d), "
                           "alignment %d\n",
                           wbytes, written, hw->info.align + 1);
                }
                to_mix -= wsamples;
                rpos = (rpos + wsamples) % hw->samples;
                break;
            }

            rpos = (rpos + chunk) % hw->samples;
            to_mix -= chunk;
        }

        if (audio_pt_lock (&esd->pt, AUDIO_FUNC)) {
            O("EsounD output thread aborting on lock error\n");
            return NULL;
        }

        esd->rpos = rpos;
        esd->live -= decr;
        esd->decr += decr;
    }
    O("EsounD output thread exiting");

 exit:
    audio_pt_unlock (&esd->pt, AUDIO_FUNC);
    return NULL;
}

static int qesd_run_out (HWVoiceOut *hw)
{
    int live, decr;
    ESDVoiceOut *esd = (ESDVoiceOut *) hw;

    if (audio_pt_lock (&esd->pt, AUDIO_FUNC)) {
        O("%s: exiting on lock error", __FUNCTION__);
        return 0;
    }

    live = audio_pcm_hw_get_live_out (hw);
    decr = audio_MIN (live, esd->decr);
    esd->decr -= decr;
    esd->live = live - decr;
    hw->rpos = esd->rpos;
    if (esd->live > 0) {
        O("%s: signaling %d samples\n", esd->live);
        audio_pt_unlock_and_signal (&esd->pt, AUDIO_FUNC);
    }
    else {
        O(".");
        audio_pt_unlock (&esd->pt, AUDIO_FUNC);
    }
    return decr;
}

static int qesd_write (SWVoiceOut *sw, void *buf, int len)
{
    return audio_pcm_sw_write (sw, buf, len);
}

static int qesd_init_out (HWVoiceOut *hw, audsettings_t *as)
{
    ESDVoiceOut *esd = (ESDVoiceOut *) hw;
    audsettings_t obt_as = *as;
    int esdfmt = ESD_STREAM | ESD_PLAY;
    int result = -1;

    /* shut down verbose debug spew */
    if (!D_ACTIVE)
        stdio_disable();

    O("initializing EsoundD audio output");
    esdfmt |= (as->nchannels == 2) ? ESD_STEREO : ESD_MONO;
    switch (as->fmt) {
    case AUD_FMT_S8:
    case AUD_FMT_U8:
        esdfmt |= ESD_BITS8;
        obt_as.fmt = AUD_FMT_U8;
        break;
#if 0
    case AUD_FMT_S32:
    case AUD_FMT_U32:
        dolog ("Will use 16 instead of 32 bit samples\n");
#endif
    case AUD_FMT_S16:
    case AUD_FMT_U16:
    deffmt:
        esdfmt |= ESD_BITS16;
        obt_as.fmt = AUD_FMT_S16;
        break;

    default:
        dolog ("Internal logic error: Bad audio format %d\n", as->fmt);
        goto deffmt;

    }
    obt_as.endianness = AUDIO_HOST_ENDIANNESS;

    audio_pcm_init_info (&hw->info, &obt_as);

    hw->samples = conf.samples;
    esd->pcm_buf = audio_calloc (AUDIO_FUNC, hw->samples, 1 << hw->info.shift);
    if (!esd->pcm_buf) {
        dolog ("Could not allocate buffer (%d bytes)\n",
               hw->samples << hw->info.shift);
        goto exit;
    }

    esd->fd = FF(esd_play_stream) (esdfmt, as->freq, conf.dac_host, NULL);
    if (esd->fd < 0) {
        if (conf.dac_host == NULL) {
            esd->fd = FF(esd_play_stream) (esdfmt, as->freq, "localhost", NULL);
        }
        if (esd->fd < 0) {
            qesd_logerr (errno, "esd_play_stream failed\n");
            goto fail2;
        }
    }

    if (audio_pt_init (&esd->pt, qesd_thread_out, esd, AUDIO_CAP, AUDIO_FUNC)) {
        goto fail3;
    }

    result = 0;  /* success */
    goto exit;

 fail3:
    if (close (esd->fd)) {
        qesd_logerr (errno, "%s: close on esd socket(%d) failed\n",
                     AUDIO_FUNC, esd->fd);
    }
    esd->fd = -1;

 fail2:
    qemu_free (esd->pcm_buf);
    esd->pcm_buf = NULL;

 exit:
    if (!D_ACTIVE)
        stdio_enable();

    return result;
}

static void qesd_fini_out (HWVoiceOut *hw)
{
    void *ret;
    ESDVoiceOut *esd = (ESDVoiceOut *) hw;

    audio_pt_lock (&esd->pt, AUDIO_FUNC);
    esd->done = 1;
    audio_pt_unlock_and_signal (&esd->pt, AUDIO_FUNC);
    audio_pt_join (&esd->pt, &ret, AUDIO_FUNC);

    if (esd->fd >= 0) {
        if (close (esd->fd)) {
            qesd_logerr (errno, "failed to close esd socket\n");
        }
        esd->fd = -1;
    }

    audio_pt_fini (&esd->pt, AUDIO_FUNC);

    qemu_free (esd->pcm_buf);
    esd->pcm_buf = NULL;
}

static int qesd_ctl_out (HWVoiceOut *hw, int cmd, ...)
{
    (void) hw;
    (void) cmd;
    return 0;
}

/* capture */
static void *qesd_thread_in (void *arg)
{
    ESDVoiceIn *esd = arg;
    HWVoiceIn *hw = &esd->hw;
    int threshold;

    threshold = conf.divisor ? hw->samples / conf.divisor : 0;

    if (audio_pt_lock (&esd->pt, AUDIO_FUNC)) {
        return NULL;
    }

    for (;;) {
        int incr, to_grab, wpos;

        for (;;) {
            if (esd->done) {
                goto exit;
            }

            if (esd->dead > threshold) {
                break;
            }

            if (audio_pt_wait (&esd->pt, AUDIO_FUNC)) {
                goto exit;
            }
        }

        incr = to_grab = esd->dead;
        wpos = hw->wpos;

        if (audio_pt_unlock (&esd->pt, AUDIO_FUNC)) {
            return NULL;
        }

        while (to_grab) {
            ssize_t nread;
            int chunk = audio_MIN (to_grab, hw->samples - wpos);
            void *buf = advance (esd->pcm_buf, wpos);

        again:
            nread = read (esd->fd, buf, chunk << hw->info.shift);
            if (nread == -1) {
                if (errno == EINTR || errno == EAGAIN) {
                    goto again;
                }
                qesd_logerr (errno, "read failed\n");
                return NULL;
            }

            if (nread != chunk << hw->info.shift) {
                int rsamples = nread >> hw->info.shift;
                int rbytes = rsamples << hw->info.shift;
                if (rbytes != nread) {
                    dolog ("warning: Misaligned write %d (requested %d), "
                           "alignment %d\n",
                           rbytes, nread, hw->info.align + 1);
                }
                to_grab -= rsamples;
                wpos = (wpos + rsamples) % hw->samples;
                break;
            }

            hw->conv (hw->conv_buf + wpos, buf, nread >> hw->info.shift,
                      &nominal_volume);
            wpos = (wpos + chunk) % hw->samples;
            to_grab -= chunk;
        }

        if (audio_pt_lock (&esd->pt, AUDIO_FUNC)) {
            return NULL;
        }

        esd->wpos = wpos;
        esd->dead -= incr;
        esd->incr += incr;
    }

 exit:
    audio_pt_unlock (&esd->pt, AUDIO_FUNC);
    return NULL;
}

static int qesd_run_in (HWVoiceIn *hw)
{
    int live, incr, dead;
    ESDVoiceIn *esd = (ESDVoiceIn *) hw;

    if (audio_pt_lock (&esd->pt, AUDIO_FUNC)) {
        return 0;
    }

    live = audio_pcm_hw_get_live_in (hw);
    dead = hw->samples - live;
    incr = audio_MIN (dead, esd->incr);
    esd->incr -= incr;
    esd->dead = dead - incr;
    hw->wpos = esd->wpos;
    if (esd->dead > 0) {
        audio_pt_unlock_and_signal (&esd->pt, AUDIO_FUNC);
    }
    else {
        audio_pt_unlock (&esd->pt, AUDIO_FUNC);
    }
    return incr;
}

static int qesd_read (SWVoiceIn *sw, void *buf, int len)
{
    return audio_pcm_sw_read (sw, buf, len);
}

static int qesd_init_in (HWVoiceIn *hw, audsettings_t *as)
{
    ESDVoiceIn *esd = (ESDVoiceIn *) hw;
    audsettings_t obt_as = *as;
    int esdfmt = ESD_STREAM | ESD_RECORD;
    int result = -1;

    /* shut down verbose debug spew */
    if (!D_ACTIVE)
        stdio_disable();

    esdfmt |= (as->nchannels == 2) ? ESD_STEREO : ESD_MONO;
    switch (as->fmt) {
    case AUD_FMT_S8:
    case AUD_FMT_U8:
        esdfmt |= ESD_BITS8;
        obt_as.fmt = AUD_FMT_U8;
        break;

    case AUD_FMT_S16:
    case AUD_FMT_U16:
        esdfmt |= ESD_BITS16;
        obt_as.fmt = AUD_FMT_S16;
        break;
    case AUD_FMT_S32:
    case AUD_FMT_U32:
        dolog ("Will use 16 instead of 32 bit samples\n");
        esdfmt |= ESD_BITS16;
        obt_as.fmt = AUD_FMT_S16;
        break;
    }
    obt_as.endianness = AUDIO_HOST_ENDIANNESS;

    audio_pcm_init_info (&hw->info, &obt_as);

    hw->samples = conf.samples;
    esd->pcm_buf = audio_calloc (AUDIO_FUNC, hw->samples, 1 << hw->info.shift);
    if (!esd->pcm_buf) {
        dolog ("Could not allocate buffer (%d bytes)\n",
               hw->samples << hw->info.shift);
        goto exit;
    }

    esd->fd = FF(esd_record_stream) (esdfmt, as->freq, conf.adc_host, NULL);
    if (esd->fd < 0) {
        if (conf.adc_host == NULL) {
            esd->fd = FF(esd_record_stream) (esdfmt, as->freq, "localhost", NULL);
        }
        if (esd->fd < 0) {
            qesd_logerr (errno, "esd_record_stream failed\n");
            goto fail2;
        }
    }

    if (audio_pt_init (&esd->pt, qesd_thread_in, esd, AUDIO_CAP, AUDIO_FUNC)) {
        goto fail3;
    }

    result = 0;  /* success */
    goto exit;

 fail3:
    if (close (esd->fd)) {
        qesd_logerr (errno, "%s: close on esd socket(%d) failed\n",
                     AUDIO_FUNC, esd->fd);
    }
    esd->fd = -1;

 fail2:
    qemu_free (esd->pcm_buf);
    esd->pcm_buf = NULL;

 exit:
    if (!D_ACTIVE)
        stdio_enable();

    return result;
}

static void qesd_fini_in (HWVoiceIn *hw)
{
    void *ret;
    ESDVoiceIn *esd = (ESDVoiceIn *) hw;

    audio_pt_lock (&esd->pt, AUDIO_FUNC);
    esd->done = 1;
    audio_pt_unlock_and_signal (&esd->pt, AUDIO_FUNC);
    audio_pt_join (&esd->pt, &ret, AUDIO_FUNC);

    if (esd->fd >= 0) {
        if (close (esd->fd)) {
            qesd_logerr (errno, "failed to close esd socket\n");
        }
        esd->fd = -1;
    }

    audio_pt_fini (&esd->pt, AUDIO_FUNC);

    qemu_free (esd->pcm_buf);
    esd->pcm_buf = NULL;
}

static int qesd_ctl_in (HWVoiceIn *hw, int cmd, ...)
{
    (void) hw;
    (void) cmd;
    return 0;
}

/* common */
static void *qesd_audio_init (void)
{
    void*    result = NULL;

    D("%s: entering", __FUNCTION__);

    if (esd_lib == NULL) {
        int  fd;

        esd_lib = dlopen( "libesd.so", RTLD_NOW );
        if (esd_lib == NULL)
            esd_lib = dlopen( "libesd.so.0", RTLD_NOW );

        if (esd_lib == NULL) {
            D("could not find libesd on this system");
            goto Exit;
        }

        if (esd_dynlink_init(esd_lib) < 0)
            goto Fail;

        fd = FF(esd_open_sound)(conf.dac_host);
        if (fd < 0) {
            D("%s: could not open direct sound server connection, trying localhost",
              __FUNCTION__);
            fd = FF(esd_open_sound)("localhost");
            if (fd < 0) {
                D("%s: could not open localhost sound server connection", __FUNCTION__);
                goto Fail;
            }
        }

        D("%s: EsounD server connection succeeded", __FUNCTION__);
        /* FF(esd_close)(fd); */
    }
    result = &conf;
    goto Exit;

Fail:
    D("%s: failed to open library", __FUNCTION__);
    dlclose(esd_lib);
    esd_lib = NULL;

Exit:
    return  result;
}

static void qesd_audio_fini (void *opaque)
{
    (void) opaque;
    if (esd_lib != NULL) {
        dlclose(esd_lib);
        esd_lib = NULL;
    }
    ldebug ("esd_fini");
}

struct audio_option qesd_options[] = {
    {"SAMPLES", AUD_OPT_INT, &conf.samples,
     "buffer size in samples", NULL, 0},

    {"DIVISOR", AUD_OPT_INT, &conf.divisor,
     "threshold divisor", NULL, 0},

    {"DAC_HOST", AUD_OPT_STR, &conf.dac_host,
     "playback host", NULL, 0},

    {"ADC_HOST", AUD_OPT_STR, &conf.adc_host,
     "capture host", NULL, 0},

    {NULL, 0, NULL, NULL, NULL, 0}
};

struct audio_pcm_ops qesd_pcm_ops = {
    qesd_init_out,
    qesd_fini_out,
    qesd_run_out,
    qesd_write,
    qesd_ctl_out,

    qesd_init_in,
    qesd_fini_in,
    qesd_run_in,
    qesd_read,
    qesd_ctl_in,
};

struct audio_driver esd_audio_driver = {
    INIT_FIELD (name           = ) "esd",
    INIT_FIELD (descr          = )
    "EsounD audio (en.wikipedia.org/wiki/Esound)",
    INIT_FIELD (options        = ) qesd_options,
    INIT_FIELD (init           = ) qesd_audio_init,
    INIT_FIELD (fini           = ) qesd_audio_fini,
    INIT_FIELD (pcm_ops        = ) &qesd_pcm_ops,
    INIT_FIELD (can_be_default = ) 1,
    INIT_FIELD (max_voices_out = ) INT_MAX,
    INIT_FIELD (max_voices_in  = ) 1,
    INIT_FIELD (voice_size_out = ) sizeof (ESDVoiceOut),
    INIT_FIELD (voice_size_in  = ) sizeof (ESDVoiceIn)
};
