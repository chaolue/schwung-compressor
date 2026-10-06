/*
 * Offline tests for the Compressor DSP, driven through the same
 * audio_fx_api_v2 surface the chain host uses. The source is included
 * directly so a few checks can also look at internal state (the smoothed
 * gain reduction) where measuring it from audio would only add noise.
 *
 *   tests/run_tests.sh            builds and runs everything
 *   build/test_compressor         runs these
 *   build/test_compressor --dump-contract   prints chain_params / ui_hierarchy
 *   build/test_compressor --dump-curve      prints a gain-computer table (JSON)
 *
 * Expected values come from the textbook compressor equations, not from this
 * implementation's own output, so a check fails when the DSP is wrong rather
 * than merely when it changes.
 */

#include "../src/dsp/compressor.c"

#include <time.h>

static int g_fail = 0, g_pass = 0;

#define CHECK(cond, ...) do {                                   \
    if (cond) { g_pass++; }                                     \
    else { g_fail++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
           printf(__VA_ARGS__); printf("\n"); }                 \
} while (0)

#define NEAR(a, b, tol) (fabs((double)(a) - (double)(b)) <= (double)(tol))

static audio_fx_api_v2_t *api(void) {
    static host_api_v1_t host;   /* zeroed; the plugin never calls into it */
    static audio_fx_api_v2_t *a = NULL;
    if (!a) a = move_audio_fx_init_v2(&host);
    return a;
}

static comp_t *make(void) { return (comp_t *)api()->create_instance(".", NULL); }
static void drop(comp_t *c) { api()->destroy_instance(c); }
static void set(comp_t *c, const char *k, const char *v) { api()->set_param(c, k, v); }
static void setf(comp_t *c, const char *k, double v) {
    char b[32]; snprintf(b, sizeof b, "%.6f", v); set(c, k, b);
}
static const char *get(comp_t *c, const char *k) {
    static char b[8192];
    int n = api()->get_param(c, k, b, sizeof b);
    if (n < 0) b[0] = '\0';
    return b;
}

/* A neutral, predictable starting point: hard knee, peak detector, no HPF. */
static comp_t *make_plain(double thr, double ratio, double atk, double rel) {
    comp_t *c = make();
    setf(c, "threshold", thr); setf(c, "ratio", ratio);
    setf(c, "attack", atk);    setf(c, "release", rel);
    setf(c, "knee", 0);        setf(c, "makeup", 0); setf(c, "mix", 1);
    set(c, "sc_hpf", "0");     set(c, "detect", "0"); set(c, "auto_makeup", "0");
    return c;
}

#define BLOCK 128
static double g_phase = 0.0;

/* Fill one block with a stereo sine of peak amplitude `amp` (linear). */
static void sine_block(int16_t *buf, double freq, double amp) {
    for (int i = 0; i < BLOCK; i++) {
        double v = amp * sin(g_phase);
        g_phase += 2.0 * M_PI * freq / 44100.0;
        if (g_phase > 2.0 * M_PI) g_phase -= 2.0 * M_PI;
        int16_t s = (int16_t)lrint(v * 32767.0);
        buf[2 * i] = s; buf[2 * i + 1] = s;
    }
}

static double db(double lin) { return 20.0 * log10(lin > 1e-12 ? lin : 1e-12); }
static double lin(double d) { return pow(10.0, d / 20.0); }

/* Run `seconds` of sine through `c`; return the output peak (dBFS) over the
 * final `measure` seconds. */
static double run_sine(comp_t *c, double freq, double amp_db, double seconds, double measure) {
    int16_t buf[BLOCK * 2];
    int blocks = (int)(seconds * 44100.0 / BLOCK);
    int from = blocks - (int)(measure * 44100.0 / BLOCK);
    double peak = 0.0;
    for (int b = 0; b < blocks; b++) {
        sine_block(buf, freq, lin(amp_db));
        api()->process_block(c, buf, BLOCK);
        if (b >= from) {
            for (int i = 0; i < BLOCK * 2; i++) {
                double v = fabs(buf[i] / 32768.0);
                if (v > peak) peak = v;
            }
        }
    }
    return db(peak);
}

