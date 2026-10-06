/*
 * Compressor -- a single-band, zero-latency feed-forward compressor for
 * Schwung, aimed at live sources on the Move's line input (vocals, acoustic
 * guitar) but general enough for anything in a chain or the Master FX.
 *
 * SIGNAL PATH (per sample, stereo linked)
 *
 *   in -> Input gain -+-> sidechain HPF -> detector (Peak | RMS) -> dB
 *                     |                                              |
 *                     |                       gain computer (threshold, ratio,
 *                     |                       soft knee) -> target reduction
 *                     |                                              |
 *                     |                       attack / release smoothing of
 *                     |                       the reduction, in dB
 *                     |                                              |
 *                     +-> x gain(makeup - reduction) -> dry/wet Mix -> out
 *
 * The design is the textbook one (Giannoulis, Massberg & Reiss, "Digital
 * Dynamic Range Compressor Design -- A Tutorial and Analysis", JAES 2012):
 * the gain computer runs on the instantaneous detector level and the
 * BALLISTICS are applied to the gain reduction in the log domain, with
 * separate attack and release one-poles ("smooth branching" detector). Attack
 * and release are TIME CONSTANTS -- the time to cover 63% of a step.
 *
 * There is deliberately no lookahead: a compressor on a live microphone that
 * the singer hears back must not add delay.
 *
 * THREADING. Every entry point below runs on the SPI audio callback (see
 * plugin_api_v1.h): nothing here allocates, locks, logs or touches a file
 * outside create/destroy. Coefficients are recomputed in set_param only when a
 * value changes, so a turn costs a handful of expf/cosf calls.
 *
 * THE METER. One read-only key, `gr`, carries everything the UI draws:
 * "<gain change> dB in <detector level>", e.g. "-4.5 dB in -12", with
 * " byp" appended while Bypass is on (the reading is then what the
 * compressor would be doing, not what is being heard). It is ONE key
 * because the knob grid refreshes live keys at one read per tick, shared
 * between them -- two keys would each update at half the rate. The string is
 * also what the header shows when knob 8 is touched, so it is written to be
 * read. The values have meter ballistics computed here (instant rise, fixed
 * fall), so every reader sees the same reading however often it asks.
 */

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_fx_api_v2.h"
#include "contract_gen.h"   /* generated from src/module.json by scripts/gen_contract.py */

#define COMP_SR          44100.0f
#define COMP_PI          3.14159265358979f
#define LN10_OVER_20     0.11512925464970229f   /* ln(10) / 20 */
#define DB_FLOOR         (-120.0f)
#define METER_FLOOR_DB   (-90.0f)               /* below this the meter reads "--" */

/* Parameter smoothing for gains that would click if they jumped: input gain,
 * makeup and mix. ~10 ms one-pole. */
#define SMOOTH_MS        10.0f
/* RMS detector window (time constant of the mean-square average). */
#define RMS_MS           10.0f
/* Meter fall rates, dB per second. Rise is instant. */
#define METER_GR_FALL    40.0f
#define METER_IN_FALL    24.0f
/* After this many meter reads with no process_block in between, the host has
 * stopped running us (an idle slot is skipped after ~1 s of silence, and a
 * bypassed FX is not called at all), so the meter is reset instead of being
 * left frozen part-way through a release. Reads arrive at most ~60/s and
 * blocks at ~344/s, so in normal running this counter never exceeds 1-2. */
#define METER_STALE_READS 24
/* Auto makeup compensates this fraction of the reduction a 0 dBFS signal
 * would get from the static curve. Half is the usual compromise: full
 * compensation at full scale makes everything below it much louder. While
 * it is on, it REPLACES the Makeup knob rather than adding to it. */
#define AUTO_MAKEUP_FRACTION 0.5f

/* ------------------------------------------------------------------ params */

