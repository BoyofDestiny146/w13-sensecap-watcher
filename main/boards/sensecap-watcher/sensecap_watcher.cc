#include "display/lv_display.h"
#include "misc/lv_event.h"
#include "wifi_board.h"
#include "sensecap_audio_codec.h"
#include "display/lcd_display.h"
#include "application.h"
#include "knob.h"
#include "config.h"
#include "led/single_led.h"
#include "power_save_timer.h"
#include "sscma_camera.h"
#include "lvgl_theme.h"

#include <esp_log.h>
#include <esp_check.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_spd2010.h>
#include <esp_adc/adc_oneshot.h>
#include <driver/spi_master.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <iot_button.h>
#include <iot_knob.h>
#include <esp_io_expander_tca95xx_16bit.h>
#include <esp_sleep.h>
#include <esp_console.h>
#include <esp_mac.h>
#include <nvs_flash.h>
#include <esp_app_desc.h>

#include "assets/lang_config.h"

#define TAG "sensecap_watcher"

class CustomLcdDisplay : public SpiLcdDisplay {
    private:
        lv_obj_t* w13_layer_ = nullptr;
        lv_obj_t* outer_ring_ = nullptr;
        lv_obj_t* middle_ring_ = nullptr;
        lv_obj_t* inner_ring_ = nullptr;
        lv_obj_t* w13_logo_ = nullptr;
        lv_obj_t* w13_state_ = nullptr;

        // W-13 Wi-Fi provisioning UI
        lv_obj_t* wifi_title_ = nullptr;
        lv_obj_t* wifi_ssid_ = nullptr;
        lv_obj_t* wifi_url_ = nullptr;

        // W-13 low-battery overlay (replaces stock white popup visuals)
        lv_obj_t* low_bat_outer_ring_ = nullptr;
        lv_obj_t* low_bat_middle_ring_ = nullptr;
        lv_obj_t* low_bat_inner_ring_ = nullptr;

        esp_timer_handle_t intro_timer_ = nullptr;
        bool intro_finished_ = false;
        bool panel_display_off_ = false;
        std::string pending_wifi_message_;

        static void RingAnim(void* obj, int32_t value) {
            lv_arc_set_value(static_cast<lv_obj_t*>(obj), value);
        }