/* ------------------------------------------------------------------ tests */

static void test_gain_computer(void) {
    printf("gain computer (static curve)\n");
    const float slope3 = 1.0f / 3.0f - 1.0f;
    CHECK(gain_computer(-30.0f, -18.0f, 0.0f, slope3) == 0.0f, "below threshold must not reduce");
    CHECK(NEAR(gain_computer(-6.0f, -18.0f, 0.0f, slope3), 8.0, 1e-4), "12 dB over at 3:1 is 8 dB");
    CHECK(NEAR(gain_computer(0.0f, -20.0f, 0.0f, 1.0f / 20.0f - 1.0f), 19.0, 1e-4), "20 dB over at 20:1 is 19 dB");
    CHECK(gain_computer(-6.0f, -18.0f, 0.0f, 0.0f) == 0.0f, "1:1 never reduces");

    /* Soft knee: continuous at both edges, quadratic in between, and equal
     * to (W/2)^2 * (1 - 1/R) / (2W) at the threshold itself. */
    const float W = 12.0f, slope4 = 1.0f / 4.0f - 1.0f;
    CHECK(NEAR(gain_computer(-18.0f, -18.0f, W, slope4), 0.75 * 36.0 / 24.0, 1e-4),
          "knee value at threshold");
    CHECK(NEAR(gain_computer(-24.0f, -18.0f, W, slope4), 0.0, 1e-4), "knee starts at T - W/2");
    CHECK(NEAR(gain_computer(-12.0f, -18.0f, W, slope4), 0.75 * 6.0, 1e-4), "knee ends at T + W/2");
    float prev = -1.0f;
    int monotonic = 1;
    for (float x = -60.0f; x <= 0.0f; x += 0.05f) {
        float g = gain_computer(x, -18.0f, W, slope4);
        if (g < prev - 1e-5f) monotonic = 0;
        /* output level must never fall as input rises (no over-compression) */
        if (x > -60.0f && (x - g) < (x - 0.05f - gain_computer(x - 0.05f, -18.0f, W, slope4)) - 1e-4f)
            monotonic = 0;
        prev = g;
    }
    CHECK(monotonic, "reduction and output both monotonic across the knee");
}

static void test_passthrough_below_threshold(void) {
    printf("bit-exact below threshold\n");
    comp_t *c = make_plain(-18, 4, 5, 100);
    int16_t in[BLOCK * 2], out[BLOCK * 2];
    int exact = 1;
    g_phase = 0;
    for (int b = 0; b < 400; b++) {
        sine_block(in, 440.0, lin(-30.0));
        memcpy(out, in, sizeof in);
        api()->process_block(c, out, BLOCK);
        if (memcmp(in, out, sizeof in) != 0) exact = 0;
    }
    CHECK(exact, "unity settings below threshold must not change a single sample");
    drop(c);
}

