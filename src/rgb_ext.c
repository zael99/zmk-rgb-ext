/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>

#include <math.h>
#include <stdlib.h>

#include <zephyr/logging/log.h>

#include <zephyr/drivers/led_strip.h>
#include <drivers/ext_power.h>

#include <zmk/rgb_ext.h>

#include <zmk/activity.h>
#include <zmk/usb.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/workqueue.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* ====== Build Checks ====== */

#if !DT_HAS_CHOSEN(zmk_rgb_ext)
#error "A zmk,rgb-ext chosen node must be declared"
#endif

BUILD_ASSERT(CONFIG_ZMK_RGB_EXT_BRT_MIN <= CONFIG_ZMK_RGB_EXT_BRT_MAX, "ERROR: RGB underglow maximum brightness is less than minimum brightness");
/* ====== Build Checks ====== */

/* ====== Defines ====== */
//#define CHOSEN_RGB_EXT DT_CHOSEN(zmk_rgb_ext)
#define STRIP_CHOSEN DT_CHOSEN(zmk_rgb_ext)
#define STRIP_NUM_PIXELS DT_PROP(STRIP_CHOSEN, chain_length)

#define HUE_MAX 360
#define SAT_MAX 100
#define BRT_MAX 100

#define HUE_MAX_MULTIPLIER (1.0f / HUE_MAX)
#define SAT_MAX_MULTIPLIER (1.0f / SAT_MAX)
#define BRT_MAX_MULTIPLIER (1.0f / BRT_MAX)

// 1/256
#define RGB_MULTIPIER 0.0039215686f
/* ====== Defines ====== */

/* ====== Properties ====== */
static const struct device *led_strip;
static struct led_rgb pixels[STRIP_NUM_PIXELS];
static struct rgb_ext_state state;

#if IS_ENABLED(CONFIG_ZMK_RGB_EXT_EXT_POWER)
    static const struct device *const ext_power = DEVICE_DT_GET(DT_INST(0, zmk_ext_power_generic));
#endif
/* ====== Properties ====== */

/* ====== Helper Functions ====== */
static struct zmk_led_hsb hsb_scale_min_max(struct zmk_led_hsb hsb) {
    hsb.b = CONFIG_ZMK_RGB_EXT_BRT_MIN +
            (CONFIG_ZMK_RGB_EXT_BRT_MAX - CONFIG_ZMK_RGB_EXT_BRT_MIN) * hsb.b * BRT_MAX_MULTIPLIER;
    return hsb;
}

static struct zmk_led_hsb hsb_scale_zero_max(struct zmk_led_hsb hsb) {
    hsb.b = hsb.b * CONFIG_ZMK_RGB_EXT_BRT_MAX * BRT_MAX_MULTIPLIER;
    return hsb;
}

static struct zmk_led_hsb rgb_to_hsb(struct led_rgb rgb) {
    float r = rgb.r * RGB_MULTIPIER;
    float g = rgb.g * RGB_MULTIPIER;
    float b = rgb.b * RGB_MULTIPIER;

    float max = (r > g) ? ((r > b) ? r : b) : ((g > b) ? g : b);
    float min = (r < g) ? ((r < b) ? r : b) : ((g < b) ? g : b);
    float delta = max - min;

    float h = 0, s = 0, v = max;

    // Calculate Saturation
    if (max > 0.0f) {
        s = delta / max;
    } else {
        s = 0.0f;
    }

    // Calculate Hue
    if (delta > 0.0f) {
        if (max == r) {
            h = (g - b) / delta + (g < b ? 6 : 0);
        } else if (max == g) {
            h = (b - r) / delta + 2;
        } else {
            h = (r - g) / delta + 4;
        }
        h *= 60.0f;
    } else {
        h = 0.0f;
    }

    struct zmk_led_hsb hsb = {
        .h = (uint16_t)h,
        .s = (uint8_t)(s * SAT_MAX),
        .b = (uint8_t)(v * BRT_MAX)
    };

