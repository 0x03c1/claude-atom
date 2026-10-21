#include "claude_display.h"

#include "device_state_event.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <font_awesome.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#define TAG "ClaudeDisplay"

LV_FONT_DECLARE(font_noto_basic_16_4);  // Latin-1 completo: acentos do portugues
LV_FONT_DECLARE(font_awesome_14_1);

namespace {

const lv_color_t COL_BG = lv_color_hex(0x141413);
const lv_color_t COL_CLAUDE = lv_color_hex(0xD97757);
const lv_color_t COL_TEXT = lv_color_hex(0xF0EEE6);
const lv_color_t COL_DIM = lv_color_hex(0x9C9A92);
const lv_color_t COL_FAINT = lv_color_hex(0x5A5955);
const lv_color_t COL_INFO = lv_color_hex(0x6A9BCC);
const lv_color_t COL_ERR = lv_color_hex(0xE5534B);

constexpr int HERO_SIZE = 60;
constexpr int SMALL_SIZE = 28;
constexpr int CHAT_X = 4, CHAT_Y = 48, CHAT_W = 120, CHAT_H = 78;
constexpr uint32_t FRAME_MS = 50;
constexpr uint32_t IDLE_CLEAR_MS = 10000;
constexpr float kPi = 3.14159265f;

uint32_t NowMs() { return esp_timer_get_time() / 1000; }

lv_obj_t* MakeLabel(lv_obj_t* parent, const lv_font_t* font, lv_color_t color) {
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, "");
    return l;
}

}  // namespace

ClaudeDisplay::ClaudeDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                             int width, int height, int offset_x, int offset_y,
                             bool mirror_x, bool mirror_y, bool swap_xy)
    : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y, swap_xy,
                    {.text_font = &font_noto_basic_16_4, .icon_font = &font_awesome_14_1, .emoji_font = font_emoji_32_init()}) {
    BuildUi();
    DeviceStateEventManager::GetInstance().RegisterStateChangeCallback([this](DeviceState, DeviceState current) {
        device_state_ = current;
        if (current != kDeviceStateListening) thinking_ = false;
        if (current == kDeviceStateListening || current == kDeviceStateSpeaking) sleepy_ = false;
    });
}