static void test_steady_state(void) {
    printf("steady-state reduction\n");
    /* -6 dBFS sine, T -18, 3:1 -> 8 dB of reduction -> -14 dBFS out. */
    comp_t *c = make_plain(-18, 3, 1, 100);
    double out = run_sine(c, 1000.0, -6.0, 1.0, 0.2);
    CHECK(NEAR(out, -14.0, 0.5), "peak detector: expected -14 dBFS, got %.2f", out);
    drop(c);

    c = make_plain(-18, 3, 1, 100);
    set(c, "detect", "RMS");
    out = run_sine(c, 1000.0, -6.0, 1.0, 0.2);
    CHECK(NEAR(out, -14.0, 0.5), "RMS detector is sine-calibrated: expected -14 dBFS, got %.2f", out);
    drop(c);

    /* A mono source on ONE channel (a TS cable into the line input) is
     * detected at the same level as on both, in either detector mode. */
    for (int mode = 0; mode < 2; mode++) {
        c = make_plain(-18, 3, 1, 100);
        set(c, "detect", mode ? "RMS" : "Peak");
        int16_t buf[BLOCK * 2];
        g_phase = 0;
        for (int b = 0; b < 400; b++) {
            sine_block(buf, 1000.0, lin(-6.0));
            for (int i = 0; i < BLOCK; i++) buf[2 * i + 1] = 0;
            api()->process_block(c, buf, BLOCK);
        }
        CHECK(NEAR(c->gr_db, 8.0, 0.5), "%s, left channel only: %.2f dB of reduction, expected ~8",
              mode ? "RMS" : "Peak", c->gr_db);
        drop(c);
    }

    /* Knee: a sine sitting exactly on the threshold gets the knee's
     * mid-point reduction, (W/2)^2 (1-1/R) / 2W = 1.125 dB for W 12, 4:1. */
    c = make_plain(-18, 4, 5, 300);
    setf(c, "knee", 12);
    set(c, "detect", "RMS");
    out = run_sine(c, 1000.0, -18.0, 1.5, 0.2);
    CHECK(NEAR(out, -19.125, 0.3), "soft knee at threshold: expected -19.13 dBFS, got %.2f", out);
    drop(c);
}

static void dc_run(comp_t *c, int16_t level, int samples) {
    int16_t buf[1024 * 2];
    while (samples > 0) {
        int n = samples > 1024 ? 1024 : samples;
        for (int i = 0; i < n; i++) { buf[2 * i] = level; buf[2 * i + 1] = level; }
        api()->process_block(c, buf, n);
        samples -= n;
    }
}

static void test_time_constants(void) {
    printf("attack / release time constants\n");
    /* A DC step keeps the detector level constant, so the reduction follows
     * a clean one-pole: 63.2% of the step after one time constant. */
    comp_t *c = make_plain(-30, 20, 10, 500);
    const double over = db(0.5) + 30.0;                 /* -6.02 dBFS is 23.98 dB over */
    const double target = over * (1.0 - 1.0 / 20.0);
    dc_run(c, (int16_t)(32768 * 0.5), 441);             /* 10 ms */
    CHECK(NEAR(c->gr_db, target * (1.0 - exp(-1.0)), target * 0.01),
          "attack: %.3f dB after 10 ms, expected %.3f", c->gr_db, target * (1.0 - exp(-1.0)));
    dc_run(c, (int16_t)(32768 * 0.5), 44100);           /* settle fully */
    CHECK(NEAR(c->gr_db, target, 0.005), "settles to the static curve: %.3f vs %.3f", c->gr_db, target);
    dc_run(c, 33, 22050);                               /* -60 dBFS for 500 ms */
    CHECK(NEAR(c->gr_db, target * exp(-1.0), target * 0.01),
          "release: %.3f dB after 500 ms, expected %.3f", c->gr_db, target * exp(-1.0));
    drop(c);
}

