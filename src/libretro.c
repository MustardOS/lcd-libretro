#include "libretro.h"
#include "content/loader.h"
#include "content/samples.h"
#include "diagnostics/audio.h"
#include "diagnostics/display.h"
#include "diagnostics/game.h"
#include "hardware/machine.h"
#include "renderer/lcd_render.h"
#include "util/hash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CORE_VERSION "0.4.7"
#define STATE_MAGIC UINT32_C(0x4C434453) /* LCDS */
#define STATE_VERSION 5u
#define FPS 60u
#define MAX_TICKS_PER_FRAME (LCD_OSC_HZ / FPS + 1u)

typedef enum { MODE_NONE, MODE_DIAGNOSTIC, MODE_MACHINE } Mode;

static retro_environment_t environment_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_t audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static retro_log_printf_t log_cb;

static Mode mode;
static bool rgb565;
static bool can_dupe;

static LCDGame game_state;
static LCDAudio sound_state;
static LCDOptions diag_options = {1u, true, true, false};
static int16_t diag_audio[LCD_SAMPLES_PER_FRAME * 2u];

static LCDContent content;
static LCDMachine machine;
static LCDRender render;
static LCDRenderOptions render_options;
static bool sound_enabled = true;
static uint32_t tick_remainder;
static int32_t dc_in, dc_out;
static unsigned frames_since_real;
static bool use_background = true;
static bool use_rtc = true;
static unsigned frames_since_load;
static uint8_t levels[MAX_TICKS_PER_FRAME];
static int16_t sample_audio[MAX_TICKS_PER_FRAME];
static LCDSamples samples;
static char system_dir[512];
static int16_t machine_audio[MAX_TICKS_PER_FRAME * 2u];

static const struct retro_variable core_options[] = {
    {"lcd_lab_rtc", "Real-time clock (supported games); Enabled|Disabled"},
    {"lcd_lab_background", "Background image (background.png); Enabled|Disabled"},
    {"lcd_lab_resolution", "Display size; 640x480|720x720|800x600|960x720|1280x960|1920x1440|480x320|320x240"},
    {"lcd_lab_condition", "LCD condition; Perfect|Good|Worn|Bad"},
    {"lcd_lab_tint", "LCD tint; MAME grey|Neutral|Green|Amber"},
    {"lcd_lab_gap", "Dual screen gap; MAME|None"},
    {"lcd_lab_sound", "Piezo sound; Enabled|Disabled"},
    {"lcd_lab_speed", "Test game speed; Normal|Fast"},
    {NULL, NULL}};

static const struct retro_input_descriptor diag_descriptors[] = {
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "Move left"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Move right"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start / restart"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "Start / restart"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Pause"},
    {0, 0, 0, 0, NULL}};

static void notify(int level, const char *message)
{
    if (log_cb)
        log_cb(level, "LCD Lab: %s\n", message);
    if (environment_cb) {
        struct retro_message m = {message, 360};
        environment_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &m);
    }
}

static const char *get_option(const char *name)
{
    struct retro_variable v = {name, NULL};
    if (environment_cb && environment_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &v))
        return v.value;
    return NULL;
}

static bool option_is(const char *name, const char *value)
{
    const char *v = get_option(name);
    return v && strcmp(v, value) == 0;
}