    return hsb;
}

static struct led_rgb hsb_to_rgb(struct zmk_led_hsb hsb) {
    float r = 0, g = 0, b = 0;

    uint8_t i = hsb.h / 60;
    float v = hsb.b * BRT_MAX_MULTIPLIER;
    float s = hsb.s * SAT_MAX_MULTIPLIER;
    float f = hsb.h * HUE_MAX_MULTIPLIER * 6 - i;
    float p = v * (1 - s);
    float q = v * (1 - f * s);
    float t = v * (1 - (1 - f) * s);

    switch (i % 6) {
        case 0:
            r = v;
            g = t;
            b = p;
            break;
        case 1:
            r = q;
            g = v;
            b = p;
            break;
        case 2:
            r = p;
            g = v;
            b = t;
            break;
        case 3:
            r = p;
            g = q;
            b = v;
            break;
        case 4:
            r = t;
            g = p;
            b = v;
            break;
        case 5:
            r = v;
            g = p;
            b = q;
            break;
    }

    struct led_rgb rgb = {r : r * 255, g : g * 255, b : b * 255};

    return rgb;
}
/* ====== Helper Functions ====== */

/* ====== RGB Effect Functions ====== */
static void set_pixel_rgb_color(int index, struct led_rgb color) {
    if (index > STRIP_NUM_PIXELS) {
        return;
    }

    pixels[index] = color;
}

static void set_solid_rgb_color(struct led_rgb color) {
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        set_pixel_rgb_color(i, color);
    }
}

static void zmk_rgb_effect_solid(void) {
    set_solid_rgb_color(state.color);
}

static void zmk_rgb_effect_breathe(void) {
    struct zmk_led_hsb hsb = rgb_to_hsb(state.color);
    hsb.b = abs(state.animation_step - 1200) / 12;

    set_solid_rgb_color(hsb_to_rgb(hsb_scale_zero_max(hsb)));

    state.animation_step += state.animation_speed * 10;

    if (state.animation_step > 2400) {
        state.animation_step = 0;
    }
}

static void zmk_rgb_effect_spectrum(void) {
    struct zmk_led_hsb hsb = rgb_to_hsb(state.color);
    hsb.h = state.animation_step;

    set_solid_rgb_color(hsb_to_rgb(hsb_scale_zero_max(hsb)));

    state.animation_step += state.animation_speed;
    state.animation_step = state.animation_step % HUE_MAX;
}

static void zmk_rgb_effect_swirl(void) {
    struct zmk_led_hsb hsb = rgb_to_hsb(state.color);

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        hsb.h = (HUE_MAX / STRIP_NUM_PIXELS * i + state.animation_step) % HUE_MAX;
        set_pixel_rgb_color(i, hsb_to_rgb(hsb_scale_min_max(hsb)));
    }
    
    state.animation_step += state.animation_speed * 2;
    state.animation_step = state.animation_step % HUE_MAX;
}

static void zmk_rgb_ext_tick(struct k_work *work) {
    switch (state.current_effect) {
        case RGB_EFFECT_SOLID:
            zmk_rgb_effect_solid();
            break;
        case RGB_EFFECT_BREATHE:
            zmk_rgb_effect_breathe();
            break;
        case RGB_EFFECT_SPECTRUM:
            zmk_rgb_effect_spectrum();
            break;
        case RGB_EFFECT_SWIRL:
            zmk_rgb_effect_swirl();
            break;
    }

    int err = led_strip_update_rgb(led_strip, pixels, STRIP_NUM_PIXELS);
    if (err < 0) {
        LOG_ERR("Failed to update the RGB strip (%d)", err);
    }
}

K_WORK_DEFINE(rgb_ext_tick_work, zmk_rgb_ext_tick);

static void zmk_rgb_ext_tick_handler(struct k_timer *timer) {
    if (!state.on) {
        return;
    }

    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &rgb_ext_tick_work);
}