static void test_mix_makeup_input(void) {
    printf("mix, makeup, auto makeup, input gain\n");
    /* Mix 0 is the dry signal exactly, however hard it is compressing. */
    comp_t *c = make_plain(-40, 20, 0.5, 50);
    setf(c, "mix", 0);
    setf(c, "makeup", 12);
    int16_t in[BLOCK * 2], ob[BLOCK * 2];
    int exact = 1;
    g_phase = 0;
    for (int b = 0; b < 200; b++) {
        sine_block(in, 1000.0, lin(-3.0));
        memcpy(ob, in, sizeof in);
        api()->process_block(c, ob, BLOCK);
        if (memcmp(in, ob, sizeof in) != 0) exact = 0;
    }
    CHECK(exact, "mix 0 must be the untouched input, from the first sample of a fresh load");
    drop(c);

    /* Turned down to 0 while running, it ramps there and then is exact. */
    c = make_plain(-40, 20, 0.5, 50);
    run_sine(c, 1000.0, -3.0, 0.2, 0.1);
    setf(c, "mix", 0);
    run_sine(c, 1000.0, -3.0, 0.5, 0.1);
    exact = 1;
    for (int b = 0; b < 50; b++) {
        sine_block(in, 1000.0, lin(-3.0));
        memcpy(ob, in, sizeof in);
        api()->process_block(c, ob, BLOCK);
        if (memcmp(in, ob, sizeof in) != 0) exact = 0;
    }
    CHECK(exact, "mix ramped to 0 settles exactly on the dry signal");
    drop(c);

    /* Mix 0.5 lands between dry and wet. */
    c = make_plain(-30, 10, 1, 100);
    double wet = run_sine(c, 1000.0, -6.0, 0.8, 0.2);
    drop(c);
    c = make_plain(-30, 10, 1, 100);
    setf(c, "mix", 0.5);
    double half = run_sine(c, 1000.0, -6.0, 0.8, 0.2);
    drop(c);
    CHECK(half > wet + 1.0 && half < -6.0, "mix 0.5 between wet %.2f and dry -6: got %.2f", wet, half);

    /* Makeup below threshold is a plain gain. */
    c = make_plain(-10, 4, 5, 100);
    setf(c, "makeup", 6);
    double out = run_sine(c, 1000.0, -30.0, 0.5, 0.1);
    CHECK(NEAR(out, -24.0, 0.1), "+6 dB makeup: expected -24 dBFS, got %.2f", out);
    drop(c);

    /* Auto makeup is half the reduction a 0 dBFS signal would get:
     * T -18 is 18 dB below full scale, x (1 - 1/3) = 12 dB -> +6 dB. */
    c = make_plain(-18, 3, 5, 100);
    set(c, "auto_makeup", "On");
    out = run_sine(c, 1000.0, -30.0, 0.5, 0.1);
    CHECK(NEAR(out, -24.0, 0.1), "auto makeup +6 dB: expected -24 dBFS, got %.2f", out);
    drop(c);

    /* Input gain drives the detector as well as the level:
     * -20 dBFS +6 dB = -14, 4 dB over -18 at 3:1 -> 2.67 dB off -> -16.67. */
    c = make_plain(-18, 3, 1, 100);
    setf(c, "input", 6);
    set(c, "detect", "RMS");
    out = run_sine(c, 1000.0, -20.0, 1.0, 0.2);
    CHECK(NEAR(out, -14.0 - 4.0 * (2.0 / 3.0), 0.3), "input gain: expected -16.67 dBFS, got %.2f", out);
    drop(c);
}

static void test_sidechain_hpf(void) {
    printf("sidechain high-pass\n");
    comp_t *c = make_plain(-18, 4, 1, 200);
    set(c, "detect", "RMS");
    run_sine(c, 50.0, -6.0, 1.0, 0.1);
    float gr_open = c->gr_db;
    drop(c);

    c = make_plain(-18, 4, 1, 200);
    set(c, "detect", "RMS");
    set(c, "sc_hpf", "250 Hz");
    run_sine(c, 50.0, -6.0, 1.0, 0.1);
    float gr_hpf = c->gr_db;
    drop(c);
    CHECK(gr_open > 8.0f, "50 Hz at -6 dBFS compresses with no HPF (%.2f dB)", gr_open);
    CHECK(gr_hpf < 0.5f, "a 250 Hz sidechain HPF ignores it (%.2f dB)", gr_hpf);

    /* The HPF is in the DETECTOR only: the audio keeps its lows. */
    c = make_plain(-6, 4, 1, 200);
    set(c, "sc_hpf", "250 Hz");
    double out = run_sine(c, 50.0, -20.0, 1.0, 0.2);
    CHECK(NEAR(out, -20.0, 0.1), "HPF must not filter the audio path (got %.2f)", out);
    drop(c);
}