static void read_options(void)
{
    const char *v = get_option("lcd_lab_resolution");
    unsigned w = 640, h = 480;
    if (v && sscanf(v, "%ux%u", &w, &h) != 2) {
        w = 640;
        h = 480;
    }
    render_options.box_w = w;
    render_options.box_h = h;
    {
        /* screen quality and battery: contrast %, unlit segment visibility %, response per frame */
        static const struct {
            const char *name;
            uint8_t contrast, ghost, rise, fall;
        } conditions[] = {
            {"Perfect", 100, 0, 255, 255},
            {"Good", 95, 3, 128, 64},
            {"Worn", 85, 8, 64, 24},
            {"Bad", 70, 14, 40, 10},
        };
        unsigned c = 0, k;
        for (k = 0; k < sizeof(conditions) / sizeof(conditions[0]); ++k)
            if (option_is("lcd_lab_condition", conditions[k].name))
                c = k;
        render_options.contrast = conditions[c].contrast;
        render_options.ghost = conditions[c].ghost;
        render_options.rise = conditions[c].rise;
        render_options.fall = conditions[c].fall;
    }
    render_options.tint = option_is("lcd_lab_tint", "Neutral") ? UINT32_C(0xffffff)
                          : option_is("lcd_lab_tint", "Green") ? UINT32_C(0xc2d1ae)
                          : option_is("lcd_lab_tint", "Amber") ? UINT32_C(0xe6d6aa)
                                                                : UINT32_C(0xb3b5b8);
    render_options.fade = false;
    render_options.gap = option_is("lcd_lab_gap", "None") ? 0u : 40u;
    sound_enabled = !option_is("lcd_lab_sound", "Disabled");
    use_background = !option_is("lcd_lab_background", "Disabled");
    use_rtc = !option_is("lcd_lab_rtc", "Disabled");

    diag_options.contrast = 1u;
    diag_options.ghost = !option_is("lcd_lab_condition", "Perfect") && get_option("lcd_lab_condition") != NULL;
    diag_options.sound = sound_enabled;
    diag_options.fast = option_is("lcd_lab_speed", "Fast");
}

unsigned retro_api_version(void) { return RETRO_API_VERSION; }

void retro_set_environment(retro_environment_t cb)
{
    bool yes = true;
    environment_cb = cb;
    if (cb) {
        cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &yes);
        cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void *)core_options);
    }
}

void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { audio_cb = cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

void retro_init(void)
{
    struct retro_log_callback logger;
    if (environment_cb && environment_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logger))
        log_cb = logger.log;
    lcd_display_init();
    lcd_game_reset(&game_state);
    memset(&sound_state, 0, sizeof(sound_state));
    mode = MODE_NONE;
    rgb565 = false;
    read_options();
}

static unsigned video_width(void) { return render.width; }
static unsigned video_height(void) { return render.height; }

void retro_deinit(void)
{
    lcd_render_free(&render);
    lcd_content_free(&content);
    lcd_samples_free(&samples);
    mode = MODE_NONE;
    log_cb = NULL;
}

void retro_get_system_info(struct retro_system_info *info)
{
    if (!info)
        return;
    memset(info, 0, sizeof(*info));
    info->library_name = "LCD Lab";
    info->library_version = CORE_VERSION;
    info->valid_extensions = "zip|gnw|lcd";
    info->need_fullpath = false;
    info->block_extract = true;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
    if (!info)
        return;
    memset(info, 0, sizeof(*info));
    info->timing.fps = (double)FPS;
    if (mode == MODE_MACHINE) {
        info->geometry.base_width = video_width();
        info->geometry.base_height = video_height();
        info->geometry.max_width = video_width();
        info->geometry.max_height = video_height();
        info->geometry.aspect_ratio = (float)video_width() / (float)video_height();
        info->timing.sample_rate = (double)LCD_OSC_HZ;
    } else {
        info->geometry.base_width = LCD_WIDTH;
        info->geometry.base_height = LCD_HEIGHT;
        info->geometry.max_width = LCD_WIDTH;
        info->geometry.max_height = LCD_HEIGHT;
        info->geometry.aspect_ratio = 4.0f / 3.0f;
        info->timing.sample_rate = (double)LCD_SAMPLE_RATE;
    }
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
    (void)port;
    (void)device;
}

void retro_reset(void)
{
    if (mode == MODE_MACHINE) {
        lcd_machine_soft_reset(&machine);
        return;
    }
    lcd_game_reset(&game_state);
    memset(&sound_state, 0, sizeof(sound_state));
}

static uint32_t joypad_mask(void)
{
    uint32_t mask = 0;
    unsigned id;
    if (!input_state_cb)
        return 0;
    for (id = 0; id <= RETRO_DEVICE_ID_JOYPAD_R3; ++id)
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, id))
            mask |= UINT32_C(1) << id;
    return mask;
}

