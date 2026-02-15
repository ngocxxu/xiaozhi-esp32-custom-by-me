#include "oled_display.h"
#include "application.h"
#include "assets/lang_config.h"
#include "device_state.h"
#include "lvgl_theme.h"
#include "lvgl_font.h"

#include <chrono>
#include <cstring>
#include <string>
#include <algorithm>

#include <esp_log.h>
#include <esp_err.h>
#include <esp_lvgl_port.h>
#include <font_awesome.h>

#define TAG "OledDisplay"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
LV_FONT_DECLARE(BUILTIN_ICON_FONT);
LV_FONT_DECLARE(font_awesome_30_1);

OledDisplay::OledDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
    int width, int height, bool mirror_x, bool mirror_y)
    : panel_io_(panel_io), panel_(panel) {
    width_ = width;
    height_ = height;

    auto text_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_TEXT_FONT);
    auto icon_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_ICON_FONT);
    auto large_icon_font = std::make_shared<LvglBuiltInFont>(&font_awesome_30_1);
    
    auto dark_theme = new LvglTheme("dark");
    dark_theme->set_text_font(text_font);
    dark_theme->set_icon_font(icon_font);
    dark_theme->set_large_icon_font(large_icon_font);

    auto& theme_manager = LvglThemeManager::GetInstance();
    theme_manager.RegisterTheme("dark", dark_theme);
    current_theme_ = dark_theme;

    ESP_LOGI(TAG, "Initialize LVGL");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
    port_cfg.task_stack = 6144;
#if CONFIG_SOC_CPU_CORES_NUM > 1
    port_cfg.task_affinity = 1;
#endif
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding OLED display");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * height_),
        .double_buffer = false,
        .trans_size = 0,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = true,
        .rotation = {
            .swap_xy = false,
            .mirror_x = mirror_x,
            .mirror_y = mirror_y,
        },
        .flags = {
            .buff_dma = 1,
            .buff_spiram = 0,
            .sw_rotate = 0,
            .full_refresh = 0,
            .direct_mode = 0,
        },
    };

    display_ = lvgl_port_add_disp(&display_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    // Note: SetupUI() should be called by Application::Initialize(), not in constructor
    // to ensure lvgl objects are created after the display is fully initialized.
}

void OledDisplay::SetupUI() {
    // Prevent duplicate calls - if already called, return early
    if (setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() called multiple times, skipping duplicate call");
        return;
    }
    
    Display::SetupUI();  // Mark SetupUI as called
    if (height_ == 64) {
        SetupUI_128x64();
    } else {
        SetupUI_128x32();
    }
}

OledDisplay::~OledDisplay() {
    bool is_128x64_layout = (container_ == nullptr && face_container_ != nullptr);

    if (is_128x64_layout) {
        if (idle_blink_timer_ != nullptr) {
            lv_timer_delete(idle_blink_timer_);
            idle_blink_timer_ = nullptr;
        }
        if (idle_blink_open_timer_ != nullptr) {
            lv_timer_delete(idle_blink_open_timer_);
            idle_blink_open_timer_ = nullptr;
        }
        if (face_container_ != nullptr) {
            lv_obj_del(face_container_);
            face_container_ = nullptr;
            left_eye_ = nullptr;
            right_eye_ = nullptr;
            mouth_ = nullptr;
        }
        if (low_battery_popup_ != nullptr) {
            lv_obj_del(low_battery_popup_);
            low_battery_popup_ = nullptr;
            low_battery_label_ = nullptr;
        }
        if (content_right_ != nullptr) {
            lv_obj_del(content_right_);
            content_right_ = nullptr;
            chat_message_label_ = nullptr;
        }
        if (status_bar_ != nullptr) {
            status_label_ = nullptr;
            notification_label_ = nullptr;
            status_face_container_ = nullptr;
            status_left_eye_ = nullptr;
            status_right_eye_ = nullptr;
            status_mouth_ = nullptr;
            lv_obj_del(status_bar_);
            status_bar_ = nullptr;
        }
        if (top_bar_ != nullptr) {
            network_label_ = nullptr;
            mute_label_ = nullptr;
            battery_label_ = nullptr;
            lv_obj_del(top_bar_);
            top_bar_ = nullptr;
        }
    } else {
        if (content_ != nullptr) {
            lv_obj_del(content_);
            content_ = nullptr;
        }
        if (status_bar_ != nullptr) {
            status_label_ = nullptr;
            notification_label_ = nullptr;
            lv_obj_del(status_bar_);
            status_bar_ = nullptr;
        }
        if (top_bar_ != nullptr) {
            network_label_ = nullptr;
            mute_label_ = nullptr;
            battery_label_ = nullptr;
            lv_obj_del(top_bar_);
            top_bar_ = nullptr;
        }
        if (side_bar_ != nullptr) {
            status_label_ = nullptr;
            notification_label_ = nullptr;
            network_label_ = nullptr;
            mute_label_ = nullptr;
            battery_label_ = nullptr;
            lv_obj_del(side_bar_);
            side_bar_ = nullptr;
        }
        if (container_ != nullptr) {
            lv_obj_del(container_);
            container_ = nullptr;
        }
    }

    if (panel_ != nullptr) {
        esp_lcd_panel_del(panel_);
    }
    if (panel_io_ != nullptr) {
        esp_lcd_panel_io_del(panel_io_);
    }
    lvgl_port_deinit();
}