static void test_meter(void) {
    printf("gain-reduction meter\n");
    comp_t *c = make_plain(-18, 3, 1, 100);
    run_sine(c, 1000.0, -6.0, 1.0, 0.1);
    float gr = -1, in = 0;
    const char *m = get(c, "gr");
    CHECK(sscanf(m, "%f dB in %f", &gr, &in) == 2, "meter format: \"%s\"", m);
    CHECK(NEAR(gr, -8.0, 0.5), "meter shows a ~-8 dB gain change (%s)", m);
    CHECK(NEAR(in, -6.0, 1.0), "meter shows the -6 dBFS input (%s)", m);
    CHECK(strcmp(get(c, "gr:effective"), get(c, "gr")) == 0, "gr:effective is the same reading");

    /* No blocks between reads: the host has stopped running us. */
    for (int i = 0; i < METER_STALE_READS + 2; i++) get(c, "gr");
    CHECK(strcmp(get(c, "gr"), "0.0 dB in --") == 0, "stale meter resets, got \"%s\"", get(c, "gr"));

    /* ...and comes back the moment audio does. */
    run_sine(c, 1000.0, -6.0, 0.3, 0.1);
    CHECK(sscanf(get(c, "gr"), "%f dB in %f", &gr, &in) == 2 && gr < -7.0f, "meter resumes");

    /* Fall rate of the input meter: 24 dB/s. */
    int16_t z[BLOCK * 2];
    memset(z, 0, sizeof z);
    float in0 = c->m_in;
    for (int b = 0; b < 34; b++) { memset(z, 0, sizeof z); api()->process_block(c, z, BLOCK); }
    double dt = 34.0 * BLOCK / 44100.0;
    CHECK(NEAR(c->m_in, in0 - 24.0 * dt, 0.2), "input meter falls at 24 dB/s (%.2f -> %.2f)", in0, c->m_in);
    drop(c);

    c = make();
    CHECK(strcmp(get(c, "gr"), "0.0 dB in --") == 0, "fresh instance reads silence, got \"%s\"", get(c, "gr"));
    drop(c);
}

static void test_params_and_wire(void) {
    printf("parameters, enum wire, clamping\n");
    comp_t *c = make();
    set(c, "sc_hpf", "100 Hz");
    CHECK(strcmp(get(c, "sc_hpf"), "2") == 0, "enum by name");
    set(c, "sc_hpf", "3");
    CHECK(strcmp(get(c, "sc_hpf"), "3") == 0, "enum by index");
    set(c, "sc_hpf", "2.0");
    CHECK(strcmp(get(c, "sc_hpf"), "2") == 0, "enum by float index");
    set(c, "sc_hpf", "bogus");
    set(c, "sc_hpf", "9");
    set(c, "sc_hpf", "-1");
    CHECK(strcmp(get(c, "sc_hpf"), "2") == 0, "invalid enum writes are refused");

    set(c, "threshold", "-24.5");
    CHECK(NEAR(atof(get(c, "threshold")), -24.5, 1e-6), "float write");
    set(c, "threshold", "abc");
    CHECK(NEAR(atof(get(c, "threshold")), -24.5, 1e-6), "non-numeric float write refused");
    set(c, "threshold", "nan");
    set(c, "threshold", "inf");
    set(c, "threshold", "-inf");
    CHECK(NEAR(atof(get(c, "threshold")), -24.5, 1e-6), "nan / inf writes refused");
    set(c, "threshold", "-100");
    CHECK(NEAR(atof(get(c, "threshold")), -60.0, 1e-6), "clamped to min");
    set(c, "ratio", "100");
    CHECK(NEAR(atof(get(c, "ratio")), 20.0, 1e-6), "clamped to max");
    set(c, "attack", "0");
    CHECK(NEAR(atof(get(c, "attack")), 0.1, 1e-6), "attack has a floor");

    /* Writes to the readout and the page key are ignored, not crashes. */
    set(c, "gr", "5");
    set(c, "curve", "1");
    set(c, "nonsense", "1");
    CHECK(get(c, "nonsense")[0] == '\0', "unknown keys answer nothing");

    char small[8];
    CHECK(api()->get_param(c, "chain_params", small, sizeof small) == -1, "too-small buffer is refused");
    CHECK(api()->get_param(c, "state", small, sizeof small) == -1, "too-small state buffer is refused");
    CHECK(api()->get_param(c, "gr", small, 4) == -1, "too-small meter buffer is refused");

    api()->process_block(c, NULL, 128);
    api()->process_block(c, (int16_t *)small, 0);
    api()->set_param(c, NULL, "1");
    api()->set_param(c, "ratio", NULL);
    CHECK(1, "null / empty calls survive");
    drop(c);
}

