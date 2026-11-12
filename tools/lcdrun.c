#define _POSIX_C_SOURCE 200809L
/* Headless runner for the built core: loads content through the Libretro API,
   applies a button script, reports timing and optionally dumps PPM frames.
   Usage: lcdrun core.so content.zip [frames] [dump_every] [script]
   script: comma separated "frame-frame:BUTTON" ranges, e.g. "60-70:START,100-400:LEFT" */
#include "libretro.h"
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static unsigned frame_no, dump_every;
static const char *script;
static size_t audio_frames;
static long audio_edges;
static int16_t last_sample;
static unsigned format = RETRO_PIXEL_FORMAT_XRGB8888;

static const struct { const char *name; unsigned id; } names[] = {
    {"B", RETRO_DEVICE_ID_JOYPAD_B}, {"Y", RETRO_DEVICE_ID_JOYPAD_Y}, {"SELECT", RETRO_DEVICE_ID_JOYPAD_SELECT},
    {"START", RETRO_DEVICE_ID_JOYPAD_START}, {"UP", RETRO_DEVICE_ID_JOYPAD_UP}, {"DOWN", RETRO_DEVICE_ID_JOYPAD_DOWN},
    {"LEFT", RETRO_DEVICE_ID_JOYPAD_LEFT}, {"RIGHT", RETRO_DEVICE_ID_JOYPAD_RIGHT}, {"A", RETRO_DEVICE_ID_JOYPAD_A},
    {"X", RETRO_DEVICE_ID_JOYPAD_X}, {"L", RETRO_DEVICE_ID_JOYPAD_L}, {"R", RETRO_DEVICE_ID_JOYPAD_R},
    {"R3", RETRO_DEVICE_ID_JOYPAD_R3}};