static uint32_t diag_buttons(uint32_t pad)
{
    uint32_t result = 0;
    if (pad >> RETRO_DEVICE_ID_JOYPAD_LEFT & 1) result |= LCD_BTN_LEFT;
    if (pad >> RETRO_DEVICE_ID_JOYPAD_RIGHT & 1) result |= LCD_BTN_RIGHT;
    if (pad >> RETRO_DEVICE_ID_JOYPAD_START & 1) result |= LCD_BTN_START;
    if (pad >> RETRO_DEVICE_ID_JOYPAD_A & 1) result |= LCD_BTN_ACTION;
    if (pad >> RETRO_DEVICE_ID_JOYPAD_SELECT & 1) result |= LCD_BTN_PAUSE;
    return result;
}

static void push_audio(const int16_t *buffer, size_t frames)
{
    size_t done = 0;
    if (audio_batch_cb)
        while (done < frames) {
            size_t n = audio_batch_cb(buffer + done * 2u, frames - done);
            if (!n)
                break;
            done += n;
        }
    if (audio_cb)
        for (; done < frames; ++done)
            audio_cb(buffer[2u * done], buffer[2u * done + 1u]);
}

static int build_render(char *err, size_t err_len)
{
    LCDSvgSource svgs[LCD_MAX_SCREENS];
    LCDRenderOptions opt = render_options;
    unsigned i;
    for (i = 0; i < content.def->screen_count; ++i) {
        svgs[i].data = content.svg[i];
        svgs[i].size = content.svg_size[i];
    }
    lcd_render_free(&render);
    if (content.gnw)
        return lcd_render_build_gnw(&render, content.gnw, &render_options, rgb565, err, err_len);
    opt.background = use_background ? content.background : NULL;
    opt.background_w = content.background_w;
    opt.background_h = content.background_h;
    /* LED displays keep their own colours */
    if (content.def->quirks & LCD_QUIRK_LED)
        opt.tint = UINT32_C(0xffffff);
    return lcd_render_build(&render, content.def, svgs, content.def->screen_count, &opt, rgb565, err, err_len);
}

static void apply_option_changes(void)
{
    bool updated = false;
    unsigned old_w = video_width(), old_h = video_height();
    bool old_background = use_background;
    LCDRenderOptions old = render_options;
    char err[160];
    if (!environment_cb || !environment_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) || !updated)
        return;
    read_options();
    if (mode != MODE_MACHINE || (memcmp(&old, &render_options, sizeof(old)) == 0 && old_background == use_background))
        return;
    if (build_render(err, sizeof(err)) != 0) {
        notify(RETRO_LOG_ERROR, err);
        render_options = old;
        use_background = old_background;
        if (build_render(err, sizeof(err)) != 0) {
            mode = MODE_NONE;
            return;
        }
    }
    if (video_width() != old_w || video_height() != old_h) {
        struct retro_system_av_info av;
        retro_get_system_av_info(&av);
        environment_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &av);
    }
}

/* host local time into the game's clock RAM */
static void sync_clock(void)
{
    time_t now = time(NULL);
    const struct tm *t = localtime(&now);
    if (use_rtc && t && lcd_machine_set_clock(&machine, (unsigned)t->tm_hour, (unsigned)t->tm_min) && log_cb)
        log_cb(RETRO_LOG_INFO, "LCD Lab: clock set to %02d:%02d\n", t->tm_hour, t->tm_min);
}

static void start_clock(void)
{
    const LCDRtcDef *rtc = lcd_registry_rtc(content.def->id);
    if (!use_rtc || !rtc || !rtc->present || !lcd_machine_boot_to_clock(&machine))
        return;
    sync_clock();
    /* let the program redraw and the LCD settle so the first frame shows the new time */
    lcd_machine_run(&machine, LCD_OSC_HZ + LCD_OSC_HZ / 4u, NULL);
}

