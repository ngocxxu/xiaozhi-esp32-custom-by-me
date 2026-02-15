#ifndef OLED_DISPLAY_H
#define OLED_DISPLAY_H

#include "lvgl_display.h"

#include <esp_lcd_panel_io.h>
#include <string>
#include <esp_lcd_panel_ops.h>


class OledDisplay : public LvglDisplay {
private:
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;

    lv_obj_t* top_bar_ = nullptr;
    lv_obj_t* time_label_ = nullptr;
    lv_obj_t* status_bar_ = nullptr;
    lv_obj_t* content_ = nullptr;
    lv_obj_t* content_left_ = nullptr;
    lv_obj_t* content_right_ = nullptr;
    lv_obj_t* container_ = nullptr;
    lv_obj_t* side_bar_ = nullptr;
    lv_obj_t *emotion_label_ = nullptr;

    // --- FIX FROM HERE (Draw Robot face) ---
    lv_obj_t* face_container_ = nullptr;
    lv_obj_t* left_eye_ = nullptr;
    lv_obj_t* right_eye_ = nullptr;
    lv_obj_t* left_eye_line_ = nullptr;
    lv_obj_t* right_eye_line_ = nullptr;
    lv_obj_t* left_eye_arc_ = nullptr;
    lv_obj_t* right_eye_arc_ = nullptr;
    lv_obj_t* mouth_ = nullptr;
    lv_obj_t* mouth_line_ = nullptr;
    lv_obj_t* mode_hint_label_ = nullptr;
    // --- END OF FIX ---

    lv_timer_t* idle_blink_timer_ = nullptr;
    lv_timer_t* idle_blink_open_timer_ = nullptr;
    lv_timer_t* idle_sleep_timer_ = nullptr;

    std::string current_emotion_;

    lv_obj_t* chat_message_label_ = nullptr;

    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;

    static void IdleBlinkTimerCb(lv_timer_t* timer);
    static void IdleBlinkOpenCb(lv_timer_t* timer);
    static void IdleSleepTimerCb(lv_timer_t* timer);

    void SetupUI_128x64();
    void SetupUI_128x32();

public:
    OledDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width, int height, bool mirror_x, bool mirror_y);
    ~OledDisplay();

    virtual void SetupUI() override;
    virtual void SetStatus(const char* status) override;
    virtual void ShowNotification(const char* notification, int duration_ms = 3000) override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void SetEmotion(const char* emotion) override;
    virtual void SetTheme(Theme* theme) override;
};

#endif // OLED_DISPLAY_H