void ClaudeDisplay::BuildUi() {
    DisplayLockGuard lock(this);
    lv_obj_t* screen = lv_screen_active();
    lv_obj_clean(screen);  // remove a interface do xiaozhi

    // Ponteiros da classe base apontavam para objetos apagados: todos sao recriados ou zerados.
    status_bar_ = content_ = container_ = side_bar_ = preview_image_ = nullptr;
    emotion_label_ = chat_message_label_ = nullptr;
    low_battery_popup_ = low_battery_label_ = nullptr;

    lv_obj_set_style_bg_color(screen, COL_BG, 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    // barra superior: rede a esquerda, mudo e bateria a direita (atualizados pelo Display base)
    status_bar_ = lv_obj_create(screen);
    lv_obj_remove_style_all(status_bar_);
    lv_obj_set_size(status_bar_, width_, 16);
    lv_obj_set_pos(status_bar_, 0, 0);
    network_label_ = MakeLabel(status_bar_, &font_awesome_14_1, COL_DIM);
    lv_obj_align(network_label_, LV_ALIGN_LEFT_MID, 3, 0);
    battery_label_ = MakeLabel(status_bar_, &font_awesome_14_1, COL_DIM);
    lv_obj_align(battery_label_, LV_ALIGN_RIGHT_MID, -3, 0);
    mute_label_ = MakeLabel(status_bar_, &font_awesome_14_1, COL_DIM);
    lv_obj_align(mute_label_, LV_ALIGN_RIGHT_MID, -22, 0);

    // faiscas (canvas RGB565 redesenhados pelo timer)
    for (auto [canvas, size] : {std::pair{&hero_canvas_, HERO_SIZE}, std::pair{&small_canvas_, SMALL_SIZE}}) {
        *canvas = lv_canvas_create(screen);
        lv_canvas_set_draw_buf(*canvas, lv_draw_buf_create(size, size, LV_COLOR_FORMAT_RGB565, 0));
        lv_canvas_fill_bg(*canvas, COL_BG, LV_OPA_COVER);
    }
    lv_obj_set_pos(hero_canvas_, (width_ - HERO_SIZE) / 2, 12);
    lv_obj_set_pos(small_canvas_, 2, 18);

    // status ("Ouvindo...", relogio) e notificacoes ocupam o mesmo lugar; o Display base alterna os dois
    status_label_ = MakeLabel(screen, fonts_.text_font, COL_TEXT);
    notification_label_ = MakeLabel(screen, fonts_.text_font, COL_CLAUDE);
    for (auto* l : {status_label_, notification_label_}) {
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    }
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    hint_label_ = MakeLabel(screen, fonts_.text_font, COL_FAINT);
    lv_label_set_text(hint_label_, "Diga \"Hi, M Five\"");
    lv_obj_set_width(hint_label_, width_);
    lv_obj_set_style_text_align(hint_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(hint_label_, 0, 102);

    // texto da conversa dentro de uma caixa que recorta e rola verticalmente
    chat_box_ = lv_obj_create(screen);
    lv_obj_remove_style_all(chat_box_);
    lv_obj_set_pos(chat_box_, CHAT_X, CHAT_Y);
    lv_obj_set_size(chat_box_, CHAT_W, CHAT_H);
    lv_obj_set_scrollbar_mode(chat_box_, LV_SCROLLBAR_MODE_OFF);
    chat_message_label_ = MakeLabel(chat_box_, fonts_.text_font, COL_TEXT);
    lv_obj_set_width(chat_message_label_, CHAT_W);
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_line_space(chat_message_label_, 1, 0);

    ApplyLayout(false);
    timer_ = lv_timer_create([](lv_timer_t* t) { static_cast<ClaudeDisplay*>(lv_timer_get_user_data(t))->Tick(); },
                             FRAME_MS, this);
}

// Em espera: faisca grande + status centralizado. Na conversa: faisca pequena + texto.
void ClaudeDisplay::ApplyLayout(bool conversation) {
    conversation_ = conversation;
    if (conversation) {
        lv_obj_add_flag(hero_canvas_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(hint_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(small_canvas_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(chat_box_, LV_OBJ_FLAG_HIDDEN);
        for (auto* l : {status_label_, notification_label_}) {
            lv_obj_set_width(l, width_ - 36);
            lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_LEFT, 0);
            lv_obj_set_pos(l, 34, 23);
        }
    } else {
        lv_obj_remove_flag(hero_canvas_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(small_canvas_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(chat_box_, LV_OBJ_FLAG_HIDDEN);
        for (auto* l : {status_label_, notification_label_}) {
            lv_obj_set_width(l, width_ - 4);
            lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_pos(l, 2, 78);
        }
    }
}

void ClaudeDisplay::SetChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);
    if (chat_message_label_ == nullptr) return;
    bool has_text = content != nullptr && content[0] != '\0';
    if (!has_text) {
        lv_label_set_text(chat_message_label_, "");
        if (conversation_) ApplyLayout(false);
        return;
    }
    bool user = strcmp(role, "user") == 0;
    bool assistant = strcmp(role, "assistant") == 0;
    lv_obj_set_style_text_color(chat_message_label_, assistant ? COL_TEXT : COL_DIM, 0);
    if (user) {
        std::string quoted = std::string("\"") + content + "\"";
        lv_label_set_text(chat_message_label_, quoted.c_str());
    } else {
        lv_label_set_text(chat_message_label_, content);
    }
    scroll_y_ = 0;
    scroll_phase_ = 0;
    scroll_phase_since_ = NowMs();
    lv_obj_scroll_to_y(chat_box_, 0, LV_ANIM_OFF);
    if (!conversation_) ApplyLayout(true);
}

void ClaudeDisplay::SetEmotion(const char* emotion) {
    if (emotion == nullptr) return;
    // O servidor manda "thinking" quando a pergunta foi entendida e o Claude esta pensando.
    thinking_ = strcmp(emotion, "thinking") == 0;
    error_ = strcmp(emotion, "triangle_exclamation") == 0 || strcmp(emotion, "sad") == 0;
    sleepy_ = strcmp(emotion, "sleepy") == 0;
    if (thinking_) {
        DisplayLockGuard lock(this);
        lv_label_set_text(status_label_, "Pensando...");
        lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
    }
}

ClaudeDisplay::Mode ClaudeDisplay::CurrentMode() const {
    if (error_) return Mode::Error;
    switch (device_state_.load()) {
        case kDeviceStateWifiConfiguring: return Mode::Setup;
        case kDeviceStateIdle: return sleepy_ ? Mode::Sleep : Mode::Idle;
        case kDeviceStateConnecting: return Mode::Connecting;
        case kDeviceStateListening:
        case kDeviceStateAudioTesting: return thinking_ ? Mode::Thinking : Mode::Listening;
        case kDeviceStateSpeaking: return Mode::Speaking;
        case kDeviceStateFatalError: return Mode::Error;
        default: return Mode::Boot;
    }
}

// Roda na tarefa do LVGL (com o lock ja tomado).
void ClaudeDisplay::Tick() {
    uint32_t now = NowMs();
    float t = now / 1000.0f;
    int state = device_state_.load();
    if (state != last_state_) {
        last_state_ = state;
        state_since_ = now;
    }
    Mode mode = CurrentMode();

    // depois de um tempo em espera, volta para a faisca grande
    if (mode == Mode::Idle && conversation_ && now - state_since_ > IDLE_CLEAR_MS) {
        lv_label_set_text(chat_message_label_, "");
        ApplyLayout(false);
    }
    bool show_hint = !conversation_ && mode == Mode::Idle;
    if (show_hint == lv_obj_has_flag(hint_label_, LV_OBJ_FLAG_HIDDEN)) {
        if (show_hint) lv_obj_remove_flag(hint_label_, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(hint_label_, LV_OBJ_FLAG_HIDDEN);
    }

    lv_color_t color = COL_CLAUDE;
    float scale = 1.0f, rot = 0, wobble = t * 0.8f;
    switch (mode) {
        case Mode::Boot: color = COL_FAINT; rot = t * 0.6f; break;
        case Mode::Setup: color = COL_INFO; rot = t * 0.3f; break;
        case Mode::Idle: scale = 0.92f + 0.08f * sinf(t * 1.6f); rot = t * 0.15f; break;
        case Mode::Sleep: color = COL_FAINT; scale = 0.85f; wobble = 0; break;
        case Mode::Connecting: rot = t * 1.5f; break;
        case Mode::Listening: color = COL_INFO; scale = 0.84f + 0.16f * fabsf(sinf(t * 2.5f)); rot = t * 0.4f; break;
        case Mode::Thinking: rot = t * 2.2f; wobble = t * 4.0f; break;
        case Mode::Speaking: rot = t * 0.8f; wobble = t * 7.0f; break;
        case Mode::Error: color = COL_ERR; wobble = t * 9.0f; break;
    }
    if (conversation_) {
        DrawSpark(small_canvas_, SMALL_SIZE, SMALL_SIZE * 0.46f * scale, rot, wobble, color);
        ScrollChat(now);
    } else {
        DrawSpark(hero_canvas_, HERO_SIZE, HERO_SIZE * 0.46f * scale, rot, wobble, color);
    }
}

// A faisca: 12 raios triangulares afinando do centro para a ponta, com comprimentos irregulares.
void ClaudeDisplay::DrawSpark(lv_obj_t* canvas, int size, float radius, float rotation, float wobble,
                              lv_color_t color) {
    lv_canvas_fill_bg(canvas, COL_BG, LV_OPA_COVER);
    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);

    const float c = size / 2.0f;
    constexpr int RAYS = 12;
    lv_draw_triangle_dsc_t tri;
    lv_draw_triangle_dsc_init(&tri);
    tri.color = color;
    tri.opa = LV_OPA_COVER;
    for (int i = 0; i < RAYS; i++) {
        float a = rotation + i * (2 * kPi / RAYS);
        float len = radius * (0.70f + 0.30f * sinf(wobble + i * 1.7f));
        float half = radius * 0.13f;
        float ca = cosf(a), sa = sinf(a);
        tri.p[0].x = c + ca * len;
        tri.p[0].y = c + sa * len;
        tri.p[1].x = c - sa * half;
        tri.p[1].y = c + ca * half;
        tri.p[2].x = c + sa * half;
        tri.p[2].y = c - ca * half;
        lv_draw_triangle(&layer, &tri);
    }
    lv_draw_rect_dsc_t dot;
    lv_draw_rect_dsc_init(&dot);
    dot.bg_color = color;
    dot.radius = LV_RADIUS_CIRCLE;
    int r = std::max(1, (int)(radius * 0.17f));
    lv_area_t area = {(int32_t)(c - r), (int32_t)(c - r), (int32_t)(c + r), (int32_t)(c + r)};
    lv_draw_rect(&layer, &dot, &area);

    lv_canvas_finish_layer(canvas, &layer);
}

// Texto maior que a caixa: pausa no topo, rola devagar ate o fim, pausa e recomeca.
void ClaudeDisplay::ScrollChat(uint32_t now) {
    lv_obj_update_layout(chat_message_label_);
    int overflow = lv_obj_get_height(chat_message_label_) - CHAT_H;
    if (overflow <= 0) return;
    uint32_t elapsed = now - scroll_phase_since_;
    if (scroll_phase_ == 0 && elapsed > 1500) {
        scroll_phase_ = 1;
        scroll_phase_since_ = now;
    } else if (scroll_phase_ == 1 && elapsed > 70) {
        scroll_phase_since_ = now;
        if (++scroll_y_ >= overflow) scroll_phase_ = 2;
        lv_obj_scroll_to_y(chat_box_, scroll_y_, LV_ANIM_OFF);
    } else if (scroll_phase_ == 2 && elapsed > 2500) {
        scroll_phase_ = 0;
        scroll_phase_since_ = now;
        scroll_y_ = 0;
        lv_obj_scroll_to_y(chat_box_, 0, LV_ANIM_OFF);
    }
}
