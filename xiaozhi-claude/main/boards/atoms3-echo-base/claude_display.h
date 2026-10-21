#ifndef CLAUDE_DISPLAY_H
#define CLAUDE_DISPLAY_H

// Interface "Claude" para a tela 128x128 do AtomS3: a faisca laranja animada conforme o estado
// do aparelho, status em portugues e o texto da conversa rolando embaixo.
// Substitui os widgets do LcdDisplay do xiaozhi, mantendo o mesmo driver SPI e a mesma API.

#include "display/lcd_display.h"
#include "device_state.h"

#include <atomic>
#include <string>

class ClaudeDisplay : public SpiLcdDisplay {
public:
    ClaudeDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy);

    void SetEmotion(const char* emotion) override;
    void SetIcon(const char* icon) override {}
    void SetPreviewImage(const lv_img_dsc_t* image) override {}
    void SetChatMessage(const char* role, const char* content) override;
    void SetTheme(const std::string& theme_name) override { Display::SetTheme(theme_name); }

private:
    enum class Mode { Boot, Setup, Idle, Connecting, Listening, Thinking, Speaking, Error, Sleep };

    void BuildUi();
    void ApplyLayout(bool conversation);
    void Tick();
    void DrawSpark(lv_obj_t* canvas, int size, float radius, float rotation, float wobble, lv_color_t color);
    void ScrollChat(uint32_t now);
    Mode CurrentMode() const;

    lv_obj_t* hero_canvas_ = nullptr;   // faisca grande (tela de espera)
    lv_obj_t* small_canvas_ = nullptr;  // faisca pequena (durante a conversa)
    lv_obj_t* chat_box_ = nullptr;
    lv_obj_t* hint_label_ = nullptr;
    lv_timer_t* timer_ = nullptr;

    std::atomic<int> device_state_{kDeviceStateStarting};
    std::atomic<bool> thinking_{false};
    std::atomic<bool> error_{false};
    std::atomic<bool> sleepy_{false};

    bool conversation_ = false;
    uint32_t state_since_ = 0;
    int last_state_ = -1;
    // rolagem vertical do texto
    int scroll_y_ = 0;
    uint32_t scroll_phase_since_ = 0;
    int scroll_phase_ = 0;  // 0 pausa no topo, 1 rolando, 2 pausa no fim
};

#endif  // CLAUDE_DISPLAY_H
