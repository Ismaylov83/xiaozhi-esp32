// robot_eyes.h  (versión 4: tacto + inclinación + sacudida + evita el reposo)
// Ojos animados estilo robot para LVGL 9 (pensado para xiaozhi-esp32).
// Dibujados solo con objetos LVGL: sin imágenes, sin GIFs, casi sin RAM.
//
// Tacto (se lee del dispositivo táctil de LVGL):
//   - dedo sobre la pantalla  -> los ojos siguen el dedo con la mirada
//   - toque corto             -> sorpresa
//   - doble toque             -> risa
//   - mantener ~0,7 s         -> caricia (ojos cariñosos)
//   - mantener ~3 s           -> sueño
//
// Sensor de movimiento QMI8658 (se lee por I2C desde una tarea propia):
//   - inclinar a un lado      -> los ojos miran hacia ese lado
//   - zarandear               -> los ojos se marean un par de segundos
//   - al arrancar, un guiño   -> el sensor se detectó bien
//     (si se pone triste un momento, no se encontró el sensor)
//
// Actividad: tocar la pantalla o mover el dispositivo avisa a la placa
// (SetActivityCallback) para que no entre en modo reposo.
#pragma once

#include <lvgl.h>
#include <atomic>
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

    // Función que se llama cuando hay "actividad" (tocar o mover el dispositivo).
    // Se puede llamar desde cualquier tarea y como máximo dos veces por segundo.
    using ActivityCallback = void (*)(void*);
    static void SetActivityCallback(ActivityCallback cb, void* ctx);

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
    static void ImuTask(void* arg);
    static void NotifyActivity();
    void Tick();
    void Apply();
    void PollTouch(uint32_t now);
    void ImuLoop();
    // ms = 0: la reacción dura mientras se mantenga el dedo
    void StartReaction(const char* name, uint32_t ms);

    static inline std::atomic<ActivityCallback> activity_cb_{nullptr};
    static inline std::atomic<void*> activity_ctx_{nullptr};
    static inline std::atomic<int64_t> activity_last_ms_{-1000};

    int width_;
    int height_;
    uint32_t bg_;
    lv_obj_t* root_ = nullptr;
    Eye eyes_[2] = {};
    lv_timer_t* timer_ = nullptr;

    Pose base_{};    // expresión que pide xiaozhi
    Pose cur_{};     // expresión que se está dibujando ahora
    Pose target_{};  // hacia la que se mueve (base o reacción)
    lv_color_t color_{};

    float blink_k_ = 1.0f;
    float wink_k_ = 1.0f;
    float idle_x_ = 0.0f;
    float idle_y_ = 0.0f;
    bool blinking_ = false;
    uint32_t next_blink_ = 0;
    uint32_t blink_end_ = 0;
    uint32_t next_gaze_ = 0;

    // Reacción temporal (tacto, sacudida...)
    bool react_active_ = false;
    bool react_hold_ = false;
    Pose react_pose_{};
    uint32_t react_until_ = 0;

    // Estado del tacto
    lv_indev_t* indev_ = nullptr;
    bool was_pressed_ = false;
    bool moved_ = false;
    bool touching_ = false;
    bool tapped_before_ = false;
    int press_x_ = 0;
    int press_y_ = 0;
    int hold_stage_ = 0;  // 0 = nada, 1 = caricia, 2 = sueño
    uint32_t press_start_ = 0;
    uint32_t last_tap_ = 0;
    float touch_tx_ = 0.0f;  // objetivo de mirada por tacto (-1..1)
    float touch_ty_ = 0.0f;
    float touch_gx_ = 0.0f;  // valor suavizado
    float touch_gy_ = 0.0f;
    float touch_w_ = 0.0f;   // 0..1: cuánto manda el dedo sobre la mirada

    // Sensor de movimiento (escrito por la tarea del sensor, leído por LVGL)
    std::atomic<float> tilt_in_{0.0f};     // inclinación lateral -1..1
    std::atomic<bool> shake_req_{false};   // se detectó una sacudida
    std::atomic<int> imu_status_{0};       // 0 = nada nuevo, 1 = sensor OK, 2 = sensor no encontrado
    std::atomic<bool> imu_stop_{false};
    std::atomic<bool> imu_running_{false};
    float tilt_gx_ = 0.0f;  // inclinación suavizada
    bool shaking_ = false;
    uint32_t shake_start_ = 0;
    uint32_t shake_until_ = 0;
    float shake_tx_ = 0.0f;  // objetivo de la órbita de mareo
    float shake_ty_ = 0.0f;
    float shake_gx_ = 0.0f;  // valor suavizado
    float shake_gy_ = 0.0f;
};
