#include "display/lv_display.h"
#include "misc/lv_event.h"
#include "wifi_board.h"
#include "sensecap_audio_codec.h"
#include "display/lcd_display.h"
#include "application.h"
#include "device_state.h"
#include "knob.h"
#include "config.h"
#include "led/single_led.h"
#include "power_save_timer.h"
#include "sscma_camera.h"
#include "lvgl_theme.h"
#include "nexus_wifi_qr.h"

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

#include <cctype>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <vector>
#include <material_symbols.h>

#include "assets/lang_config.h"

LV_FONT_DECLARE(font_noto_sans_basic_20_4);
LV_FONT_DECLARE(font_material_symbols_30_4);

#define TAG "sensecap_watcher"

// V4 UI bench mode (knob-rotation screen cycling). Keep at 0 for real automatic V4 flow.
// Rendering helpers (Show*Screen) remain; only navigation ownership changes.
#ifndef W13_V4_UI_BENCH_MODE
#define W13_V4_UI_BENCH_MODE 0
#endif

class CustomLcdDisplay : public SpiLcdDisplay {
    private:
        enum class V4Screen {
            Boot,
            ProvisionQr,
            WaitingWifi,
            Binding,
            ReadySplash,
            Main,
            Listening,
            Speaking,
        };

        // Right-arc wheel menu (Main / Listening / Speaking). Geometry unchanged.
        enum class WheelMenuItem : int {
            Talk = 0,
            Tasks = 1,
            Volume = 2,
            ChangeWifi = 3,
            Count = 4,
        };

        enum class WheelMode {
            Inactive,          // provisioning / boot / ready — menu not interactive
            Browse,            // rotate moves selection; click activates
            VolumeAdjust,      // rotate changes speaker volume; click exits
            TasksPlaceholder,  // "Tasks" / "Coming soon"; click exits
            WifiConfirm,       // CHANGE WI-FI? No/Yes prototype; click confirms
        };

        lv_obj_t* v4_layer_ = nullptr;
        lv_obj_t* perimeter_a_ = nullptr;
        lv_obj_t* perimeter_b_ = nullptr;

        // Shared content widgets
        lv_obj_t* title_label_ = nullptr;
        lv_obj_t* subtitle_label_ = nullptr;
        lv_obj_t* body_label_ = nullptr;
        lv_obj_t* icon_label_ = nullptr;
        lv_obj_t* qr_image_ = nullptr;

        // Main / listening chrome
        lv_obj_t* status_time_ = nullptr;
        lv_obj_t* status_wifi_ = nullptr;
        lv_obj_t* status_battery_ = nullptr;
        lv_obj_t* nexus_logo_ = nullptr;
        lv_obj_t* listening_label_ = nullptr;
        lv_obj_t* icon_talk_ = nullptr;
        lv_obj_t* icon_task_ = nullptr;
        lv_obj_t* icon_volume_ = nullptr;
        lv_obj_t* icon_wifi_ = nullptr;
        // Screen 4 ready check — LVGL line polyline (not a scaled font glyph).
        lv_obj_t* ready_check_ = nullptr;

        // Low-battery overlay perimeter
        lv_obj_t* low_bat_a_ = nullptr;
        lv_obj_t* low_bat_b_ = nullptr;

        esp_timer_handle_t ready_timer_ = nullptr;
        bool panel_display_off_ = false;
        bool binding_ui_shown_ = false;
        bool ready_splash_active_ = false;
        V4Screen screen_ = V4Screen::Boot;
        int bench_index_ = 0;
        int64_t last_bench_cycle_us_ = 0;
        int64_t last_wheel_rotate_us_ = 0;

        WheelMenuItem menu_selected_ = WheelMenuItem::Talk;  // initial selection
        WheelMode wheel_mode_ = WheelMode::Inactive;
        bool wifi_confirm_yes_ = false;  // default No on Change Wi-Fi confirm

        static constexpr uint32_t kPerimeterColor = 0x7CFF14;  // lime green per V4
        static constexpr int kPerimeterSize = 408;
        static constexpr int kPerimeterWidth = 8;
        static constexpr uint32_t kReadySplashUs = 4000000;  // 4 seconds
        // Side icons: prior bench size was 480/256 ≈ 1.875×; +20% → 576/256 = 2.25×.
        // Applied on MAIN + LISTENING only. Centers follow the right circular arc.
        static constexpr int32_t kSideIconScale = 576;  // 480 * 1.2
        static constexpr int kSideIconTalkX = 326;
        static constexpr int kSideIconTalkY = 115;
        static constexpr int kSideIconTasksX = 346;
        static constexpr int kSideIconTasksY = 175;
        static constexpr int kSideIconVolumeX = 346;
        static constexpr int kSideIconVolumeY = 237;
        static constexpr int kSideIconWifiX = 326;
        static constexpr int kSideIconWifiY = 299;
        // Screen 1 geometry for 412x412 circular panel (safe content ~65..347).
        // Do NOT use transform_scale on Screen 1 text — it overflows the circle.
        // Do NOT scale the I1 QR — LVGL cannot transform indexed images.
        static constexpr int kProvisionTitleCenterY = 82;
        static constexpr int kProvisionSubtitleCenterY = 120;
        static constexpr int kProvisionQrX = 126;
        static constexpr int kProvisionQrY = 155;
        static constexpr int kProvisionTitleMaxW = 270;
        static constexpr int kProvisionSubtitleMaxW = 250;
        // Screens 2+3 shared large Wi-Fi: ~60 px (within 55–65), center (206, 115).
        static constexpr int32_t kLargeWifiScale = 512;  // 30px font × 2.0 = 60 px
        static constexpr int kLargeWifiCenterX = 206;
        static constexpr int kLargeWifiCenterY = 115;
        // Screen 4 Ready: vector check center + text.
        static constexpr int kReadyCheckCenterX = 206;
        static constexpr int kReadyCheckCenterY = 150;
        static constexpr int kReadyTextCenterY = 215;
        // Screen 5 MAIN status along upper circular arc (centers).
        static constexpr int kStatusTimeX = 105;
        static constexpr int kStatusTimeY = 78;
        static constexpr int kStatusWifiX = 206;
        static constexpr int kStatusWifiY = 72;
        static constexpr int kStatusBatteryX = 280;
        static constexpr int kStatusBatteryY = 78;
        static constexpr int kBenchScreenCount = 6;
        static constexpr int kVolumeStep = 5;
        static constexpr int kVolumeMin = 0;
        static constexpr int kVolumeMax = 100;
        static constexpr int64_t kWheelRotateDebounceUs = 120000;  // 120 ms

        static bool IsClockText(const char* status) {
            return status != nullptr && strlen(status) == 5 && status[2] == ':' &&
                   isdigit(static_cast<unsigned char>(status[0])) &&
                   isdigit(static_cast<unsigned char>(status[1])) &&
                   isdigit(static_cast<unsigned char>(status[3])) &&
                   isdigit(static_cast<unsigned char>(status[4]));
        }

        // Static lime-green split bands (gaps at 12 o'clock and 6 o'clock). Never animated.
        void CreateStaticSplitPerimeter(lv_obj_t* parent, lv_obj_t** arc_a, lv_obj_t** arc_b) {
            auto make_arc = [&](int start_deg, int end_deg) -> lv_obj_t* {
                lv_obj_t* ring = lv_arc_create(parent);
                lv_obj_set_size(ring, kPerimeterSize, kPerimeterSize);
                lv_obj_center(ring);
                lv_arc_set_bg_angles(ring, 0, 360);
                lv_arc_set_angles(ring, start_deg, end_deg);
                lv_arc_set_rotation(ring, 270);  // 0° at top
                lv_obj_remove_style(ring, nullptr, LV_PART_KNOB);
                lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_set_style_arc_width(ring, kPerimeterWidth, LV_PART_MAIN);
                lv_obj_set_style_arc_opa(ring, LV_OPA_TRANSP, LV_PART_MAIN);
                lv_obj_set_style_arc_width(ring, kPerimeterWidth, LV_PART_INDICATOR);
                lv_obj_set_style_arc_color(ring, lv_color_hex(kPerimeterColor), LV_PART_INDICATOR);
                lv_obj_set_style_arc_opa(ring, LV_OPA_COVER, LV_PART_INDICATOR);
                lv_obj_set_style_arc_rounded(ring, true, LV_PART_INDICATOR);
                return ring;
            };
            // Gaps ~8° at top and bottom.
            *arc_a = make_arc(8, 172);
            *arc_b = make_arc(188, 352);
        }