static const char *const HPF_OPTIONS[]    = { "Off", "60 Hz", "100 Hz", "150 Hz", "250 Hz" };
static const float       HPF_FREQS[]      = { 0.0f, 60.0f, 100.0f, 150.0f, 250.0f };
static const char *const DETECT_OPTIONS[] = { "Peak", "RMS" };
static const char *const ONOFF_OPTIONS[]  = { "Off", "On" };

#define N_HPF    ((int)(sizeof(HPF_OPTIONS) / sizeof(HPF_OPTIONS[0])))
#define N_DETECT 2
#define N_ONOFF  2

typedef struct {
    float threshold_db;   /* -60 .. 0 dBFS              */
    float ratio;          /* 1 .. 20 (:1)               */
    float attack_ms;      /* 0.1 .. 100                 */
    float release_ms;     /* 10 .. 2000                 */
    float knee_db;        /* 0 .. 24 (full width)       */
    float makeup_db;      /* -12 .. +24                 */
    float mix;            /* 0 .. 1 (wet fraction)      */
    float input_db;       /* -24 .. +24                 */
    int   sc_hpf;         /* index into HPF_FREQS       */
    int   detect;         /* 0 = Peak, 1 = RMS          */
    int   auto_makeup;    /* 0 = Off, 1 = On            */
    int   bypass;         /* 0 = Off, 1 = On            */
} comp_params_t;

typedef enum { PT_FLOAT, PT_ENUM } param_type_t;

typedef struct {
    const char         *key;
    param_type_t        type;
    size_t              off;
    float               min, max;
    const char *const  *options;
    int                 n_options;
    int                 in_presets;   /* does a factory preset set it? */
} param_def_t;

#define PF(k, field, lo, hi)       { k, PT_FLOAT, offsetof(comp_params_t, field), lo, hi, NULL, 0, 1 }
#define PE(k, field, opts, n, pre) { k, PT_ENUM,  offsetof(comp_params_t, field), 0, (float)((n) - 1), opts, n, pre }

/* One table drives set_param, get_param, state save/restore and presets, so a
 * new parameter is one line here plus its entry in module.json. */
static const param_def_t PARAMS[] = {
    PF("threshold", threshold_db, -60.0f,   0.0f),
    PF("ratio",     ratio,          1.0f,  20.0f),
    PF("attack",    attack_ms,      0.1f, 100.0f),
    PF("release",   release_ms,    10.0f, 2000.0f),
    PF("knee",      knee_db,        0.0f,  24.0f),
    PF("makeup",    makeup_db,    -12.0f,  24.0f),
    PF("mix",       mix,            0.0f,   1.0f),
    /* Input gain is a property of the SOURCE (how hot your mic is), not of a
     * sound, so loading a factory preset leaves it where you set it. */
    { "input", PT_FLOAT, offsetof(comp_params_t, input_db), -24.0f, 24.0f, NULL, 0, 0 },
    PE("sc_hpf",      sc_hpf,      HPF_OPTIONS,    N_HPF,    1),
    PE("detect",      detect,      DETECT_OPTIONS, N_DETECT, 1),
    PE("auto_makeup", auto_makeup, ONOFF_OPTIONS,  N_ONOFF,  1),
    /* Bypass is an A/B switch, not part of a sound: loading a preset while
     * listening bypassed must not switch the compressor back in. */
    PE("bypass",      bypass,      ONOFF_OPTIONS,  N_ONOFF,  0),
};
#define N_PARAMS ((int)(sizeof(PARAMS) / sizeof(PARAMS[0])))

static const comp_params_t DEFAULTS = {
    .threshold_db = -18.0f, .ratio = 3.0f, .attack_ms = 10.0f, .release_ms = 150.0f,
    .knee_db = 6.0f, .makeup_db = 0.0f, .mix = 1.0f, .input_db = 0.0f,
    .sc_hpf = 0, .detect = 0, .auto_makeup = 0, .bypass = 0,
};