static void test_state_and_presets(void) {
    printf("state round-trip and presets\n");
    comp_t *a = make();
    set(a, "preset", "4");
    CHECK(strcmp(get(a, "preset_name"), PRESETS[4].name) == 0, "preset name follows the index");
    setf(a, "threshold", -33.5);
    setf(a, "input", 7.5);
    set(a, "detect", "RMS");
    set(a, "auto_makeup", "On");
    char blob[2048];
    snprintf(blob, sizeof blob, "%s", get(a, "state"));

    comp_t *b = make();
    set(b, "state", blob);
    int same = 1;
    for (int i = 0; i < N_PARAMS; i++) {
        char va[64], vb[64];
        snprintf(va, sizeof va, "%s", get(a, PARAMS[i].key));
        snprintf(vb, sizeof vb, "%s", get(b, PARAMS[i].key));
        if (strcmp(va, vb) != 0) { same = 0; printf("    %s: %s vs %s\n", PARAMS[i].key, va, vb); }
    }
    CHECK(same, "every parameter survives state save/restore");
    CHECK(strcmp(get(b, "preset"), "4") == 0, "preset index restored as a label");
    CHECK(NEAR(atof(get(b, "threshold")), -33.5, 1e-3), "restoring the preset label does not re-apply it");
    drop(a); drop(b);

    b = make();
    set(b, "state", "{\"sc_hpf\":\"150 Hz\",\"detect\":\"RMS\",\"ratio\":4}");
    CHECK(strcmp(get(b, "sc_hpf"), "3") == 0 && strcmp(get(b, "detect"), "1") == 0,
          "enums restore from names too");
    CHECK(NEAR(atof(get(b, "ratio")), 4.0, 1e-6), "partial state leaves the rest alone");
    drop(b);

    comp_t *c = make();
    CHECK(atoi(get(c, "preset_count")) == N_PRESETS && N_PRESETS >= 8, "preset count");
    setf(c, "input", 5.0);
    int in_range = 1, names_ok = 1;
    for (int p = 0; p < N_PRESETS; p++) {
        char idx[8]; snprintf(idx, sizeof idx, "%d", p);
        set(c, "preset", idx);
        if (atoi(get(c, "preset")) != p) names_ok = 0;
        if (!PRESETS[p].name[0] || strlen(PRESETS[p].name) > 16) names_ok = 0;
        for (int q = 0; q < p; q++) if (strcmp(PRESETS[p].name, PRESETS[q].name) == 0) names_ok = 0;
        for (int i = 0; i < N_PARAMS; i++) {
            const param_def_t *d = &PARAMS[i];
            double v = atof(get(c, d->key));
            if (v < d->min - 1e-6 || v > d->max + 1e-6) { in_range = 0; printf("    %s.%s = %g\n", PRESETS[p].name, d->key, v); }
        }
        if (!NEAR(atof(get(c, "input")), 5.0, 1e-6)) in_range = 0;
    }
    CHECK(names_ok, "preset names are unique, short and selectable");
    CHECK(in_range, "every preset is inside every declared range, and keeps Input");
    set(c, "preset", "1");
    set(c, "preset", "99");
    set(c, "preset", "-1");
    set(c, "preset", "x");
    CHECK(strcmp(get(c, "preset"), "1") == 0, "invalid preset indices are refused");
    drop(c);
}