        void HideAllContent() {
            auto hide = [](lv_obj_t* o) {
                if (o != nullptr) {
                    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
                }
            };
            hide(title_label_);
            hide(subtitle_label_);
            hide(body_label_);
            hide(icon_label_);
            hide(qr_image_);
            hide(status_time_);
            hide(status_wifi_);
            hide(status_battery_);
            hide(nexus_logo_);
            hide(listening_label_);
            hide(icon_talk_);
            hide(icon_task_);
            hide(icon_volume_);
            hide(icon_wifi_);
            hide(ready_check_);
            if (emoji_box_ != nullptr) {
                lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
            }
        }

        void ShowV4Layer() {
            if (container_ != nullptr)
                lv_obj_add_flag(container_, LV_OBJ_FLAG_HIDDEN);
            if (top_bar_ != nullptr)
                lv_obj_add_flag(top_bar_, LV_OBJ_FLAG_HIDDEN);
            if (status_bar_ != nullptr)
                lv_obj_add_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
            if (bottom_bar_ != nullptr)
                lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
            if (v4_layer_ != nullptr) {
                lv_obj_remove_flag(v4_layer_, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_style_bg_color(v4_layer_, lv_color_hex(0x000000), 0);
                lv_obj_set_style_bg_opa(v4_layer_, LV_OPA_COVER, 0);
                lv_obj_move_foreground(v4_layer_);
            }
            if (perimeter_a_ != nullptr)
                lv_obj_move_foreground(perimeter_a_);
            if (perimeter_b_ != nullptr)
                lv_obj_move_foreground(perimeter_b_);
        }

        void SetObjScale(lv_obj_t* obj, int32_t scale) {
            if (obj == nullptr) {
                return;
            }
            lv_obj_set_style_transform_pivot_x(obj, lv_pct(50), 0);
            lv_obj_set_style_transform_pivot_y(obj, lv_pct(50), 0);
            lv_obj_set_style_transform_scale(obj, scale, 0);
        }

        void ClearProvisionScales() {
            // Restore shared labels after Screen 1 / splash icon scales.
            SetObjScale(title_label_, LV_SCALE_NONE);
            SetObjScale(subtitle_label_, LV_SCALE_NONE);
            SetObjScale(icon_label_, LV_SCALE_NONE);
            if (qr_image_ != nullptr) {
                lv_image_set_scale(qr_image_, LV_SCALE_NONE);
            }
            auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
            const lv_font_t* text_font =
                (lvgl_theme != nullptr && lvgl_theme->text_font() != nullptr)
                    ? lvgl_theme->text_font()->font()
                    : nullptr;
            const lv_font_t* icon_font =
                (lvgl_theme != nullptr && lvgl_theme->icon_font() != nullptr)
                    ? lvgl_theme->icon_font()->font()
                    : nullptr;
            if (title_label_ != nullptr) {
                if (text_font != nullptr) {
                    lv_obj_set_style_text_font(title_label_, text_font, 0);
                }
                lv_obj_set_style_text_letter_space(title_label_, 0, 0);
                lv_obj_set_width(title_label_, LV_SIZE_CONTENT);
            }
            if (subtitle_label_ != nullptr) {
                if (text_font != nullptr) {
                    lv_obj_set_style_text_font(subtitle_label_, text_font, 0);
                }
                lv_obj_set_style_text_letter_space(subtitle_label_, 0, 0);
                lv_obj_set_width(subtitle_label_, LV_SIZE_CONTENT);
            }
            if (icon_label_ != nullptr && icon_font != nullptr) {
                lv_obj_set_style_text_font(icon_label_, icon_font, 0);
            }
            if (ready_check_ != nullptr) {
                lv_obj_add_flag(ready_check_, LV_OBJ_FLAG_HIDDEN);
            }
        }

        void PlaceLabelAtCenter(lv_obj_t* obj, int cx, int cy, int32_t scale) {
            if (obj == nullptr) {
                return;
            }
            // Pivot 50% keeps the visual center at the unscaled object center after scale.
            SetObjScale(obj, scale);
            const lv_font_t* font = lv_obj_get_style_text_font(obj, LV_PART_MAIN);
            int32_t w = (font != nullptr && font->line_height > 0) ? font->line_height : 20;
            int32_t h = w;
            const char* txt = lv_label_get_text(obj);
            if (txt != nullptr && font != nullptr && txt[0] != '\0') {
                lv_point_t sz = {};
                lv_text_get_size(&sz, txt, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
                if (sz.x > 0) {
                    w = sz.x;
                }
                if (sz.y > 0) {
                    h = sz.y;
                }
            }
            lv_obj_set_pos(obj, cx - w / 2, cy - h / 2);
        }

        void PlaceSideIconAtCenter(lv_obj_t* obj, int cx, int cy) {
            PlaceLabelAtCenter(obj, cx, cy, kSideIconScale);
        }

        // Shared by Screen 2 (Waiting) and Screen 3 (Binding) so the icon cannot drift.
        void ShowLargeWifiIcon() {
            if (icon_label_ == nullptr) {
                return;
            }
            lv_label_set_text(icon_label_, MATERIAL_SYMBOLS_WIFI);
            lv_obj_set_style_text_font(icon_label_, &font_material_symbols_30_4, 0);
            lv_obj_set_style_text_color(icon_label_, lv_color_hex(kPerimeterColor), 0);
            PlaceLabelAtCenter(icon_label_, kLargeWifiCenterX, kLargeWifiCenterY, kLargeWifiScale);
            lv_obj_remove_flag(icon_label_, LV_OBJ_FLAG_HIDDEN);
        }

        void ApplyMainStatusLayout() {
            // Upper circular arc: time left, Wi-Fi top-center, battery right — not a packed row.
            if (status_time_ != nullptr) {
                lv_obj_set_style_text_font(status_time_, &font_noto_sans_basic_20_4, 0);
                lv_obj_set_style_text_color(status_time_, lv_color_hex(0xFFFFFF), 0);
                // Closest built-in to ~22–24 px without transform_scale.
                PlaceLabelAtCenter(status_time_, kStatusTimeX, kStatusTimeY, LV_SCALE_NONE);
            }
            if (status_wifi_ != nullptr) {
                lv_obj_set_style_text_font(status_wifi_, &font_material_symbols_30_4, 0);
                lv_obj_set_style_text_color(status_wifi_, lv_color_hex(0xFFFFFF), 0);
                PlaceLabelAtCenter(status_wifi_, kStatusWifiX, kStatusWifiY, LV_SCALE_NONE);
            }
            if (status_battery_ != nullptr) {
                lv_obj_set_style_text_font(status_battery_, &font_material_symbols_30_4, 0);
                lv_obj_set_style_text_color(status_battery_, lv_color_hex(0xFFFFFF), 0);
                PlaceLabelAtCenter(status_battery_, kStatusBatteryX, kStatusBatteryY, LV_SCALE_NONE);
            }
        }

        void ApplySideIconLayout() {
            // Four controls along the right-hand circular arc (inward curve):
            // Talk / Tasks / Volume / Change Wi-Fi — not a straight column.
            PlaceSideIconAtCenter(icon_talk_, kSideIconTalkX, kSideIconTalkY);
            PlaceSideIconAtCenter(icon_task_, kSideIconTasksX, kSideIconTasksY);
            PlaceSideIconAtCenter(icon_volume_, kSideIconVolumeX, kSideIconVolumeY);
            PlaceSideIconAtCenter(icon_wifi_, kSideIconWifiX, kSideIconWifiY);
        }

        void ApplyIconSelectionStyle(lv_obj_t* icon, bool selected) {
            if (icon == nullptr) {
                return;
            }
            // Geometry untouched — selection = green brightness only (no outline/box/glow).
            lv_obj_set_style_text_color(icon, lv_color_hex(kPerimeterColor), 0);
            lv_obj_set_style_text_opa(icon, selected ? LV_OPA_COVER : LV_OPA_40, 0);
            lv_obj_set_style_outline_width(icon, 0, 0);
            lv_obj_set_style_outline_pad(icon, 0, 0);
            lv_obj_set_style_outline_opa(icon, LV_OPA_TRANSP, 0);
        }

        void ApplyMenuSelectionVisuals() {
            ApplySideIconLayout();
            ApplyIconSelectionStyle(icon_talk_, menu_selected_ == WheelMenuItem::Talk);
            ApplyIconSelectionStyle(icon_task_, menu_selected_ == WheelMenuItem::Tasks);
            ApplyIconSelectionStyle(icon_volume_, menu_selected_ == WheelMenuItem::Volume);
            ApplyIconSelectionStyle(icon_wifi_, menu_selected_ == WheelMenuItem::ChangeWifi);
            if (icon_talk_ != nullptr)
                lv_obj_remove_flag(icon_talk_, LV_OBJ_FLAG_HIDDEN);
            if (icon_task_ != nullptr)
                lv_obj_remove_flag(icon_task_, LV_OBJ_FLAG_HIDDEN);
            if (icon_volume_ != nullptr)
                lv_obj_remove_flag(icon_volume_, LV_OBJ_FLAG_HIDDEN);
            if (icon_wifi_ != nullptr)
                lv_obj_remove_flag(icon_wifi_, LV_OBJ_FLAG_HIDDEN);
        }

        void ShowSideIcons(bool /*talk_active*/) {
            // talk_active retained for call-site compatibility; selection owns highlight.
            ApplyMenuSelectionVisuals();
        }

        bool IsMenuScreen() const {
            return screen_ == V4Screen::Main || screen_ == V4Screen::Listening ||
                   screen_ == V4Screen::Speaking;
        }

        void EnterBrowseMode() {
            wheel_mode_ = WheelMode::Browse;
            wifi_confirm_yes_ = false;
        }

        void HideCenterChromeForOverlay() {
            if (nexus_logo_ != nullptr)
                lv_obj_add_flag(nexus_logo_, LV_OBJ_FLAG_HIDDEN);
            if (listening_label_ != nullptr)
                lv_obj_add_flag(listening_label_, LV_OBJ_FLAG_HIDDEN);
            if (emoji_box_ != nullptr)
                lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
            if (status_time_ != nullptr)
                lv_obj_add_flag(status_time_, LV_OBJ_FLAG_HIDDEN);
            if (status_wifi_ != nullptr)
                lv_obj_add_flag(status_wifi_, LV_OBJ_FLAG_HIDDEN);
            if (status_battery_ != nullptr)
                lv_obj_add_flag(status_battery_, LV_OBJ_FLAG_HIDDEN);
            if (ready_check_ != nullptr)
                lv_obj_add_flag(ready_check_, LV_OBJ_FLAG_HIDDEN);
            if (qr_image_ != nullptr)
                lv_obj_add_flag(qr_image_, LV_OBJ_FLAG_HIDDEN);
            if (icon_label_ != nullptr)
                lv_obj_add_flag(icon_label_, LV_OBJ_FLAG_HIDDEN);
        }

        void ShowOverlayText(const char* title, const char* subtitle, const char* body) {
            HideCenterChromeForOverlay();
            if (title_label_ != nullptr) {
                lv_label_set_text(title_label_, title != nullptr ? title : "");
                lv_obj_set_style_text_color(title_label_, lv_color_hex(0xFFFFFF), 0);
                lv_obj_set_style_text_opa(title_label_, LV_OPA_COVER, 0);
                lv_obj_set_width(title_label_, LV_HOR_RES * 0.75);
                lv_obj_set_style_text_align(title_label_, LV_TEXT_ALIGN_CENTER, 0);
                lv_obj_align(title_label_, LV_ALIGN_CENTER, -30, -40);
                if (title != nullptr && title[0] != '\0') {
                    lv_obj_remove_flag(title_label_, LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_add_flag(title_label_, LV_OBJ_FLAG_HIDDEN);
                }
            }
            if (subtitle_label_ != nullptr) {
                lv_label_set_text(subtitle_label_, subtitle != nullptr ? subtitle : "");
                lv_obj_set_style_text_color(subtitle_label_, lv_color_hex(0xFFFFFF), 0);
                lv_obj_set_width(subtitle_label_, LV_HOR_RES * 0.75);
                lv_obj_set_style_text_align(subtitle_label_, LV_TEXT_ALIGN_CENTER, 0);
                lv_obj_align(subtitle_label_, LV_ALIGN_CENTER, -30, 10);
                if (subtitle != nullptr && subtitle[0] != '\0') {
                    lv_obj_remove_flag(subtitle_label_, LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_add_flag(subtitle_label_, LV_OBJ_FLAG_HIDDEN);
                }
            }
            if (body_label_ != nullptr) {
                lv_label_set_text(body_label_, body != nullptr ? body : "");
                lv_obj_set_style_text_color(body_label_, lv_color_hex(0xFFFFFF), 0);
                lv_obj_set_width(body_label_, LV_HOR_RES * 0.75);
                lv_obj_set_style_text_align(body_label_, LV_TEXT_ALIGN_CENTER, 0);
                lv_obj_align(body_label_, LV_ALIGN_CENTER, -30, 55);
                if (body != nullptr && body[0] != '\0') {
                    lv_obj_remove_flag(body_label_, LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_add_flag(body_label_, LV_OBJ_FLAG_HIDDEN);
                }
            }
            ApplyMenuSelectionVisuals();
            BringPerimeterFront();
        }

        void ApplyWifiConfirmOptionVisuals() {
            // No = subtitle, Yes = body. Selection = brightness only (no outline).
            if (subtitle_label_ != nullptr) {
                const bool selected = !wifi_confirm_yes_;
                lv_obj_set_style_text_opa(subtitle_label_, selected ? LV_OPA_COVER : LV_OPA_40, 0);
                lv_obj_set_style_outline_width(subtitle_label_, 0, 0);
                lv_obj_set_style_outline_opa(subtitle_label_, LV_OPA_TRANSP, 0);
            }
            if (body_label_ != nullptr) {
                const bool selected = wifi_confirm_yes_;
                lv_obj_set_style_text_opa(body_label_, selected ? LV_OPA_COVER : LV_OPA_40, 0);
                lv_obj_set_style_outline_width(body_label_, 0, 0);
                lv_obj_set_style_outline_opa(body_label_, LV_OPA_TRANSP, 0);
            }
        }

        void RefreshVolumeOverlay() {
            auto codec = Board::GetInstance().GetAudioCodec();
            const int vol = codec != nullptr ? codec->output_volume() : 0;
            char buf[16];
            snprintf(buf, sizeof(buf), "%d", vol);
            ShowOverlayText("Volume", buf, nullptr);
        }

        void ShowTasksPlaceholder() {
            wheel_mode_ = WheelMode::TasksPlaceholder;
            menu_selected_ = WheelMenuItem::Tasks;
            ShowOverlayText("Tasks", "Coming soon", nullptr);
        }

        void ShowVolumeAdjustMode() {
            wheel_mode_ = WheelMode::VolumeAdjust;
            menu_selected_ = WheelMenuItem::Volume;
            RefreshVolumeOverlay();
        }

        void ShowWifiConfirmMode() {
            wheel_mode_ = WheelMode::WifiConfirm;
            menu_selected_ = WheelMenuItem::ChangeWifi;
            wifi_confirm_yes_ = false;  // default No
            ShowOverlayText("CHANGE WI-FI?", "No", "Yes");
            ApplyWifiConfirmOptionVisuals();
        }

        void ReturnToMainWithSelection() {
            wheel_mode_ = WheelMode::Browse;
            wifi_confirm_yes_ = false;
            ShowMainScreen();
        }

        void MoveMenuSelection(bool clockwise) {
            int idx = static_cast<int>(menu_selected_);
            const int count = static_cast<int>(WheelMenuItem::Count);
            idx = clockwise ? (idx + 1) % count : (idx - 1 + count) % count;
            menu_selected_ = static_cast<WheelMenuItem>(idx);
            ApplyMenuSelectionVisuals();
            ESP_LOGI(TAG, "Wheel menu select %d", idx);
        }

        void AdjustSpeakerVolume(bool clockwise) {
            auto codec = Board::GetInstance().GetAudioCodec();
            if (codec == nullptr) {
                return;
            }
            // Volume Adjust mode only: clockwise increases, counter-clockwise decreases.
            int current = codec->output_volume();
            int next = current + (clockwise ? kVolumeStep : -kVolumeStep);
            if (next > kVolumeMax) {
                next = kVolumeMax;
            } else if (next < kVolumeMin) {
                next = kVolumeMin;
            }
            codec->SetOutputVolume(next);
            ESP_LOGI(TAG, "Wheel volume %d -> %d", current, next);
            RefreshVolumeOverlay();
        }

        void ActivateSelectedMenuItem() {
            switch (menu_selected_) {
                case WheelMenuItem::Talk:
                    // Idle → Listening (or leave Listening/Speaking). Approved V4 Listening preserved.
                    Application::GetInstance().ToggleChatState();
                    break;
                case WheelMenuItem::Tasks:
                    if (screen_ != V4Screen::Main) {
                        ESP_LOGI(TAG, "Tasks activates on Main only (selection kept)");
                        return;
                    }
                    ShowTasksPlaceholder();
                    break;
                case WheelMenuItem::Volume:
                    if (screen_ != V4Screen::Main) {
                        ESP_LOGI(TAG, "Volume adjust activates on Main only (selection kept)");
                        return;
                    }
                    ShowVolumeAdjustMode();
                    break;
                case WheelMenuItem::ChangeWifi:
                    if (screen_ != V4Screen::Main) {
                        ESP_LOGI(TAG, "Change Wi-Fi confirm activates on Main only (selection kept)");
                        return;
                    }
                    ShowWifiConfirmMode();
                    break;
                default:
                    break;
            }
        }

        void BringPerimeterFront() {
            if (perimeter_a_ != nullptr)
                lv_obj_move_foreground(perimeter_a_);
            if (perimeter_b_ != nullptr)
                lv_obj_move_foreground(perimeter_b_);
        }

        // Replaces stock white "Initializing..." — black + frozen perimeter + NEXUS.
        // No status row, no right-side controls, no animation. Screens 1–6 unchanged.
        void ShowBootInitializingScreen() {
            screen_ = V4Screen::Boot;
            ready_splash_active_ = false;
            wheel_mode_ = WheelMode::Inactive;
            ShowV4Layer();
            HideAllContent();

            if (nexus_logo_ != nullptr) {
                lv_label_set_text(nexus_logo_, "NEXUS");
                lv_obj_set_style_text_color(nexus_logo_, lv_color_hex(0xFFFFFF), 0);
                lv_obj_set_style_text_letter_space(nexus_logo_, 4, 0);
                // True center (MAIN uses x=-30 to clear side icons).
                lv_obj_align(nexus_logo_, LV_ALIGN_CENTER, 0, -16);
                lv_obj_remove_flag(nexus_logo_, LV_OBJ_FLAG_HIDDEN);
            }
            if (subtitle_label_ != nullptr) {
                lv_label_set_text(subtitle_label_, Lang::Strings::INITIALIZING);
                lv_obj_set_style_text_color(subtitle_label_, lv_color_hex(0xFFFFFF), 0);
                lv_obj_set_style_text_letter_space(subtitle_label_, 0, 0);
                lv_obj_set_width(subtitle_label_, LV_SIZE_CONTENT);
                lv_obj_align(subtitle_label_, LV_ALIGN_CENTER, 0, 28);
                lv_obj_remove_flag(subtitle_label_, LV_OBJ_FLAG_HIDDEN);
            }
            BringPerimeterFront();
        }

        void ShowProvisionQrScreen() {
            screen_ = V4Screen::ProvisionQr;
            ready_splash_active_ = false;
            wheel_mode_ = WheelMode::Inactive;
            ShowV4Layer();
            HideAllContent();

            // Screen 1: perimeter + heading + subtitle + QR inside circular safe area.
            // No transform_scale on text. No image scale on I1 QR (unsupported by LVGL).
            // Hierarchy: CONNECT TO NEXUS → Scan to begin setup → [QR].
            constexpr int kDisplayCenter = 206;

            if (title_label_ != nullptr) {
                lv_label_set_text(title_label_, "CONNECT TO NEXUS");
                lv_obj_set_style_text_color(title_label_, lv_color_hex(0xFFFFFF), 0);
                // Board text font is Noto Sans 30 — fits ~26–30px target without scaling.
                // Slight negative tracking keeps one line inside max width 270.
                lv_obj_set_style_text_letter_space(title_label_, -2, 0);
                lv_label_set_long_mode(title_label_, LV_LABEL_LONG_CLIP);
                lv_obj_set_width(title_label_, kProvisionTitleMaxW);
                lv_obj_set_style_text_align(title_label_, LV_TEXT_ALIGN_CENTER, 0);
                SetObjScale(title_label_, LV_SCALE_NONE);
                lv_obj_align(title_label_, LV_ALIGN_CENTER, 0,
                             kProvisionTitleCenterY - kDisplayCenter);
                lv_obj_remove_flag(title_label_, LV_OBJ_FLAG_HIDDEN);
                lv_obj_move_foreground(title_label_);
            }
            if (subtitle_label_ != nullptr) {
                lv_label_set_text(subtitle_label_, "Scan to begin setup");
                lv_obj_set_style_text_color(subtitle_label_, lv_color_hex(0xFFFFFF), 0);
                // ~20px subtitle (within 18–22) via dedicated font — no transform_scale.
                lv_obj_set_style_text_font(subtitle_label_, &font_noto_sans_basic_20_4, 0);
                lv_obj_set_style_text_letter_space(subtitle_label_, 0, 0);
                lv_label_set_long_mode(subtitle_label_, LV_LABEL_LONG_CLIP);
                lv_obj_set_width(subtitle_label_, kProvisionSubtitleMaxW);
                lv_obj_set_style_text_align(subtitle_label_, LV_TEXT_ALIGN_CENTER, 0);
                SetObjScale(subtitle_label_, LV_SCALE_NONE);
                lv_obj_align(subtitle_label_, LV_ALIGN_CENTER, 0,
                             kProvisionSubtitleCenterY - kDisplayCenter);
                lv_obj_remove_flag(subtitle_label_, LV_OBJ_FLAG_HIDDEN);
                lv_obj_move_foreground(subtitle_label_);
            }
            if (qr_image_ != nullptr) {
                // Re-assert source / visibility / opacity every show (bench cycles hide it).
                lv_image_set_src(qr_image_, &nexus_wifi_qr);
                lv_image_set_scale(qr_image_, LV_SCALE_NONE);  // I1 cannot be scaled
                lv_image_set_antialias(qr_image_, false);      // hard pixel edges
                lv_obj_set_style_opa(qr_image_, LV_OPA_COVER, 0);
                lv_obj_set_style_image_opa(qr_image_, LV_OPA_COVER, 0);
                lv_obj_set_style_image_recolor_opa(qr_image_, LV_OPA_TRANSP, 0);
                // Native 165x165 ≈ target 160; top-left ≈ (126, 155), above black bg.
                lv_obj_set_pos(qr_image_, kProvisionQrX, kProvisionQrY);
                lv_obj_remove_flag(qr_image_, LV_OBJ_FLAG_HIDDEN);
                lv_obj_move_foreground(qr_image_);
            }
            // Perimeter stays on top of content but does not cover the center QR.
            BringPerimeterFront();
        }

        void ShowWaitingWifiScreen() {
            screen_ = V4Screen::WaitingWifi;
            ready_splash_active_ = false;
            wheel_mode_ = WheelMode::Inactive;
            ClearProvisionScales();
            ShowV4Layer();
            HideAllContent();

            // Same large Wi-Fi as Screen 3 — only text below differs.
            ShowLargeWifiIcon();
            if (title_label_ != nullptr) {
                lv_label_set_text(title_label_, "Waiting for Wi-Fi");
                lv_obj_set_style_text_color(title_label_, lv_color_hex(0xFFFFFF), 0);
                lv_obj_set_width(title_label_, LV_SIZE_CONTENT);
                lv_label_set_long_mode(title_label_, LV_LABEL_LONG_CLIP);
                lv_obj_align(title_label_, LV_ALIGN_CENTER, 0, 10);
                lv_obj_remove_flag(title_label_, LV_OBJ_FLAG_HIDDEN);
            }
            if (subtitle_label_ != nullptr) {
                lv_label_set_text(subtitle_label_, "Setup...");
                lv_obj_set_style_text_color(subtitle_label_, lv_color_hex(0xFFFFFF), 0);
                lv_obj_align(subtitle_label_, LV_ALIGN_CENTER, 0, 42);
                lv_obj_remove_flag(subtitle_label_, LV_OBJ_FLAG_HIDDEN);
            }
            // Secondary local setup URL if captive portal does not auto-open.
            if (body_label_ != nullptr) {
                lv_label_set_text(body_label_, "http://192.168.4.1");
                lv_obj_set_style_text_color(body_label_, lv_color_hex(kPerimeterColor), 0);
                lv_obj_align(body_label_, LV_ALIGN_CENTER, 0, 80);
                lv_obj_remove_flag(body_label_, LV_OBJ_FLAG_HIDDEN);
            }
            BringPerimeterFront();
        }

        void ShowBindingScreen(bool mark_real_binding = false) {
            screen_ = V4Screen::Binding;
            // Only mark real binding when ACTIVATION status arrives (not mere version check).
            if (mark_real_binding) {
                binding_ui_shown_ = true;
            }
            ready_splash_active_ = false;
            wheel_mode_ = WheelMode::Inactive;
            ClearProvisionScales();
            ShowV4Layer();
            HideAllContent();

            // Same large Wi-Fi as Screen 2 — only text below differs.
            ShowLargeWifiIcon();
            if (subtitle_label_ != nullptr) {
                lv_label_set_text(subtitle_label_, "Wi-Fi Connected");
                lv_obj_set_style_text_color(subtitle_label_, lv_color_hex(0xFFFFFF), 0);
                lv_obj_align(subtitle_label_, LV_ALIGN_CENTER, 0, -30);
                lv_obj_remove_flag(subtitle_label_, LV_OBJ_FLAG_HIDDEN);
            }
            if (title_label_ != nullptr) {
                lv_label_set_text(title_label_, "Binding device to");
                lv_obj_set_style_text_color(title_label_, lv_color_hex(0xFFFFFF), 0);
                lv_obj_set_width(title_label_, LV_HOR_RES * 0.75);
                lv_obj_set_style_text_align(title_label_, LV_TEXT_ALIGN_CENTER, 0);
                lv_label_set_long_mode(title_label_, LV_LABEL_LONG_CLIP);
                lv_obj_align(title_label_, LV_ALIGN_CENTER, 0, 10);
                lv_obj_remove_flag(title_label_, LV_OBJ_FLAG_HIDDEN);
            }
            if (body_label_ != nullptr) {
                lv_label_set_text(body_label_, "your account");
                lv_obj_set_style_text_color(body_label_, lv_color_hex(0xFFFFFF), 0);
                lv_obj_align(body_label_, LV_ALIGN_CENTER, 0, 48);
                lv_obj_remove_flag(body_label_, LV_OBJ_FLAG_HIDDEN);
            }
            BringPerimeterFront();
        }

        static void ReadySplashTimerCallback(void* arg) {
            auto self = static_cast<CustomLcdDisplay*>(arg);
            DisplayLockGuard lock(self);
#if W13_V4_UI_BENCH_MODE
            // Bench mode keeps READY visible until knob cycles away.
            (void)self;
            return;
#else
            self->ready_splash_active_ = false;
            self->ShowMainScreen();
#endif
        }

        void ShowReadySplashScreen() {
            screen_ = V4Screen::ReadySplash;
            ready_splash_active_ = true;
            wheel_mode_ = WheelMode::Inactive;
#if !W13_V4_UI_BENCH_MODE
            binding_ui_shown_ = false;
#endif
            ClearProvisionScales();
            ShowV4Layer();
            HideAllContent();

            // Large lime check via LVGL line polyline (~60 px) — not a scaled glyph.
            if (icon_label_ != nullptr) {
                lv_obj_add_flag(icon_label_, LV_OBJ_FLAG_HIDDEN);
            }
            if (ready_check_ != nullptr) {
                lv_obj_remove_flag(ready_check_, LV_OBJ_FLAG_HIDDEN);
                lv_obj_move_foreground(ready_check_);
            }
            if (title_label_ != nullptr) {
                lv_label_set_text(title_label_, "Nexus is ready!");
                lv_obj_set_style_text_color(title_label_, lv_color_hex(0xFFFFFF), 0);
                lv_obj_set_width(title_label_, LV_HOR_RES * 0.75);
                lv_obj_set_style_text_align(title_label_, LV_TEXT_ALIGN_CENTER, 0);
                lv_obj_align(title_label_, LV_ALIGN_CENTER, 0, kReadyTextCenterY - 206);
                lv_obj_remove_flag(title_label_, LV_OBJ_FLAG_HIDDEN);
            }
            BringPerimeterFront();

            if (ready_timer_ != nullptr) {
                esp_timer_stop(ready_timer_);
#if !W13_V4_UI_BENCH_MODE
                esp_timer_start_once(ready_timer_, kReadySplashUs);
#endif
            }
        }

        void RefreshStatusRow() {
            auto& board = Board::GetInstance();

            if (status_time_ != nullptr) {
                time_t now = time(nullptr);
                struct tm* tm_now = localtime(&now);
                if (tm_now != nullptr && tm_now->tm_year >= (2025 - 1900)) {
                    char buf[8];
                    strftime(buf, sizeof(buf), "%H:%M", tm_now);
                    lv_label_set_text(status_time_, buf);
                } else {
                    lv_label_set_text(status_time_, "--:--");
                }
            }

            if (status_wifi_ != nullptr) {
                const char* icon = board.GetNetworkStateIcon();
                lv_label_set_text(status_wifi_, icon != nullptr ? icon : MATERIAL_SYMBOLS_WIFI_OFF);
            }

            if (status_battery_ != nullptr) {
                int level = 0;
                bool charging = false;
                bool discharging = false;
                const char* icon = MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_FULL;
                if (board.GetBatteryLevel(level, charging, discharging)) {
                    if (charging) {
                        icon = MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_BOLT;
                    } else {
                        const char* levels[] = {
                            MATERIAL_SYMBOLS_BATTERY_ANDROID_0,
                            MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_1,
                            MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_2,
                            MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_3,
                            MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_4,
                            MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_5,
                            MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_6,
                            MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_FULL,
                        };
                        int idx = level <= 0 ? 0
                                             : (level >= 100 ? 7 : 1 + ((level - 1) * 6 / 99));
                        icon = levels[idx];
                    }
                }
                lv_label_set_text(status_battery_, icon);
            }
        }

        void ClearOverlayOptionStyles() {
            auto clear = [](lv_obj_t* obj) {
                if (obj == nullptr) {
                    return;
                }
                lv_obj_set_style_outline_width(obj, 0, 0);
                lv_obj_set_style_outline_pad(obj, 0, 0);
                lv_obj_set_style_outline_opa(obj, LV_OPA_TRANSP, 0);
                lv_obj_set_style_text_opa(obj, LV_OPA_COVER, 0);
            };
            clear(title_label_);
            clear(subtitle_label_);
            clear(body_label_);
        }

        void ShowMainScreen() {
            screen_ = V4Screen::Main;
            ready_splash_active_ = false;
            EnterBrowseMode();
            ClearProvisionScales();
            ClearOverlayOptionStyles();
            ShowV4Layer();
            HideAllContent();
            RefreshStatusRow();
            ApplyMainStatusLayout();

            if (status_time_ != nullptr)
                lv_obj_remove_flag(status_time_, LV_OBJ_FLAG_HIDDEN);
            if (status_wifi_ != nullptr)
                lv_obj_remove_flag(status_wifi_, LV_OBJ_FLAG_HIDDEN);
            if (status_battery_ != nullptr)
                lv_obj_remove_flag(status_battery_, LV_OBJ_FLAG_HIDDEN);

            if (nexus_logo_ != nullptr) {
                lv_label_set_text(nexus_logo_, "NEXUS");
                lv_obj_set_style_text_letter_space(nexus_logo_, 4, 0);
                lv_obj_align(nexus_logo_, LV_ALIGN_CENTER, -30, 0);
                lv_obj_remove_flag(nexus_logo_, LV_OBJ_FLAG_HIDDEN);
            }

            ApplyMenuSelectionVisuals();
            BringPerimeterFront();
        }

        void ShowListeningScreen() {
            screen_ = V4Screen::Listening;
            ready_splash_active_ = false;
            EnterBrowseMode();
            ClearProvisionScales();
            ShowV4Layer();
            HideAllContent();

            if (listening_label_ != nullptr) {
                lv_label_set_text(listening_label_, "Listening");
                lv_obj_remove_flag(listening_label_, LV_OBJ_FLAG_HIDDEN);
            }

            // Closest built-in yellow happy face (approved — size/position unchanged).
            SpiLcdDisplay::SetEmotion("happy");
            if (emoji_box_ != nullptr) {
                lv_obj_align(emoji_box_, LV_ALIGN_CENTER, -20, 10);
                lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
            }

            ApplyMenuSelectionVisuals();
            BringPerimeterFront();
        }

        void ShowSpeakingScreen() {
            screen_ = V4Screen::Speaking;
            ready_splash_active_ = false;
            EnterBrowseMode();
            ClearProvisionScales();
            ShowV4Layer();
            HideAllContent();

            if (listening_label_ != nullptr) {
                lv_label_set_text(listening_label_, "Speaking");
                lv_obj_remove_flag(listening_label_, LV_OBJ_FLAG_HIDDEN);
            }
            if (emoji_box_ != nullptr) {
                lv_obj_align(emoji_box_, LV_ALIGN_CENTER, -20, 10);
                lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
            }
            ApplyMenuSelectionVisuals();
            BringPerimeterFront();
        }

        void ShowBenchScreenIndex(int index) {
            bench_index_ = ((index % kBenchScreenCount) + kBenchScreenCount) % kBenchScreenCount;
            ESP_LOGI(TAG, "V4 bench screen %d/6", bench_index_ + 1);
            switch (bench_index_) {
                case 0:
                    ShowProvisionQrScreen();
                    break;
                case 1:
                    ShowWaitingWifiScreen();
                    break;
                case 2:
                    ShowBindingScreen(false);
                    break;
                case 3:
                    ShowReadySplashScreen();
                    break;
                case 4:
                    ShowMainScreen();
                    break;
                case 5:
                default:
                    ShowListeningScreen();
                    break;
            }
        }

        void RestoreV4ScreenForDeviceState() {
#if W13_V4_UI_BENCH_MODE
            if (screen_ == V4Screen::Boot) {
                ShowBootInitializingScreen();
            } else {
                ShowBenchScreenIndex(bench_index_);
            }
            return;
#endif
            auto state = Application::GetInstance().GetDeviceState();
            switch (state) {
                case kDeviceStateStarting:
                    ShowBootInitializingScreen();
                    break;
                case kDeviceStateWifiConfiguring:
                    ShowProvisionQrScreen();
                    break;
                case kDeviceStateConnecting:
                    // Chat channel open also uses Connecting; only Screen 2 during provisioning.
                    if (screen_ == V4Screen::ProvisionQr || screen_ == V4Screen::WaitingWifi ||
                        screen_ == V4Screen::Binding) {
                        ShowWaitingWifiScreen();
                    } else {
                        ShowMainScreen();
                    }
                    break;
                case kDeviceStateActivating:
                    ShowBindingScreen(binding_ui_shown_);
                    break;
                case kDeviceStateListening:
                    ShowListeningScreen();
                    break;
                case kDeviceStateSpeaking:
                case kDeviceStateNotifying:
                    ShowSpeakingScreen();
                    break;
                case kDeviceStateIdle:
                default:
                    if (ready_splash_active_) {
                        ShowReadySplashScreen();
                    } else {
                        ShowMainScreen();
                    }
                    break;
            }
        }

        void AssertV4LowBatteryStyle(const lv_font_t* text_font) {
            if (low_battery_popup_ == nullptr || low_battery_label_ == nullptr) {
                return;
            }
            lv_obj_set_style_bg_color(low_battery_popup_, lv_color_hex(0x000000), 0);
            lv_obj_set_style_bg_opa(low_battery_popup_, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(low_battery_popup_, 0, 0);
            lv_obj_set_style_radius(low_battery_popup_, 0, 0);
            lv_label_set_text(low_battery_label_, "BATTERY LOW");
            lv_obj_set_style_text_color(low_battery_label_, lv_color_hex(0xF01818), 0);
            if (text_font != nullptr) {
                lv_obj_set_style_text_font(low_battery_label_, text_font, 0);
            }
            lv_obj_set_style_text_letter_space(low_battery_label_, 3, 0);
            lv_obj_set_style_text_align(low_battery_label_, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_move_foreground(low_battery_label_);
            if (low_bat_a_ != nullptr)
                lv_obj_move_foreground(low_bat_a_);
            if (low_bat_b_ != nullptr)
                lv_obj_move_foreground(low_bat_b_);
        }

        void RebindV4TextFonts(const lv_font_t* text_font, const lv_font_t* icon_font) {
            if (text_font != nullptr) {
                lv_obj_t* text_objs[] = {title_label_,  subtitle_label_, body_label_,
                                         nexus_logo_,   listening_label_, status_time_,
                                         low_battery_label_};
                for (lv_obj_t* o : text_objs) {
                    if (o != nullptr) {
                        lv_obj_set_style_text_font(o, text_font, 0);
                    }
                }
            }
            if (icon_font != nullptr) {
                lv_obj_t* icon_objs[] = {icon_label_,   status_wifi_, status_battery_,
                                         icon_talk_,    icon_task_,   icon_volume_,
                                         icon_wifi_};
                for (lv_obj_t* o : icon_objs) {
                    if (o != nullptr) {
                        lv_obj_set_style_text_font(o, icon_font, 0);
                    }
                }
            }
        }

        void SetupV4LowBatteryUi(const lv_font_t* text_font) {
            if (low_battery_popup_ == nullptr || low_battery_label_ == nullptr) {
                return;
            }
            lv_obj_set_size(low_battery_popup_, LV_HOR_RES, LV_VER_RES);
            lv_obj_align(low_battery_popup_, LV_ALIGN_CENTER, 0, 0);
            lv_obj_set_style_pad_all(low_battery_popup_, 0, 0);
            lv_obj_clear_flag(low_battery_popup_, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
            CreateStaticSplitPerimeter(low_battery_popup_, &low_bat_a_, &low_bat_b_);
            lv_obj_set_width(low_battery_label_, LV_HOR_RES * 0.8);
            lv_label_set_long_mode(low_battery_label_, LV_LABEL_LONG_WRAP);
            lv_obj_align(low_battery_label_, LV_ALIGN_CENTER, 0, 0);
            AssertV4LowBatteryStyle(text_font);
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
                ESP_LOGW(TAG, "esp_lcd_panel_disp_on_off(%s) failed: %s", on ? "on" : "off",
                         esp_err_to_name(err));
                return;
            }
            panel_display_off_ = !on;
        }

        lv_obj_t* MakeLabel(lv_obj_t* parent, const lv_font_t* font, uint32_t color) {
            lv_obj_t* label = lv_label_create(parent);
            lv_obj_set_style_text_font(label, font, 0);
            lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
            lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
            return label;
        }

    public:
        CustomLcdDisplay(esp_lcd_panel_io_handle_t io_handle, esp_lcd_panel_handle_t panel_handle,
                         int width, int height, int offset_x, int offset_y, bool mirror_x,
                         bool mirror_y, bool swap_xy)
            // Black pre-LVGL clear so early RestoreBrightness never flashes white.
            : SpiLcdDisplay(io_handle, panel_handle, width, height, offset_x, offset_y, mirror_x,
                            mirror_y, swap_xy, /*clear_color=*/0x0000) {}

        virtual void SetupUI() override {
            SpiLcdDisplay::SetupUI();

            {
            DisplayLockGuard lock(this);
            auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
            auto text_font = lvgl_theme->text_font()->font();
            auto icon_font = lvgl_theme->icon_font()->font();

            // Keep stock bar geometry for any fallback paths, but hide chrome.
            lv_obj_set_size(top_bar_, LV_HOR_RES, text_font->line_height);
            lv_obj_set_style_layout(top_bar_, LV_LAYOUT_NONE, 0);
            lv_obj_set_size(status_bar_, LV_HOR_RES, text_font->line_height);
            lv_obj_set_style_layout(status_bar_, LV_LAYOUT_NONE, 0);
            lv_obj_set_y(status_bar_, text_font->line_height);
            lv_obj_add_flag(status_bar_, LV_OBJ_FLAG_IGNORE_LAYOUT);
            lv_obj_set_parent(mute_label_, top_bar_);
            lv_obj_set_parent(battery_label_, top_bar_);
            lv_obj_set_style_pad_bottom(bottom_bar_, 30, 0);
            lv_obj_set_width(chat_message_label_, LV_HOR_RES * 0.75);

            lv_obj_t* screen = lv_screen_active();
            v4_layer_ = lv_obj_create(screen);
            lv_obj_remove_style_all(v4_layer_);
            lv_obj_set_pos(v4_layer_, 0, 0);
            lv_obj_set_size(v4_layer_, LV_HOR_RES, LV_VER_RES);
            lv_obj_set_style_bg_color(v4_layer_, lv_color_hex(0x000000), 0);
            lv_obj_set_style_bg_opa(v4_layer_, LV_OPA_COVER, 0);
            lv_obj_clear_flag(v4_layer_, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_move_foreground(v4_layer_);

            if (container_ != nullptr)
                lv_obj_add_flag(container_, LV_OBJ_FLAG_HIDDEN);
            if (emoji_box_ != nullptr) {
                lv_obj_set_parent(emoji_box_, v4_layer_);
                lv_obj_align(emoji_box_, LV_ALIGN_CENTER, -20, 10);
                lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
            }
            if (top_bar_ != nullptr)
                lv_obj_add_flag(top_bar_, LV_OBJ_FLAG_HIDDEN);
            if (status_bar_ != nullptr)
                lv_obj_add_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
            if (bottom_bar_ != nullptr)
                lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);

            CreateStaticSplitPerimeter(v4_layer_, &perimeter_a_, &perimeter_b_);

            title_label_ = MakeLabel(v4_layer_, text_font, 0xFFFFFF);
            subtitle_label_ = MakeLabel(v4_layer_, text_font, 0xFFFFFF);
            body_label_ = MakeLabel(v4_layer_, text_font, kPerimeterColor);
            icon_label_ = MakeLabel(v4_layer_, icon_font, kPerimeterColor);

            qr_image_ = lv_image_create(v4_layer_);
            lv_image_set_src(qr_image_, &nexus_wifi_qr);
            lv_image_set_scale(qr_image_, LV_SCALE_NONE);
            lv_image_set_antialias(qr_image_, false);
            lv_obj_set_style_opa(qr_image_, LV_OPA_COVER, 0);
            lv_obj_set_style_image_opa(qr_image_, LV_OPA_COVER, 0);
            lv_obj_set_style_image_recolor_opa(qr_image_, LV_OPA_TRANSP, 0);
            lv_obj_set_pos(qr_image_, kProvisionQrX, kProvisionQrY);
            lv_obj_add_flag(qr_image_, LV_OBJ_FLAG_HIDDEN);

            status_time_ = MakeLabel(v4_layer_, &font_noto_sans_basic_20_4, 0xFFFFFF);
            status_wifi_ = MakeLabel(v4_layer_, &font_material_symbols_30_4, 0xFFFFFF);
            status_battery_ = MakeLabel(v4_layer_, &font_material_symbols_30_4, 0xFFFFFF);
            ApplyMainStatusLayout();

            // Screen 4: lime vector checkmark (~60 px), centered near (206, 150).
            ready_check_ = lv_line_create(v4_layer_);
            {
                static lv_point_precise_t check_pts[] = {
                    {14, 34},
                    {28, 50},
                    {58, 12},
                };
                lv_line_set_points(ready_check_, check_pts, 3);
                lv_obj_set_style_line_width(ready_check_, 8, 0);
                lv_obj_set_style_line_color(ready_check_, lv_color_hex(kPerimeterColor), 0);
                lv_obj_set_style_line_rounded(ready_check_, true, 0);
                lv_obj_set_style_line_opa(ready_check_, LV_OPA_COVER, 0);
                // Bounding box ~72×62 → visual center ≈ (206, 150).
                lv_obj_set_pos(ready_check_, kReadyCheckCenterX - 36, kReadyCheckCenterY - 31);
                lv_obj_add_flag(ready_check_, LV_OBJ_FLAG_HIDDEN);
            }

            nexus_logo_ = MakeLabel(v4_layer_, text_font, 0xFFFFFF);
            lv_label_set_text(nexus_logo_, "NEXUS");
            lv_obj_set_style_text_letter_space(nexus_logo_, 4, 0);
            lv_obj_align(nexus_logo_, LV_ALIGN_CENTER, -30, 0);

            listening_label_ = MakeLabel(v4_layer_, text_font, 0xFFB300);
            lv_obj_align(listening_label_, LV_ALIGN_CENTER, -30, -90);

            // Right-side arc controls (MAIN/LISTENING): Talk / Tasks / Volume / Change Wi-Fi.
            // Talk active; others visual placeholders only (no touch / no new behavior).
            icon_talk_ = MakeLabel(v4_layer_, icon_font, kPerimeterColor);
            lv_label_set_text(icon_talk_, MATERIAL_SYMBOLS_CHAT_BUBBLE);

            icon_task_ = MakeLabel(v4_layer_, icon_font, kPerimeterColor);
            lv_label_set_text(icon_task_, MATERIAL_SYMBOLS_CALENDAR_MONTH);

            icon_volume_ = MakeLabel(v4_layer_, icon_font, kPerimeterColor);
            lv_label_set_text(icon_volume_, MATERIAL_SYMBOLS_VOLUME_UP);

            icon_wifi_ = MakeLabel(v4_layer_, icon_font, kPerimeterColor);
            lv_label_set_text(icon_wifi_, MATERIAL_SYMBOLS_WIFI);
            ApplySideIconLayout();

            SetupV4LowBatteryUi(text_font);

            esp_timer_create_args_t ready_args = {};
            ready_args.callback = ReadySplashTimerCallback;
            ready_args.arg = this;
            ready_args.dispatch_method = ESP_TIMER_TASK;
            ready_args.name = "v4_ready";
            ESP_ERROR_CHECK(esp_timer_create(&ready_args, &ready_timer_));

            // V4 boot UI; force a synchronous LVGL flush so GRAM holds this frame.
            // Real flow: SetStatus / device-state mapping advances screens automatically.
            ShowBootInitializingScreen();
            lv_refr_now(display_);
#if W13_V4_UI_BENCH_MODE
            ESP_LOGW(TAG, "V4 UI BENCH MODE ON — boot shown; knob rotate cycles 6 screens; click=Talk");
#else
            ESP_LOGI(TAG,
                     "V4 UI real flow — wheel menu: rotate=select, click=activate (Talk default)");
#endif
            }
        }

        // TEMPORARY bench API: knob rotation cycles the 6 V4 screens for photo validation.
        void BenchCycleScreen(bool clockwise) {
#if W13_V4_UI_BENCH_MODE
            const int64_t now = esp_timer_get_time();
            if (now - last_bench_cycle_us_ < 250000) {
                return;
            }
            last_bench_cycle_us_ = now;
            DisplayLockGuard lock(this);
            // First rotation leaves the boot initializing screen and enters Screen 1–6.
            if (screen_ == V4Screen::Boot) {
                ShowBenchScreenIndex(clockwise ? 0 : (kBenchScreenCount - 1));
                return;
            }
            ShowBenchScreenIndex(bench_index_ + (clockwise ? 1 : -1));
#else
            (void)clockwise;
#endif
        }

        // Wheel menu: rotate moves selection / adjusts volume; click activates.
        // Returns true if the event was consumed (caller should not run stock fallbacks).
        bool HandleWheelRotate(bool clockwise) {
#if W13_V4_UI_BENCH_MODE
            (void)clockwise;
            return false;
#else
            const int64_t now = esp_timer_get_time();
            if (now - last_wheel_rotate_us_ < kWheelRotateDebounceUs) {
                return wheel_mode_ != WheelMode::Inactive;
            }
            last_wheel_rotate_us_ = now;

            DisplayLockGuard lock(this);
            switch (wheel_mode_) {
                case WheelMode::Browse:
                    if (!IsMenuScreen()) {
                        return false;
                    }
                    MoveMenuSelection(clockwise);
                    return true;
                case WheelMode::VolumeAdjust:
                    AdjustSpeakerVolume(clockwise);
                    return true;
                case WheelMode::WifiConfirm:
                    wifi_confirm_yes_ = !wifi_confirm_yes_;
                    ApplyWifiConfirmOptionVisuals();
                    ESP_LOGI(TAG, "Change Wi-Fi confirm highlight: %s",
                             wifi_confirm_yes_ ? "Yes" : "No");
                    return true;
                case WheelMode::TasksPlaceholder:
                    // Rotate ignored; click exits.
                    return true;
                case WheelMode::Inactive:
                default:
                    return false;
            }
#endif
        }

        bool HandleWheelClick() {
#if W13_V4_UI_BENCH_MODE
            return false;
#else
            DisplayLockGuard lock(this);
            switch (wheel_mode_) {
                case WheelMode::Browse:
                    if (!IsMenuScreen()) {
                        return false;
                    }
                    ActivateSelectedMenuItem();
                    return true;
                case WheelMode::VolumeAdjust:
                    ESP_LOGI(TAG, "Wheel volume adjust exit");
                    ReturnToMainWithSelection();
                    return true;
                case WheelMode::TasksPlaceholder:
                    ESP_LOGI(TAG, "Tasks placeholder exit");
                    ReturnToMainWithSelection();
                    return true;
                case WheelMode::WifiConfirm:
                    if (wifi_confirm_yes_) {
                        // Prototype only — do NOT erase credentials / enter config.
                        ESP_LOGW(TAG,
                                 "Change Wi-Fi confirmation reached: YES "
                                 "(credentials NOT erased — prototype only)");
                    } else {
                        ESP_LOGI(TAG, "Change Wi-Fi confirmation: No — cancelled");
                    }
                    ReturnToMainWithSelection();
                    return true;
                case WheelMode::Inactive:
                default:
                    return false;
            }
#endif
        }

        static constexpr bool IsBenchModeEnabled() {
#if W13_V4_UI_BENCH_MODE
            return true;
#else
            return false;
#endif
        }

        // Rebind fonts after Assets::Apply / SetTextFont theme refresh (preserve lifetime fix).
        virtual void SetTheme(Theme* theme) override {
            LcdDisplay::SetTheme(theme);
            DisplayLockGuard lock(this);
            auto lvgl_theme = static_cast<LvglTheme*>(theme);
            if (lvgl_theme == nullptr || lvgl_theme->text_font() == nullptr) {
                return;
            }
            const lv_font_t* text_font = lvgl_theme->text_font()->font();
            const lv_font_t* icon_font =
                lvgl_theme->icon_font() != nullptr ? lvgl_theme->icon_font()->font() : nullptr;
            RebindV4TextFonts(text_font, icon_font);
            AssertV4LowBatteryStyle(text_font);
            if (screen_ == V4Screen::Main) {
                ApplyMainStatusLayout();
            }
        }

        // Screen-off + listening: blank panel; on wake restore correct V4 screen.
        virtual void SetPowerSaveMode(bool on) override {
            DisplayLockGuard lock(this);
            if (on) {
                SetPanelDisplayOn(false);
                ShowV4Layer();
            } else {
                SetPanelDisplayOn(true);
                RestoreV4ScreenForDeviceState();
            }
        }

        virtual void UpdateStatusBar(bool update_all = false) override {
            // Drive low-battery popup via parent, then refresh V4 status row.
            LvglDisplay::UpdateStatusBar(update_all);
            DisplayLockGuard lock(this);
            if (screen_ == V4Screen::Main || screen_ == V4Screen::Listening ||
                screen_ == V4Screen::Speaking) {
                RefreshStatusRow();
            }
            if (low_battery_popup_ != nullptr &&
                !lv_obj_has_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN)) {
                lv_obj_move_foreground(low_battery_popup_);
                AssertV4LowBatteryStyle(nullptr);
            }
        }

        virtual void SetStatus(const char* status) override {
            SpiLcdDisplay::SetStatus(status);
            if (status == nullptr) {
                return;
            }

            DisplayLockGuard lock(this);

            // Parent clock path writes HH:MM into SetStatus while idle — only update time tile.
            if (IsClockText(status)) {
                if (status_time_ != nullptr && screen_ == V4Screen::Main) {
                    lv_label_set_text(status_time_, status);
                }
                return;
            }

#if W13_V4_UI_BENCH_MODE
            // Bench mode owns the visible screen; ignore automatic state navigation.
            return;
#endif

            if (strcmp(status, Lang::Strings::WIFI_CONFIG_MODE) == 0 ||
                strcmp(status, Lang::Strings::ENTERING_WIFI_CONFIG_MODE) == 0) {
                ShowProvisionQrScreen();
                return;
            }

            // CONNECTING is also used when opening a chat channel — only treat it as
            // Wi-Fi setup wait when we are already in the provisioning flow.
            if (strcmp(status, Lang::Strings::CONNECTING) == 0) {
                if (screen_ == V4Screen::ProvisionQr || screen_ == V4Screen::WaitingWifi) {
                    ShowWaitingWifiScreen();
                }
                return;
            }

            if (strcmp(status, Lang::Strings::ACTIVATION) == 0) {
                ShowBindingScreen(true);
                return;
            }

            // Post-Wi-Fi OTA check during first-time setup → binding visuals.
            // Do not map routine reboot version checks or LOADING_PROTOCOL here.
            if (strcmp(status, Lang::Strings::CHECKING_NEW_VERSION) == 0) {
                if (screen_ == V4Screen::ProvisionQr || screen_ == V4Screen::WaitingWifi ||
                    screen_ == V4Screen::Binding) {
                    ShowBindingScreen(false);
                }
                return;
            }

            if (strcmp(status, Lang::Strings::LISTENING) == 0) {
                ShowListeningScreen();
                return;
            }

            if (strcmp(status, Lang::Strings::SPEAKING) == 0) {
                ShowSpeakingScreen();
                return;
            }

            if (strcmp(status, Lang::Strings::STANDBY) == 0) {
                if (binding_ui_shown_) {
                    // Real activation/binding finished → ready splash, then main.
                    ShowReadySplashScreen();
                } else if (!ready_splash_active_) {
                    ShowMainScreen();
                }
                return;
            }
        }

        virtual void SetEmotion(const char* emotion) override {
            DisplayLockGuard lock(this);

            if (emotion != nullptr && strcmp(emotion, "sleepy") == 0) {
                // Screen-off path may request sleepy; keep current V4 screen under blank panel.
                return;
            }

            if (screen_ == V4Screen::Listening) {
                // V4 listening face is always happy; ignore app's neutral.
                SpiLcdDisplay::SetEmotion("happy");
                if (emoji_box_ != nullptr) {
                    lv_obj_align(emoji_box_, LV_ALIGN_CENTER, -20, 10);
                    lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
                }
                BringPerimeterFront();
                return;
            }

            SpiLcdDisplay::SetEmotion(emotion);

            if (screen_ == V4Screen::Speaking) {
                if (emoji_box_ != nullptr) {
                    lv_obj_align(emoji_box_, LV_ALIGN_CENTER, -20, 10);
                    lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
                }
                BringPerimeterFront();
                return;
            }

            if (screen_ == V4Screen::Main || screen_ == V4Screen::ReadySplash ||
                screen_ == V4Screen::ProvisionQr || screen_ == V4Screen::WaitingWifi ||
                screen_ == V4Screen::Binding) {
                if (emoji_box_ != nullptr) {
                    lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
                }
            }
        }

        virtual void SetChatMessage(const char* role, const char* content) override {
            // Hotspot provisioning message → V4 QR screen (SoftAP path).
            if (role != nullptr && content != nullptr && strcmp(role, "system") == 0 &&
                strstr(content, "Hotspot: ") != nullptr) {
#if W13_V4_UI_BENCH_MODE
                return;
#else
                DisplayLockGuard lock(this);
                ShowProvisionQrScreen();
                return;
#endif
            }

            // Suppress stock system chat while V4 UI owns the screen.
            if (role != nullptr && strcmp(role, "system") == 0) {
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
            // SetPowerSaveMode blanks the panel; wake restores the current V4 screen.
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
        power_save_timer_->WakeUp();

#if W13_V4_UI_BENCH_MODE
        // TEMPORARY: rotation cycles V4 bench screens (volume via knob paused in bench mode).
        // Knob CLICK still runs ToggleChatState() for Talk.
        // Display is always CustomLcdDisplay on this board (no RTTI required).
        static_cast<CustomLcdDisplay*>(GetDisplay())->BenchCycleScreen(clockwise);
        return;
#endif

        // Wheel menu owns rotation on Main/Listening/Speaking and in adjust/confirm modes.
        auto* v4 = static_cast<CustomLcdDisplay*>(GetDisplay());
        if (v4->HandleWheelRotate(clockwise)) {
            return;
        }
        // Non-menu screens: wake only (no global volume steal).
    }

    void OnKnobClick() {
        power_save_timer_->WakeUp();

        auto& app = Application::GetInstance();
        if (app.GetDeviceState() == kDeviceStateStarting) {
            EnterWifiConfigMode();
            return;
        }

#if W13_V4_UI_BENCH_MODE
        app.ToggleChatState();
        return;
#endif

        auto* v4 = static_cast<CustomLcdDisplay*>(GetDisplay());
        if (v4->HandleWheelClick()) {
            return;
        }
        // Fallback outside wheel modes (e.g. boot/provision): preserve Talk toggle.
        app.ToggleChatState();
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
            self->OnKnobClick();
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
        // Normal SenseCap startup: PWM backlight ramps after display init (GRAM is black).
        GetBacklight()->RestoreBrightness();
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