/* ----------------------------------------------------------------- presets */

typedef struct {
    const char    *name;
    comp_params_t  p;     /* input_db is ignored -- see PARAMS */
} comp_preset_t;

/* HPF index: 0 Off, 1 60 Hz, 2 100 Hz, 3 150 Hz, 4 250 Hz. Detect: 0 Peak, 1 RMS. */
static const comp_preset_t PRESETS[] = {
    { "Default",        { -18.0f,  3.0f, 10.0f, 150.0f,  6.0f,  0.0f, 1.00f, 0.0f, 0, 0, 0, 0 } },
    { "Vocal Leveler",  { -20.0f,  2.5f, 12.0f, 160.0f,  8.0f,  4.0f, 1.00f, 0.0f, 2, 1, 0, 0 } },
    { "Vocal Upfront",  { -24.0f,  4.0f,  4.0f,  90.0f,  5.0f,  7.0f, 1.00f, 0.0f, 2, 0, 0, 0 } },
    { "Vocal Gentle",   { -16.0f,  2.0f, 20.0f, 250.0f, 10.0f,  3.0f, 1.00f, 0.0f, 2, 1, 0, 0 } },
    { "Spoken Word",    { -26.0f,  3.5f,  8.0f, 180.0f,  8.0f,  8.0f, 1.00f, 0.0f, 3, 1, 0, 0 } },
    { "Acoustic Strum", { -20.0f,  3.0f, 25.0f, 140.0f,  6.0f,  4.0f, 1.00f, 0.0f, 2, 0, 0, 0 } },
    { "Fingerpicked",   { -24.0f,  2.5f, 15.0f, 220.0f,  8.0f,  5.0f, 1.00f, 0.0f, 1, 1, 0, 0 } },
    { "Parallel Crush", { -36.0f, 10.0f,  1.0f,  80.0f,  0.0f, 12.0f, 0.35f, 0.0f, 0, 0, 0, 0 } },
    { "Peak Catcher",   {  -8.0f, 12.0f,  0.5f,  60.0f,  2.0f,  0.0f, 1.00f, 0.0f, 0, 0, 0, 0 } },
    { "Bus Glue",       { -16.0f,  2.0f, 30.0f, 300.0f,  6.0f,  2.0f, 1.00f, 0.0f, 1, 1, 0, 0 } },
    { "Drum Punch",     { -20.0f,  4.0f, 25.0f,  70.0f,  3.0f,  4.0f, 1.00f, 0.0f, 0, 0, 0, 0 } },
};
#define N_PRESETS ((int)(sizeof(PRESETS) / sizeof(PRESETS[0])))

/* ---------------------------------------------------------------- instance */

typedef struct {
    float b0, b1, b2, a1, a2;   /* normalised, a0 == 1 */
} biquad_t;

typedef struct {
    comp_params_t p;
    int   preset;               /* last factory preset loaded */
    int   dirty;                /* coefficients need recomputing */

    /* derived */
    float a_att, a_rel;         /* reduction ballistics */
    float a_rms;                /* RMS averaging coefficient (1 - pole) */
    float k_smooth;             /* gain smoothing coefficient (1 - pole) */
    float slope;                /* 1/ratio - 1 (<= 0) */
    float makeup_total_db;      /* Makeup, or the auto value while Auto is on */
    float in_gain_target;       /* linear */
    int   hpf_on;
    int   hpf_idx;              /* the HPF setting the state below belongs to */
    biquad_t hpf;

    /* running state */
    float hz[2][2];             /* HPF state per channel (TDF-II) */
    float ms;                   /* RMS mean square */
    float gr_db;                /* smoothed gain reduction, >= 0 */
    float in_gain;              /* smoothed */
    float makeup_db_s;          /* smoothed */
    float mix_s;                /* smoothed */
    float wet_s;                /* smoothed 1 - bypass */
    int   started;              /* a block has been processed */

    /* meter (display values, with ballistics) */
    float m_gr;                 /* dB of reduction */
    float m_in;                 /* detector level, dBFS */
    unsigned stale_reads;
} comp_t;