bool OledDisplay::Lock(int timeout_ms) {
    return lvgl_port_lock(timeout_ms);
}

void OledDisplay::Unlock() {
    lvgl_port_unlock();
}

void OledDisplay::SetChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);
    if (chat_message_label_ == nullptr) {
        return;
    }

    // Replace all newlines with spaces
    std::string content_str = content;
    std::replace(content_str.begin(), content_str.end(), '\n', ' ');

    if (content_right_ == nullptr) {
        lv_label_set_text(chat_message_label_, content_str.c_str());
    } else {
        if (content == nullptr || content[0] == '\0') {
            lv_obj_add_flag(content_right_, LV_OBJ_FLAG_HIDDEN);
            if (face_container_ != nullptr) {
                lv_obj_remove_flag(face_container_, LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            lv_label_set_text(chat_message_label_, content_str.c_str());
            lv_obj_remove_flag(content_right_, LV_OBJ_FLAG_HIDDEN);
            if (face_container_ != nullptr) {
                lv_obj_add_flag(face_container_, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

void OledDisplay::SetStatus(const char* status) {
    if (status_face_container_ == nullptr) {
        LvglDisplay::SetStatus(status);
        return;
    }
    DisplayLockGuard lock(this);
    if (status_label_ == nullptr) return;
    if (strcmp(status, Lang::Strings::LISTENING) == 0 || strcmp(status, Lang::Strings::SPEAKING) == 0) {
        lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(status_face_container_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(status_face_container_, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(status_label_, status);
        lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
    }
    last_status_update_time_ = std::chrono::system_clock::now();
}

void OledDisplay::IdleBlinkOpenCb(lv_timer_t* timer) {
    auto* self = static_cast<OledDisplay*>(lv_timer_get_user_data(timer));
    self->idle_blink_open_timer_ = nullptr;
    if (!self->Lock(0)) return;
    lv_obj_set_size(self->left_eye_, 10, 12);
    lv_obj_set_size(self->right_eye_, 10, 12);
    if (self->face_container_ != nullptr) {
        lv_obj_invalidate(self->face_container_);
    }
    self->Unlock();
}

void OledDisplay::IdleBlinkTimerCb(lv_timer_t* timer) {
    auto* self = static_cast<OledDisplay*>(lv_timer_get_user_data(timer));
    if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle) return;
    if (self->left_eye_ == nullptr) return;
    if (!self->Lock(0)) return;
    lv_obj_set_size(self->left_eye_, 10, 2);
    lv_obj_set_size(self->right_eye_, 10, 2);
    self->Unlock();
    self->idle_blink_open_timer_ = lv_timer_create(IdleBlinkOpenCb, 80, self);
    lv_timer_set_repeat_count(self->idle_blink_open_timer_, 1);
}

void OledDisplay::SetupUI_128x64() {
    DisplayLockGuard lock(this);

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font(); 

    auto screen = lv_screen_active();
    lv_obj_clean(screen); // Xóa sạch màn hình cũ
    lv_obj_set_style_bg_color(screen, lv_color_white(), 0);

    // --- TẦNG 1: TOP BAR (WIFI, PIN) - Cao 16px ---
    top_bar_ = lv_obj_create(screen);
    lv_obj_set_size(top_bar_, 128, 16);
    lv_obj_align(top_bar_, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(top_bar_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(top_bar_, 0, 0);
    lv_obj_set_style_pad_all(top_bar_, 0, 0);
    lv_obj_set_scrollbar_mode(top_bar_, LV_SCROLLBAR_MODE_OFF);

    // Wifi Icon (Góc trái)
    network_label_ = lv_label_create(top_bar_);
    lv_label_set_text(network_label_, ""); 
    lv_obj_set_style_text_font(network_label_, icon_font, 0);
    lv_obj_align(network_label_, LV_ALIGN_LEFT_MID, 2, 0);

    // Các icon bên phải (Mute, Pin)
    lv_obj_t* right_icons = lv_obj_create(top_bar_);
    lv_obj_set_size(right_icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right_icons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_icons, 0, 0);
    lv_obj_set_style_pad_all(right_icons, 0, 0);
    lv_obj_align(right_icons, LV_ALIGN_RIGHT_MID, -2, 0);
    lv_obj_set_flex_flow(right_icons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_icons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    mute_label_ = lv_label_create(right_icons);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, icon_font, 0);

    battery_label_ = lv_label_create(right_icons);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, icon_font, 0);


    // --- TẦNG 2: STATUS BAR (TEXT THÔNG BÁO) - Cao 14px ---
    // Nằm ngay dưới Top Bar (Y = 16)
    status_bar_ = lv_obj_create(screen);
    lv_obj_set_size(status_bar_, 128, 14); 
    lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, 16); 
    lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_scrollbar_mode(status_bar_, LV_SCROLLBAR_MODE_OFF);

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(notification_label_, 128);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(notification_label_, lv_color_black(), 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(status_label_, 128);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label_, lv_color_black(), 0);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
    lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 0);

    status_face_container_ = lv_obj_create(status_bar_);
    lv_obj_set_size(status_face_container_, 40, 14);
    lv_obj_align(status_face_container_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(status_face_container_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(status_face_container_, 0, 0);
    lv_obj_set_style_pad_all(status_face_container_, 0, 0);
    lv_obj_add_flag(status_face_container_, LV_OBJ_FLAG_HIDDEN);

    status_left_eye_ = lv_obj_create(status_face_container_);
    lv_obj_set_size(status_left_eye_, 4, 6);
    lv_obj_set_style_radius(status_left_eye_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(status_left_eye_, lv_color_black(), 0);
    lv_obj_set_style_border_width(status_left_eye_, 0, 0);
    lv_obj_align(status_left_eye_, LV_ALIGN_TOP_LEFT, 4, 2);

    status_right_eye_ = lv_obj_create(status_face_container_);
    lv_obj_set_size(status_right_eye_, 4, 6);
    lv_obj_set_style_radius(status_right_eye_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(status_right_eye_, lv_color_black(), 0);
    lv_obj_set_style_border_width(status_right_eye_, 0, 0);
    lv_obj_align(status_right_eye_, LV_ALIGN_TOP_RIGHT, -4, 2);

    status_mouth_ = lv_arc_create(status_face_container_);
    lv_obj_set_size(status_mouth_, 8, 8);
    lv_arc_set_rotation(status_mouth_, 0);
    lv_arc_set_bg_angles(status_mouth_, 0, 360);
    lv_obj_set_style_arc_width(status_mouth_, 0, LV_PART_MAIN);
    lv_arc_set_angles(status_mouth_, 20, 160);
    lv_obj_remove_style(status_mouth_, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(status_mouth_, 1, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(status_mouth_, lv_color_black(), LV_PART_INDICATOR);
    lv_obj_align(status_mouth_, LV_ALIGN_BOTTOM_MID, 0, -1);

    // --- TẦNG 3: MẶT ROBOT + VÙNG CHAT ---
    // Khu vực này cao khoảng 34px (64 - 16 - 14)
    face_container_ = lv_obj_create(screen);
    lv_obj_set_size(face_container_, 64, 32); 
    lv_obj_align(face_container_, LV_ALIGN_BOTTOM_MID, 0, -2); // Căn sát đáy
    lv_obj_set_style_bg_opa(face_container_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(face_container_, 0, 0);
    lv_obj_set_style_pad_all(face_container_, 0, 0);

    // Mắt Trái (Size nhỏ: 10x12)
    left_eye_ = lv_obj_create(face_container_);
    lv_obj_set_size(left_eye_, 10, 12); 
    lv_obj_set_style_radius(left_eye_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(left_eye_, lv_color_black(), 0);
    lv_obj_set_style_border_width(left_eye_, 0, 0);
    lv_obj_align(left_eye_, LV_ALIGN_TOP_LEFT, 12, 5); // Cách lề trái 12px

    // Mắt Phải (Size nhỏ: 10x12)
    right_eye_ = lv_obj_create(face_container_);
    lv_obj_set_size(right_eye_, 10, 12);
    lv_obj_set_style_radius(right_eye_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(right_eye_, lv_color_black(), 0);
    lv_obj_set_style_border_width(right_eye_, 0, 0);
    lv_obj_align(right_eye_, LV_ALIGN_TOP_RIGHT, -12, 5); // Cách lề phải 12px

    mouth_ = lv_arc_create(face_container_);
    lv_obj_set_size(mouth_, 16, 16);
    lv_arc_set_rotation(mouth_, 0);
    lv_arc_set_bg_angles(mouth_, 0, 360);
    lv_obj_set_style_arc_width(mouth_, 0, LV_PART_MAIN);
    lv_arc_set_angles(mouth_, 20, 160);
    lv_obj_remove_style(mouth_, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(mouth_, 2, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(mouth_, lv_color_black(), LV_PART_INDICATOR);
    lv_obj_align(mouth_, LV_ALIGN_BOTTOM_MID, 0, -3);

    content_right_ = lv_obj_create(screen);
    lv_obj_set_size(content_right_, 128, 32);
    lv_obj_align(content_right_, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_style_bg_opa(content_right_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content_right_, 0, 0);
    lv_obj_set_style_pad_all(content_right_, 2, 0);
    lv_obj_add_flag(content_right_, LV_OBJ_FLAG_HIDDEN);

    chat_message_label_ = lv_label_create(content_right_);
    lv_obj_set_width(chat_message_label_, 124);
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_color(chat_message_label_, lv_color_black(), 0);
    lv_obj_align(chat_message_label_, LV_ALIGN_TOP_LEFT, 0, 0);

    static lv_anim_t a_64;
    lv_anim_init(&a_64);
    lv_anim_set_delay(&a_64, 1000);
    lv_anim_set_repeat_count(&a_64, LV_ANIM_REPEAT_INFINITE);
    lv_obj_set_style_anim(chat_message_label_, &a_64, LV_PART_MAIN);
    lv_obj_set_style_anim_duration(chat_message_label_, lv_anim_speed_clamped(60, 300, 60000), LV_PART_MAIN);

    // Popup pin yếu (giữ nguyên logic cũ)
    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(low_battery_popup_, LV_HOR_RES * 0.9, text_font->line_height * 2);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(low_battery_popup_, lv_color_white(), 0);
    lv_obj_set_style_radius(low_battery_popup_, 10, 0);
    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_label_set_text(low_battery_label_, Lang::Strings::BATTERY_NEED_CHARGE);
    lv_obj_set_style_text_color(low_battery_label_, lv_color_black(), 0);
    lv_obj_center(low_battery_label_);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);

    idle_blink_timer_ = lv_timer_create(IdleBlinkTimerCb, 4000, this);
}

void OledDisplay::SetupUI_128x32() {
    DisplayLockGuard lock(this);

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);

    /* Container */
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_flex_flow(container_, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_pad_column(container_, 0, 0);

    /* Emotion label on the left side */
    content_ = lv_obj_create(container_);
    lv_obj_set_size(content_, 32, 32);
    lv_obj_set_style_pad_all(content_, 0, 0);
    lv_obj_set_style_border_width(content_, 0, 0);
    lv_obj_set_style_radius(content_, 0, 0);

    emotion_label_ = lv_label_create(content_);
    lv_obj_set_style_text_font(emotion_label_, large_icon_font, 0);
    lv_label_set_text(emotion_label_, FONT_AWESOME_MICROCHIP_AI);
    lv_obj_center(emotion_label_);

    /* Right side */
    side_bar_ = lv_obj_create(container_);
    lv_obj_set_size(side_bar_, width_ - 32, 32);
    lv_obj_set_flex_flow(side_bar_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(side_bar_, 0, 0);
    lv_obj_set_style_border_width(side_bar_, 0, 0);
    lv_obj_set_style_radius(side_bar_, 0, 0);
    lv_obj_set_style_pad_row(side_bar_, 0, 0);

    /* Status bar */
    status_bar_ = lv_obj_create(side_bar_);
    lv_obj_set_size(status_bar_, width_ - 32, 16);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_flex_flow(status_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_column(status_bar_, 0, 0);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_flex_grow(status_label_, 1);
    lv_obj_set_style_pad_left(status_label_, 2, 0);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_flex_grow(notification_label_, 1);
    lv_obj_set_style_pad_left(notification_label_, 2, 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    mute_label_ = lv_label_create(status_bar_);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, icon_font, 0);

    network_label_ = lv_label_create(status_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, icon_font, 0);

    battery_label_ = lv_label_create(status_bar_);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, icon_font, 0);

    chat_message_label_ = lv_label_create(side_bar_);
    lv_obj_set_size(chat_message_label_, width_ - 32, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_left(chat_message_label_, 2, 0);
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(chat_message_label_, "");

    // Start scrolling subtitle after a delay
    static lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_delay(&a, 1000);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_obj_set_style_anim(chat_message_label_, &a, LV_PART_MAIN);
    lv_obj_set_style_anim_duration(chat_message_label_, lv_anim_speed_clamped(60, 300, 60000), LV_PART_MAIN);
}

void OledDisplay::SetEmotion(const char* emotion) {
    DisplayLockGuard lock(this);
    if (left_eye_ == nullptr || right_eye_ == nullptr) return;

    int eye_width = 10;
    int eye_height = 12;
    bool show_mouth = true;
    int mouth_start = 20;
    int mouth_end = 160;

    std::string emo = emotion;
    if (emo.find("happy") != std::string::npos || emo.find("joy") != std::string::npos) {
        eye_height = 12;
        mouth_start = 0;
        mouth_end = 180;
    } else if (emo.find("sad") != std::string::npos) {
        eye_height = 5;
        show_mouth = true;
        mouth_start = 180;
        mouth_end = 360;
    } else if (emo.find("sleep") != std::string::npos) {
        eye_height = 2;
        show_mouth = false;
    } else if (emo.find("thinking") != std::string::npos) {
        lv_obj_set_size(left_eye_, 10, 12);
        lv_obj_set_size(right_eye_, 10, 4);
        if (mouth_) {
            lv_obj_remove_flag(mouth_, LV_OBJ_FLAG_HIDDEN);
            lv_arc_set_angles(mouth_, 20, 160);
        }
        if (face_container_ != nullptr) {
            lv_obj_invalidate(face_container_);
        }
        if (status_left_eye_ != nullptr) {
            lv_obj_set_size(status_left_eye_, 4, 6);
            lv_obj_set_size(status_right_eye_, 4, 2);
            if (status_mouth_) {
                lv_obj_remove_flag(status_mouth_, LV_OBJ_FLAG_HIDDEN);
                lv_arc_set_angles(status_mouth_, 20, 160);
            }
            lv_obj_invalidate(status_face_container_);
        }
        return;
    } else if (emo.find("speaking") != std::string::npos) {
        eye_height = 12;
        mouth_start = 10;
        mouth_end = 170;
    } else if (emo.find("listening") != std::string::npos) {
        eye_height = 12;
        mouth_start = 40;
        mouth_end = 140;
    } else {
        eye_height = 12;
        mouth_start = 20;
        mouth_end = 160;
    }

    lv_obj_set_size(left_eye_, eye_width, eye_height);
    lv_obj_set_size(right_eye_, eye_width, eye_height);

    if (mouth_) {
        if (show_mouth) {
            lv_obj_remove_flag(mouth_, LV_OBJ_FLAG_HIDDEN);
            lv_arc_set_angles(mouth_, mouth_start, mouth_end);
        } else {
            lv_obj_add_flag(mouth_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (face_container_ != nullptr) {
        lv_obj_invalidate(face_container_);
    }

    if (status_left_eye_ != nullptr) {
        const int sw = 4;
        int sh = 6;
        if (emo.find("sad") != std::string::npos) sh = 2;
        else if (emo.find("sleep") != std::string::npos) sh = 2;
        lv_obj_set_size(status_left_eye_, sw, sh);
        lv_obj_set_size(status_right_eye_, sw, sh);
        if (status_mouth_) {
            if (show_mouth) {
                lv_obj_remove_flag(status_mouth_, LV_OBJ_FLAG_HIDDEN);
                lv_arc_set_angles(status_mouth_, mouth_start, mouth_end);
            } else {
                lv_obj_add_flag(status_mouth_, LV_OBJ_FLAG_HIDDEN);
            }
        }
        lv_obj_invalidate(status_face_container_);
    }
}

void OledDisplay::SetTheme(Theme* theme) {
    DisplayLockGuard lock(this);

    auto lvgl_theme = static_cast<LvglTheme*>(theme);
    auto text_font = lvgl_theme->text_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
}