        void StartRingAnimation(lv_obj_t* ring, uint32_t duration, uint32_t delay = 0) {
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, ring);
            lv_anim_set_exec_cb(&a, RingAnim);
            lv_anim_set_values(&a, 0, 100);
            lv_anim_set_duration(&a, duration);
            lv_anim_set_delay(&a, delay);
            lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
            lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
            lv_anim_start(&a);
        }

        lv_obj_t* CreateRing(lv_obj_t* parent, int size, int width, uint32_t color) {
            lv_obj_t* ring = lv_arc_create(parent);
            lv_obj_set_size(ring, size, size);
            lv_obj_center(ring);

            lv_arc_set_range(ring, 0, 100);
            lv_arc_set_bg_angles(ring, 0, 360);
            lv_arc_set_rotation(ring, 270);
            lv_arc_set_value(ring, 70);

            lv_obj_remove_style(ring, nullptr, LV_PART_KNOB);

            lv_obj_set_style_arc_width(ring, width, LV_PART_MAIN);
            lv_obj_set_style_arc_color(
                ring, lv_color_hex(0x073844), LV_PART_MAIN);
            lv_obj_set_style_arc_opa(ring, LV_OPA_60, LV_PART_MAIN);

            lv_obj_set_style_arc_width(ring, width, LV_PART_INDICATOR);
            lv_obj_set_style_arc_color(
                ring, lv_color_hex(color), LV_PART_INDICATOR);
            lv_obj_set_style_arc_opa(ring, LV_OPA_COVER, LV_PART_INDICATOR);

            return ring;
        }

        void SetW13State(const char* text) {
            if (w13_state_ != nullptr) {
                lv_label_set_text(w13_state_, text);
            }
        }

        static void IntroTimerCallback(void* arg) {
            auto self = static_cast<CustomLcdDisplay*>(arg);
            self->intro_finished_ = true;

            DisplayLockGuard lock(self);

            if (!self->pending_wifi_message_.empty()) {
                self->ShowWifiScreenFromMessage(
                    self->pending_wifi_message_.c_str());
            }
        }

        void ShowIntroScreen() {
            ShowW13Home();

            if (w13_logo_ != nullptr)
                lv_obj_remove_flag(w13_logo_, LV_OBJ_FLAG_HIDDEN);

            if (w13_state_ != nullptr) {
                lv_label_set_text(w13_state_, "NEXUS");
                lv_obj_remove_flag(w13_state_, LV_OBJ_FLAG_HIDDEN);
            }

            if (wifi_title_ != nullptr)
                lv_obj_add_flag(wifi_title_, LV_OBJ_FLAG_HIDDEN);

            if (wifi_ssid_ != nullptr)
                lv_obj_add_flag(wifi_ssid_, LV_OBJ_FLAG_HIDDEN);

            if (wifi_url_ != nullptr)
                lv_obj_add_flag(wifi_url_, LV_OBJ_FLAG_HIDDEN);

            if (w13_layer_ != nullptr)
                lv_obj_move_foreground(w13_layer_);
        }

        void ShowWifiScreen(const char* ssid, const char* url) {
            ShowW13Home();

            if (w13_logo_ != nullptr)
                lv_obj_add_flag(w13_logo_, LV_OBJ_FLAG_HIDDEN);

            if (w13_state_ != nullptr)
                lv_obj_add_flag(w13_state_, LV_OBJ_FLAG_HIDDEN);

            if (wifi_title_ != nullptr) {
                lv_label_set_text(wifi_title_, "WI-FI SETUP");
                lv_obj_remove_flag(wifi_title_, LV_OBJ_FLAG_HIDDEN);
            }

            if (wifi_ssid_ != nullptr) {
                lv_label_set_text_fmt(
                    wifi_ssid_, "HOTSPOT\n%s",
                    (ssid != nullptr) ? ssid : "");
                lv_obj_remove_flag(wifi_ssid_, LV_OBJ_FLAG_HIDDEN);
            }

            if (wifi_url_ != nullptr) {
                lv_label_set_text_fmt(
                    wifi_url_, "OPEN IN BROWSER\n%s",
                    (url != nullptr) ? url : "");
                lv_obj_remove_flag(wifi_url_, LV_OBJ_FLAG_HIDDEN);
            }

            if (w13_layer_ != nullptr)
                lv_obj_move_foreground(w13_layer_);
        }

        void ShowWifiScreenFromMessage(const char* message) {
            if (message == nullptr)
                return;

            std::string msg(message);
            std::string ssid;
            std::string url;

            const std::string hotspot_tag = "Hotspot: ";
            const std::string url_tag = " Config URL: ";

            auto hp = msg.find(hotspot_tag);
            auto up = msg.find(url_tag);

            if (hp != std::string::npos) {
                hp += hotspot_tag.size();

                if (up != std::string::npos && up > hp) {
                    ssid = msg.substr(hp, up - hp);
                    url = msg.substr(up + url_tag.size());
                } else {
                    ssid = msg.substr(hp);
                }
            }

            ShowWifiScreen(
                ssid.empty() ? "Xiaozhi" : ssid.c_str(),
                url.empty() ? "http://192.168.4.1" : url.c_str());
        }

        void ShowStockUiForSetup() {
            if (w13_layer_ != nullptr)
                lv_obj_add_flag(w13_layer_, LV_OBJ_FLAG_HIDDEN);

            if (container_ != nullptr)
                lv_obj_remove_flag(container_, LV_OBJ_FLAG_HIDDEN);
            if (top_bar_ != nullptr)
                lv_obj_remove_flag(top_bar_, LV_OBJ_FLAG_HIDDEN);
            if (status_bar_ != nullptr)
                lv_obj_remove_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
            if (bottom_bar_ != nullptr)
                lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        }

        void ShowW13Home() {
            if (container_ != nullptr)
                lv_obj_add_flag(container_, LV_OBJ_FLAG_HIDDEN);
            if (emoji_box_ != nullptr)
                lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
            if (top_bar_ != nullptr)
                lv_obj_add_flag(top_bar_, LV_OBJ_FLAG_HIDDEN);
            if (status_bar_ != nullptr)
                lv_obj_add_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
            if (bottom_bar_ != nullptr)
                lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);

            if (w13_layer_ != nullptr) {
                lv_obj_remove_flag(w13_layer_, LV_OBJ_FLAG_HIDDEN);
                lv_obj_move_foreground(w13_layer_);
            }
        }

        void SetupW13LowBatteryUi(const lv_font_t* text_font) {
            if (low_battery_popup_ == nullptr || low_battery_label_ == nullptr) {
                return;
            }

            // Full-screen black W-13 low-battery surface (no stock white popup).
            lv_obj_set_size(low_battery_popup_, LV_HOR_RES, LV_VER_RES);
            lv_obj_align(low_battery_popup_, LV_ALIGN_CENTER, 0, 0);
            lv_obj_set_style_bg_color(low_battery_popup_, lv_color_hex(0x000000), 0);
            lv_obj_set_style_bg_opa(low_battery_popup_, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(low_battery_popup_, 0, 0);
            lv_obj_set_style_radius(low_battery_popup_, 0, 0);
            lv_obj_set_style_pad_all(low_battery_popup_, 0, 0);
            lv_obj_clear_flag(low_battery_popup_, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);

            low_bat_outer_ring_ = CreateRing(low_battery_popup_, 406, 5, 0x18E7F2);
            low_bat_middle_ring_ = CreateRing(low_battery_popup_, 392, 4, 0x19BCEB);
            low_bat_inner_ring_ = CreateRing(low_battery_popup_, 378, 3, 0x68FFF2);
            StartRingAnimation(low_bat_outer_ring_, 2600, 0);
            StartRingAnimation(low_bat_middle_ring_, 1900, 300);
            StartRingAnimation(low_bat_inner_ring_, 1400, 600);

            lv_label_set_text(low_battery_label_, "BATTERY LOW");
            lv_obj_set_style_text_color(low_battery_label_, lv_color_hex(0xF01818), 0);
            lv_obj_set_style_text_font(low_battery_label_, text_font, 0);
            lv_obj_set_style_text_letter_space(low_battery_label_, 3, 0);
            lv_obj_set_width(low_battery_label_, LV_HOR_RES * 0.8);
            lv_obj_set_style_text_align(low_battery_label_, LV_TEXT_ALIGN_CENTER, 0);
            lv_label_set_long_mode(low_battery_label_, LV_LABEL_LONG_WRAP);
            lv_obj_align(low_battery_label_, LV_ALIGN_CENTER, 0, 0);
            lv_obj_move_foreground(low_battery_label_);
        }

        void SetPanelDisplayOn(bool on) {
            if (panel_ == nullptr) {
                return;
            }
            esp_err_t err = esp_lcd_panel_disp_on_off(panel_, on);
            if (err == ESP_ERR_NOT_SUPPORTED) {
                ESP_LOGW(TAG, "Panel does not support disp_on_off; backlight-only screen-off");
                return;
            }
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "esp_lcd_panel_disp_on_off(%s) failed: %s",
                         on ? "on" : "off", esp_err_to_name(err));
                return;
            }
            panel_display_off_ = !on;
        }

    public:
        CustomLcdDisplay(esp_lcd_panel_io_handle_t io_handle, 
                        esp_lcd_panel_handle_t panel_handle,
                        int width,
                        int height,
                        int offset_x,
                        int offset_y,
                        bool mirror_x,
                        bool mirror_y,
                        bool swap_xy) 
            : SpiLcdDisplay(io_handle, panel_handle, width, height, offset_x, offset_y, mirror_x, mirror_y, swap_xy) {
            // Note: UI customization should be done in SetupUI(), not in constructor
            // to ensure lvgl objects are created before accessing them
        }

        virtual void SetupUI() override {
            // Call parent SetupUI() first to create all lvgl objects
            SpiLcdDisplay::SetupUI();

            DisplayLockGuard lock(this);
            auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
            auto text_font = lvgl_theme->text_font()->font();
            auto icon_font = lvgl_theme->icon_font()->font();

            lv_obj_set_size(top_bar_, LV_HOR_RES, text_font->line_height);
            lv_obj_set_style_layout(top_bar_, LV_LAYOUT_NONE, 0);
            lv_obj_set_style_pad_top(top_bar_, 10, 0);
            lv_obj_set_style_pad_bottom(top_bar_, 1, 0);

            lv_obj_set_size(status_bar_, LV_HOR_RES, text_font->line_height);
            lv_obj_set_style_layout(status_bar_, LV_LAYOUT_NONE, 0);
            lv_obj_set_style_pad_top(status_bar_, 10, 0);
            lv_obj_set_style_pad_bottom(status_bar_, 1, 0);
            lv_obj_set_y(status_bar_, text_font->line_height);
            lv_obj_add_flag(status_bar_, LV_OBJ_FLAG_IGNORE_LAYOUT);

            // Reparent mute and battery labels to top_bar_ to allow absolute positioning
            lv_obj_set_parent(mute_label_, top_bar_);
            lv_obj_set_parent(battery_label_, top_bar_);
            lv_obj_set_style_margin_left(battery_label_, 0, 0);

            // 针对圆形屏幕调整位置
            //      network  mute  battery     //
            //               status            //
            lv_obj_align(network_label_, LV_ALIGN_TOP_MID, -1.5 * icon_font->line_height, 0);
            lv_obj_align(mute_label_, LV_ALIGN_TOP_MID, 1.0 * icon_font->line_height, 0);
            lv_obj_align(battery_label_, LV_ALIGN_TOP_MID, 2.5 * icon_font->line_height, 0);
            
            lv_obj_align(status_label_, LV_ALIGN_BOTTOM_MID, 0, 0);
            lv_obj_set_flex_grow(status_label_, 0);
            lv_obj_set_width(status_label_, LV_HOR_RES * 0.75);
            lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);

            lv_obj_align(notification_label_, LV_ALIGN_BOTTOM_MID, 0, 0);
            lv_obj_set_width(notification_label_, LV_HOR_RES * 0.75);
            lv_label_set_long_mode(notification_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);

            // 针对圆形屏幕调整底部对话框位置，避免被圆角遮挡
            lv_obj_set_style_pad_bottom(bottom_bar_, 30, 0);
            lv_obj_set_width(chat_message_label_, LV_HOR_RES * 0.75); // 限制宽度，避免文字贴边

            // =========================================================
            // Warehouse 13 / Nexus animated Watcher interface
            // =========================================================

            lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);

            // W-13 is a full-screen overlay on the ROOT LVGL screen.
            // Do not attach it to content_; the stock Watcher UI uses
            // separate screen-level top/status/bottom layers.
            lv_obj_t* screen = lv_screen_active();

            w13_layer_ = lv_obj_create(screen);
            lv_obj_remove_style_all(w13_layer_);
            lv_obj_set_pos(w13_layer_, 0, 0);
            lv_obj_set_size(w13_layer_, LV_HOR_RES, LV_VER_RES);
            lv_obj_move_foreground(w13_layer_);
            lv_obj_set_style_bg_color(
                w13_layer_, lv_color_hex(0x000000), 0);
            lv_obj_set_style_bg_opa(
                w13_layer_, LV_OPA_COVER, 0);
            lv_obj_clear_flag(w13_layer_, LV_OBJ_FLAG_SCROLLABLE);

            // Hide the normal XiaoZhi Watcher interface.
            if (container_ != nullptr)
                lv_obj_add_flag(container_, LV_OBJ_FLAG_HIDDEN);
            if (emoji_box_ != nullptr)
                lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
            if (top_bar_ != nullptr)
                lv_obj_add_flag(top_bar_, LV_OBJ_FLAG_HIDDEN);
            if (status_bar_ != nullptr)
                lv_obj_add_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
            if (bottom_bar_ != nullptr)
                lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);

            // Make absolutely certain W-13 remains the top layer.
            lv_obj_move_foreground(w13_layer_);

            // W-13 perimeter animation for the 412 x 412 Watcher.
            // Outer arc is only a few pixels inside the physical rim.
            outer_ring_ = CreateRing(
                w13_layer_, 406, 5, 0x18E7F2);

            middle_ring_ = CreateRing(
                w13_layer_, 392, 4, 0x19BCEB);

            inner_ring_ = CreateRing(
                w13_layer_, 378, 3, 0x68FFF2);

            StartRingAnimation(outer_ring_, 2600, 0);
            StartRingAnimation(middle_ring_, 1900, 300);
            StartRingAnimation(inner_ring_, 1400, 600);

            // W-13 center mark
            w13_logo_ = lv_label_create(w13_layer_);
            lv_label_set_text(w13_logo_, "W-13");
            lv_obj_set_style_text_color(
                w13_logo_, lv_color_hex(0xF01818), 0);
            lv_obj_set_style_text_font(
                w13_logo_, text_font, 0);
            lv_obj_set_style_text_letter_space(
                w13_logo_, 4, 0);
            lv_obj_set_style_transform_scale(
                w13_logo_, 125, 0);
            lv_obj_align(
                w13_logo_, LV_ALIGN_CENTER, 0, -6);

            // State caption
            w13_state_ = lv_label_create(w13_layer_);
            lv_label_set_text(w13_state_, "NEXUS");
            lv_obj_set_style_text_color(
                w13_state_, lv_color_hex(0x8EFAFF), 0);
            lv_obj_set_style_text_font(
                w13_state_, text_font, 0);
            lv_obj_set_style_text_letter_space(
                w13_state_, 2, 0);
            lv_obj_set_style_transform_scale(
                w13_state_, 112, 0);
            lv_obj_align(
                w13_state_, LV_ALIGN_CENTER, 0, 42);

            // -------------------------------------------------
            // Custom W-13 Wi-Fi provisioning screen
            // -------------------------------------------------

            wifi_title_ = lv_label_create(w13_layer_);
            lv_label_set_text(wifi_title_, "WI-FI SETUP");
            lv_obj_set_style_text_color(
                wifi_title_, lv_color_hex(0x7CFAFF), 0);
            lv_obj_set_style_text_font(
                wifi_title_, text_font, 0);
            lv_obj_set_style_text_letter_space(
                wifi_title_, 2, 0);
            lv_obj_align(
                wifi_title_, LV_ALIGN_CENTER, 0, -75);
            lv_obj_add_flag(
                wifi_title_, LV_OBJ_FLAG_HIDDEN);

            wifi_ssid_ = lv_label_create(w13_layer_);
            lv_label_set_text(wifi_ssid_, "");
            lv_obj_set_width(wifi_ssid_, 300);
            lv_obj_set_style_text_color(
                wifi_ssid_, lv_color_hex(0x18E7F2), 0);
            lv_obj_set_style_text_font(
                wifi_ssid_, text_font, 0);
            lv_obj_set_style_text_align(
                wifi_ssid_, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_align(
                wifi_ssid_, LV_ALIGN_CENTER, 0, -10);
            lv_obj_add_flag(
                wifi_ssid_, LV_OBJ_FLAG_HIDDEN);

            wifi_url_ = lv_label_create(w13_layer_);
            lv_label_set_text(wifi_url_, "");
            lv_obj_set_width(wifi_url_, 320);
            lv_obj_set_style_text_color(
                wifi_url_, lv_color_hex(0xA8FFFF), 0);
            lv_obj_set_style_text_font(
                wifi_url_, text_font, 0);
            lv_obj_set_style_text_align(
                wifi_url_, LV_TEXT_ALIGN_CENTER, 0);
            lv_label_set_long_mode(
                wifi_url_, LV_LABEL_LONG_WRAP);
            lv_obj_align(
                wifi_url_, LV_ALIGN_CENTER, 0, 70);
            lv_obj_add_flag(
                wifi_url_, LV_OBJ_FLAG_HIDDEN);

            // Replace stock low-battery popup with W-13 language.
            SetupW13LowBatteryUi(text_font);

            // Start with the branded W-13/NEXUS introduction.
            ShowIntroScreen();

            // Hold intro for a minimum of six seconds.
            esp_timer_create_args_t intro_args = {};
            intro_args.callback = IntroTimerCallback;
            intro_args.arg = this;
            intro_args.dispatch_method = ESP_TIMER_TASK;
            intro_args.name = "w13_intro";

            ESP_ERROR_CHECK(
                esp_timer_create(
                    &intro_args,
                    &intro_timer_));

            ESP_ERROR_CHECK(
                esp_timer_start_once(
                    intro_timer_,
                    6000000));
        }

        // Screen-off listening: keep W-13 UI; do not fall back to stock sleepy emotion.
        virtual void SetPowerSaveMode(bool on) override {
            DisplayLockGuard lock(this);
            if (on) {
                SetPanelDisplayOn(false);
                ShowW13Home();
                if (wifi_title_ == nullptr ||
                    lv_obj_has_flag(wifi_title_, LV_OBJ_FLAG_HIDDEN)) {
                    SetW13State("STANDBY");
                }
            } else {
                SetPanelDisplayOn(true);
                ShowW13Home();
                if (wifi_title_ != nullptr &&
                    !lv_obj_has_flag(wifi_title_, LV_OBJ_FLAG_HIDDEN)) {
                    // Keep Wi-Fi setup screen visible after wake.
                    if (w13_layer_ != nullptr) {
                        lv_obj_move_foreground(w13_layer_);
                    }
                } else {
                    SetW13State("NEXUS");
                    if (outer_ring_ != nullptr) {
                        lv_obj_set_style_arc_color(
                            outer_ring_,
                            lv_color_hex(0x18E7F2),
                            LV_PART_INDICATOR);
                    }
                }
            }
        }

        virtual void UpdateStatusBar(bool update_all = false) override {
            LvglDisplay::UpdateStatusBar(update_all);
            DisplayLockGuard lock(this);
            if (low_battery_popup_ != nullptr &&
                !lv_obj_has_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN)) {
                // Keep W-13 low-battery overlay above home layer when active.
                lv_obj_move_foreground(low_battery_popup_);
            }
        }

        virtual void SetStatus(const char* status) override {
            SpiLcdDisplay::SetStatus(status);

            if (status == nullptr) {
                return;
            }

            DisplayLockGuard lock(this);

            // Never show stock white UI for Wi-Fi setup.
            // Keep W-13 intro up until 6-second timer completes.
            if (strcmp(status, Lang::Strings::WIFI_CONFIG_MODE) == 0 ||
                strcmp(status, Lang::Strings::ENTERING_WIFI_CONFIG_MODE) == 0) {

                if (!intro_finished_) {
                    ShowIntroScreen();
                }
                return;
            }

            ShowW13Home();

            if (w13_state_ == nullptr) {
                return;
            }

            if (strcmp(status, Lang::Strings::LISTENING) == 0) {
                SetW13State("LISTENING");

                lv_obj_set_style_arc_color(
                    outer_ring_,
                    lv_color_hex(0x35F4FF),
                    LV_PART_INDICATOR);

            } else if (strcmp(status, Lang::Strings::SPEAKING) == 0) {
                SetW13State("SPEAKING");

                lv_obj_set_style_arc_color(
                    outer_ring_,
                    lv_color_hex(0x16C7F3),
                    LV_PART_INDICATOR);

            } else {
                SetW13State("NEXUS");

                lv_obj_set_style_arc_color(
                    outer_ring_,
                    lv_color_hex(0x18E7F2),
                    LV_PART_INDICATOR);
            }
        }

        virtual void SetChatMessage(
            const char* role,
            const char* content) override {

            // Capture Wi-Fi provisioning message before stock UI renders it.
            if (role != nullptr &&
                content != nullptr &&
                strcmp(role, "system") == 0 &&
                strstr(content, "Hotspot: ") != nullptr) {

                pending_wifi_message_ = content;

                DisplayLockGuard lock(this);

                if (intro_finished_) {
                    ShowWifiScreenFromMessage(content);
                } else {
                    ShowIntroScreen();
                }

                return;
            }

            // Suppress stock system text while W-13 UI is active.
            if (role != nullptr &&
                strcmp(role, "system") == 0) {
                return;
            }

            SpiLcdDisplay::SetChatMessage(role, content);
        }
};