static const host_api_v1_t *g_host = NULL;

/* -------------------------------------------------------------- math bits */

static inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline float db_to_lin(float db) { return expf(db * LN10_OVER_20); }

static inline float one_pole(float ms) {
    /* Pole for a time constant of `ms` milliseconds. */
    if (ms <= 0.0f) return 0.0f;
    return expf(-1000.0f / (ms * COMP_SR));
}

/*
 * The static curve: how many dB of reduction a steady level `x_db` gets.
 * Soft knee of full width `w` centred on the threshold (quadratic blend
 * between the 1:1 line and the ratio line); w == 0 is a hard knee.
 */
static inline float gain_computer(float x_db, float thr, float w, float slope) {
    const float over = x_db - thr;
    if (w > 0.01f) {
        if (2.0f * over < -w) return 0.0f;
        if (2.0f * over <= w) {
            const float t = over + 0.5f * w;
            return -slope * t * t / (2.0f * w);
        }
        return -slope * over;
    }
    return over > 0.0f ? -slope * over : 0.0f;
}

static void hpf_design(biquad_t *bq, float freq) {
    /* RBJ cookbook high-pass, Q = 1/sqrt(2) (Butterworth, 12 dB/oct). */
    const float w0 = 2.0f * COMP_PI * freq / COMP_SR;
    const float cw = cosf(w0);
    const float alpha = sinf(w0) * 0.70710678f;   /* sin(w0) / (2Q), Q = 0.7071 */
    const float a0 = 1.0f + alpha;
    bq->b0 = (1.0f + cw) * 0.5f / a0;
    bq->b1 = -(1.0f + cw) / a0;
    bq->b2 = bq->b0;
    bq->a1 = -2.0f * cw / a0;
    bq->a2 = (1.0f - alpha) / a0;
}

static inline float biquad_run(const biquad_t *bq, float *z, float x) {
    const float y = bq->b0 * x + z[0];
    z[0] = bq->b1 * x - bq->a1 * y + z[1];
    z[1] = bq->b2 * x - bq->a2 * y;
    return y;
}

static inline float flush_denormal(float v) {
    return fabsf(v) < 1e-20f ? 0.0f : v;
}

static float auto_makeup_db(const comp_params_t *p) {
    const float slope = 1.0f / p->ratio - 1.0f;
    return AUTO_MAKEUP_FRACTION * gain_computer(0.0f, p->threshold_db, p->knee_db, slope);
}

static void update_coeffs(comp_t *c) {
    const comp_params_t *p = &c->p;
    c->a_att  = one_pole(p->attack_ms);
    c->a_rel  = one_pole(p->release_ms);
    c->slope  = 1.0f / p->ratio - 1.0f;
    c->makeup_total_db = p->auto_makeup ? auto_makeup_db(p) : p->makeup_db;
    c->in_gain_target  = db_to_lin(p->input_db);
    c->hpf_on = p->sc_hpf > 0 && p->sc_hpf < N_HPF;
    if (c->hpf_on) hpf_design(&c->hpf, HPF_FREQS[p->sc_hpf]);
    if (p->sc_hpf != c->hpf_idx) {
        /* State left over from another corner (or from long ago, if it was
         * Off) is a step into the detector; start the new filter clean. */
        memset(c->hz, 0, sizeof(c->hz));
        c->hpf_idx = p->sc_hpf;
    }
    c->dirty = 0;
}

/* --------------------------------------------------------------- lifecycle */

