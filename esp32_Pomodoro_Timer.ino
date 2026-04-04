#include <lvgl.h>
#include <math.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#include <Wire.h>
#include "Arduino_GFX_Library.h"
#include "Arduino_DriveBus_Library.h"
#include "pin_config.h"
#include "lv_conf.h"
#include "HWCDC.h"
#include "SensorQMI8658.hpp"

HWCDC USBSerial;

Arduino_DataBus *bus = new Arduino_ESP32SPI(LCD_DC, LCD_CS, LCD_SCK, LCD_MOSI);
Arduino_GFX *gfx = new Arduino_ST7789(
  bus, LCD_RST, 0, true, LCD_WIDTH, LCD_HEIGHT, 0, 20, 0, 0
);

std::shared_ptr<Arduino_IIC_DriveBus> IIC_Bus =
  std::make_shared<Arduino_HWIIC>(IIC_SDA, IIC_SCL, &Wire);

void IRAM_ATTR Arduino_IIC_Touch_Interrupt(void);
static volatile bool g_touchInterruptFlag = false;

std::unique_ptr<Arduino_IIC> CST816T(new Arduino_CST816x(
  IIC_Bus, CST816T_DEVICE_ADDRESS, TP_RST, TP_INT, Arduino_IIC_Touch_Interrupt
));

void IRAM_ATTR Arduino_IIC_Touch_Interrupt(void) {
  // Keep ISR IRAM-safe: only touch a volatile flag here.
  g_touchInterruptFlag = true;
}

static constexpr uint32_t kLvglTickPeriodMs = 2;
static constexpr int kBeePin = 42;
static constexpr int kSysEnPin = 41;   // Keep board power enabled when HIGH.
static constexpr int kSysOutPin = 40;  // PWR button signal (LOW while pressed).
static constexpr uint16_t kAlarmFreqHz = 1760;
static constexpr uint32_t kAlarmShortBeepMs = 90;
static constexpr uint32_t kAlarmShortGapMs = 110;
static constexpr uint32_t kAlarmGroupGapMs = 260;
static constexpr uint8_t kAlarmBeepsPerGroup = 3;
static constexpr uint8_t kAlarmGroupCount = 3;
static constexpr uint16_t kUiClickFreqHz = 1047;
static constexpr uint32_t kUiClickMs = 42;
static constexpr int kTimerBtnW = 220;
static constexpr int kTimerBtnH = 62;
static constexpr int kTimerBtnGap = 8;
static constexpr int kTimerBtnBottomPad = 8;
static constexpr int kCountdownArcSize = 178;
static constexpr int kCountdownArcTopY = 28;
static constexpr int kArcLabelSessionYOfs = -34;
static constexpr int kArcLabelTimeYOfs = 30;
/** Fixed width so "Work" / "Rest" stay optically centered (proportional font). */
static constexpr lv_coord_t kLabelSessionWidth = 108;
/** Fixed width so "MM:SS" / "-MM:SS" stays optically centered (proportional font). */
static constexpr lv_coord_t kLabelTimeWidth = 172;
static constexpr uint32_t kCountdownArcModeToggleDebounceMs = 280;
static constexpr uint32_t kPwrDimHoldMs = 900;
static constexpr uint32_t kPwrOffHoldMs = 2200;
static constexpr uint32_t kPwrDebounceMs = 50;
static constexpr uint32_t kPwrIgnoreAfterBootMs = 3000;
static constexpr uint32_t kPwrIgnoreAfterWakeMs = 1500;
static constexpr uint32_t kPwrReleaseWaitTimeoutMs = 5000;
static constexpr int kModeButtonPin = 0;
static constexpr uint32_t kModeShortPressMaxMs = 800;
static constexpr uint32_t kModeDebounceMs = 40;
static constexpr uint32_t kBacklightPwmHz = 5000;
static constexpr uint8_t kBacklightPwmBits = 8;
static constexpr uint8_t kBacklightFull = 255;
static constexpr uint8_t kBacklightDim = 50;
static constexpr uint32_t kIdleDimTimeoutMs = 60000;
static constexpr int kVoltageDividerPin = 1;  // GPIO1
static constexpr float kVRef = 3.3f;
static constexpr float kR1 = 143000.0f;
static constexpr float kR2 = 65000.0f;

/** Auto-rotation from QMI8658 accelerometer only (gyro off).
 *  Datasheet Table 15: low-power accel-only (e.g. 21 Hz ≈42 µA) vs ~182 µA @ 1 kHz;
 *  gyro off avoids ~750–1000 µA 6DOF (Table 17).
 */
static SensorQMI8658 g_qmi8658;
static bool g_qmi8658_ok = false;
static bool g_disp_rot180 = false;
static bool g_imu_orient_calibrated = false;
static int8_t g_imu_dom_axis = -1;
static float g_imu_ref_axis_g = 0.0f;
static float g_imu_ax_f = 0.0f;
static float g_imu_ay_f = 0.0f;
static float g_imu_az_f = 0.0f;
static uint32_t g_last_imu_poll_ms = 0;
static uint8_t g_imu_flip_agree_frames = 0;
static bool g_imu_want180_pending = false;
static bool g_imu_lpf_inited = false;

/** Poll near accel ODR (~21 Hz ≈48 ms); debounce keeps rotation < ~500 ms total. */
static constexpr uint32_t kImuPollMs = 55;
static constexpr float kImuLpfAlpha = 0.5f;
static constexpr float kImuNear1GTol = 0.35f;
static constexpr float kImuFlipProd = 0.3f;
static constexpr float kImuAxisTrustG = 0.52f;
static constexpr uint8_t kImuFlipNeedFrames = 4;

enum class SessionType : uint8_t {
  Work = 0,
  Rest = 1
};

enum class TimerState : uint8_t {
  Stopped = 0,
  Running = 1,
  Paused = 2
};

enum class UIMode : uint8_t {
  Timer = 0,
  Set = 1
};

