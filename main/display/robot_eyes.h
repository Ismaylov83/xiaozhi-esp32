// robot_eyes.h
// Ojos animados estilo robot para LVGL 9 (pensado para xiaozhi-esp32).
// Dibujados solo con objetos LVGL: sin imágenes, sin GIFs, casi sin RAM.
#pragma once

#include <lvgl.h>
#include <cstdint>

class RobotEyes {
public:
    // Parámetros de una expresión. Todo en fracciones del tamaño base.
    struct Pose {
        float w;           // ancho del ojo (1.0 = normal)
        float h;           // alto del ojo (1.0 = normal)
        float radius;      // redondez: 0 = rectángulo, 0.5 = píldora/círculo
        float lid_top;     // párpado superior: 0 = abierto, 1 = cerrado
        float lid_angle;   // inclinación del párpado en grados (+ enfadado, - triste)
        float lid_bottom;  // párpado inferior curvo (ojos felices "^")
        float gaze_x;      // mirada fija horizontal (-1..1)
        float gaze_y;      // mirada fija vertical (-1..1)
        uint32_t color;    // color del ojo 0xRRGGBB
        bool wink;         // guiño: cierra el ojo derecho
    };

    // width/height: área total donde viven los dos ojos.
    // bg_color: color del fondo de tu pantalla (los párpados se pintan de este color).
    RobotEyes(lv_obj_t* parent, int width, int height, uint32_t bg_color = 0x000000);
    ~RobotEyes();

    RobotEyes(const RobotEyes&) = delete;
    RobotEyes& operator=(const RobotEyes&) = delete;

    // Llamar con el bloqueo de LVGL tomado (en xiaozhi: DisplayLockGuard).
    // Acepta los nombres de emoción de xiaozhi: "neutral", "happy", "sad", "angry"...
    void SetEmotion(const char* emotion);

    lv_obj_t* root() const { return root_; }

private:
    struct Eye {
        lv_obj_t* body;
        lv_obj_t* lid_top;
        lv_obj_t* lid_bottom;
    };

    static void TimerCb(lv_timer_t* t);
    void Tick();
    void Apply();

    int width_;
    int height_;
    uint32_t bg_;
    lv_obj_t* root_ = nullptr;
    Eye eyes_[2] = {};
    lv_timer_t* timer_ = nullptr;

    Pose cur_{};
    Pose target_{};
    lv_color_t color_{};

    float blink_k_ = 1.0f;
    float wink_k_ = 1.0f;
    float idle_x_ = 0.0f;
    float idle_y_ = 0.0f;
    bool blinking_ = false;
    uint32_t next_blink_ = 0;
    uint32_t blink_end_ = 0;
    uint32_t next_gaze_ = 0;
};