static void *comp_create(const char *module_dir, const char *config_json) {
    (void)module_dir; (void)config_json;
    comp_t *c = (comp_t *)calloc(1, sizeof(comp_t));
    if (!c) return NULL;
    c->p = DEFAULTS;
    c->preset = 0;
    c->a_rms = 1.0f - one_pole(RMS_MS);
    c->k_smooth = 1.0f - one_pole(SMOOTH_MS);
    update_coeffs(c);
    /* Start the smoothed values AT their targets: a freshly inserted FX must
     * not fade in from silence or from 0 dB makeup. */
    c->in_gain = c->in_gain_target;
    c->makeup_db_s = c->makeup_total_db;
    c->mix_s = c->p.mix;
    c->wet_s = c->p.bypass ? 0.0f : 1.0f;
    c->m_in = DB_FLOOR;
    return c;
}

static void comp_destroy(void *instance) {
    free(instance);
}

/* ------------------------------------------------------------------- audio */

static void comp_process(void *instance, int16_t *io, int frames) {
    comp_t *c = (comp_t *)instance;
    if (!c || !io || frames <= 0) return;
    if (c->dirty) update_coeffs(c);
    if (!c->started) {
        /* Until audio has run, a change is a LOAD (create, then the saved
         * state), not a gesture: start at the restored values instead of
         * ramping to them from the defaults. */
        c->in_gain = c->in_gain_target;
        c->makeup_db_s = c->makeup_total_db;
        c->mix_s = c->p.mix;
        c->wet_s = c->p.bypass ? 0.0f : 1.0f;
        c->started = 1;
    }

    const float thr = c->p.threshold_db;
    const float knee = c->p.knee_db;
    const float slope = c->slope;
    const float a_att = c->a_att, a_rel = c->a_rel;
    const float k = c->k_smooth;
    const float in_t = c->in_gain_target;
    const float mk_t = c->makeup_total_db;
    const float mix_t = c->p.mix;
    const float wet_t = c->p.bypass ? 0.0f : 1.0f;
    const int rms = c->p.detect == 1;
    const int hpf = c->hpf_on;

    float gr = c->gr_db;
    float in_gain = c->in_gain, mk = c->makeup_db_s, mix = c->mix_s, wet = c->wet_s;
    float ms = c->ms;
    float blk_gr = 0.0f, blk_in = DB_FLOOR;

    for (int i = 0; i < frames; i++) {
        in_gain += k * (in_t - in_gain);
        mk      += k * (mk_t - mk);
        mix     += k * (mix_t - mix);
        wet     += k * (wet_t - wet);

        /* The untouched input, before Input gain: what Bypass lets through. */
        const float dl = (float)io[2 * i]     * (1.0f / 32768.0f);
        const float dr = (float)io[2 * i + 1] * (1.0f / 32768.0f);
        const float l = dl * in_gain;
        const float r = dr * in_gain;

        float sl = l, sr = r;
        if (hpf) {
            sl = biquad_run(&c->hpf, c->hz[0], l);
            sr = biquad_run(&c->hpf, c->hz[1], r);
        }

        float x_db;
        if (rms) {
            /* The LOUDER channel's power, not the average: a mono mic on one
             * side of the line input (a TS cable) must read the same level
             * as the same mic on both, or it compresses 3 dB less. */
            const float pl = sl * sl, pr = sr * sr;
            ms += c->a_rms * ((pl > pr ? pl : pr) - ms);
            /* x2: sine-calibrated, so a sine reads the same level in RMS as
             * its peak does in Peak mode and a threshold means one thing. */
            x_db = 10.0f * log10f(2.0f * ms + 1e-12f);
        } else {
            const float al = fabsf(sl), ar = fabsf(sr);
            x_db = 20.0f * log10f((al > ar ? al : ar) + 1e-6f);
        }
        if (x_db > blk_in) blk_in = x_db;

        const float target = gain_computer(x_db, thr, knee, slope);
        gr = target > gr ? a_att * gr + (1.0f - a_att) * target
                         : a_rel * gr + (1.0f - a_rel) * target;
        if (gr > blk_gr) blk_gr = gr;

        const float g = expf((mk - gr) * LN10_OVER_20);
        float ol = l + mix * (l * g - l);
        float orr = r + mix * (r * g - r);
        /* Bypass crossfades to the input as it arrived -- no Input gain, no
         * Makeup, no Mix -- over the same ~10 ms as the other gains, so an
         * A/B switch does not click. The detector keeps running underneath,
         * so switching back in is seamless and the meter still shows what
         * the compressor WOULD be doing. Fully bypassed, dl * 32768 is the
         * original integer, so the output is bit-exact. */
        ol  = dl + wet * (ol - dl);
        orr = dr + wet * (orr - dr);

        ol = clampf(ol * 32768.0f, -32768.0f, 32767.0f);
        orr = clampf(orr * 32768.0f, -32768.0f, 32767.0f);
        io[2 * i]     = (int16_t)(ol >= 0.0f ? ol + 0.5f : ol - 0.5f);
        io[2 * i + 1] = (int16_t)(orr >= 0.0f ? orr + 0.5f : orr - 0.5f);
    }

    c->gr_db = gr < 1e-6f ? 0.0f : gr;
    /* Land exactly on target rather than approaching it forever, so that
     * settings meant to be transparent (Mix 0, 0 dB) are bit-exact. */
    c->in_gain     = fabsf(in_gain - in_t) < 1e-6f ? in_t : in_gain;
    c->makeup_db_s = fabsf(mk - mk_t) < 1e-5f ? mk_t : mk;
    c->mix_s       = fabsf(mix - mix_t) < 1e-6f ? mix_t : mix;
    c->wet_s       = fabsf(wet - wet_t) < 1e-6f ? wet_t : wet;
    c->ms = flush_denormal(ms);
    for (int ch = 0; ch < 2; ch++) {
        c->hz[ch][0] = flush_denormal(c->hz[ch][0]);
        c->hz[ch][1] = flush_denormal(c->hz[ch][1]);
    }

    /* Meter ballistics, once per block. */
    const float dt = (float)frames / COMP_SR;
    const float gr_fall = c->m_gr - METER_GR_FALL * dt;
    c->m_gr = blk_gr > gr_fall ? blk_gr : (gr_fall > 0.0f ? gr_fall : 0.0f);
    const float in_fall = c->m_in - METER_IN_FALL * dt;
    c->m_in = blk_in > in_fall ? blk_in : (in_fall > DB_FLOOR ? in_fall : DB_FLOOR);
    c->stale_reads = 0;
}

