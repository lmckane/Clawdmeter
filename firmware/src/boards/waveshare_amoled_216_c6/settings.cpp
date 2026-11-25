#include "settings.h"
#include "wifi_setup.h"
#include "../../ble.h"
#include "../../brightness.h"
#include "../../ui.h"
#include "../../clawd_director.h"
#include <lvgl.h>
#include <stdio.h>
#include <string.h>

// Built only while open: the C6 has no PSRAM. The meters and their one Clawd
// continue to exist underneath; closing this overlay frees every widget.
static lv_obj_t* overlay = nullptr;
static lv_obj_t* ble_label = nullptr;
static lv_obj_t* wifi_label = nullptr;
static lv_obj_t* portal_button_label = nullptr;
static lv_obj_t* confirm = nullptr;
static ble_state_t last_state = BLE_STATE_INIT;
static bool state_shown = false;
static char last_wifi_status[176] = {};
static uint32_t last_wifi_update = 0;

static void close_settings(lv_event_t*) {
    director_set_touch_blocked(false);
    if (overlay) lv_obj_delete(overlay);
    overlay = nullptr;
    ble_label = nullptr;
    wifi_label = nullptr;
    portal_button_label = nullptr;
    confirm = nullptr;
}

static lv_obj_t* add_button(lv_obj_t* parent, const char* text, int y,
                            lv_event_cb_t cb) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_set_size(button, 340, 55);
    lv_obj_align(button, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    return button;
}

static void cycle_brightness(lv_event_t*) { brightness_cycle(); }
static void toggle_portal(lv_event_t*) {
    if (c6_wifi_portal_active()) c6_wifi_stop_portal();
    else c6_wifi_start_portal();
    last_wifi_update = 0;
    c6_settings_tick();
}

static void request_reset(lv_event_t*) {
    if (confirm) return;
    confirm = lv_obj_create(overlay);
    lv_obj_set_size(confirm, 390, 220);
    lv_obj_center(confirm);
    lv_obj_set_style_bg_color(confirm, lv_color_hex(0x20202b), 0);
    lv_obj_t* message = lv_label_create(confirm);
    lv_label_set_text(message, "Forget all Bluetooth pairings?\nReconnect in the desktop app.");
    lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(message, LV_ALIGN_TOP_MID, 0, 15);
    lv_obj_t* cancel = lv_button_create(confirm);
    lv_obj_set_size(cancel, 145, 55);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 5, -5);
    lv_obj_add_event_cb(cancel, [](lv_event_t*) {
        lv_obj_delete(confirm);
        confirm = nullptr;
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* cancel_text = lv_label_create(cancel);
    lv_label_set_text(cancel_text, "Cancel");
    lv_obj_center(cancel_text);
    lv_obj_t* reset = lv_button_create(confirm);
    lv_obj_set_size(reset, 145, 55);
    lv_obj_align(reset, LV_ALIGN_BOTTOM_RIGHT, -5, -5);
    lv_obj_add_event_cb(reset, [](lv_event_t*) {
        ble_clear_bonds();
        ui_set_pair_hint(PAIR_HINT_CLEARED);
        lv_obj_delete(confirm);
        confirm = nullptr;
        last_state = BLE_STATE_INIT;
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* reset_text = lv_label_create(reset);
    lv_label_set_text(reset_text, "Forget");
    lv_obj_center(reset_text);
}

void c6_settings_toggle(void) {
    if (overlay) { close_settings(nullptr); return; }
    director_set_touch_blocked(true);
    overlay = lv_obj_create(lv_screen_active());
    lv_obj_set_size(overlay, 480, 480);
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0x14141e), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_radius(overlay, 0, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(overlay);
    lv_label_set_text(title, "SETTINGS");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);
    ble_label = lv_label_create(overlay);
    lv_obj_align(ble_label, LV_ALIGN_TOP_MID, 0, 72);
    state_shown = false;
    c6_settings_tick();

    add_button(overlay, "Forget Bluetooth pairing", 110, request_reset);
    add_button(overlay, "Cycle brightness", 178, cycle_brightness);
    wifi_label = lv_label_create(overlay);
    lv_obj_set_width(wifi_label, 420);
    lv_obj_set_style_text_align(wifi_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(wifi_label, LV_ALIGN_TOP_MID, 0, 250);
    lv_obj_t* portal = add_button(overlay, "Set up Wi-Fi", 335, toggle_portal);
    portal_button_label = lv_obj_get_child(portal, 0);
    add_button(overlay, "Close", 405, close_settings);
    last_wifi_status[0] = '\0';
    last_wifi_update = 0;
    c6_settings_tick();
}

void c6_settings_tick(void) {
    if (!ble_label) return;
    const ble_state_t state = ble_get_state();
    if (!state_shown || state != last_state) {
        last_state = state;
        state_shown = true;
        const char* label = state == BLE_STATE_CONNECTED ? "Bluetooth: connected" :
                            state == BLE_STATE_ADVERTISING ? "Bluetooth: ready to pair" :
                            "Bluetooth: disconnected";
        lv_label_set_text(ble_label, label);
    }
    if (!wifi_label) return;
    const uint32_t now = lv_tick_get();
    if (last_wifi_update && now - last_wifi_update < 1000) return;
    last_wifi_update = now;
    char status[176];
    c6_wifi_status(status, sizeof(status));
    if (strcmp(status, last_wifi_status) != 0) {
        snprintf(last_wifi_status, sizeof(last_wifi_status), "%s", status);
        lv_label_set_text(wifi_label, status);
    }
    lv_label_set_text(portal_button_label, c6_wifi_portal_active() ? "Stop setup AP" : "Set up Wi-Fi");
}