K_TIMER_DEFINE(rgb_ext_tick, zmk_rgb_ext_tick_handler, NULL);

int zmk_rgb_ext_select_effect(int effect) {
    if (!led_strip)
        return -ENODEV;

    if (effect < 0 || effect >= RGB_EFFECT_NUMBER) {
        return -EINVAL;
    }

    state.current_effect = effect;
    state.animation_step = 0;
    
    return zmk_rgb_ext_save_state();
}

int zmk_rgb_ext_calc_effect(int direction) {
    return (state.current_effect + RGB_EFFECT_NUMBER + direction) % RGB_EFFECT_NUMBER;
}

int zmk_rgb_ext_cycle_effect(int direction) {
    return zmk_rgb_ext_select_effect(zmk_rgb_ext_calc_effect(direction));
}

int zmk_rgb_ext_change_spd(int direction) {
    if (!led_strip)
        return -ENODEV;

    if (state.animation_speed == 1 && direction < 0) {
        return 0;
    }

    state.animation_speed += direction;

    if (state.animation_speed > 5) {
        state.animation_speed = 5;
    }

    return zmk_rgb_ext_save_state();
}
/* ====== RGB Effect Functions ====== */

/* ====== Behavior Settings ====== */
#if IS_ENABLED(CONFIG_SETTINGS)
static int rgb_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    const char *next;
    int rc;

    if (settings_name_steq(name, "state", &next) && !next) {
        if (len != sizeof(state)) {
            return -EINVAL;
        }

        rc = read_cb(cb_arg, &state, sizeof(state));
        if (rc >= 0) {
            if (state.on) {
                k_timer_start(&rgb_ext_tick, K_NO_WAIT, K_MSEC(50));
            }

            return 0;
        }

        return rc;
    }

    return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(rgb_ext, "rgb/ext", NULL, rgb_settings_set, NULL, NULL);

static void zmk_rgb_ext_save_state_work(struct k_work *_work) {
    settings_save_one("rgb/underglow/state", &state, sizeof(state));
}

static struct k_work_delayable underglow_save_work;
#endif // IS_ENABLED(CONFIG_SETTINGS)

// Define different versions of zmk_rgb_ext_save_state depending on if CONFIG_SETTINGS is enabled
int zmk_rgb_ext_save_state(void) {
    int ret = k_work_reschedule(&underglow_save_work, K_MSEC(CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE));
#if IS_ENABLED(CONFIG_SETTINGS)
    return MIN(ret, 0);
#else
    return 0;
#endif // IS_ENABLED(CONFIG_SETTINGS)
}
/* ====== Behavior Settings ====== */

/* ====== On/Off State ====== */
int zmk_rgb_ext_get_state(bool *on_off) {
    if (!led_strip)
        return -ENODEV;

    *on_off = state.on;
    return 0;
}

int zmk_rgb_ext_on(void) {
    if (!led_strip)
        return -ENODEV;

#if IS_ENABLED(CONFIG_ZMK_RGB_EXT_EXT_POWER)
    if (ext_power != NULL) {
        int rc = ext_power_enable(ext_power);
        if (rc != 0) {
            LOG_ERR("Unable to enable EXT_POWER: %d", rc);
        }
    }
#endif

    state.on = true;
    state.animation_step = 0;
    k_timer_start(&rgb_ext_tick, K_NO_WAIT, K_MSEC(50));

    return zmk_rgb_ext_save_state();
}

static void zmk_rgb_ext_off_handler(struct k_work *work) {
    set_solid_rgb_color((struct led_rgb){r : 0, g : 0, b : 0});
    led_strip_update_rgb(led_strip, pixels, STRIP_NUM_PIXELS);
}

K_WORK_DEFINE(underglow_off_work, zmk_rgb_ext_off_handler);