/* -------------------------------------------------------------- param I/O */

static const param_def_t *find_param(const char *key) {
    for (int i = 0; i < N_PARAMS; i++)
        if (strcmp(PARAMS[i].key, key) == 0) return &PARAMS[i];
    return NULL;
}

static float *param_f(comp_t *c, const param_def_t *d) { return (float *)((char *)&c->p + d->off); }
static int   *param_i(comp_t *c, const param_def_t *d) { return (int *)((char *)&c->p + d->off); }

/* Parse a number, refusing text that is not one (atof would read it as 0). */
static int parse_number(const char *s, float *out) {
    if (!s) return 0;
    char *end = NULL;
    const float v = strtof(s, &end);
    if (end == s || !isfinite(v)) return 0;
    *out = v;
    return 1;
}

/*
 * An enum accepts its option NAME or its INDEX, and nothing else. get_param
 * reports the index, which is what the knob grid learns the wire from.
 */
static int parse_enum(const param_def_t *d, const char *val, int *out) {
    for (int i = 0; i < d->n_options; i++) {
        if (strcmp(val, d->options[i]) == 0) { *out = i; return 1; }
    }
    float v;
    if (!parse_number(val, &v)) return 0;
    const int idx = (int)lrintf(v);
    if (idx < 0 || idx >= d->n_options) return 0;
    *out = idx;
    return 1;
}