static int pressed(unsigned id)
{
    const char *p = script;
    while (p && *p) {
        unsigned a, b, i;
        char btn[16];
        if (sscanf(p, "%u-%u:%15[A-Z0-9]", &a, &b, btn) == 3 && frame_no >= a && frame_no <= b)
            for (i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
                if (!strcmp(btn, names[i].name) && names[i].id == id)
                    return 1;
        p = strchr(p, ',');
        if (p)
            p++;
    }
    return 0;
}

static void log_cb(enum retro_log_level level, const char *fmt, ...)
{
    va_list ap;
    if (level < RETRO_LOG_INFO)
        return;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static bool env(unsigned cmd, void *data)
{
    if (cmd == RETRO_ENVIRONMENT_SET_PIXEL_FORMAT) {
        format = *(enum retro_pixel_format *)data;
        return format == RETRO_PIXEL_FORMAT_XRGB8888;
    }
    if (cmd == RETRO_ENVIRONMENT_GET_VARIABLE) {
        /* core options come from the environment, e.g. lcd_lab_condition=Worn */
        struct retro_variable *v = data;
        v->value = getenv(v->key);
        return v->value != NULL;
    }
    if (cmd == RETRO_ENVIRONMENT_GET_LOG_INTERFACE) {
        ((struct retro_log_callback *)data)->log = log_cb;
        return true;
    }
    if (cmd == RETRO_ENVIRONMENT_SET_MESSAGE) {
        fprintf(stderr, "message: %s\n", ((const struct retro_message *)data)->msg);
        return true;
    }
    return cmd == RETRO_ENVIRONMENT_SET_VARIABLES || cmd == RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS ||
           cmd == RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME;
}

static void video(const void *data, unsigned w, unsigned h, size_t pitch)
{
    char path[64];
    FILE *f;
    unsigned x, y;
    if (!data || !dump_every || frame_no % dump_every)
        return;
    snprintf(path, sizeof(path), "frame_%05u.ppm", frame_no);
    f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (y = 0; y < h; ++y)
        for (x = 0; x < w; ++x) {
            uint32_t v = ((const uint32_t *)((const uint8_t *)data + y * pitch))[x];
            unsigned char c[3] = {(unsigned char)(v >> 16), (unsigned char)(v >> 8), (unsigned char)v};
            fwrite(c, 1, 3, f);
        }
    fclose(f);
}

static size_t audio(const int16_t *d, size_t n)
{
    size_t i;
    for (i = 0; i < n; ++i) {
        if ((d[i * 2] > 1000) != (last_sample > 1000))
            audio_edges++;
        last_sample = d[i * 2];
    }
    audio_frames += n;
    return n;
}

static void poll(void) {}
static int16_t input(unsigned port, unsigned dev, unsigned idx, unsigned id)
{
    (void)dev;
    (void)idx;
    return port == 0 && pressed(id);
}


#define SYM(name)       \
    name##_t name;      \
    *(void **)(&name) = dlsym(core, #name)
typedef void (*retro_set_environment_t)(retro_environment_t);
typedef void (*retro_set_video_refresh_t)(retro_video_refresh_t);
typedef void (*retro_set_audio_sample_batch_t)(retro_audio_sample_batch_t);
typedef void (*retro_set_input_poll_t)(retro_input_poll_t);
typedef void (*retro_set_input_state_t)(retro_input_state_t);
typedef void (*retro_init_t)(void);
typedef bool (*retro_load_game_t)(const struct retro_game_info *);
typedef void (*retro_run_t)(void);
typedef void (*retro_get_system_av_info_t)(struct retro_system_av_info *);

int main(int argc, char **argv)
{
    void *core;
    struct retro_game_info info;
    struct retro_system_av_info av;
    unsigned frames = argc > 3 ? (unsigned)atoi(argv[3]) : 600;
    struct timespec t0, t1;
    double worst = 0, total = 0;
    unsigned slow = 0, slow_first = 0;
    if (argc < 3) {
        fprintf(stderr, "usage: %s core.so content [frames] [dump_every] [script]\n", argv[0]);
        return 2;
    }
    dump_every = argc > 4 ? (unsigned)atoi(argv[4]) : 0;
    script = argc > 5 ? argv[5] : NULL;
    core = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!core) {
        fprintf(stderr, "%s\n", dlerror());
        return 1;
    }
    {
        SYM(retro_set_environment);
        SYM(retro_set_video_refresh);
        SYM(retro_set_audio_sample_batch);
        SYM(retro_set_input_poll);
        SYM(retro_set_input_state);
        SYM(retro_init);
        SYM(retro_load_game);
        SYM(retro_run);
        SYM(retro_get_system_av_info);
        retro_set_environment(env);
        retro_set_video_refresh(video);
        retro_set_audio_sample_batch(audio);
        retro_set_input_poll(poll);
        retro_set_input_state(input);
        retro_init();
        memset(&info, 0, sizeof(info));
        info.path = argv[2];
        clock_gettime(CLOCK_MONOTONIC, &t0);
        if (!retro_load_game(&info)) {
            fprintf(stderr, "load failed\n");
            return 1;
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        printf("load: %.1f ms\n", (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);
        retro_get_system_av_info(&av);
        printf("video: %ux%u @ %.0f fps, audio %.0f Hz\n", av.geometry.base_width, av.geometry.base_height,
               av.timing.fps, av.timing.sample_rate);
        for (frame_no = 0; frame_no < frames; ++frame_no) {
            double ms;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            retro_run();
            clock_gettime(CLOCK_MONOTONIC, &t1);
            ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
            total += ms;
            if (ms > worst)
                worst = ms;
            if (ms > 1000.0 / 60.0 && !slow++)
                slow_first = frame_no;
        }
    }
    printf("frames: %u, retro_run avg %.3f ms, worst %.3f ms, over 16.7 ms: %u (first at frame %u), audio frames %zu, "
           "piezo edges %ld\n", frames, total / frames, worst, slow, slow_first, audio_frames, audio_edges);
    return 0;
}