static void test_smoothing_and_safety(void) {
    printf("click-free changes, clipping, denormals\n");
    /* A +12 dB makeup jump ramps instead of stepping. */
    comp_t *c = make_plain(0, 2, 5, 100);
    run_sine(c, 1000.0, -30.0, 0.2, 0.1);
    setf(c, "makeup", 12);
    double after1ms = run_sine(c, 1000.0, -30.0, 128.0 * 1 / 44100.0, 128.0 / 44100.0);
    double after100 = run_sine(c, 1000.0, -30.0, 0.1, 0.02);
    CHECK(after1ms < -26.0 && after1ms > -30.0 + 0.5, "makeup ramps (%.2f dBFS one block after the change)", after1ms);
    CHECK(NEAR(after100, -18.0, 0.2), "and arrives (%.2f dBFS after 100 ms)", after100);
    drop(c);

    /* Full-scale input, +24 dB makeup, no compression: clips, never wraps. */
    c = make_plain(0, 1, 5, 100);
    setf(c, "makeup", 24);
    int16_t in[BLOCK * 2], out[BLOCK * 2];
    int sane = 1;
    g_phase = 0;
    for (int b = 0; b < 100; b++) {
        sine_block(in, 100.0, 1.0);
        memcpy(out, in, sizeof in);
        api()->process_block(c, out, BLOCK);
        for (int i = 0; i < BLOCK * 2; i++)
            if ((in[i] > 0 && out[i] < 0) || (in[i] < 0 && out[i] > 0)) sane = 0;
    }
    CHECK(sane, "hard clipping keeps the sign of every sample");
    drop(c);

    /* A long silence after a loud burst leaves no denormal state behind. */
    c = make_plain(-20, 4, 1, 50);
    set(c, "sc_hpf", "100 Hz");
    set(c, "detect", "RMS");
    run_sine(c, 200.0, -3.0, 0.5, 0.1);
    run_sine(c, 200.0, -200.0, 20.0, 0.1);
    CHECK(c->ms == 0.0f && c->hz[0][0] == 0.0f && c->hz[0][1] == 0.0f &&
          c->hz[1][0] == 0.0f && c->hz[1][1] == 0.0f,
          "detector and filter state flush to zero (ms=%g z=%g)", c->ms, c->hz[0][0]);
    CHECK(isfinite(c->gr_db) && c->gr_db == 0.0f, "reduction returns to exactly zero");
    drop(c);
}

static void bench(void) {
    comp_t *c = make();
    set(c, "preset", "1");          /* RMS + HPF: the expensive path */
    int16_t buf[BLOCK * 2];
    g_phase = 0;
    sine_block(buf, 440.0, 0.5);
    struct timespec t0, t1;
    const int N = 20000;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < N; i++) api()->process_block(c, buf, BLOCK);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double us = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / 1e3 / N;
    printf("bench: %.2f us per 128-frame block on this machine (budget ~2900 us)\n", us);
    drop(c);
}

static void dump_curve(void) {
    /* For the canvas.js cross-check: the JS curve must match this one. */
    const float thr[] = { -18.0f, -40.0f, -6.0f };
    const float rat[] = { 3.0f, 20.0f, 1.5f };
    const float kn[]  = { 6.0f, 0.0f, 24.0f };
    printf("[");
    int first = 1;
    for (int s = 0; s < 3; s++)
        for (float x = -60.0f; x <= 0.0f; x += 1.5f) {
            printf("%s{\"x\":%.2f,\"t\":%.1f,\"r\":%.1f,\"k\":%.1f,\"g\":%.6f}", first ? "" : ",",
                   (double)x, (double)thr[s], (double)rat[s], (double)kn[s],
                   (double)gain_computer(x, thr[s], kn[s], 1.0f / rat[s] - 1.0f));
            first = 0;
        }
    printf("]\n");
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--dump-contract") == 0) {
        comp_t *c = make();
        printf("%s\n", get(c, "chain_params"));
        printf("%s\n", get(c, "ui_hierarchy"));
        drop(c);
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--dump-curve") == 0) { dump_curve(); return 0; }

    test_gain_computer();
    test_passthrough_below_threshold();
    test_steady_state();
    test_time_constants();
    test_mix_makeup_input();
    test_sidechain_hpf();
    test_meter();
    test_params_and_wire();
    test_state_and_presets();
    test_smoothing_and_safety();
    bench();

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