int zmk_rgb_ext_off(void) {
    if (!led_strip)
        return -ENODEV;

#if IS_ENABLED(CONFIG_ZMK_RGB_EXT_EXT_POWER)
    if (ext_power != NULL) {
        int rc = ext_power_disable(ext_power);
        if (rc != 0) {
            LOG_ERR("Unable to disable EXT_POWER: %d", rc);
        }
    }
#endif

    // Add work to queue
    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &underglow_off_work);

    k_timer_stop(&rgb_ext_tick);
    state.on = false;

    return zmk_rgb_ext_save_state();
}

int zmk_rgb_ext_toggle(void) {
    return state.on ? zmk_rgb_ext_off() : zmk_rgb_ext_on();
}
/* ====== On/Off State ====== */

/* ====== RGB Functions ====== */
int zmk_rgb_ext_set_rgb(struct led_rgb color) {
    state.color = color;

    return 0;
}
/* ====== RGB Functions ====== */

/* ====== HSB Functions ====== */
int zmk_rgb_ext_set_hsb(struct zmk_led_hsb color) {
    if (color.h > HUE_MAX || color.s > SAT_MAX || color.b > BRT_MAX) {
        return -ENOTSUP;
    }

    struct led_rgb rgb = hsb_to_rgb(color);
    zmk_rgb_ext_set_rgb(rgb);

    return 0;
}

struct zmk_led_hsb zmk_rgb_ext_calc_hue(int direction) {
    struct zmk_led_hsb color = rgb_to_hsb(state.color);

    color.h += HUE_MAX + (direction * CONFIG_ZMK_RGB_EXT_HUE_STEP);
    color.h %= HUE_MAX;

    return color;
}

struct zmk_led_hsb zmk_rgb_ext_calc_sat(int direction) {
    struct zmk_led_hsb color = rgb_to_hsb(state.color);

    int s = color.s + (direction * CONFIG_ZMK_RGB_EXT_SAT_STEP);
    if (s < 0) {
        s = 0;
    } else if (s > SAT_MAX) {
        s = SAT_MAX;
    }
    color.s = s;

    return color;
}

struct zmk_led_hsb zmk_rgb_ext_calc_brt(int direction) {
    struct zmk_led_hsb color = rgb_to_hsb(state.color);

    int b = color.b + (direction * CONFIG_ZMK_RGB_EXT_BRT_STEP);
    color.b = CLAMP(b, 0, BRT_MAX);

    return color;
}

int zmk_rgb_ext_change_hue(int direction) {
    if (!led_strip)
        return -ENODEV;
    
    
    zmk_rgb_ext_set_hsb(zmk_rgb_ext_calc_hue(direction));

    return zmk_rgb_ext_save_state();
}

int zmk_rgb_ext_change_sat(int direction) {
    if (!led_strip)
        return -ENODEV;

    zmk_rgb_ext_set_hsb(zmk_rgb_ext_calc_sat(direction));

    return zmk_rgb_ext_save_state();
}

int zmk_rgb_ext_change_brt(int direction) {
    if (!led_strip)
        return -ENODEV;

    zmk_rgb_ext_set_hsb(zmk_rgb_ext_calc_brt(direction));

    return zmk_rgb_ext_save_state();
}
/* ====== HSB Functions ====== */

/* ====== Sleep State ====== */
#if IS_ENABLED(CONFIG_ZMK_RGB_EXT_AUTO_OFF_IDLE) || IS_ENABLED(CONFIG_ZMK_RGB_EXT_AUTO_OFF_USB)
static int rgb_underglow_auto_state(bool target_wake_state) {
    static struct rgb_ext_sleep_state sleep_state = {
        is_awake : true,
        rgb_state_before_sleeping : false
    };

    // wake up event while awake, or sleep event while sleeping -> no-op
    if (target_wake_state == sleep_state.is_awake) {
        return 0;
    }
    sleep_state.is_awake = target_wake_state;

    if (sleep_state.is_awake) {
        if (sleep_state.rgb_state_before_sleeping) {
            return zmk_rgb_ext_on();
        } else {
            return zmk_rgb_ext_off();
        }
    } else {
        sleep_state.rgb_state_before_sleeping = state.on;
        return zmk_rgb_ext_off();
    }
}