static void run_machine(uint32_t pad)
{
    unsigned ticks, i;
    bool changed;
    const void *frame;
    uint8_t keys[LCD_MAX_INPUTS];
    const LCDInputDef *inputs = content.def->inputs;

    for (i = 0; i < LCD_MAX_INPUTS; ++i)
        keys[i] = (uint8_t)(inputs[i].label && inputs[i].key && input_state_cb &&
                            input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, inputs[i].key));
    lcd_machine_set_input(&machine, pad, keys);
    if (frames_since_load < UINT32_MAX)
        frames_since_load++;
    tick_remainder += LCD_OSC_HZ;
    ticks = tick_remainder / FPS;
    tick_remainder %= FPS;
    lcd_machine_run_pcm(&machine, ticks, levels, sample_audio);

    for (i = 0; i < ticks; ++i) {
        /* piezo level through a DC blocking high-pass, Q15; samples at twice the piezo's level, as MAME mixes them */
        int32_t in = sound_enabled ? levels[i] * 32 + sample_audio[i] / 2 : 0;
        int32_t out = in - dc_in + (int32_t)(((int64_t)dc_out * 32604) / 32768);
        int16_t s;
        dc_in = in;
        dc_out = out;
        s = (int16_t)(out > 32767 ? 32767 : out < -32768 ? -32768 : out);
        machine_audio[i * 2u] = s;
        machine_audio[i * 2u + 1u] = s;
    }
    push_audio(machine_audio, ticks);

    frame = lcd_render_frame(&render, &machine, &changed);
    /* duplicates only for a while: frontends that drop frames (warm-up, menus) get a real one again */
    if (changed || !can_dupe || ++frames_since_real >= 30u) {
        frames_since_real = 0;
    } else {
        frame = NULL;
    }
    if (video_cb)
        video_cb(frame, video_width(), video_height(),
                 video_width() * (rgb565 ? sizeof(uint16_t) : sizeof(uint32_t)));
}

static void run_diagnostic(uint32_t pad)
{
    unsigned event = lcd_game_frame(&game_state, diag_buttons(pad), diag_options.fast);
    const void *frame;
    if (event)
        lcd_audio_start(&sound_state, event);
    lcd_audio_render(&sound_state, diag_audio, diag_options.sound);
    push_audio(diag_audio, LCD_SAMPLES_PER_FRAME);
    frame = lcd_display_render(&game_state, &diag_options, rgb565);
    if (video_cb)
        video_cb(frame, LCD_WIDTH, LCD_HEIGHT, LCD_WIDTH * (rgb565 ? sizeof(uint16_t) : sizeof(uint32_t)));
}

void retro_run(void)
{
    uint32_t pad;
    if (mode == MODE_NONE)
        return;
    apply_option_changes();
    if (input_poll_cb)
        input_poll_cb();
    pad = joypad_mask();
    if (mode == MODE_MACHINE)
        run_machine(pad);
    else if (mode == MODE_DIAGNOSTIC)
        run_diagnostic(pad);
}

/* save states: 20 byte little endian header, then the mode payload */

#define HEADER_SIZE 20u
#define MACHINE_EXTRA 12u

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static size_t payload_size(void)
{
    if (mode == MODE_MACHINE)
        return lcd_machine_state_size() + MACHINE_EXTRA;
    return sizeof(LCDGame) + sizeof(LCDAudio);
}

static uint32_t content_id(void)
{
    const char *id = mode == MODE_MACHINE ? content.def->id : "catch";
    return lcd_crc32(0, id, strlen(id));
}

size_t retro_serialize_size(void) { return mode == MODE_NONE ? 0 : HEADER_SIZE + payload_size(); }