class SensecapWatcher : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    LcdDisplay* display_;
    std::unique_ptr<Knob> knob_;
    esp_io_expander_handle_t io_exp_handle;
    button_handle_t btns;
    PowerSaveTimer* power_save_timer_;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    uint32_t long_press_cnt_;
    button_driver_t* btn_driver_ = nullptr;
    static SensecapWatcher* instance_;
    SscmaCamera* camera_ = nullptr;

    void InitializePowerSaveTimer() {
        // Screen-off + listening: cpu_max_freq=-1 keeps wake-word and mic alive.
        // seconds_to_shutdown=-1 disables the old 300s BSP_PWR_SYSTEM cut.
        power_save_timer_ = new PowerSaveTimer(-1, 60, -1);
        power_save_timer_->OnEnterSleepMode([this]() {
            ESP_LOGI(TAG, "Enter screen-off listening mode");
            // Backlight first so the panel-off transition is not visible.
            GetBacklight()->SetBrightness(0);
            // SetPowerSaveMode restores W-13 UI and blanks the panel when supported.
            // Does not touch Wi-Fi, mic, wake-word, BSP_PWR_LCD, or BSP_PWR_CODEC_PA.
            GetDisplay()->SetPowerSaveMode(true);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            ESP_LOGI(TAG, "Exit screen-off listening mode");
            GetDisplay()->SetPowerSaveMode(false);
            GetBacklight()->RestoreBrightness();
        });
        // No OnShutdownRequest: screen-off listening must not cut BSP_PWR_SYSTEM.
        power_save_timer_->SetEnabled(true);
    }

    void InitializeI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)0,
            .sda_io_num = BSP_GENERAL_I2C_SDA,
            .scl_io_num = BSP_GENERAL_I2C_SCL,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));

        // pulldown for lcd i2c
        const gpio_config_t io_config = {
            .pin_bit_mask = (1ULL << BSP_TOUCH_I2C_SDA) | (1ULL << BSP_TOUCH_I2C_SCL) | (1ULL << BSP_SPI3_HOST_PCLK) | (1ULL << BSP_SPI3_HOST_DATA0) | (1ULL << BSP_SPI3_HOST_DATA1)
                            | (1ULL << BSP_SPI3_HOST_DATA2) | (1ULL << BSP_SPI3_HOST_DATA3) | (1ULL << BSP_LCD_SPI_CS) | (1UL << DISPLAY_BACKLIGHT_PIN),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io_config);

        gpio_set_level(BSP_TOUCH_I2C_SDA, 0);
        gpio_set_level(BSP_TOUCH_I2C_SCL, 0);
    
        gpio_set_level(BSP_LCD_SPI_CS, 0);
        gpio_set_level(DISPLAY_BACKLIGHT_PIN, 0);
        gpio_set_level(BSP_SPI3_HOST_PCLK, 0);
        gpio_set_level(BSP_SPI3_HOST_DATA0, 0);
        gpio_set_level(BSP_SPI3_HOST_DATA1, 0);
        gpio_set_level(BSP_SPI3_HOST_DATA2, 0);
        gpio_set_level(BSP_SPI3_HOST_DATA3, 0);

    }

    esp_err_t IoExpanderSetLevel(uint16_t pin_mask, uint8_t level) {
        return esp_io_expander_set_level(io_exp_handle, pin_mask, level);
    }

    uint8_t IoExpanderGetLevel(uint16_t pin_mask) {
        uint32_t pin_val = 0;
        esp_io_expander_get_level(io_exp_handle, DRV_IO_EXP_INPUT_MASK, &pin_val);
        pin_mask &= DRV_IO_EXP_INPUT_MASK;
        return (uint8_t)((pin_val & pin_mask) ? 1 : 0);
    }

    void InitializeExpander() {
        esp_err_t ret = ESP_OK;
        esp_io_expander_new_i2c_tca95xx_16bit(i2c_bus_, ESP_IO_EXPANDER_I2C_TCA9555_ADDRESS_001, &io_exp_handle);

        ret |= esp_io_expander_set_dir(io_exp_handle, DRV_IO_EXP_INPUT_MASK, IO_EXPANDER_INPUT);
        ret |= esp_io_expander_set_dir(io_exp_handle, DRV_IO_EXP_OUTPUT_MASK, IO_EXPANDER_OUTPUT);
        ret |= esp_io_expander_set_level(io_exp_handle, DRV_IO_EXP_OUTPUT_MASK, 0);
        ret |= esp_io_expander_set_level(io_exp_handle, BSP_PWR_SYSTEM, 1);
        vTaskDelay(100 / portTICK_PERIOD_MS);
        ret |= esp_io_expander_set_level(io_exp_handle, BSP_PWR_START_UP, 1);
        vTaskDelay(50 / portTICK_PERIOD_MS);
    
        uint32_t pin_val = 0;
        ret |= esp_io_expander_get_level(io_exp_handle, DRV_IO_EXP_INPUT_MASK, &pin_val);
        ESP_LOGI(TAG, "IO expander initialized: %x", DRV_IO_EXP_OUTPUT_MASK | (uint16_t)pin_val);
    
        assert(ret == ESP_OK);
    }

    void OnKnobRotate(bool clockwise) {
        auto codec = GetAudioCodec();
        int current_volume = codec->output_volume();
        int new_volume = current_volume + (clockwise ? -5 : 5); 

        // 确保音量在有效范围内
        if (new_volume > 100) {
            new_volume = 100;
            ESP_LOGW(TAG, "Volume reached maximum limit: %d", new_volume);
        } else if (new_volume < 0) {
            new_volume = 0;
            ESP_LOGW(TAG, "Volume reached minimum limit: %d", new_volume);
        }

        codec->SetOutputVolume(new_volume);
        ESP_LOGI(TAG, "Volume changed from %d to %d", current_volume, new_volume);
        
        // 显示通知前检查实际变化
        if (new_volume != codec->output_volume()) {
            ESP_LOGE(TAG, "Failed to set volume! Expected:%d Actual:%d", 
                   new_volume, codec->output_volume());
        }
        GetDisplay()->ShowNotification(std::string(Lang::Strings::VOLUME) + ": "+std::to_string(codec->output_volume()));
        power_save_timer_->WakeUp();
    }

    void InitializeKnob() {
        knob_ = std::make_unique<Knob>(BSP_KNOB_A_PIN, BSP_KNOB_B_PIN);
        knob_->OnRotate([this](bool clockwise) {
            ESP_LOGD(TAG, "Knob rotation detected. Clockwise:%s", clockwise ? "true" : "false");
            OnKnobRotate(clockwise);
        });
        ESP_LOGI(TAG, "Knob initialized with pins A:%d B:%d", BSP_KNOB_A_PIN, BSP_KNOB_B_PIN);
    }

    void InitializeButton() {
        // 设置静态实例指针
        instance_ = this;
        
        // watcher 是通过长按滚轮进行开机的, 需要等待滚轮释放, 否则用户开机松手时可能会误触成单击
        ESP_LOGI(TAG, "waiting for knob button release");
        while(IoExpanderGetLevel(BSP_KNOB_BTN) == 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        button_config_t btn_config = {
            .long_press_time = 2000,
            .short_press_time = 0
        };
        btn_driver_ = (button_driver_t*)calloc(1, sizeof(button_driver_t));
        btn_driver_->enable_power_save = false;
        btn_driver_->get_key_level = [](button_driver_t *button_driver) -> uint8_t {
            return !instance_->IoExpanderGetLevel(BSP_KNOB_BTN);
        };
        
        ESP_ERROR_CHECK(iot_button_create(&btn_config, btn_driver_, &btns));
        
        iot_button_register_cb(btns, BUTTON_SINGLE_CLICK, nullptr, [](void* button_handle, void* usr_data) {
            auto self = static_cast<SensecapWatcher*>(usr_data);
            self->power_save_timer_->WakeUp();

            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                self->EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        }, this);
        
        iot_button_register_cb(btns, BUTTON_LONG_PRESS_START, nullptr, [](void* button_handle, void* usr_data) {
            auto self = static_cast<SensecapWatcher*>(usr_data);
            bool is_charging = (self->IoExpanderGetLevel(BSP_PWR_VBUS_IN_DET) == 0);
            self->long_press_cnt_ = 0;
            if (is_charging) {
                ESP_LOGI(TAG, "charging");
            } else {
                self->IoExpanderSetLevel(BSP_PWR_LCD, 0);
                self->IoExpanderSetLevel(BSP_PWR_SYSTEM, 0);
            }
        }, this);

        iot_button_register_cb(btns, BUTTON_LONG_PRESS_HOLD, nullptr, [](void* button_handle, void* usr_data) {
            auto self = static_cast<SensecapWatcher*>(usr_data);
            self->long_press_cnt_++; // 每隔20ms加一
            // 长按10s 恢复出厂设置: 2+0.02*400 = 10
            if (self->long_press_cnt_ > 400) {
                ESP_LOGI(TAG, "Factory reset");
                nvs_flash_erase();
                esp_restart();
            }
        }, this);
    }

    void InitializeSpi() {
        ESP_LOGI(TAG, "Initialize SSCMA SPI bus");
        spi_bus_config_t spi_cfg = {0};

        spi_cfg.mosi_io_num = BSP_SPI2_HOST_MOSI;
        spi_cfg.miso_io_num = BSP_SPI2_HOST_MISO;
        spi_cfg.sclk_io_num = BSP_SPI2_HOST_SCLK;
        spi_cfg.quadwp_io_num = -1;
        spi_cfg.quadhd_io_num = -1;
        spi_cfg.isr_cpu_id = ESP_INTR_CPU_AFFINITY_1;
        spi_cfg.max_transfer_sz = 4095;
   
        ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &spi_cfg, SPI_DMA_CH_AUTO));

        ESP_LOGI(TAG, "Initialize QSPI bus");

        spi_bus_config_t qspi_cfg = {0};
        qspi_cfg.sclk_io_num = BSP_SPI3_HOST_PCLK;
        qspi_cfg.data0_io_num = BSP_SPI3_HOST_DATA0;
        qspi_cfg.data1_io_num = BSP_SPI3_HOST_DATA1;
        qspi_cfg.data2_io_num = BSP_SPI3_HOST_DATA2;
        qspi_cfg.data3_io_num = BSP_SPI3_HOST_DATA3;
        qspi_cfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * DRV_LCD_BITS_PER_PIXEL / 8 / CONFIG_BSP_LCD_SPI_DMA_SIZE_DIV;
    
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &qspi_cfg, SPI_DMA_CH_AUTO));
    }

    void Initializespd2010Display() {
        ESP_LOGI(TAG, "Install panel IO");
        const esp_lcd_panel_io_spi_config_t io_config = {
            .cs_gpio_num = BSP_LCD_SPI_CS,
            .dc_gpio_num = GPIO_NUM_NC,
            .spi_mode = 3,
            .pclk_hz = DRV_LCD_PIXEL_CLK_HZ,
            .trans_queue_depth = 2,
            .lcd_cmd_bits = DRV_LCD_CMD_BITS,
            .lcd_param_bits = DRV_LCD_PARAM_BITS,
            .flags = {
                .quad_mode = true,
            },
        };
        spd2010_vendor_config_t vendor_config = {
            .flags = {
                .use_qspi_interface = 1,
            },
        };
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_LCD_SPI_NUM, &io_config, &panel_io_);
    
        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.rgb_ele_order = DRV_LCD_RGB_ELEMENT_ORDER;
        panel_config.bits_per_pixel = DRV_LCD_BITS_PER_PIXEL;
        panel_config.reset_gpio_num = BSP_LCD_GPIO_RST; // Shared with Touch reset
        panel_config.vendor_config = &vendor_config;
        esp_lcd_new_panel_spd2010(panel_io_, &panel_config, &panel_);

        esp_lcd_panel_reset(panel_);
        esp_lcd_panel_init(panel_);
        esp_lcd_panel_mirror(panel_, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        esp_lcd_panel_disp_on_off(panel_, true);

        display_ = new CustomLcdDisplay(panel_io_, panel_,
            DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        
        // 使每次刷新的起始列数索引是4的倍数且列数总数是4的倍数，以满足SPD2010的要求
        lv_display_add_event_cb(lv_display_get_default(), [](lv_event_t *e) {
            lv_area_t *area = (lv_area_t *)lv_event_get_param(e);
            uint16_t x1 = area->x1;
            uint16_t x2 = area->x2;
            // round the start of area down to the nearest 4N number
            area->x1 = (x1 >> 2) << 2;
            // round the end of area up to the nearest 4M+3 number
            area->x2 = ((x2 >> 2) << 2) + 3;
        }, LV_EVENT_INVALIDATE_AREA, NULL);
        
    }

    uint16_t BatterygetVoltage(void) {
        static bool initialized = false;
        static adc_oneshot_unit_handle_t adc_handle;
        static adc_cali_handle_t cali_handle = NULL;
        if (!initialized) {
            adc_oneshot_unit_init_cfg_t init_config = {
                .unit_id = ADC_UNIT_1,
            };
            adc_oneshot_new_unit(&init_config, &adc_handle);
    
            adc_oneshot_chan_cfg_t ch_config = {
                .atten = BSP_BAT_ADC_ATTEN,
                .bitwidth = ADC_BITWIDTH_DEFAULT,
            };
            adc_oneshot_config_channel(adc_handle, BSP_BAT_ADC_CHAN, &ch_config);
    
            adc_cali_curve_fitting_config_t cali_config = {
                .unit_id = ADC_UNIT_1,
                .chan = BSP_BAT_ADC_CHAN,
                .atten = BSP_BAT_ADC_ATTEN,
                .bitwidth = ADC_BITWIDTH_DEFAULT,
            };
            if (adc_cali_create_scheme_curve_fitting(&cali_config, &cali_handle) == ESP_OK) {
                initialized = true;
            }
        }
        if (initialized) {
            int raw_value = 0;
            int voltage = 0; // mV
            adc_oneshot_read(adc_handle, BSP_BAT_ADC_CHAN, &raw_value);
            adc_cali_raw_to_voltage(cali_handle, raw_value, &voltage);
            voltage = voltage * 82 / 20;
            // ESP_LOGI(TAG, "voltage: %dmV", voltage);
            return (uint16_t)voltage;
        }
        return 0;
    }

    uint8_t BatterygetPercent(bool print = false) {
        int voltage = 0;
        for (uint8_t i = 0; i < 10; i++) {
            voltage += BatterygetVoltage();
        }
        voltage /= 10;
        int percent = (-1 * voltage * voltage + 9016 * voltage - 19189000) / 10000;
        percent = (percent > 100) ? 100 : (percent < 0) ? 0 : percent;
        if (print) {
            printf("voltage: %dmV, percentage: %d%%\r\n", voltage, percent);
        }
        return (uint8_t)percent;
    }

    void InitializeCmd() {
        esp_console_repl_t *repl = NULL;
        esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
        repl_config.max_cmdline_length = 1024;
        repl_config.prompt = "SenseCAP>";
        
        const esp_console_cmd_t cmd1 = {
            .command = "reboot",
            .help = "reboot the device",
            .hint = nullptr,
            .func = [](int argc, char** argv) -> int {
                esp_restart();
                return 0;
            },
            .argtable = nullptr
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmd1));

        const esp_console_cmd_t cmd2 = {
            .command = "shutdown",
            .help = "shutdown the device",
            .hint = nullptr,
            .func = NULL,
            .argtable = NULL,
            .func_w_context = [](void *context,int argc, char** argv) -> int {
                auto self = static_cast<SensecapWatcher*>(context);
                self->GetBacklight()->SetBrightness(0);
                self->IoExpanderSetLevel(BSP_PWR_SYSTEM, 0);
                return 0;
            },
            .context =this
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmd2));

        const esp_console_cmd_t cmd3 = {
            .command = "battery",
            .help = "get battery percent",
            .hint = NULL,
            .func = NULL,
            .argtable = NULL,
            .func_w_context = [](void *context,int argc, char** argv) -> int {
                auto self = static_cast<SensecapWatcher*>(context);
                self->BatterygetPercent(true);
                return 0;
            },
            .context =this
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmd3));

        const esp_console_cmd_t cmd4 = {
            .command = "factory_reset",
            .help = "factory reset and reboot the device",
            .hint = NULL,
            .func = NULL,
            .argtable = NULL,
            .func_w_context = [](void *context,int argc, char** argv) -> int {
                nvs_flash_erase();
                esp_restart();
                return 0;
            },
            .context =this
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmd4));

        const esp_console_cmd_t cmd5 = {
            .command = "read_mac",
            .help = "Read mac address",
            .hint = NULL,
            .func = NULL,
            .argtable = NULL,
            .func_w_context = [](void *context,int argc, char** argv) -> int {
                uint8_t mac[6];
                esp_read_mac(mac, ESP_MAC_WIFI_STA);
                printf("wifi_sta_mac: " MACSTR "\n", MAC2STR(mac));
                esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
                printf("wifi_softap_mac: " MACSTR "\n", MAC2STR(mac));
                esp_read_mac(mac, ESP_MAC_BT);
                printf("bt_mac: " MACSTR "\n", MAC2STR(mac));
                return 0;
            },
            .context =this
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmd5));

        const esp_console_cmd_t cmd6 = {
            .command = "version",
            .help = "Read version info",
            .hint = NULL,
            .func = NULL,
            .argtable = NULL,
            .func_w_context = [](void *context,int argc, char** argv) -> int {
                auto self = static_cast<SensecapWatcher*>(context);
                auto app_desc = esp_app_get_description();
                const char* region = "UNKNOWN";
                #if defined(CONFIG_LANGUAGE_ZH_CN)
                    region = "CN";
                #elif defined(CONFIG_LANGUAGE_EN_US)
                    region = "US";
                #elif defined(CONFIG_LANGUAGE_JA_JP)
                    region = "JP";
                #elif defined(CONFIG_LANGUAGE_ES_ES)
                    region = "ES";
                #elif defined(CONFIG_LANGUAGE_DE_DE)
                    region = "DE";
                #elif defined(CONFIG_LANGUAGE_FR_FR)
                    region = "FR";
                #elif defined(CONFIG_LANGUAGE_IT_IT)
                    region = "IT";
                #elif defined(CONFIG_LANGUAGE_PT_PT)
                    region = "PT";
                #elif defined(CONFIG_LANGUAGE_RU_RU)
                    region = "RU";
                #elif defined(CONFIG_LANGUAGE_KO_KR)
                    region = "KR";
                #endif
                printf("{\"type\":0,\"name\":\"VER?\",\"code\":0,\"data\":{\"software\":\"%s\",\"hardware\":\"watcher xiaozhi agent\",\"camera\":%d,\"region\":\"%s\"}}\n",
                       app_desc->version,
                       self->GetCamera() == nullptr ? 0 : 1,
                       region);
                return 0;
            },
            .context =this
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmd6));

        esp_console_dev_uart_config_t hw_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw_config, &repl_config, &repl));
        ESP_ERROR_CHECK(esp_console_start_repl(repl));
    }

    void InitializeCamera() {

        ESP_LOGI(TAG, "Initialize Camera");

        // !!!NOTE: SD Card use same SPI bus as sscma client, so we need to disable SD card CS pin first
        const gpio_config_t io_config = {
            .pin_bit_mask = (1ULL << BSP_SD_SPI_CS),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        esp_err_t ret = gpio_config(&io_config);
        if (ret != ESP_OK)
            return;

        gpio_set_level(BSP_SD_SPI_CS, 1);

        camera_ = new SscmaCamera(io_exp_handle);
    }

public:
    SensecapWatcher() {
        ESP_LOGI(TAG, "Initialize Sensecap Watcher");
        InitializePowerSaveTimer();
        InitializeI2c();
        InitializeSpi();
        InitializeExpander();
        InitializeCmd();  //工厂生产测试使用
        InitializeButton();
        InitializeKnob();
        Initializespd2010Display();
        GetBacklight()->RestoreBrightness();  // 对于不带摄像头的版本，InitializeCamera需要3s, 所以先恢复背光亮度
        InitializeCamera();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static SensecapAudioCodec audio_codec(
            i2c_bus_, 
            AUDIO_INPUT_SAMPLE_RATE, 
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, 
            AUDIO_I2S_GPIO_BCLK, 
            AUDIO_I2S_GPIO_WS, 
            AUDIO_I2S_GPIO_DOUT, 
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, 
            AUDIO_CODEC_ES8311_ADDR, 
            AUDIO_CODEC_ES7243E_ADDR, 
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
    
    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    // 根据 https://github.com/Seeed-Studio/OSHW-SenseCAP-Watcher/blob/main/Hardware/SenseCAP_Watcher_v1.0_SCH.pdf
    // RGB LED型号为 ws2813 mini, 连接在GPIO 40，供电电压 3.3v, 没有连接 BIN 双信号线
    // 可以直接兼容SingleLED采用的ws2812
    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level != PowerSaveLevel::LOW_POWER) {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveLevel(level);
    }

    virtual bool GetBatteryLevel(int &level, bool& charging, bool& discharging) override {
        static bool last_discharging = false;
        charging = (IoExpanderGetLevel(BSP_PWR_VBUS_IN_DET) == 0);
        discharging = !charging;
        level = (int)BatterygetPercent(false);

        if (discharging != last_discharging) {
            power_save_timer_->SetEnabled(discharging);
            last_discharging = discharging;
        }
        if (level <= 1  &&  discharging) {
            ESP_LOGI(TAG, "Battery level is low, shutting down");
            IoExpanderSetLevel(BSP_PWR_SYSTEM, 0);
        }
        return true;
    }

    virtual Camera* GetCamera() override {
        return camera_;
    }
};

DECLARE_BOARD(SensecapWatcher);

// 定义静态成员变量
SensecapWatcher* SensecapWatcher::instance_ = nullptr;
