/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/drivers/led_strip.h>

enum rgb_effect {
    RGB_EFFECT_SOLID,
    RGB_EFFECT_BREATHE,
    RGB_EFFECT_SPECTRUM,
    RGB_EFFECT_SWIRL,
    RGB_EFFECT_NUMBER // Used to track number of underglow effects
};

struct zmk_led_hsb {
    uint16_t h;
    uint8_t s;
    uint8_t b;
};

struct rgb_ext_state {
    struct led_rgb color;
    uint8_t animation_speed;
    uint8_t current_effect;
    uint16_t animation_step;
    bool on;
};

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_IDLE) || IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_USB)
struct rgb_ext_sleep_state {
    bool is_awake;
    bool rgb_state_before_sleeping;
};
#endif

int zmk_rgb_ext_toggle(void);
int zmk_rgb_ext_get_state(bool *state);
int zmk_rgb_ext_on(void);
int zmk_rgb_ext_off(void);
int zmk_rgb_ext_cycle_effect(int direction);
int zmk_rgb_ext_calc_effect(int direction);
int zmk_rgb_ext_select_effect(int effect);
struct zmk_led_hsb zmk_rgb_ext_calc_hue(int direction);
struct zmk_led_hsb zmk_rgb_ext_calc_sat(int direction);
struct zmk_led_hsb zmk_rgb_ext_calc_brt(int direction);
int zmk_rgb_ext_change_hue(int direction);
int zmk_rgb_ext_change_sat(int direction);
int zmk_rgb_ext_change_brt(int direction);
int zmk_rgb_ext_change_spd(int direction);
int zmk_rgb_ext_set_rgb(struct led_rgb color);
int zmk_rgb_ext_set_hsb(struct zmk_led_hsb color);