static uint16_t g_workMinutes = 25;
static uint16_t g_restMinutes = 5;
static SessionType g_sessionType = SessionType::Work;
static TimerState g_timerState = TimerState::Stopped;
static UIMode g_uiMode = UIMode::Timer;
static int32_t g_remainingSeconds = (int32_t)(g_workMinutes * 60);
static uint32_t g_lastSecondTickMs = 0;
static bool g_backlightDimmed = false;
static bool g_screenOff = false;

static int g_pwrLastLevel = HIGH;
static uint32_t g_pwrLastLevelChangeMs = 0;
static bool g_pwrPressedStable = false;
static uint32_t g_pwrPressStartMs = 0;
static bool g_pwrDimTriggered = false;
static uint32_t g_bootMs = 0;
static uint32_t g_wakeMs = 0;
static bool g_powerOffInitiated = false;
static uint32_t g_lastActivityMs = 0;
static uint32_t g_toneUntilMs = 0;
static bool g_alarm333Active = false;
static bool g_alarmToneOn = false;
static uint8_t g_alarmBeepsDoneInGroup = 0;
static uint8_t g_alarmGroupsDone = 0;
static uint32_t g_alarmNextMs = 0;

static int g_modeBtnLastLevel = HIGH;
static uint32_t g_modeBtnLowSinceMs = 0;
static lv_obj_t *g_timerPanel = nullptr;
static lv_obj_t *g_setPanel = nullptr;
static lv_obj_t *g_labelSession = nullptr;
static lv_obj_t *g_labelTime = nullptr;
static lv_obj_t *g_countdownArc = nullptr;
static lv_obj_t *g_btnLeft = nullptr;
static lv_obj_t *g_btnRight = nullptr;
static lv_obj_t *g_labelBtnLeft = nullptr;
static lv_obj_t *g_labelBtnRight = nullptr;
static lv_obj_t *g_labelSetWorkVal = nullptr;
static lv_obj_t *g_labelSetRestVal = nullptr;
static lv_obj_t *g_battOutline = nullptr;
static lv_obj_t *g_battFill = nullptr;
static lv_obj_t *g_battCap = nullptr;
static lv_obj_t *g_battText = nullptr;
static float g_battVoltage = NAN;
static uint8_t g_battPct = 0;
static bool g_countdownArcFingerDown = false;
static uint32_t g_lastCountdownArcModeToggleMs = 0;

static lv_disp_draw_buf_t draw_buf;

void my_disp_flush(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *px_map) {
  const uint32_t w = (area->x2 - area->x1 + 1);
  const uint32_t h = (area->y2 - area->y1 + 1);

#if (LV_COLOR_16_SWAP != 0)
  gfx->draw16bitBeRGBBitmap(area->x1, area->y1, (uint16_t *)px_map, w, h);
#else
  gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)px_map, w, h);
#endif

  lv_disp_flush_ready(disp_drv);
}

static void touch_activity_ping();