bool retro_serialize(void *data, size_t size)
{
    uint8_t *p = data;
    size_t payload = payload_size();
    if (mode == MODE_NONE || !data || size < HEADER_SIZE + payload)
        return false;
    memset(p, 0, HEADER_SIZE + payload);
    put32(p, STATE_MAGIC);
    put32(p + 4, STATE_VERSION);
    put32(p + 8, (uint32_t)mode);
    put32(p + 12, content_id());
    put32(p + 16, (uint32_t)payload);
    p += HEADER_SIZE;
    if (mode == MODE_MACHINE) {
        size_t ms = lcd_machine_state_size();
        if (!lcd_machine_save(&machine, p, ms))
            return false;
        put32(p + ms, tick_remainder);
        put32(p + ms + 4u, (uint32_t)dc_in);
        put32(p + ms + 8u, (uint32_t)dc_out);
    } else {
        memcpy(p, &game_state, sizeof(game_state));
        memcpy(p + sizeof(game_state), &sound_state, sizeof(sound_state));
    }
    return true;
}

bool retro_unserialize(const void *data, size_t size)
{
    const uint8_t *p = data;
    size_t payload = payload_size();
    if (mode == MODE_NONE || !data || size < HEADER_SIZE + payload || get32(p) != STATE_MAGIC ||
        get32(p + 4) != STATE_VERSION || get32(p + 8) != (uint32_t)mode || get32(p + 12) != content_id() ||
        get32(p + 16) != payload)
        return false;
    p += HEADER_SIZE;
    if (mode == MODE_MACHINE) {
        size_t ms = lcd_machine_state_size();
        uint32_t rem = get32(p + ms);
        if (rem >= FPS || !lcd_machine_load(&machine, p, ms))
            return false;
        tick_remainder = rem;
        dc_in = (int32_t)get32(p + ms + 4u);
        dc_out = (int32_t)get32(p + ms + 8u);
        render.valid_frame = false;
        /* a state restored straight after loading (frontend resume) gets today's time;
           later loads (rewind, manual states) are left exactly as saved */
        if (frames_since_load < 300u)
            sync_clock();
    } else {
        LCDGame g;
        memcpy(&g, p, sizeof(g));
        if (g.lane >= LCD_LANES || g.misses > 3u || g.mode > LCD_FINISHED || g.paused > 1u ||
            g.left_repeat > 250u || g.right_repeat > 250u)
            return false;
        game_state = g;
        memcpy(&sound_state, p + sizeof(g), sizeof(sound_state));
    }
    return true;
}

static bool negotiate_pixel_format(void)
{
    enum retro_pixel_format format = RETRO_PIXEL_FORMAT_XRGB8888;
    rgb565 = false;
    if (!environment_cb || environment_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format))
        return true;
    format = RETRO_PIXEL_FORMAT_RGB565;
    if (environment_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format)) {
        rgb565 = true;
        return true;
    }
    notify(RETRO_LOG_ERROR, "Frontend must support XRGB8888 or RGB565.");
    return false;
}

static void set_machine_descriptors(const LCDMachineDef *def)
{
    struct retro_input_descriptor desc[LCD_MAX_INPUTS + 1];
    unsigned i, n = 0;
    memset(desc, 0, sizeof(desc));
    for (i = 0; i < LCD_MAX_INPUTS && def->inputs[i].label; ++i) {
        if (def->inputs[i].retro_id == LCD_NO_PAD)
            continue;
        desc[n].device = RETRO_DEVICE_JOYPAD;
        desc[n].id = def->inputs[i].retro_id;
        desc[n].description = def->inputs[i].label;
        n++;
    }
    if (environment_cb)
        environment_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);
}

/* MAME sample pack: <content dir>/samples/<id>.zip or <system>/lcd_lab/samples/<id>.zip */
static void load_samples(const char *path)
{
    char paths[4][1024];
    const char *list[4];
    const char *slash = path ? strrchr(path, '/') : NULL;
    const char *id = content.def->id;
    unsigned n = 0, i;
    if (slash)
        snprintf(paths[n++], sizeof(paths[0]), "%.*s/samples/%s.zip", (int)(slash - path), path, id);
    if (system_dir[0]) {
        snprintf(paths[n++], sizeof(paths[0]), "%s/lcd_lab/samples/%s.zip", system_dir, id);
        snprintf(paths[n++], sizeof(paths[0]), "%s/mame/samples/%s.zip", system_dir, id);
        snprintf(paths[n++], sizeof(paths[0]), "%s/samples/%s.zip", system_dir, id);
    }
    for (i = 0; i < n; ++i)
        list[i] = paths[i];
    if (lcd_samples_load(&samples, content.def->samples, list, n)) {
        lcd_machine_attach_samples(&machine, &samples.bank);
        if (log_cb)
            log_cb(RETRO_LOG_INFO, "LCD Lab: %u voice samples loaded for %s\n", samples.loaded, id);
    } else {
        lcd_machine_attach_samples(&machine, NULL);
        notify(RETRO_LOG_INFO, "No MAME sample pack (samples/<set>.zip): voice clips are silent, as in MAME");
    }
}