static int rgb_underglow_event_listener(const zmk_event_t *eh) {
#if IS_ENABLED(CONFIG_ZMK_RGB_EXT_AUTO_OFF_IDLE)
    if (as_zmk_activity_state_changed(eh)) {
        return rgb_underglow_auto_state(zmk_activity_get_state() == ZMK_ACTIVITY_ACTIVE);
    }
#endif

#if IS_ENABLED(CONFIG_ZMK_RGB_EXT_AUTO_OFF_USB)
    if (as_zmk_usb_conn_state_changed(eh)) {
        return rgb_underglow_auto_state(zmk_usb_is_powered());
    }
#endif

    return -ENOTSUP;
}

ZMK_LISTENER(rgb_underglow, rgb_underglow_event_listener);

#if IS_ENABLED(CONFIG_ZMK_RGB_EXT_AUTO_OFF_IDLE)
ZMK_SUBSCRIPTION(rgb_underglow, zmk_activity_state_changed);
#endif

#if IS_ENABLED(CONFIG_ZMK_RGB_EXT_AUTO_OFF_USB)
ZMK_SUBSCRIPTION(rgb_underglow, zmk_usb_conn_state_changed);
#endif

#endif // IS_ENABLED(CONFIG_ZMK_RGB_EXT_AUTO_OFF_IDLE) || IS_ENABLED(CONFIG_ZMK_RGB_EXT_AUTO_OFF_USB)
/* ====== Sleep State ====== */


/* ====== Life-Cycle ====== */
static int zmk_rgb_ext_init(void) {
    led_strip = DEVICE_DT_GET(STRIP_CHOSEN);

#if IS_ENABLED(CONFIG_ZMK_RGB_EXT_EXT_POWER)
    if (!device_is_ready(ext_power)) {
        LOG_ERR("External power device \"%s\" is not ready", ext_power->name);
        return -ENODEV;
    }
#endif //IS_ENABLED(CONFIG_ZMK_RGB_EXT_EXT_POWER)

    state = (struct rgb_ext_state){
        color : {
            r : CONFIG_ZMK_RGB_EXT_HUE_START,
            g : CONFIG_ZMK_RGB_EXT_SAT_START,
            b : CONFIG_ZMK_RGB_EXT_BRT_START,
        },
        animation_speed : CONFIG_ZMK_RGB_EXT_SPD_START,
        current_effect : CONFIG_ZMK_RGB_EXT_EFF_START,
        animation_step : 0,
        on : IS_ENABLED(CONFIG_ZMK_RGB_EXT_ON_START)
    };

#ifdef CONFIG_ZMK_RGB_EXT_HUE_START && CONFIG_ZMK_RGB_EXT_SAT_START && CONFIG_ZMK_RGB_EXT_BRT_START
    struct zmk_led_hsb hsb = {
        h : CONFIG_ZMK_RGB_EXT_HUE_START,
        s : CONFIG_ZMK_RGB_EXT_SAT_START,
        b : CONFIG_ZMK_RGB_EXT_BRT_START
    };
    zmk_rgb_ext_set_hsb(hsb);
#endif

#if IS_ENABLED(CONFIG_SETTINGS)
    k_work_init_delayable(&underglow_save_work, zmk_rgb_ext_save_state_work);
#endif

#if IS_ENABLED(CONFIG_ZMK_RGB_EXT_AUTO_OFF_USB)
    state.on = zmk_usb_is_powered();
#endif

    if (state.on) {
        k_timer_start(&rgb_ext_tick, K_NO_WAIT, K_MSEC(50));
    }

    return 0;
}

SYS_INIT(zmk_rgb_ext_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
//SYS_INIT(zmk_rgb_ext_init, APPLICATION, 32);
/* ====== Life-Cycle ====== */