static void set_one(comp_t *c, const param_def_t *d, const char *val) {
    if (d->type == PT_FLOAT) {
        float v;
        if (!parse_number(val, &v)) return;
        v = clampf(v, d->min, d->max);
        if (*param_f(c, d) != v) { *param_f(c, d) = v; c->dirty = 1; }
    } else {
        int idx;
        if (!parse_enum(d, val, &idx)) return;
        if (*param_i(c, d) != idx) { *param_i(c, d) = idx; c->dirty = 1; }
    }
}

static void apply_preset(comp_t *c, int idx) {
    if (idx < 0 || idx >= N_PRESETS) return;
    const comp_params_t *src = &PRESETS[idx].p;
    for (int i = 0; i < N_PARAMS; i++) {
        const param_def_t *d = &PARAMS[i];
        if (!d->in_presets) continue;
        if (d->type == PT_FLOAT) *param_f(c, d) = *(const float *)((const char *)src + d->off);
        else                     *param_i(c, d) = *(const int *)((const char *)src + d->off);
    }
    c->preset = idx;
    c->dirty = 1;
}

/* Find `"key"` in a flat JSON object and return a pointer to its value. */
static const char *json_value(const char *json, const char *key) {
    char pat[48];
    const int n = snprintf(pat, sizeof(pat), "\"%s\"", key);
    if (n <= 0 || n >= (int)sizeof(pat)) return NULL;
    const char *p = strstr(json, pat);
    if (!p) return NULL;
    p += n;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != ':') return NULL;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

static void restore_state(comp_t *c, const char *json) {
    for (int i = 0; i < N_PARAMS; i++) {
        const param_def_t *d = &PARAMS[i];
        const char *v = json_value(json, d->key);
        if (!v) continue;
        if (*v == '"') {
            /* An enum saved by name. Copy it out of the quotes. */
            char name[32];
            const char *end = strchr(v + 1, '"');
            if (!end || end - (v + 1) >= (int)sizeof(name)) continue;
            memcpy(name, v + 1, (size_t)(end - (v + 1)));
            name[end - (v + 1)] = '\0';
            set_one(c, d, name);
        } else {
            set_one(c, d, v);
        }
    }
    /* The preset INDEX is restored as a label only -- re-applying it would
     * overwrite the edits saved alongside it. */
    const char *pv = json_value(json, "preset");
    float f;
    if (pv && parse_number(pv, &f)) {
        const int idx = (int)lrintf(f);
        if (idx >= 0 && idx < N_PRESETS) c->preset = idx;
    }
    c->dirty = 1;
}

static void comp_set_param(void *instance, const char *key, const char *val) {
    comp_t *c = (comp_t *)instance;
    if (!c || !key || !val) return;

    const param_def_t *d = find_param(key);
    if (d) { set_one(c, d, val); return; }

    if (strcmp(key, "preset") == 0) {
        float v;
        if (parse_number(val, &v)) apply_preset(c, (int)lrintf(v));
        return;
    }
    if (strcmp(key, "state") == 0) {
        restore_state(c, val);
        return;
    }
    /* `gr` is a readout and `curve` is a page; writes to either mean nothing. */
}

/* snprintf reports the length it WANTED; a truncated answer is a failed one. */
static int fit(int n, int len) {
    return (n < 0 || n >= len) ? -1 : n;
}

static int put(char *buf, int len, const char *s) {
    const int n = (int)strlen(s);
    if (n >= len) return -1;
    memcpy(buf, s, (size_t)n + 1);
    return n;
}