static bool start_diagnostic(void)
{
    if (environment_cb)
        environment_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, (void *)diag_descriptors);
    memset(&game_state, 0, sizeof(game_state));
    lcd_game_reset(&game_state);
    memset(&sound_state, 0, sizeof(sound_state));
    mode = MODE_DIAGNOSTIC;
    return true;
}

bool retro_load_game(const struct retro_game_info *info)
{
    char err[160];
    const char *sys = NULL;
    mode = MODE_NONE;
    lcd_render_free(&render);
    lcd_content_free(&content);
    lcd_samples_free(&samples);
    read_options();
    system_dir[0] = '\0';
    if (environment_cb && environment_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &sys) && sys)
        snprintf(system_dir, sizeof(system_dir), "%s", sys);
    lcd_content_set_system_dir(system_dir[0] ? system_dir : NULL);
    if (!negotiate_pixel_format())
        return false;
    can_dupe = false;
    if (environment_cb)
        environment_cb(RETRO_ENVIRONMENT_GET_CAN_DUPE, &can_dupe);

    if (!info)
        return start_diagnostic();
    if (lcd_content_load(&content, info->path, info->data, info->size) != 0) {
        notify(RETRO_LOG_ERROR, content.message);
        return false;
    }
    if (content.kind == LCD_CONTENT_DIAGNOSTIC) {
        lcd_content_free(&content);
        return start_diagnostic();
    }
    if (content.message[0])
        notify(RETRO_LOG_WARN, content.message);
    if (lcd_machine_init(&machine, content.def, content.rom, content.rom_size) != 0) {
        notify(RETRO_LOG_ERROR, "Machine initialisation failed");
        lcd_content_free(&content);
        return false;
    }
    lcd_machine_attach_melody(&machine, content.melody, content.melody_size);
    if (content.def->quirks & LCD_QUIRK_ADPCM)
        load_samples(info->path);
    if (build_render(err, sizeof(err)) != 0) {
        notify(RETRO_LOG_ERROR, err);
        lcd_content_free(&content);
        return false;
    }
    set_machine_descriptors(content.def);
    frames_since_load = 0;
    start_clock();
    tick_remainder = 0;
    dc_in = dc_out = 0;
    mode = MODE_MACHINE;
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "LCD Lab: loaded %s (%s), %ux%u%s\n", content.def->title, content.def->id,
               video_width(), video_height(), content.background ? " with background.png" : "");
    return true;
}

bool retro_load_game_special(unsigned game_type, const struct retro_game_info *info, size_t num_info)
{
    (void)game_type;
    (void)info;
    (void)num_info;
    return false;
}

void retro_unload_game(void)
{
    lcd_render_free(&render);
    lcd_content_free(&content);
    lcd_samples_free(&samples);
    mode = MODE_NONE;
}

unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }

void *retro_get_memory_data(unsigned id)
{
    return mode == MODE_MACHINE && id == RETRO_MEMORY_SYSTEM_RAM ? machine.cpu.ram : NULL;
}

size_t retro_get_memory_size(unsigned id)
{
    return mode == MODE_MACHINE && id == RETRO_MEMORY_SYSTEM_RAM ? sizeof(machine.cpu.ram) : 0u;
}

void retro_cheat_reset(void) {}

void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
    (void)index;
    (void)enabled;
    (void)code;
}