void my_touchpad_read(lv_indev_drv_t * /*indev_drv*/, lv_indev_data_t *data) {
  if (g_touchInterruptFlag) {
    g_touchInterruptFlag = false;
    touch_activity_ping();
    data->state = LV_INDEV_STATE_PRESSED;
    const int32_t touchX = CST816T->IIC_Read_Device_Value(
      CST816T->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_X
    );
    const int32_t touchY = CST816T->IIC_Read_Device_Value(
      CST816T->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_Y
    );
    if (touchX >= 0 && touchY >= 0) {
      if (g_disp_rot180) {
        data->point.x = (lv_coord_t)(LCD_WIDTH - 1 - touchX);
        data->point.y = (lv_coord_t)(LCD_HEIGHT - 1 - touchY);
      } else {
        data->point.x = touchX;
        data->point.y = touchY;
      }
    }
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

static void backlight_set(uint8_t duty) {
  ledcWrite(LCD_BL, duty);
}

static void screen_off() {
  noTone(kBeePin);
  g_toneUntilMs = 0;
  g_alarm333Active = false;
  g_alarmToneOn = false;
  g_alarmNextMs = 0;
  backlight_set(0);
  g_screenOff = true;
  g_backlightDimmed = false;
}

static void screen_on() {
  backlight_set(kBacklightFull);
  g_screenOff = false;
  g_backlightDimmed = false;
}

static void touch_activity_ping() {
  g_lastActivityMs = millis();
  if (!g_screenOff && g_backlightDimmed) {
    screen_on();
  }
}

static void idle_backlight_poll() {
  if (g_screenOff) {
    return;
  }
  const uint32_t now = millis();
  if ((now - g_lastActivityMs) < kIdleDimTimeoutMs) {
    return;
  }
  if (g_backlightDimmed) {
    return;
  }
  if (g_pwrPressedStable) {
    return;
  }
  g_backlightDimmed = true;
  backlight_set(kBacklightDim);
}

static inline void reset_power_button_state() {
  g_pwrLastLevel = digitalRead(kSysOutPin);
  g_pwrLastLevelChangeMs = millis();
  g_pwrPressedStable = false;
  g_pwrPressStartMs = 0;
  g_pwrDimTriggered = false;
}

static void enter_poweroff_sleep(const bool wait_for_pwr_release) {
  if (g_powerOffInitiated) {
    return;
  }
  g_powerOffInitiated = true;

  noTone(kBeePin);
  g_toneUntilMs = 0;
  g_alarm333Active = false;
  g_alarmToneOn = false;
  g_alarmNextMs = 0;
  // Descending confirmation tone before power off.
  tone(kBeePin, 523);
  delay(80);
  tone(kBeePin, 392);
  delay(80);
  tone(kBeePin, 330);
  delay(80);
  tone(kBeePin, 261);
  delay(100);
  noTone(kBeePin);

  backlight_set(kBacklightDim);
  delay(30);

  // Try board power cut path first.
  digitalWrite(kSysEnPin, LOW);
  delay(50);

  if (wait_for_pwr_release) {
    const uint32_t startMs = millis();
    while (digitalRead(kSysOutPin) == LOW && (millis() - startMs) < kPwrReleaseWaitTimeoutMs) {
      delay(10);
    }
  }

  // USB fallback: light sleep wake by PWR pin low level.
  gpio_wakeup_enable((gpio_num_t)kSysOutPin, GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();
  backlight_set(0);
  delay(10);
  esp_light_sleep_start();

  // Resume after wake.
  digitalWrite(kSysEnPin, HIGH);
  screen_on();
  g_lastActivityMs = millis();
  g_wakeMs = millis();
  reset_power_button_state();
  g_powerOffInitiated = false;
}

static uint8_t voltage_to_percent(float v) {
  if (!isfinite(v)) return 0;
  if (v >= 4.10f) return 100;
  if (v <= 3.50f) return 0;
  if (v >= 4.00f) return (uint8_t)(85 + (v - 4.00f) * (15.0f / 0.10f));
  if (v >= 3.90f) return (uint8_t)(65 + (v - 3.90f) * (20.0f / 0.10f));
  if (v >= 3.80f) return (uint8_t)(40 + (v - 3.80f) * (25.0f / 0.10f));
  if (v >= 3.70f) return (uint8_t)(20 + (v - 3.70f) * (20.0f / 0.10f));
  if (v >= 3.60f) return (uint8_t)(8 + (v - 3.60f) * (12.0f / 0.10f));
  return (uint8_t)((v - 3.50f) * (8.0f / 0.10f));
}

static void battery_poll_and_update() {
  static uint32_t lastMs = 0;
  static int8_t lastColorState = -1;  // -1 unknown, 0 normal, 1 low, 2 full
  const uint32_t now = millis();
  if (now - lastMs < 1000) {
    return;
  }
  lastMs = now;

  const int adcValue = analogRead(kVoltageDividerPin);
  const float voltage = (float)adcValue * (kVRef / 4095.0f);
  const float actualVoltage = voltage * ((kR1 + kR2) / kR2);

  if (!isfinite(g_battVoltage)) {
    g_battVoltage = actualVoltage;
  } else {
    constexpr float alpha = 0.15f;
    g_battVoltage = g_battVoltage + alpha * (actualVoltage - g_battVoltage);
  }

  g_battPct = voltage_to_percent(g_battVoltage);
  const bool low = (g_battPct < 20);
  const bool full = (g_battPct > 95);

  if (g_battOutline && g_battFill) {
    const int innerW = 34 - 2;
    const int fillW = (int)lroundf((innerW - 2) * (g_battPct / 100.0f));
    const int clamped = constrain(fillW, 0, innerW - 2);
    lv_obj_set_width(g_battFill, clamped);

    const int8_t colorState = low ? 1 : (full ? 2 : 0);
    if (colorState != lastColorState) {
      lastColorState = colorState;
      const lv_color_t c = low
                             ? lv_palette_lighten(LV_PALETTE_RED, 2)
                             : (full ? lv_palette_lighten(LV_PALETTE_GREEN, 2) : lv_color_white());
      lv_obj_set_style_bg_color(g_battFill, c, 0);
      if (g_battText) {
        lv_obj_set_style_text_color(g_battText, c, 0);
      }
    }
  }

  if (g_battText) {
    char s[8];
    snprintf(s, sizeof(s), "%u%%", (unsigned)g_battPct);
    lv_label_set_text(g_battText, s);
  }
}

static void update_ui() {
  if (g_timerPanel == nullptr || g_setPanel == nullptr || g_labelTime == nullptr ||
      g_countdownArc == nullptr || g_btnLeft == nullptr || g_btnRight == nullptr ||
      g_labelBtnLeft == nullptr || g_labelBtnRight == nullptr || g_labelSession == nullptr ||
      g_labelSetWorkVal == nullptr || g_labelSetRestVal == nullptr) {
    return;
  }

  if (g_uiMode == UIMode::Timer) {
    lv_obj_clear_flag(g_timerPanel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_setPanel, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(g_timerPanel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(g_setPanel, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text_fmt(g_labelSetWorkVal, "%u min", g_workMinutes);
    lv_label_set_text_fmt(g_labelSetRestVal, "%u min", g_restMinutes);
    return;
  }

  lv_label_set_text(g_labelSession, (g_sessionType == SessionType::Work) ? "Work" : "Rest");

  char line[40];
  if (g_remainingSeconds >= 0) {
    const uint32_t mm = (uint32_t)(g_remainingSeconds / 60);
    const uint32_t ss = (uint32_t)(g_remainingSeconds % 60);
    snprintf(line, sizeof(line), "%02lu:%02lu", (unsigned long)mm, (unsigned long)ss);
  } else {
    const int32_t neg = -g_remainingSeconds;
    const uint32_t mm = (uint32_t)(neg / 60);
    const uint32_t ss = (uint32_t)(neg % 60);
    snprintf(line, sizeof(line), "-%02lu:%02lu", (unsigned long)mm, (unsigned long)ss);
  }
  lv_label_set_text(g_labelTime, line);

  const uint32_t totalSeconds =
    ((g_sessionType == SessionType::Work) ? g_workMinutes : g_restMinutes) * 60UL;
  uint16_t progressPermille = 0;
  if (totalSeconds > 0) {
    if (g_sessionType == SessionType::Work) {
      // Work phase: countdown style (full -> empty).
      if (g_remainingSeconds > 0) {
        progressPermille = (uint16_t)(((uint32_t)g_remainingSeconds * 1000UL) / totalSeconds);
      }
    } else {
      // Rest phase: increasing style (empty -> full).
      int32_t elapsed = (int32_t)totalSeconds - g_remainingSeconds;
      if (elapsed < 0) {
        elapsed = 0;
      } else if ((uint32_t)elapsed > totalSeconds) {
        elapsed = (int32_t)totalSeconds;
      }
      progressPermille = (uint16_t)(((uint32_t)elapsed * 1000UL) / totalSeconds);
    }
  }
  lv_arc_set_value(g_countdownArc, progressPermille);
  lv_obj_set_style_arc_color(
    g_countdownArc,
    lv_color_hex((g_sessionType == SessionType::Work) ? 0xFF3B30 : 0x22C55E),
    LV_PART_INDICATOR
  );

  const bool stopped = (g_timerState == TimerState::Stopped);
  const bool positive = (g_remainingSeconds > 0);

  if (stopped) {
    lv_label_set_text(g_labelBtnLeft, "Start");
  } else if (positive) {
    lv_label_set_text(g_labelBtnLeft, (g_timerState == TimerState::Paused) ? "Resume" : "Pause");
  } else {
    lv_label_set_text(g_labelBtnLeft, "Next");
  }
  lv_label_set_text(g_labelBtnRight, "Reset");
}

static void reset_remaining_for_current_session() {
  g_remainingSeconds =
    (int32_t)((g_sessionType == SessionType::Work ? g_workMinutes : g_restMinutes) * 60);
}

static void clear_buzzer() {
  noTone(kBeePin);
  g_toneUntilMs = 0;
  g_alarm333Active = false;
  g_alarmToneOn = false;
  g_alarmBeepsDoneInGroup = 0;
  g_alarmGroupsDone = 0;
  g_alarmNextMs = 0;
}

static void ui_click_beep() {
  const uint32_t now = millis();
  tone(kBeePin, kUiClickFreqHz);
  g_toneUntilMs = now + kUiClickMs;
}

static void play_welcome_sound() {
  // Same greeting arpeggio as AxiCube project: C6 E6 G6 C7.
  static constexpr uint16_t kNotesHz[] = {1047, 1319, 1568, 2093};
  for (uint16_t hz : kNotesHz) {
    tone(kBeePin, hz);
    delay(45);
    noTone(kBeePin);
    delay(15);
  }
}

static void start_alarm_333() {
  g_alarm333Active = true;
  g_alarmToneOn = false;
  g_alarmBeepsDoneInGroup = 0;
  g_alarmGroupsDone = 0;
  g_alarmNextMs = millis();
}

static void do_restart() {
  reset_remaining_for_current_session();
  g_timerState = TimerState::Running;
  g_lastSecondTickMs = millis();
  clear_buzzer();
  update_ui();
}

static void do_pause_resume() {
  if (g_timerState == TimerState::Running) {
    g_timerState = TimerState::Paused;
  } else if (g_timerState == TimerState::Paused) {
    g_timerState = TimerState::Running;
    g_lastSecondTickMs = millis();
  }
  update_ui();
}

static void do_stop_reset() {
  g_timerState = TimerState::Stopped;
  reset_remaining_for_current_session();
  clear_buzzer();
  update_ui();
}

static void do_next_session() {
  g_sessionType = (g_sessionType == SessionType::Work) ? SessionType::Rest : SessionType::Work;
  reset_remaining_for_current_session();
  g_timerState = TimerState::Running;
  g_lastSecondTickMs = millis();
  clear_buzzer();
  update_ui();
}

static void on_btn_left(lv_event_t * /*e*/) {
  if (g_uiMode != UIMode::Timer) {
    return;
  }
  if (g_timerState == TimerState::Stopped) {
    do_restart();
  } else if (g_remainingSeconds > 0) {
    do_pause_resume();
  } else {
    do_next_session();
  }
}

static void on_btn_right(lv_event_t * /*e*/) {
  if (g_uiMode != UIMode::Timer) {
    return;
  }
  do_stop_reset();
}

static void on_countdown_arc_block_drag(lv_event_t *e) {
  if (lv_event_get_code(e) == LV_EVENT_PRESSING) {
    lv_event_stop_processing(e);
  }
}

static void on_countdown_arc_session_touch(lv_event_t *e) {
  const lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_PRESSED) {
    g_countdownArcFingerDown = true;
    return;
  }
  if (code == LV_EVENT_PRESS_LOST) {
    g_countdownArcFingerDown = false;
    return;
  }
  if (code != LV_EVENT_RELEASED) {
    return;
  }
  if (!g_countdownArcFingerDown) {
    return;
  }
  g_countdownArcFingerDown = false;

  if (g_uiMode != UIMode::Timer) {
    return;
  }
  if (g_timerState == TimerState::Running) {
    return;
  }
  const uint32_t now = millis();
  if ((now - g_lastCountdownArcModeToggleMs) < kCountdownArcModeToggleDebounceMs) {
    return;
  }
  g_lastCountdownArcModeToggleMs = now;

  g_sessionType = (g_sessionType == SessionType::Work) ? SessionType::Rest : SessionType::Work;
  g_timerState = TimerState::Stopped;
  reset_remaining_for_current_session();
  clear_buzzer();
  ui_click_beep();
  update_ui();
}

static void adjust_minutes(bool work, int delta) {
  if (g_uiMode != UIMode::Set) {
    return;
  }
  uint16_t &target = work ? g_workMinutes : g_restMinutes;
  int next = (int)target + delta;
  if (next < 1) {
    next = 1;
  } else if (next > 99) {
    next = 99;
  }
  target = (uint16_t)next;
  if (g_timerState == TimerState::Stopped) {
    reset_remaining_for_current_session();
  }
  update_ui();
}

static void on_work_minus(lv_event_t * /*e*/) { adjust_minutes(true, -1); }
static void on_work_plus(lv_event_t * /*e*/) { adjust_minutes(true, +1); }
static void on_rest_minus(lv_event_t * /*e*/) { adjust_minutes(false, -1); }
static void on_rest_plus(lv_event_t * /*e*/) { adjust_minutes(false, +1); }

static void on_ui_tap_beep(lv_event_t * /*e*/) { ui_click_beep(); }

static void style_timer_action_btn(lv_obj_t *btn, uint32_t normalHex, uint32_t pressedHex) {
  lv_obj_set_style_radius(btn, 10, LV_PART_MAIN);
  lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(btn, lv_color_hex(normalHex), (lv_style_selector_t)(LV_PART_MAIN | LV_STATE_DEFAULT));
  lv_obj_set_style_bg_color(btn, lv_color_hex(pressedHex), (lv_style_selector_t)(LV_PART_MAIN | LV_STATE_PRESSED));
}

static lv_obj_t *make_button(
  lv_obj_t *parent, const char *txt, lv_event_cb_t cb, lv_align_t align, int x_ofs, int y_ofs, int w, int h
) {
  lv_obj_t *btn = lv_btn_create(parent);
  lv_obj_set_size(btn, w, h);
  lv_obj_align(btn, align, x_ofs, y_ofs);
  // Action first (may call clear_buzzer for session tone), then click beep.
  lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_add_event_cb(btn, on_ui_tap_beep, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *label = lv_label_create(btn);
  lv_label_set_text(label, txt);
  lv_obj_center(label);
  return btn;
}

static void timed_buzzer_poll() {
  const uint32_t now = millis();
  if (g_alarm333Active) {
    if ((int32_t)(now - g_alarmNextMs) < 0) {
      return;
    }
    if (!g_alarmToneOn) {
      tone(kBeePin, kAlarmFreqHz);
      g_alarmToneOn = true;
      g_alarmNextMs = now + kAlarmShortBeepMs;
      return;
    }

    noTone(kBeePin);
    g_alarmToneOn = false;
    g_alarmBeepsDoneInGroup++;

    if (g_alarmBeepsDoneInGroup < kAlarmBeepsPerGroup) {
      g_alarmNextMs = now + kAlarmShortGapMs;
      return;
    }

    g_alarmBeepsDoneInGroup = 0;
    g_alarmGroupsDone++;
    if (g_alarmGroupsDone < kAlarmGroupCount) {
      g_alarmNextMs = now + kAlarmGroupGapMs;
    } else {
      g_alarm333Active = false;
      g_alarmNextMs = 0;
    }
    return;
  }

  if (g_toneUntilMs == 0) {
    return;
  }
  if ((int32_t)(now - g_toneUntilMs) < 0) {
    return;
  }
  noTone(kBeePin);
  g_toneUntilMs = 0;
}

static void apply_display_rotation180(bool rot180) {
  if (rot180 == g_disp_rot180) {
    return;
  }
  g_disp_rot180 = rot180;
  gfx->setRotation(rot180 ? 2 : 0);
  if (lv_scr_act() != nullptr) {
    lv_obj_invalidate(lv_scr_act());
  }
}

static float imu_axis_value(int8_t axis) {
  if (axis == 0) {
    return g_imu_ax_f;
  }
  if (axis == 1) {
    return g_imu_ay_f;
  }
  return g_imu_az_f;
}

static void gravity_orientation_poll() {
  if (!g_qmi8658_ok) {
    return;
  }
  const uint32_t now = millis();
  if ((uint32_t)(now - g_last_imu_poll_ms) < kImuPollMs) {
    return;
  }
  g_last_imu_poll_ms = now;

  if (!g_qmi8658.getDataReady()) {
    return;
  }

  float ax, ay, az;
  if (!g_qmi8658.getAccelerometer(ax, ay, az)) {
    return;
  }

  if (!g_imu_lpf_inited) {
    g_imu_ax_f = ax;
    g_imu_ay_f = ay;
    g_imu_az_f = az;
    g_imu_lpf_inited = true;
  } else {
    g_imu_ax_f += kImuLpfAlpha * (ax - g_imu_ax_f);
    g_imu_ay_f += kImuLpfAlpha * (ay - g_imu_ay_f);
    g_imu_az_f += kImuLpfAlpha * (az - g_imu_az_f);
  }

  const float mag = sqrtf(g_imu_ax_f * g_imu_ax_f + g_imu_ay_f * g_imu_ay_f + g_imu_az_f * g_imu_az_f);
  if (fabsf(mag - 1.0f) > kImuNear1GTol) {
    g_imu_flip_agree_frames = 0;
    return;
  }

  const float xa = fabsf(g_imu_ax_f);
  const float ya = fabsf(g_imu_ay_f);
  const float za = fabsf(g_imu_az_f);

  if (!g_imu_orient_calibrated) {
    int8_t dom = 2;
    float s = g_imu_az_f;
    if (xa >= ya && xa >= za) {
      dom = 0;
      s = g_imu_ax_f;
    } else if (ya >= xa && ya >= za) {
      dom = 1;
      s = g_imu_ay_f;
    }
    if (fabsf(s) < kImuAxisTrustG) {
      return;
    }
    g_imu_dom_axis = dom;
    g_imu_ref_axis_g = s;
    g_imu_orient_calibrated = true;
    return;
  }

  const float s_now = imu_axis_value(g_imu_dom_axis);
  if (fabsf(s_now) < kImuAxisTrustG) {
    g_imu_flip_agree_frames = 0;
    return;
  }

  bool want180 = g_disp_rot180;
  const float prod = s_now * g_imu_ref_axis_g;
  if (prod < -kImuFlipProd) {
    want180 = true;
  } else if (prod > kImuFlipProd) {
    want180 = false;
  } else {
    g_imu_flip_agree_frames = 0;
    return;
  }

  if (want180 == g_disp_rot180) {
    g_imu_flip_agree_frames = 0;
    return;
  }

  if (g_imu_flip_agree_frames == 0 || want180 != g_imu_want180_pending) {
    g_imu_want180_pending = want180;
    g_imu_flip_agree_frames = 1;
  } else {
    g_imu_flip_agree_frames++;
  }

  if (g_imu_flip_agree_frames >= kImuFlipNeedFrames) {
    apply_display_rotation180(want180);
    g_imu_flip_agree_frames = 0;
  }
}

static void mode_button_poll() {
  const uint32_t now = millis();
  const int level = digitalRead(kModeButtonPin);

  if (level == LOW && g_modeBtnLastLevel == HIGH) {
    g_modeBtnLowSinceMs = now;
  }
  if (level == HIGH && g_modeBtnLastLevel == LOW) {
    const uint32_t held = now - g_modeBtnLowSinceMs;
    if (held >= kModeDebounceMs && held < kModeShortPressMaxMs) {
      g_uiMode = (g_uiMode == UIMode::Timer) ? UIMode::Set : UIMode::Timer;
      ui_click_beep();
      update_ui();
    }
  }
  g_modeBtnLastLevel = level;
}

static void build_ui() {
  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x10131A), 0);
  lv_obj_set_style_text_color(scr, lv_color_hex(0xF0F3FA), 0);

  g_timerPanel = lv_obj_create(scr);
  lv_obj_set_size(g_timerPanel, LCD_WIDTH, LCD_HEIGHT);
  lv_obj_align(g_timerPanel, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_set_style_bg_opa(g_timerPanel, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(g_timerPanel, 0, 0);
  lv_obj_set_style_pad_all(g_timerPanel, 0, 0);
  lv_obj_clear_flag(g_timerPanel, LV_OBJ_FLAG_SCROLLABLE);

  g_setPanel = lv_obj_create(scr);
  lv_obj_set_size(g_setPanel, LCD_WIDTH, LCD_HEIGHT);
  lv_obj_align(g_setPanel, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_set_style_bg_opa(g_setPanel, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(g_setPanel, 0, 0);
  lv_obj_set_style_pad_all(g_setPanel, 0, 0);
  lv_obj_add_flag(g_setPanel, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(g_setPanel, LV_OBJ_FLAG_SCROLLABLE);

  g_labelSession = lv_label_create(g_timerPanel);
  lv_obj_set_style_text_font(g_labelSession, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(g_labelSession, lv_color_hex(0xF2F2F2), 0);
  lv_obj_set_style_text_align(g_labelSession, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_bg_opa(g_labelSession, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_all(g_labelSession, 0, 0);
  lv_obj_set_width(g_labelSession, kLabelSessionWidth);
  lv_label_set_long_mode(g_labelSession, LV_LABEL_LONG_CLIP);

  g_countdownArc = lv_arc_create(g_timerPanel);
  lv_obj_set_size(g_countdownArc, kCountdownArcSize, kCountdownArcSize);
  lv_obj_align(g_countdownArc, LV_ALIGN_TOP_MID, 0, kCountdownArcTopY);
  lv_arc_set_rotation(g_countdownArc, 135);
  lv_arc_set_bg_angles(g_countdownArc, 0, 270);
  lv_arc_set_range(g_countdownArc, 0, 1000);
  lv_arc_set_value(g_countdownArc, 1000);
  lv_arc_set_mode(g_countdownArc, LV_ARC_MODE_NORMAL);

  lv_obj_set_style_arc_width(g_countdownArc, 12, LV_PART_MAIN);
  lv_obj_set_style_arc_color(g_countdownArc, lv_color_hex(0x1B2230), LV_PART_MAIN);
  lv_obj_set_style_arc_width(g_countdownArc, 12, LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(g_countdownArc, true, LV_PART_INDICATOR);

  lv_obj_add_flag(g_countdownArc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(
    g_countdownArc, on_countdown_arc_block_drag, (lv_event_code_t)(LV_EVENT_PRESSING | LV_EVENT_PREPROCESS), nullptr
  );
  lv_obj_add_event_cb(g_countdownArc, on_countdown_arc_session_touch, LV_EVENT_PRESSED, nullptr);
  lv_obj_add_event_cb(g_countdownArc, on_countdown_arc_session_touch, LV_EVENT_RELEASED, nullptr);
  lv_obj_add_event_cb(g_countdownArc, on_countdown_arc_session_touch, LV_EVENT_PRESS_LOST, nullptr);

  g_labelTime = lv_label_create(g_timerPanel);
  lv_obj_set_style_text_font(g_labelTime, &lv_font_montserrat_36, 0);
  lv_obj_set_style_text_color(g_labelTime, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_align(g_labelTime, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_bg_opa(g_labelTime, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_all(g_labelTime, 0, 0);
  lv_obj_set_width(g_labelTime, kLabelTimeWidth);
  lv_label_set_long_mode(g_labelTime, LV_LABEL_LONG_CLIP);

  lv_obj_align_to(g_labelSession, g_countdownArc, LV_ALIGN_CENTER, 0, kArcLabelSessionYOfs);
  lv_obj_align_to(g_labelTime, g_countdownArc, LV_ALIGN_CENTER, 0, kArcLabelTimeYOfs);
  lv_obj_move_foreground(g_labelSession);
  lv_obj_move_foreground(g_labelTime);

  const lv_coord_t bottomSectionH = LCD_HEIGHT / 4;
  const lv_coord_t bottomSectionY = LCD_HEIGHT - bottomSectionH;
  const lv_coord_t halfW = LCD_WIDTH / 2;

  // Bottom quarter split into left/right halves.
  g_btnRight = lv_btn_create(g_timerPanel);
  lv_obj_set_size(g_btnRight, LCD_WIDTH - halfW, bottomSectionH);
  lv_obj_align(g_btnRight, LV_ALIGN_TOP_LEFT, halfW, bottomSectionY);
  style_timer_action_btn(g_btnRight, 0xD32F2F, 0xA82525);
  lv_obj_set_style_radius(g_btnRight, 0, LV_PART_MAIN);

  g_btnLeft = lv_btn_create(g_timerPanel);
  lv_obj_set_size(g_btnLeft, halfW, bottomSectionH);
  lv_obj_align(g_btnLeft, LV_ALIGN_TOP_LEFT, 0, bottomSectionY);
  style_timer_action_btn(g_btnLeft, 0x1FA855, 0x15803D);
  lv_obj_set_style_radius(g_btnLeft, 0, LV_PART_MAIN);

  lv_obj_add_event_cb(g_btnLeft, on_btn_left, LV_EVENT_CLICKED, nullptr);
  lv_obj_add_event_cb(g_btnLeft, on_ui_tap_beep, LV_EVENT_CLICKED, nullptr);
  lv_obj_add_event_cb(g_btnRight, on_btn_right, LV_EVENT_CLICKED, nullptr);
  lv_obj_add_event_cb(g_btnRight, on_ui_tap_beep, LV_EVENT_CLICKED, nullptr);

  g_labelBtnLeft = lv_label_create(g_btnLeft);
  lv_label_set_text(g_labelBtnLeft, "Pause");
  lv_obj_set_style_text_font(g_labelBtnLeft, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(g_labelBtnLeft, lv_color_hex(0xFFFFFF), 0);
  lv_obj_center(g_labelBtnLeft);

  g_labelBtnRight = lv_label_create(g_btnRight);
  lv_label_set_text(g_labelBtnRight, "Reset");
  lv_obj_set_style_text_font(g_labelBtnRight, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(g_labelBtnRight, lv_color_hex(0xFFFFFF), 0);
  lv_obj_center(g_labelBtnRight);

  lv_obj_t *rowWork = lv_obj_create(g_setPanel);
  lv_obj_set_size(rowWork, LCD_WIDTH, LCD_HEIGHT / 2);
  lv_obj_align(rowWork, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_set_style_bg_opa(rowWork, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(rowWork, 0, 0);
  lv_obj_set_style_pad_all(rowWork, 0, 0);

  lv_obj_t *titleW = lv_label_create(rowWork);
  lv_label_set_text(titleW, "Work");
  lv_obj_set_style_text_font(titleW, &lv_font_montserrat_18, 0);
  lv_obj_set_style_text_color(titleW, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(titleW, LV_ALIGN_TOP_MID, 0, 10);

  g_labelSetWorkVal = lv_label_create(rowWork);
  lv_obj_set_style_text_font(g_labelSetWorkVal, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(g_labelSetWorkVal, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(g_labelSetWorkVal, LV_ALIGN_TOP_MID, 0, 40);
  lv_label_set_text(g_labelSetWorkVal, "25 min");

  {
    // Keep ± in the lower band so they do not cover the minute label (was LEFT_MID/RIGHT_MID).
    lv_obj_t *bm = make_button(rowWork, "-", on_work_minus, LV_ALIGN_BOTTOM_LEFT, 10, -6, 76, 58);
    lv_obj_set_style_text_font(lv_obj_get_child(bm, 0), &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(bm, 0), lv_color_hex(0xFFFFFF), 0);
    lv_obj_t *bp = make_button(rowWork, "+", on_work_plus, LV_ALIGN_BOTTOM_RIGHT, -10, -6, 76, 58);
    lv_obj_set_style_text_font(lv_obj_get_child(bp, 0), &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(bp, 0), lv_color_hex(0xFFFFFF), 0);
  }

  lv_obj_t *rowRest = lv_obj_create(g_setPanel);
  lv_obj_set_size(rowRest, LCD_WIDTH, LCD_HEIGHT / 2);
  lv_obj_align(rowRest, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  lv_obj_set_style_bg_opa(rowRest, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(rowRest, 0, 0);
  lv_obj_set_style_pad_all(rowRest, 0, 0);

  lv_obj_t *titleR = lv_label_create(rowRest);
  lv_label_set_text(titleR, "Rest");
  lv_obj_set_style_text_font(titleR, &lv_font_montserrat_18, 0);
  lv_obj_set_style_text_color(titleR, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(titleR, LV_ALIGN_TOP_MID, 0, 10);

  g_labelSetRestVal = lv_label_create(rowRest);
  lv_obj_set_style_text_font(g_labelSetRestVal, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(g_labelSetRestVal, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(g_labelSetRestVal, LV_ALIGN_TOP_MID, 0, 40);
  lv_label_set_text(g_labelSetRestVal, "5 min");

  {
    lv_obj_t *bm = make_button(rowRest, "-", on_rest_minus, LV_ALIGN_BOTTOM_LEFT, 10, -6, 76, 58);
    lv_obj_set_style_text_font(lv_obj_get_child(bm, 0), &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(bm, 0), lv_color_hex(0xFFFFFF), 0);
    lv_obj_t *bp = make_button(rowRest, "+", on_rest_plus, LV_ALIGN_BOTTOM_RIGHT, -10, -6, 76, 58);
    lv_obj_set_style_text_font(lv_obj_get_child(bp, 0), &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(bp, 0), lv_color_hex(0xFFFFFF), 0);
  }

  g_battOutline = lv_obj_create(scr);
  lv_obj_remove_style_all(g_battOutline);
  lv_obj_set_size(g_battOutline, 34, 14);
  lv_obj_align(g_battOutline, LV_ALIGN_TOP_RIGHT, -12, 8);
  lv_obj_set_style_border_width(g_battOutline, 1, 0);
  lv_obj_set_style_border_color(g_battOutline, lv_color_white(), 0);
  lv_obj_set_style_radius(g_battOutline, 2, 0);
  lv_obj_set_style_bg_opa(g_battOutline, LV_OPA_TRANSP, 0);

  g_battFill = lv_obj_create(g_battOutline);
  lv_obj_remove_style_all(g_battFill);
  lv_obj_set_pos(g_battFill, 2, 2);
  lv_obj_set_size(g_battFill, 1, 10);
  lv_obj_set_style_bg_opa(g_battFill, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(g_battFill, lv_color_white(), 0);
  lv_obj_set_style_radius(g_battFill, 1, 0);

  g_battCap = lv_obj_create(scr);
  lv_obj_remove_style_all(g_battCap);
  lv_obj_set_size(g_battCap, 3, 8);
  lv_obj_align_to(g_battCap, g_battOutline, LV_ALIGN_OUT_RIGHT_MID, 1, 0);
  lv_obj_set_style_bg_opa(g_battCap, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(g_battCap, lv_color_white(), 0);
  lv_obj_set_style_radius(g_battCap, 1, 0);

  g_battText = lv_label_create(scr);
  lv_obj_set_style_text_font(g_battText, &lv_font_montserrat_12, 0);
  lv_obj_align_to(g_battText, g_battOutline, LV_ALIGN_OUT_LEFT_MID, -4, 0);
  lv_label_set_text(g_battText, "--");

  lv_obj_move_foreground(g_battOutline);
  lv_obj_move_foreground(g_battCap);
  lv_obj_move_foreground(g_battText);

  update_ui();
}

static void power_button_poll() {
  const uint32_t now = millis();
  if (now - g_bootMs < kPwrIgnoreAfterBootMs) {
    return;
  }
  if (g_wakeMs != 0 && (now - g_wakeMs) < kPwrIgnoreAfterWakeMs) {
    return;
  }

  const int level = digitalRead(kSysOutPin);
  if (level != g_pwrLastLevel) {
    g_pwrLastLevel = level;
    g_pwrLastLevelChangeMs = now;
  }

  if ((now - g_pwrLastLevelChangeMs) < kPwrDebounceMs) {
    return;
  }

  const bool pressed = (level == LOW);
  if (pressed) {
    if (!g_pwrPressedStable) {
      g_pwrPressedStable = true;
      g_pwrPressStartMs = now;
      g_pwrDimTriggered = false;
      if (g_screenOff) {
        screen_on();
        g_lastActivityMs = millis();
        return;
      }
    }

    const uint32_t heldMs = now - g_pwrPressStartMs;
    if (!g_pwrDimTriggered && heldMs >= kPwrDimHoldMs) {
      g_pwrDimTriggered = true;
      g_backlightDimmed = true;
      backlight_set(kBacklightDim);
    }
    if (heldMs >= kPwrOffHoldMs) {
      enter_poweroff_sleep(true);
    }
  } else {
    if (g_pwrPressedStable && g_backlightDimmed && !g_screenOff) {
      g_backlightDimmed = false;
      backlight_set(kBacklightFull);
      g_lastActivityMs = millis();
    }
    g_pwrPressedStable = false;
  }
}

void setup() {
  USBSerial.begin(115200);

  while (CST816T->begin() == false) {
    USBSerial.println("CST816T init failed");
    delay(500);
  }
  CST816T->IIC_Write_Device_State(
    CST816T->Arduino_IIC_Touch::Device::TOUCH_DEVICE_INTERRUPT_MODE,
    CST816T->Arduino_IIC_Touch::Device_Mode::TOUCH_DEVICE_INTERRUPT_PERIODIC
  );

  if (!g_qmi8658.begin(Wire, QMI8658_L_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)) {
    g_qmi8658_ok = false;
    USBSerial.println("QMI8658 init failed (auto-rotation disabled)");
  } else {
    (void)g_qmi8658.disableGyroscope();
    (void)g_qmi8658.configAccelerometer(
      SensorQMI8658::ACC_RANGE_2G,
      SensorQMI8658::ACC_ODR_LOWPOWER_21Hz,
      SensorQMI8658::LPF_MODE_1);
    g_qmi8658.enableAccelerometer();
    g_qmi8658_ok = true;
    USBSerial.println("QMI8658 OK (accel 21 Hz low-power, gyro off)");
  }

  pinMode(kBeePin, OUTPUT);
  digitalWrite(kBeePin, LOW);
  pinMode(kSysEnPin, OUTPUT);
  digitalWrite(kSysEnPin, HIGH);
  pinMode(kSysOutPin, INPUT_PULLUP);
  pinMode(kVoltageDividerPin, INPUT);
  pinMode(kModeButtonPin, INPUT_PULLUP);
  g_modeBtnLastLevel = digitalRead(kModeButtonPin);
  play_welcome_sound();

  gfx->begin();
  pinMode(LCD_BL, OUTPUT);
  ledcAttach(LCD_BL, kBacklightPwmHz, kBacklightPwmBits);
  backlight_set(kBacklightFull);
  reset_power_button_state();
  g_bootMs = millis();

  lv_init();

  const uint32_t screenWidth = gfx->width();
  const uint32_t screenHeight = gfx->height();
  lv_color_t *buf1 = (lv_color_t *)heap_caps_malloc(screenWidth * screenHeight / 4 * sizeof(lv_color_t), MALLOC_CAP_DMA);
  lv_color_t *buf2 = (lv_color_t *)heap_caps_malloc(screenWidth * screenHeight / 4 * sizeof(lv_color_t), MALLOC_CAP_DMA);
  if (buf1 == nullptr || buf2 == nullptr) {
    USBSerial.println("LVGL draw buffer alloc failed");
    while (true) {
      delay(1000);
    }
  }

  const uint32_t buf_h = screenHeight / 4;
  const uint32_t buf_pixels = screenWidth * buf_h;

  lv_disp_draw_buf_init(&draw_buf, buf1, buf2, buf_pixels);

  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = screenWidth;
  disp_drv.ver_res = screenHeight;
  disp_drv.flush_cb = my_disp_flush;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_t *disp = lv_disp_drv_register(&disp_drv);
  lv_disp_set_default(disp);

  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = my_touchpad_read;
  lv_indev_drv_register(&indev_drv);

  build_ui();
  g_lastActivityMs = millis();
  USBSerial.println("Pomodoro timer ready");
}

void loop() {
  static uint32_t lastLvTickMs = millis();
  const uint32_t nowTick = millis();
  const uint32_t elapsed = nowTick - lastLvTickMs;
  if (elapsed > 0) {
    lv_tick_inc(elapsed);
    lastLvTickMs = nowTick;
  }

  const uint32_t now = millis();
  if (g_timerState == TimerState::Running && (now - g_lastSecondTickMs) >= 1000) {
    g_lastSecondTickMs += 1000;
    const int32_t before = g_remainingSeconds;
    g_remainingSeconds--;
    if (before > 0 && g_remainingSeconds == 0) {
      g_lastActivityMs = millis();
      if (!g_screenOff && g_backlightDimmed) {
        screen_on();
      }
      start_alarm_333();
    }
    update_ui();
  }

  timed_buzzer_poll();
  power_button_poll();
  mode_button_poll();
  idle_backlight_poll();
  battery_poll_and_update();
  gravity_orientation_poll();
  lv_timer_handler();
  delay(5);
}