static int format_meter(comp_t *c, char *buf, int len) {
    if (++c->stale_reads > METER_STALE_READS) {
        c->stale_reads = METER_STALE_READS + 1;   /* no wrap */
        c->m_gr = 0.0f;
        c->m_in = DB_FLOOR;
    }
    /* Written as a GAIN CHANGE, so negative: "-6.2 dB in -9". Besides being
     * how a reduction meter is read, it is what makes the grid's 5-character
     * cell label land cleanly on "-6.2" when the knob is touched. Exactly
     * zero prints as "0.0", never "-0.0". */
    const double change = c->m_gr < 0.05f ? 0.0 : -(double)c->m_gr;
    const char *byp = c->p.bypass ? " byp" : "";
    if (c->m_in <= METER_FLOOR_DB)
        return fit(snprintf(buf, len, "%.1f dB in --%s", change, byp), len);
    return fit(snprintf(buf, len, "%.1f dB in %d%s", change, (int)lrintf(c->m_in), byp), len);
}

static int comp_get_param(void *instance, const char *key, char *buf, int len) {
    comp_t *c = (comp_t *)instance;
    if (!c || !key || !buf || len <= 0) return -1;

    const param_def_t *d = find_param(key);
    if (d) {
        if (d->type == PT_FLOAT) return fit(snprintf(buf, len, "%.3f", (double)*param_f(c, d)), len);
        return fit(snprintf(buf, len, "%d", *param_i(c, d)), len);
    }

    /* The meter. The grid asks for `gr:effective` every tick (the param is
     * declared live), and for `gr` / `gr:base` on its value rotation; all
     * three are the same reading. */
    if (strcmp(key, "gr") == 0 || strcmp(key, "gr:effective") == 0 ||
        strcmp(key, "gr:base") == 0)
        return format_meter(c, buf, len);

    if (strcmp(key, "preset") == 0)       return fit(snprintf(buf, len, "%d", c->preset), len);
    if (strcmp(key, "preset_count") == 0) return fit(snprintf(buf, len, "%d", N_PRESETS), len);
    if (strcmp(key, "preset_name") == 0)
        return put(buf, len, (c->preset >= 0 && c->preset < N_PRESETS) ? PRESETS[c->preset].name : "");
    if (strcmp(key, "name") == 0) return put(buf, len, "Compressor");

    if (strcmp(key, "state") == 0) {
        int n = snprintf(buf, len, "{");
        for (int i = 0; i < N_PARAMS && n > 0 && n < len; i++) {
            const param_def_t *dd = &PARAMS[i];
            if (dd->type == PT_FLOAT)
                n += snprintf(buf + n, len - n, "\"%s\":%.3f,", dd->key, (double)*param_f(c, dd));
            else
                n += snprintf(buf + n, len - n, "\"%s\":%d,", dd->key, *param_i(c, dd));
        }
        if (n <= 0 || n >= len) return -1;
        n += snprintf(buf + n, len - n, "\"preset\":%d}", c->preset);
        return n < len ? n : -1;
    }

    /* Served from here, not left to module.json: the chain host's fallback
     * re-serialises module.json's chain_params and drops `viz`, `live`,
     * `access` and `step`, which would lose the meter widget, the live
     * refresh and the readout. Both strings are generated from module.json,
     * so there is one source of truth. */
    if (strcmp(key, "chain_params") == 0) return put(buf, len, COMP_CHAIN_PARAMS_JSON);
    if (strcmp(key, "ui_hierarchy") == 0) return put(buf, len, COMP_UI_HIERARCHY_JSON);

    return -1;
}

/* -------------------------------------------------------------- entry point */

static audio_fx_api_v2_t g_api;

audio_fx_api_v2_t *move_audio_fx_init_v2(const host_api_v1_t *host) {
    g_host = host;
    memset(&g_api, 0, sizeof(g_api));
    g_api.api_version      = AUDIO_FX_API_VERSION_2;
    g_api.create_instance  = comp_create;
    g_api.destroy_instance = comp_destroy;
    g_api.process_block    = comp_process;
    g_api.set_param        = comp_set_param;
    g_api.get_param        = comp_get_param;
    g_api.on_midi          = NULL;
    return &g_api;
}
