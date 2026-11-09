#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "display/oled_display.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "mqtt_reporter.h"
#include "sleepfan_status.h"
#include "mcp_server.h"
#include "led/single_led.h"
#include "assets/lang_config.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <driver/uart.h>
#include <esp_adc/adc_oneshot.h>
#include <freertos/semphr.h>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <driver/i2c_master.h>
#include <driver/ledc.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>

#ifdef SH1106
#include <esp_lcd_panel_sh1106.h>
#endif

#define TAG "CompactWifiBoard"

/*
 * 风扇摇头步进电机控制（28BYJ-48 + ULN2003）
 *
 * 开环控制说明：固件无法知道机械中心在哪，所以"开始摇头"时把当前位置当作中心，
 * 先摆到一侧，再在两侧之间往返。使用前请手动把风扇头拨到中间位置。
 *
 * 脉冲生成方式（重要）：
 *   用 esp_timer 周期定时器输出步进脉冲，而不是"任务里忙等延时"。
 *   原因：本工程 audio_input 任务优先级 8、LVGL/WiFi 任务也在跑，
 *   任务上下文里的延时随时会被抢占，脉冲间隔忽长忽短 → 电机抖动、失步。
 *   esp_timer 的回调运行在高优先级定时器任务里，间隔稳定在微秒级。
 */
static const uint8_t kStepSeq[8][4] = {
    {1, 0, 0, 0}, {1, 1, 0, 0}, {0, 1, 0, 0}, {0, 1, 1, 0},
    {0, 0, 1, 0}, {0, 0, 1, 1}, {0, 0, 0, 1}, {1, 0, 0, 1},
};

class StepperMotor;   /* 前向声明：指针声明需要，完整定义在下方 */
static StepperMotor* g_stepper = nullptr;   /* 供 BOOT 键直接控制摇头（语音故障时备用） */
class FanModeController;                    /* 前向声明：按键回调在 InitializeTools 之前注册 */
static FanModeController* g_fan_ui = nullptr;

class StepperMotor {
public:
    StepperMotor(gpio_num_t in1, gpio_num_t in2, gpio_num_t in3, gpio_num_t in4)
    {
        pins_[0] = in1;
        pins_[1] = in2;
        pins_[2] = in3;
        pins_[3] = in4;

        gpio_config_t cfg = {
            .pin_bit_mask = (1ULL << in1) | (1ULL << in2) | (1ULL << in3) | (1ULL << in4),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&cfg));
        Release();

        const esp_timer_create_args_t timer_args = {
            .callback = &StepperMotor::TimerCallback,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "step_pulse",
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer_));

        lock_ = xSemaphoreCreateMutex();
        if (lock_ == nullptr) {
            ESP_LOGE(TAG, "stepper mutex create failed");
        }
    }

    void Start()
    {
        if (lock_ != nullptr && xSemaphoreTake(lock_, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGW(TAG, "stepper lock timeout in Start");
            return;
        }
        if (task_ != nullptr) {
            if (lock_ != nullptr) {
                xSemaphoreGive(lock_);
            }
            return;
        }
        /* 阶段管理任务优先级不需要很高，脉冲由 esp_timer 负责 */
        BaseType_t ret = xTaskCreatePinnedToCore(&StepperMotor::TaskEntry, "stepper_osc",
                                                 3072, this, 5, &task_, 1);
        if (ret != pdPASS) {
            task_ = nullptr;
            ESP_LOGE(TAG, "failed to create stepper task");
        } else {
            ESP_LOGI(TAG, "stepper oscillate started");
        }
        if (lock_ != nullptr) {
            xSemaphoreGive(lock_);
        }
    }

    void Stop()
    {
        if (lock_ != nullptr && xSemaphoreTake(lock_, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGW(TAG, "stepper lock timeout in Stop");
            return;
        }
        if (task_ != nullptr) {
            vTaskDelete(task_);
            task_ = nullptr;
        }
        if (timer_ != nullptr) {
            esp_timer_stop(timer_);
        }
        steps_left_ = 0;
        Release();
        if (lock_ != nullptr) {
            xSemaphoreGive(lock_);
        }
        ESP_LOGI(TAG, "stepper oscillate stopped");
    }

    /* 用户要求重新校准：停掉动作并清除"已归零"标记，下次启动会重新撞限位归零 */
    void RequestCalibrate()
    {
        if (lock_ != nullptr && xSemaphoreTake(lock_, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGW(TAG, "stepper lock timeout in RequestCalibrate");
            return;
        }
        if (task_ != nullptr) {
            vTaskDelete(task_);
            task_ = nullptr;
        }
        if (timer_ != nullptr) {
            esp_timer_stop(timer_);
        }
        steps_left_ = 0;
        Release();
        homed_ = false;
        if (lock_ != nullptr) {
            xSemaphoreGive(lock_);
        }
        ESP_LOGI(TAG, "calibration requested: next start will home to limit");
    }

    bool IsRunning() const { return task_ != nullptr; }

    /*
     * 上电逐相常亮测试（不需要万用表！）
     *
     * 每次只让一相通电并保持 3 秒，共 4 相 12 秒：
     *   1) 看 ULN2003 上对应位置的 LED 是否单独亮（其他 3 个应熄灭）
     *   2) 用手轻拨电机轴 —— 通电的那一相会把转子"锁住"，应有明显阻力/顿挫
     *
     * 判读：
     *   LED 亮 + 有阻力      → 该相完全正常
     *   LED 亮 + 无阻力      → 输出级坏（或电机该相线圈断）
     *   LED 不亮             → 输入级坏 / 该根信号线没通
     *   4 个 LED 同时亮      → IN1~IN4 被短接在一起
     */
    void SelfTest()
    {
        ESP_LOGI(TAG, "=== PHASE-BY-PHASE TEST (no multimeter needed) ===");
        ESP_LOGI(TAG, "pins IN1=%d IN2=%d IN3=%d IN4=%d", (int)pins_[0], (int)pins_[1],
                 (int)pins_[2], (int)pins_[3]);
        ESP_LOGI(TAG, "touch the motor shaft when each phase is ON - it should feel LOCKED");

        for (int i = 0; i < 4; i++) {
            for (int j = 0; j < 4; j++) {
                gpio_set_level(pins_[j], (j == i) ? 1 : 0);
            }
            ESP_LOGI(TAG, ">>> NOW: only IN%d is ON (GPIO%d) - 3s", i + 1, (int)pins_[i]);
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
        Release();
        ESP_LOGI(TAG, "=== PHASE TEST DONE ===");
    }

private:
    static void TaskEntry(void* arg)
    {
        static_cast<StepperMotor*>(arg)->TaskLoop();
    }

    /* 高优先级定时器回调：每 STEPPER_STEP_DELAY_US 走一拍 */
    static void TimerCallback(void* arg)
    {
        auto* self = static_cast<StepperMotor*>(arg);
        if (self->steps_left_ <= 0) {
            return;
        }
        self->phase_ = (self->phase_ + self->dir_ + 8) % 8;
        const uint8_t* row = kStepSeq[self->phase_];
        for (int i = 0; i < 4; i++) {
            gpio_set_level(self->pins_[i], row[i]);
        }
        self->steps_left_ = self->steps_left_ - 1;
        /* 每走一步就更新位置：这样即使中途被 Stop 打断，位置也始终准确 */
        self->position_ = self->position_ + self->dir_;
    }

    void TaskLoop()
    {
        /*
         * 位置策略（复刻原厂风扇的"机械限位归零"做法）：
         *   1) 上电后第一次启动：慢速朝一侧走到机械限位（多余步数丢步，位置自然归到限位），
         *      然后回半幅到中心，置 homed_ = true。约 4 秒，冲击小。
         *   2) 之后每次停止再启动：直接用位置计数器 position_ 快速回中心，无噪音、约 0.6 秒。
         *   3) 断电重启 → homed_ 复位，下次启动重新做一次慢速归零。
         */
        if (!homed_) {
            ESP_LOGI(TAG, "first start: slow homing to mechanical limit (%d steps)", STEPPER_HOME_STEPS);
            RunSegment(STEPPER_HOME_STEPS, +1, STEPPER_HOME_DELAY_US);
            position_ = STEPPER_HALF_STEPS;   /* 限位处 = 中心 + 半幅 */
            vTaskDelay(pdMS_TO_TICKS(200));

            ESP_LOGI(TAG, "back to center from limit");
            RunSegment(STEPPER_HALF_STEPS, -1, STEPPER_STEP_DELAY_US);
            vTaskDelay(pdMS_TO_TICKS(200));
            homed_ = true;
            ESP_LOGI(TAG, "calibration done, position = center");
        } else {
            int offset = position_;
            if (offset != 0) {
                ESP_LOGI(TAG, "return to center: %d steps", offset);
                RunSegment(offset > 0 ? offset : -offset, offset > 0 ? -1 : +1,
                           STEPPER_STEP_DELAY_US);
                vTaskDelay(pdMS_TO_TICKS(200));
            }
        }

        ESP_LOGI(TAG, "start symmetric swing +/-%d steps", STEPPER_HALF_STEPS);

        /*
         * 关键：必须先从中心摆到一侧，之后才在两侧之间往返（每段走 2 倍半幅）。
         * 若直接从中心走 2 倍半幅，会越过另一侧限位，表现就是"一边大一边小"。
         */
        RunSegment(STEPPER_HALF_STEPS, +1, STEPPER_STEP_DELAY_US);
        vTaskDelay(pdMS_TO_TICKS(STEPPER_DWELL_MS));

        int dir = -1;
        while (true) {
            RunSegment(STEPPER_HALF_STEPS * 2, dir, STEPPER_STEP_DELAY_US);
            vTaskDelay(pdMS_TO_TICKS(STEPPER_DWELL_MS));
            dir = -dir;
        }
    }

    /* 用定时器走完 steps 拍，走完才返回（本任务只是等待，不占 CPU 时序）
     * period_us 决定速度：正常摆动用 STEPPER_STEP_DELAY_US，归零时用更慢的 STEPPER_HOME_DELAY_US */
    void RunSegment(int steps, int dir, uint32_t period_us)
    {
        dir_ = dir;
        steps_left_ = steps;
        ESP_ERROR_CHECK(esp_timer_start_periodic(timer_, period_us));
        while (steps_left_ > 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        esp_timer_stop(timer_);
        Release();
    }

    void Release()
    {
        for (int i = 0; i < 4; i++) {
            gpio_set_level(pins_[i], 0);
        }
    }

    gpio_num_t pins_[4];
    esp_timer_handle_t timer_ = nullptr;
    TaskHandle_t task_ = nullptr;
    SemaphoreHandle_t lock_ = nullptr;   /* 保护 Start/Stop/RequestCalibrate，避免并发建任务 */
    volatile int phase_ = 0;
    volatile int dir_ = 1;
    volatile int steps_left_ = 0;
    volatile int position_ = 0;   /* 相对机械中心的步数偏移（正=一侧，负=另一侧） */
    volatile bool homed_ = false; /* 是否已完成一次"撞限位"的绝对归零（断电后复位） */
};

/*
 * 直流风扇电机控制（DRV8833 + LEDC PWM 调速）
 *
 * 接线（当前实际使用 L298N）：ENB=GPIO12(PWM), IN3=3.3V, IN4=GND,
 *       OUT3/OUT4=电机两线；AIN2 不再使用（FAN_AIN2_GPIO = GPIO_NUM_NC，
 *       步进 IN3 现用 GPIO21（避开板载 RGB 的 38））。
 *
 * PWM 频率取 20kHz，超出人耳可听范围，避免电机发出啸叫。
 */
class FanMotor {
public:
    FanMotor(gpio_num_t ain1, gpio_num_t ain2)
    {
        /* AIN2 固定拉低，与 AIN1 的 PWM 组成单向驱动；L298N 的 IN2 直接接 GND 时传 GPIO_NUM_NC */
        if (ain2 != GPIO_NUM_NC) {
            gpio_config_t cfg = {
                .pin_bit_mask = (1ULL << ain2),
                .mode = GPIO_MODE_OUTPUT,
                .pull_up_en = GPIO_PULLUP_DISABLE,
                .pull_down_en = GPIO_PULLDOWN_DISABLE,
                .intr_type = GPIO_INTR_DISABLE,
            };
            ESP_ERROR_CHECK(gpio_config(&cfg));
            gpio_set_level(ain2, 0);
        }

        ledc_timer_config_t timer_cfg = {};
        timer_cfg.speed_mode = LEDC_LOW_SPEED_MODE;
        timer_cfg.duty_resolution = LEDC_TIMER_10_BIT;
        timer_cfg.timer_num = LEDC_TIMER_0;
        timer_cfg.freq_hz = 20000;
        timer_cfg.clk_cfg = LEDC_AUTO_CLK;
        ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));

        ledc_channel_config_t ch_cfg = {};
        ch_cfg.gpio_num = ain1;
        ch_cfg.speed_mode = LEDC_LOW_SPEED_MODE;
        ch_cfg.channel = LEDC_CHANNEL_0;
        ch_cfg.intr_type = LEDC_INTR_DISABLE;
        ch_cfg.timer_sel = LEDC_TIMER_0;
        ch_cfg.duty = 0;
        ch_cfg.hpoint = 0;
        ESP_ERROR_CHECK(ledc_channel_config(&ch_cfg));

        ESP_LOGI(TAG, "fan motor ready: AIN1=GPIO%d (PWM 20kHz), AIN2=GPIO%d", ain1, ain2);
    }

    void SetPercent(int percent)
    {
        if (percent < 0) {
            percent = 0;
        }
        if (percent > 100) {
            percent = 100;
        }
        percent_ = percent;
        uint32_t duty = (uint32_t)((1023 * percent) / 100);
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    }

    int percent() const { return percent_; }

private:
    volatile int percent_ = 0;
};

/*
 * 传感器集线器：SHT30 温湿度 + 毫米波雷达 + MQ-135 空气质量
 *
 * - SHT30：复用 OLED 已建立的 I2C 总线（地址 0x44，与 OLED 的 0x3C 不冲突）
 * - 雷达：UART1，256000 8N1，协议为 LD2420 族（帧头 F4F3F2F1 / 帧尾 F8F7F6F5）
 * - MQ-135：ADC1_CH8（GPIO9），AO 必须经 10k/20k 分压后再接，否则会损坏 GPIO
 */
class SensorHub {
public:
    SensorHub(i2c_master_bus_handle_t i2c_bus, gpio_num_t radar_rx, gpio_num_t radar_tx, int adc_channel)
        : adc_channel_(adc_channel)
    {
        InitSht30(i2c_bus);
        InitAdc();
        InitRadar(radar_rx, radar_tx);
        BaseType_t ok = xTaskCreatePinnedToCore(&SensorHub::RadarTaskEntry, "radar_rx",
                                                4096, this, 5, &radar_task_, 1);
        if (ok != pdPASS) {
            ESP_LOGE(TAG, "radar task create FAILED (out of memory?)");
            radar_task_ = nullptr;
        }
    }

    /* 读温湿度，成功返回 true */
    bool ReadTempHum(float* temp, float* hum)
    {
        if (sht30_ == nullptr) {
            return false;
        }
        uint8_t cmd[2] = {0x2C, 0x06};   /* 高重复性、无时钟延展 */
        if (i2c_master_transmit(sht30_, cmd, 2, 100) != ESP_OK) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(25));
        uint8_t buf[6];
        if (i2c_master_receive(sht30_, buf, 6, 100) != ESP_OK) {
            return false;
        }
        uint16_t raw_t = (uint16_t)((buf[0] << 8) | buf[1]);
        uint16_t raw_h = (uint16_t)((buf[3] << 8) | buf[4]);
        *temp = -45.0f + 175.0f * (float)raw_t / 65535.0f;
        *hum = 100.0f * (float)raw_h / 65535.0f;
        return true;
    }

    int radar_state() const { return radar_state_; }
    int radar_distance_cm() const { return radar_dist_; }
    bool radar_online() const { return radar_online_; }

    /* 由 DecisionEngine 每 2 秒刷新一次缓存；MCP 工具只读缓存，不做阻塞 I2C 事务 */
    void RefreshCache()
    {
        float t = 0.0f;
        float h = 0.0f;
        if (ReadTempHum(&t, &h)) {
            cached_temp_ = t;
            cached_hum_ = h;
            cached_valid_ = true;
        } else {
            cached_valid_ = false;
        }
        /* MQ-135 跟温湿度一起在决策任务里采样。MQTT / 其它任务只读缓存。 */
        int raw = 0;
        if (adc_ != nullptr &&
            adc_oneshot_read(adc_, (adc_channel_t)adc_channel_, &raw) == ESP_OK) {
            cached_mq135_ = raw;
        }
    }
    float cached_temp() const { return cached_temp_; }
    float cached_hum() const { return cached_hum_; }
    bool cached_valid() const { return cached_valid_; }
    int cached_mq135() const { return cached_mq135_; }

    bool sleeping() const { return sleeping_; }
    int active_seconds_10min() const { return active_10min_; }
    int sampled_seconds() const { return sampled_; }

    int mq135_raw()
    {
        int raw = 0;
        if (adc_ != nullptr) {
            adc_oneshot_read(adc_, (adc_channel_t)adc_channel_, &raw);
        }
        return raw;
    }

private:
    i2c_master_dev_handle_t sht30_ = nullptr;
    adc_oneshot_unit_handle_t adc_ = nullptr;
    int adc_channel_ = 8;
    TaskHandle_t radar_task_ = nullptr;
    volatile int radar_state_ = 0;
    volatile int radar_dist_ = 0;
    volatile bool radar_online_ = false;
    int64_t last_log_us_ = 0;

    /* ---- 睡眠判定的活动量时间序列（每秒一个值，环形缓冲，10 分钟窗口）---- */
    static const int kSleepWindowSec = 600;
    uint8_t activity_[600] = {0};
    int act_idx_ = 0;
    uint8_t cur_sec_max_ = 0;
    int64_t sec_start_us_ = 0;
    int sampled_ = 0;
    volatile bool sleeping_ = false;
    volatile int active_10min_ = 0;

    /* 温湿度缓存（DecisionEngine 每 2 秒刷新，MCP 工具只读缓存，避免阻塞 I2C） */
    volatile float cached_temp_ = 0.0f;
    volatile float cached_hum_ = 0.0f;
    volatile bool cached_valid_ = false;
    volatile int cached_mq135_ = 0;

    /* 雷达无帧超时检测 */
    int64_t last_frame_us_ = 0;

    /* 每秒结算一次：把这一秒内最大的运动能量写进环形缓冲，并重新判定睡眠状态 */
    void TickOneSecond()
    {
        if (sec_start_us_ == 0) {
            sec_start_us_ = esp_timer_get_time();
            return;
        }
        sec_start_us_ += 1000000;
        activity_[act_idx_] = cur_sec_max_;
        act_idx_ = (act_idx_ + 1) % kSleepWindowSec;
        cur_sec_max_ = 0;
        if (sampled_ < kSleepWindowSec) {
            sampled_++;
        }
        UpdateSleepState();
    }

    /*
     * 睡眠判定：活动量阈值 + 时间窗统计（双阈值防抖）
     *   进入睡眠：最近 10 分钟内"活跃秒数"（运动能量 > 20）少于 60 秒
     *   退出睡眠：最近 2 分钟内活跃秒数超过 60 秒（翻个身就恢复）
     * 注意：至少采样 120 秒才做判定，避免开机时窗口全是 0 被误判为入睡。
     */
    void UpdateSleepState()
    {
        /* 只统计"已采样"的范围：activity_ 初始为 0，若遍历整个 600 格，
         * 未采样槽位会被当成"这一秒很安静"，开机 2 分钟后必然误判入睡。 */
        int window = (sampled_ < kSleepWindowSec) ? sampled_ : kSleepWindowSec;

        int active = 0;
        for (int k = 0; k < window; k++) {
            int i = (act_idx_ - 1 - k + kSleepWindowSec * 2) % kSleepWindowSec;
            if (activity_[i] > 20) {
                active++;
            }
        }
        active_10min_ = active;

        if (sampled_ < 120) {
            return;   /* 数据不足，先不判定 */
        }

        /* 进入阈值按窗口长度等比缩放（完整 600 秒 → 60 秒 ≈ 10%），保底 8 秒 */
        int enter_threshold = window * 60 / kSleepWindowSec;
        if (enter_threshold < 8) {
            enter_threshold = 8;
        }

        /* 退出：最近 2 分钟活跃秒数，阈值放宽到 15 秒，避免静坐时进得去出不来 */
        int recent = (window < 120) ? window : 120;
        int active_recent = 0;
        for (int k = 1; k <= recent; k++) {
            int i = (act_idx_ - k + kSleepWindowSec * 2) % kSleepWindowSec;
            if (activity_[i] > 20) {
                active_recent++;
            }
        }
        int exit_threshold = recent * 15 / 120;
        if (exit_threshold < 4) {
            exit_threshold = 4;
        }

        if (!sleeping_ && active < enter_threshold) {
            sleeping_ = true;
            ESP_LOGW(TAG, "sleep state -> ASLEEP (active %ds in %ds window)", active, window);
        } else if (sleeping_ && active_recent >= exit_threshold) {
            sleeping_ = false;
            ESP_LOGW(TAG, "sleep state -> AWAKE (active %ds in last %ds)", active_recent, recent);
        }
    }

    void InitSht30(i2c_master_bus_handle_t bus)
    {
        if (bus == nullptr) {
            ESP_LOGE(TAG, "no i2c bus for SHT30");
            return;
        }
        i2c_device_config_t cfg = {};
        cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        cfg.device_address = 0x44;
        cfg.scl_speed_hz = 100000;
        if (i2c_master_bus_add_device(bus, &cfg, &sht30_) != ESP_OK) {
            ESP_LOGE(TAG, "SHT30 not found on i2c bus");
            sht30_ = nullptr;
        } else {
            ESP_LOGI(TAG, "SHT30 ready (addr 0x44)");
        }
    }

    void InitAdc()
    {
        adc_oneshot_unit_init_cfg_t init_cfg = {};
        init_cfg.unit_id = ADC_UNIT_1;
        if (adc_oneshot_new_unit(&init_cfg, &adc_) != ESP_OK) {
            ESP_LOGE(TAG, "ADC1 init failed");
            adc_ = nullptr;
            return;
        }
        adc_oneshot_chan_cfg_t ch_cfg = {};
        ch_cfg.atten = ADC_ATTEN_DB_12;
        ch_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
        adc_oneshot_config_channel(adc_, (adc_channel_t)adc_channel_, &ch_cfg);
        ESP_LOGI(TAG, "MQ-135 ADC channel %d ready", adc_channel_);
    }

    void InitRadar(gpio_num_t rx, gpio_num_t tx)
    {
        uart_config_t cfg = {};
        cfg.baud_rate = 256000;
        cfg.data_bits = UART_DATA_8_BITS;
        cfg.parity = UART_PARITY_DISABLE;
        cfg.stop_bits = UART_STOP_BITS_1;
        cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
        cfg.source_clk = UART_SCLK_DEFAULT;
        if (uart_driver_install(UART_NUM_1, 2048, 0, 0, nullptr, 0) != ESP_OK) {
            ESP_LOGE(TAG, "radar uart install failed");
            return;
        }
        uart_param_config(UART_NUM_1, &cfg);
        uart_set_pin(UART_NUM_1, tx, rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        ESP_LOGI(TAG, "radar uart ready: RX=GPIO%d TX=GPIO%d @256000", rx, tx);
    }

    static void RadarTaskEntry(void* arg)
    {
        static_cast<SensorHub*>(arg)->RadarLoop();
    }

    void RadarLoop()
    {
        static const uint8_t HDR_A[4] = {0xF4, 0xF3, 0xF2, 0xF1};
        static const uint8_t HDR_B[4] = {0xFD, 0xFC, 0xFB, 0xFA};
        static const uint8_t TAIL_A[4] = {0x04, 0x03, 0x02, 0x01};
        static const uint8_t TAIL_B[4] = {0xF8, 0xF7, 0xF6, 0xF5};

        uint8_t buf[256];
        int len = 0;
        uint8_t chunk[64];

        while (true) {
            int n = uart_read_bytes(UART_NUM_1, chunk, sizeof(chunk), pdMS_TO_TICKS(100));
            if (sec_start_us_ == 0) {
                sec_start_us_ = esp_timer_get_time();
            } else if (esp_timer_get_time() - sec_start_us_ >= 1000000) {
                TickOneSecond();
            }
            /* 每 5 秒打印一次雷达状态，便于确认数据是否真的在进来 */
            int64_t now_us = esp_timer_get_time();

            /* 10 秒收不到任何帧就认为雷达掉线并清状态，
             * 否则 presence 会卡在最后一帧的值（风扇永远认为有人） */
            if (n > 0) {
                last_frame_us_ = now_us;
            } else if (radar_online_ && last_frame_us_ != 0 &&
                       now_us - last_frame_us_ > 10000000) {
                radar_online_ = false;
                radar_state_ = 0;
                radar_dist_ = 0;
                ESP_LOGW(TAG, "radar offline: no frame for 10s, presence cleared");
            }
            if (now_us - last_log_us_ > 5000000) {
                last_log_us_ = now_us;
                ESP_LOGI(TAG, "radar status: online=%d state=%d dist=%dcm last_bytes=%d",
                         radar_online_, radar_state_, radar_dist_, n);
            }
            if (n <= 0) {
                continue;
            }
            if (len + n > (int)sizeof(buf)) {
                len = 0;
            }
            memcpy(buf + len, chunk, n);
            len += n;

            while (true) {
                int off = -1;
                for (int i = 0; i + 4 <= len; i++) {
                    if (memcmp(buf + i, HDR_A, 4) == 0 || memcmp(buf + i, HDR_B, 4) == 0) {
                        off = i;
                        break;
                    }
                }
                if (off < 0) {
                    if (len > 3) {
                        memmove(buf, buf + len - 3, 3);
                        len = 3;
                    }
                    break;
                }
                if (off > 0) {
                    memmove(buf, buf + off, len - off);
                    len -= off;
                }
                if (len < 6) {
                    break;
                }
                int dlen = buf[4] | (buf[5] << 8);
                if (dlen < 11 || dlen > 64) {
                    memmove(buf, buf + 1, len - 1);
                    len -= 1;
                    continue;
                }
                int total = 4 + 2 + dlen + 4;
                if (len < total) {
                    break;
                }
                bool tail_ok = memcmp(buf + 4 + 2 + dlen, TAIL_A, 4) == 0 ||
                               memcmp(buf + 4 + 2 + dlen, TAIL_B, 4) == 0;
                if (!tail_ok) {
                    memmove(buf, buf + 1, len - 1);
                    len -= 1;
                    continue;
                }
                const uint8_t* d = buf + 6;
                if (d[0] == 0x02 && d[1] == 0xAA) {
                    radar_state_ = d[2];
                    int mov = d[3] | (d[4] << 8);
                    int still = d[6] | (d[7] << 8);
                    radar_dist_ = (mov > 0) ? mov : still;
                    radar_online_ = true;
                    /* d[5] 是运动目标能量，作为"活动量" */
                    uint8_t energy = d[5];
                    if (energy > cur_sec_max_) {
                        cur_sec_max_ = energy;
                    }
                }
                memmove(buf, buf + total, len - total);
                len -= total;
            }
        }
    }
};
/*
 * 自动融合决策引擎
 *
 * 每 2 秒评估一次，把三路传感器数据融合成风扇档位：
 *   雷达（有人/无人） × 温湿度（冷热） × 睡眠判定（睡没睡）
 *
 * 规则优先级（高 → 低）：
 *   1. 无人               → 关机（节能）
 *   2. 有人且已入睡       → 1 档静音（不管温度，避免着凉）
 *   3. 有人 温度 > 30℃    → 5 档
 *   4. 有人 温度 28~30℃   → 4 档
 *   5. 有人 温度 26~28℃   → 3 档
 *   6. 有人 温度 < 26℃    → 2 档
 *
 * auto_mode 默认关闭：只有语音"打开自动模式"后才真正驱动风扇，
 * 避免开机就动（也便于答辩时先 demonstrate 决策日志再开启执行）。
 */
class DecisionEngine {
public:
    DecisionEngine(FanMotor* fan, SensorHub* sensors) : fan_(fan), sensors_(sensors) {}

    void Start()
    {
        BaseType_t ok = xTaskCreatePinnedToCore(&DecisionEngine::TaskEntry, "decision",
                                                4096, this, 4, &task_, 1);
        if (ok != pdPASS) {
            ESP_LOGE(TAG, "decision task create FAILED (out of memory?)");
            task_ = nullptr;
        }
    }

    void SetAutoMode(bool on)
    {
        auto_mode_ = on;
        ESP_LOGW(TAG, "auto mode -> %s", on ? "ON" : "OFF");
    }

    bool auto_mode() const { return auto_mode_; }
    int last_level() const { return last_level_; }
    const char* last_reason() const { return last_reason_; }

    /* MQTT 任务只拷贝最近一次决策，不在这里读传感器。 */
    void CopyReport(bool* auto_mode, int* level, char* reason, size_t reason_len) const
    {
        if (auto_mode != nullptr) {
            *auto_mode = auto_mode_;
        }
        if (level != nullptr) {
            *level = last_level_;
        }
        if (reason != nullptr && reason_len > 0) {
            strncpy(reason, last_reason_, reason_len - 1);
            reason[reason_len - 1] = '\0';
        }
    }

private:
    FanMotor* fan_;
    SensorHub* sensors_;
    TaskHandle_t task_ = nullptr;
    volatile bool auto_mode_ = false;
    volatile int last_level_ = -1;
    char last_reason_[40] = "init";
    int64_t away_since_us_ = 0;

    static void TaskEntry(void* arg)
    {
        static_cast<DecisionEngine*>(arg)->TaskLoop();
    }

    void TaskLoop()
    {
        while (true) {
            Evaluate();
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }

    void Evaluate()
    {
        int state = sensors_->radar_state();
        bool present = (state != 0);

        /* 本任务每 2 秒刷新一次温湿度缓存（唯一做 I2C 事务的地方），
         * MCP 工具与其它任务只读缓存，避免阻塞 */
        sensors_->RefreshCache();
        float t = sensors_->cached_temp();
        float h = sensors_->cached_hum();
        (void)h;   /* 湿度暂不参与档位决策，保留以备扩展 */
        bool th_ok = sensors_->cached_valid();
        bool sleeping = sensors_->sleeping();

        int64_t now = esp_timer_get_time();
        if (!present) {
            if (away_since_us_ == 0) {
                away_since_us_ = now;
            }
        } else {
            away_since_us_ = 0;
        }

        int level = 0;
        char reason[40] = "";

        if (!present) {
            int64_t away_s = (now - away_since_us_) / 1000000;
            level = 0;
            if (away_s >= 300) {
                snprintf(reason, sizeof(reason), "away %llds", (long long)away_s);
            } else {
                snprintf(reason, sizeof(reason), "no presence");
            }
        } else if (sleeping) {
            level = 1;
            snprintf(reason, sizeof(reason), "asleep -> silent");
        } else if (th_ok && t > 30.0f) {
            level = 5;
            snprintf(reason, sizeof(reason), "hot %.1fC", t);
        } else if (th_ok && t > 28.0f) {
            level = 4;
            snprintf(reason, sizeof(reason), "warm %.1fC", t);
        } else if (th_ok && t >= 26.0f) {
            level = 3;
            snprintf(reason, sizeof(reason), "mild %.1fC", t);
        } else if (th_ok) {
            level = 2;
            snprintf(reason, sizeof(reason), "cool %.1fC", t);
        } else {
            level = 2;
            snprintf(reason, sizeof(reason), "no th data");
        }

        /* 只在结果变化时打印，避免刷屏 */
        if (level != last_level_ || strcmp(reason, last_reason_) != 0) {
            last_level_ = level;
            strncpy(last_reason_, reason, sizeof(last_reason_) - 1);
            ESP_LOGI(TAG, "Decision: presence=%d sleeping=%d state=%d -> level %d (%s)%s",
                     present, sleeping, state, level, reason,
                     auto_mode_ ? " [AUTO]" : " [idle]");
        }

        if (auto_mode_) {
            static const int kDutyByLevel[6] = {0, 30, 45, 60, 80, 100};
            fan_->SetPercent(kDutyByLevel[level]);
        }
    }
};


/*
 * 本地 UI 状态机：四模式 + 风速档 + 定时关机。
 * 输入来自旋转编码器（转=风速，SW 短按=模式，SW 长按=定时）与 MCP/语音。
 * 在 Smart 模式下不写 PWM（交给 DecisionEngine）；其它模式 SetAutoMode(false)。
 * 回调经 g_fan_ui 空指针保护，InitializeTools 里 Start() 后才真正生效。
 */
class FanModeController {
public:
    enum class Mode : int {
        Comfort = 0,
        Natural = 1,
        Sleep = 2,
        Smart = 3,
    };

    FanModeController(FanMotor* fan, DecisionEngine* decision)
        : fan_(fan), decision_(decision) {}

    void Start()
    {
        /* 上电默认智能模式，保持语音/自动决策可用；日志标明便于串口确认 */
        ApplyMode(Mode::Smart, /*force=*/true);
        BaseType_t ok = xTaskCreatePinnedToCore(&FanModeController::TaskEntry, "fan_ui",
                                                3072, this, 3, &task_, 1);
        if (ok != pdPASS) {
            ESP_LOGE(TAG, "fan_ui task create FAILED");
            task_ = nullptr;
        } else {
            ESP_LOGI(TAG, "FanModeController started, default mode=smart");
        }
    }

    enum class Focus : int {
        Mode = 0,
        Timer = 1,
        Speed = 2,
    };

    /* 触控：在 模式/定时/风速 三个焦点间循环；默认风速，旋钮仍先调档 */
    void CycleFocus()
    {
        int next = (static_cast<int>(focus_) + 1) % 3;
        focus_ = static_cast<Focus>(next);
        ESP_LOGI(TAG, "ui focus -> %s", FocusId());
        NotifyFocus();
    }

    /* 编码器一格：按当前焦点改模式 / 定时 / 风速 */
    void OnEncoder(int dir)
    {
        if (dir == 0) {
            return;
        }
        switch (focus_) {
            case Focus::Mode:
                CycleMode(dir);
                break;
            case Focus::Timer:
                BumpTimer(dir);
                break;
            case Focus::Speed:
            default:
                BumpLevel(dir);
                break;
        }
    }

    void CycleMode(int dir = +1)
    {
        ClearTimerPause();
        if (dir == 0) {
            dir = 1;
        }
        int m = static_cast<int>(mode_) + dir;
        m %= 4;
        if (m < 0) {
            m += 4;
        }
        ApplyMode(static_cast<Mode>(m), /*force=*/true);
        NotifyMode();
    }

    void BumpLevel(int delta)
    {
        ClearTimerPause();
        /* 手动调速一律回到舒适恒定档，并关掉自动，避免 DecisionEngine 抢 PWM */
        if (mode_ != Mode::Comfort) {
            mode_ = Mode::Comfort;
            if (decision_ != nullptr) {
                decision_->SetAutoMode(false);
            }
        }
        int level = manual_level_ + delta;
        /* 五档：1..5；0=关。不循环；在 0 再减仍为 0，在 5 再加仍为 5 */
        if (level > 5) {
            level = 5;
        }
        if (level < 0) {
            level = 0;
        }
        manual_level_ = level;
        ApplyComfortDuty();
        ESP_LOGI(TAG, "fan level -> %d (comfort)", manual_level_);
        NotifyLevel();
    }

    /* 定时：每格 ±10 分钟，0=关，上限 8 小时=480 分钟；从当前时刻起算截止 */
    void BumpTimer(int delta)
    {
        ClearTimerPause();
        int step = 0;
        if (delta > 0) {
            step = 10;
        } else if (delta < 0) {
            step = -10;
        }
        int minutes = timer_minutes_ + step;
        if (minutes < 0) {
            minutes = 0;
        }
        if (minutes > 480) {
            minutes = 480;
        }
        timer_minutes_ = minutes;
        if (timer_minutes_ == 0) {
            timer_deadline_us_ = 0;
            ESP_LOGI(TAG, "fan timer OFF");
        } else {
            timer_deadline_us_ = esp_timer_get_time() +
                                 (int64_t)timer_minutes_ * 60LL * 1000000LL;
            ESP_LOGI(TAG, "fan timer -> %d min", timer_minutes_);
        }
        NotifyTimer();
    }

    /* MCP / 语音：切到智能 */
    void EnterSmart()
    {
        ClearTimerPause();
        ApplyMode(Mode::Smart, /*force=*/true);
    }

    /* MCP / 语音：关自动 → 舒适，保持当前占空比推算档位 */
    void EnterComfortKeepDuty()
    {
        ClearTimerPause();
        if (decision_ != nullptr) {
            decision_->SetAutoMode(false);
        }
        mode_ = Mode::Comfort;
        if (fan_ != nullptr) {
            int duty = fan_->percent();
            int level = SleepfanDutyToLevel(duty);
            if (level >= 0) {
                manual_level_ = level;
            }
        }
        ESP_LOGI(TAG, "mode -> comfort (keep duty), level=%d", manual_level_);
    }

    /* MCP set_speed：舒适 + 指定档 */
    void SetComfortLevel(int level)
    {
        ClearTimerPause();
        if (level < 0) level = 0;
        if (level > 5) level = 5;
        if (decision_ != nullptr) {
            decision_->SetAutoMode(false);
        }
        mode_ = Mode::Comfort;
        manual_level_ = level;
        ApplyComfortDuty();
        ESP_LOGI(TAG, "MCP/comfort level -> %d", manual_level_);
    }

    /* MCP set_duty / stop：舒适 + 自定义占空比（档位尽量对齐表） */
    void SetComfortDuty(int percent)
    {
        ClearTimerPause();
        if (percent < 0) percent = 0;
        if (percent > 100) percent = 100;
        if (decision_ != nullptr) {
            decision_->SetAutoMode(false);
        }
        mode_ = Mode::Comfort;
        int level = SleepfanDutyToLevel(percent);
        if (level >= 0) {
            manual_level_ = level;
        } else if (percent == 0) {
            manual_level_ = 0;
        }
        if (fan_ != nullptr) {
            fan_->SetPercent(percent);
        }
        ESP_LOGI(TAG, "MCP/comfort duty -> %d%% (level=%d)", percent, manual_level_);
    }

    bool SetModeByName(const char* name)
    {
        if (name == nullptr) {
            return false;
        }
        ClearTimerPause();
        if (strcmp(name, "comfort") == 0) {
            ApplyMode(Mode::Comfort, true);
            return true;
        }
        if (strcmp(name, "natural") == 0) {
            ApplyMode(Mode::Natural, true);
            return true;
        }
        if (strcmp(name, "sleep") == 0) {
            ApplyMode(Mode::Sleep, true);
            return true;
        }
        if (strcmp(name, "smart") == 0) {
            ApplyMode(Mode::Smart, true);
            return true;
        }
        return false;
    }

    const char* ModeId() const
    {
        switch (mode_) {
            case Mode::Comfort: return "comfort";
            case Mode::Natural: return "natural";
            case Mode::Sleep:   return "sleep";
            case Mode::Smart:   return "smart";
        }
        return "comfort";
    }

    const char* ModeZh() const
    {
        switch (mode_) {
            case Mode::Comfort: return "舒适";
            case Mode::Natural: return "自然风";
            case Mode::Sleep:   return "睡觉";
            case Mode::Smart:   return "智能";
        }
        return "舒适";
    }

    int manual_level() const { return manual_level_; }

    int TimerRemainS() const
    {
        if (timer_deadline_us_ <= 0) {
            return 0;
        }
        int64_t left = timer_deadline_us_ - esp_timer_get_time();
        if (left <= 0) {
            return 0;
        }
        return (int)((left + 999999) / 1000000);
    }

    /* 兼容旧接口：按已设定分钟数换算整小时（不足 1 小时为 0） */
    int timer_hours() const { return timer_minutes_ / 60; }

    void CopyMode(char* out, size_t out_len) const
    {
        if (out == nullptr || out_len == 0) {
            return;
        }
        strncpy(out, ModeId(), out_len - 1);
        out[out_len - 1] = '\0';
    }

private:
    static constexpr int kDutyByLevel[6] = {0, 30, 45, 60, 80, 100};

    FanMotor* fan_;
    DecisionEngine* decision_;
    TaskHandle_t task_ = nullptr;
    Mode mode_ = Mode::Smart;
    int manual_level_ = 2;
    int sleep_level_ = 1;
    int64_t sleep_enter_us_ = 0;
    Focus focus_ = Focus::Speed;  /* 默认风速：旋钮先调档 */
    int timer_minutes_ = 0;       /* 0=关，步进 10，上限 480 */
    int64_t timer_deadline_us_ = 0;
    volatile bool paused_by_timer_ = false;

    static void TaskEntry(void* arg)
    {
        static_cast<FanModeController*>(arg)->TaskLoop();
    }

    void TaskLoop()
    {
        while (true) {
            Tick();
            vTaskDelay(pdMS_TO_TICKS(250));
        }
    }

    void ClearTimerPause()
    {
        paused_by_timer_ = false;
    }

    void ApplyComfortDuty()
    {
        if (fan_ != nullptr) {
            fan_->SetPercent(kDutyByLevel[manual_level_]);
        }
    }

    void ApplyMode(Mode m, bool force)
    {
        if (!force && m == mode_) {
            return;
        }
        Mode prev = mode_;
        mode_ = m;

        if (m == Mode::Smart) {
            if (decision_ != nullptr) {
                decision_->SetAutoMode(true);
            }
            ESP_LOGI(TAG, "mode -> smart (DecisionEngine auto ON)");
            return;
        }

        /* 离开智能：先关自动，再由本控制器接管 PWM */
        if (decision_ != nullptr) {
            decision_->SetAutoMode(false);
        }

        if (m == Mode::Comfort) {
            ApplyComfortDuty();
            ESP_LOGI(TAG, "mode -> comfort, level=%d duty=%d",
                     manual_level_, kDutyByLevel[manual_level_]);
        } else if (m == Mode::Natural) {
            ESP_LOGI(TAG, "mode -> natural, base_level=%d",
                     manual_level_ == 0 ? 2 : manual_level_);
        } else if (m == Mode::Sleep) {
            sleep_level_ = manual_level_;
            if (sleep_level_ > 2) {
                sleep_level_ = 2;
            }
            if (sleep_level_ < 1) {
                sleep_level_ = 1;
            }
            sleep_enter_us_ = esp_timer_get_time();
            if (fan_ != nullptr) {
                fan_->SetPercent(kDutyByLevel[sleep_level_]);
            }
            ESP_LOGI(TAG, "mode -> sleep, start_level=%d (no auto oscillate)",
                     sleep_level_);
        }

        (void)prev;
    }

    void Tick()
    {
        /* 定时到期：停转 + 关自动，保持当前 mode，直到用户再按键 */
        if (timer_deadline_us_ > 0 && esp_timer_get_time() >= timer_deadline_us_) {
            timer_deadline_us_ = 0;
            timer_minutes_ = 0;
            paused_by_timer_ = true;
            if (decision_ != nullptr) {
                decision_->SetAutoMode(false);
            }
            if (fan_ != nullptr) {
                fan_->SetPercent(0);
            }
            ESP_LOGW(TAG, "fan timer expired -> stop, auto OFF (mode kept=%s)", ModeId());
            return;
        }

        if (paused_by_timer_) {
            return;
        }

        if (mode_ == Mode::Smart) {
            /* DecisionEngine 每 2s Evaluate 写 PWM，这里不动 */
            return;
        }

        if (mode_ == Mode::Comfort) {
            /* 恒定档：进入/调速时已写过 PWM，这里不再刷，避免盖掉 set_duty 自定义值 */
            return;
        }

        if (mode_ == Mode::Natural) {
            int base_level = (manual_level_ == 0) ? 2 : manual_level_;
            int base = kDutyByLevel[base_level];
            /* 约 12 秒一个慢正弦周期，振幅 ±18 个百分点，夹到 20..100 */
            const int64_t period_us = 12000000LL;
            int64_t now = esp_timer_get_time();
            float phase = (float)(now % period_us) / (float)period_us * 6.2831853f;
            int duty = base + (int)(18.0f * sinf(phase));
            if (duty < 20) duty = 20;
            if (duty > 100) duty = 100;
            if (fan_ != nullptr) {
                fan_->SetPercent(duty);
            }
            return;
        }

        if (mode_ == Mode::Sleep) {
            int64_t elapsed = esp_timer_get_time() - sleep_enter_us_;
            /* 每 30 分钟降一档，最低 1 档微风 */
            int steps = (int)(elapsed / (30LL * 60LL * 1000000LL));
            int target = sleep_level_ - steps;
            if (target < 1) {
                target = 1;
            }
            if (fan_ != nullptr) {
                fan_->SetPercent(kDutyByLevel[target]);
            }
            return;
        }
    }

    const char* FocusId() const
    {
        switch (focus_) {
            case Focus::Mode:  return "mode";
            case Focus::Timer: return "timer";
            case Focus::Speed: return "speed";
        }
        return "speed";
    }

    const char* FocusZh() const
    {
        switch (focus_) {
            case Focus::Mode:  return "模式";
            case Focus::Timer: return "定时";
            case Focus::Speed: return "风速";
        }
        return "风速";
    }

    void NotifyFocus()
    {
        auto* board = Board::GetInstance().GetDisplay();
        if (board != nullptr) {
            char buf[32];
            snprintf(buf, sizeof(buf), "选择:%s", FocusZh());
            board->ShowNotification(buf);
        }
    }

    void NotifyMode()
    {
        auto* board = Board::GetInstance().GetDisplay();
        if (board != nullptr) {
            char buf[32];
            snprintf(buf, sizeof(buf), "模式:%s", ModeZh());
            board->ShowNotification(buf);
        }
    }

    void NotifyLevel()
    {
        auto* board = Board::GetInstance().GetDisplay();
        if (board != nullptr) {
            char buf[32];
            snprintf(buf, sizeof(buf), "风速:%d档", manual_level_);
            board->ShowNotification(buf);
        }
    }

    void NotifyTimer()
    {
        auto* board = Board::GetInstance().GetDisplay();
        if (board != nullptr) {
            char buf[40];
            if (timer_minutes_ <= 0) {
                snprintf(buf, sizeof(buf), "定时:关闭");
            } else if (timer_minutes_ < 60) {
                snprintf(buf, sizeof(buf), "定时:%d分钟", timer_minutes_);
            } else if (timer_minutes_ % 60 == 0) {
                snprintf(buf, sizeof(buf), "定时:%d小时", timer_minutes_ / 60);
            } else {
                snprintf(buf, sizeof(buf), "定时:%d小时%d分",
                         timer_minutes_ / 60, timer_minutes_ % 60);
            }
            board->ShowNotification(buf);
        }
    }
};

/* SLEEPFAN_FILL_BEGIN */
struct SleepfanReportCtx {
    FanMotor* fan;
    SensorHub* sensors;
    DecisionEngine* decision;
    FanModeController* fan_ui;
};

/* 只读 SensorHub / DecisionEngine / FanMotor 缓存，不做 I2C、不采样 ADC。 */
static void FillSleepfanStatus(SleepfanStatus* out, void* ctx)
{
    auto* src = static_cast<SleepfanReportCtx*>(ctx);
    if (out == nullptr || src == nullptr || src->fan == nullptr ||
        src->sensors == nullptr || src->decision == nullptr) {
        return;
    }
    out->temperature = src->sensors->cached_temp();
    out->humidity = src->sensors->cached_hum();
    out->temp_hum_ok = src->sensors->cached_valid();
    out->presence = (src->sensors->radar_state() != 0);
    out->distance_cm = src->sensors->radar_distance_cm();
    out->radar_online = src->sensors->radar_online();
    out->sleeping = src->sensors->sleeping();
    out->duty = src->fan->percent();
    out->fan_level = SleepfanDutyToLevel(out->duty);
    out->mq135_raw = src->sensors->cached_mq135();
    src->decision->CopyReport(&out->auto_mode, &out->decision_level,
                              out->decision_reason, sizeof(out->decision_reason));
    if (src->fan_ui != nullptr) {
        src->fan_ui->CopyMode(out->fan_mode, sizeof(out->fan_mode));
        out->timer_remain_s = src->fan_ui->TimerRemainS();
    } else {
        out->fan_mode[0] = '\0';
        out->timer_remain_s = 0;
    }
}
/* SLEEPFAN_FILL_END */


/*
 * KY-040 旋转编码器：1ms 轮询 + 标准 16 项正交状态表。
 * 累加到 ±4 才算一格（每格 4 次有效跳变），避免弹跳/双计。
 * 禁止在 GPIO ISR 里调 BumpLevel（会打日志/刷 OLED）。
 * 若方向反了，对调 CLK/DT 两根线即可。
 */
class RotaryEncoder {
public:
    RotaryEncoder(gpio_num_t pin_a, gpio_num_t pin_b) : pin_a_(pin_a), pin_b_(pin_b)
    {
        gpio_config_t cfg = {
            .pin_bit_mask = (1ULL << pin_a) | (1ULL << pin_b),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&cfg));
        prev_ = ReadState();
        ESP_LOGI(TAG, "rotary encoder ready: CLK=GPIO%d DT=GPIO%d (poll 1 tick, 4-step detent)",
                 (int)pin_a, (int)pin_b);
    }

    void Start()
    {
        BaseType_t ok = xTaskCreatePinnedToCore(&RotaryEncoder::TaskEntry, "encoder",
                                                2048, this, 5, &task_, 1);
        if (ok != pdPASS) {
            ESP_LOGE(TAG, "encoder task create FAILED");
            task_ = nullptr;
        }
    }

private:
    /* index = (old<<2)|new，old/new = (A<<1)|B；CW 正、CCW 负 */
    static constexpr int kQuadTable[16] = {
         0, +1, -1,  0,
        -1,  0,  0, +1,
        +1,  0,  0, -1,
         0, -1, +1,  0
    };

    gpio_num_t pin_a_;
    gpio_num_t pin_b_;
    TaskHandle_t task_ = nullptr;
    int prev_ = 0;
    int accum_ = 0;

    static void TaskEntry(void* arg)
    {
        static_cast<RotaryEncoder*>(arg)->TaskLoop();
    }

    int ReadState() const
    {
        return (gpio_get_level(pin_a_) << 1) | gpio_get_level(pin_b_);
    }

    void TaskLoop()
    {
        while (true) {
            int curr = ReadState();
            int idx = (prev_ << 2) | curr;
            int step = kQuadTable[idx & 15];
            if (step != 0) {
                accum_ += step;
                if (accum_ >= 4 || accum_ <= -4) {
                    int dir = (accum_ > 0) ? +1 : -1;
                    accum_ = 0;
                    if (g_fan_ui != nullptr) {
                        g_fan_ui->OnEncoder(dir);
                        ESP_LOGI(TAG, "encoder dir=%+d", dir);
                    }
                }
            }
            prev_ = curr;
            /* CONFIG_FREERTOS_HZ=100 时 pdMS_TO_TICKS(1)==0，空转占满 CPU1，
             * IDLE/LVGL 被饿死，OLED 不刷新且触发 task_wdt。至少让出 1 个 tick。 */
            vTaskDelay(1);
        }
    }
};

class CompactWifiBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t display_i2c_bus_;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    Display* display_ = nullptr;
    Button boot_button_;
    Button touch_button_;        /* GPIO14 TTP223，构造时 active_high=true */
    Button volume_up_button_;    /* GPIO_NUM_NC：40 已作编码器 CLK，勿再占 */
    Button volume_down_button_;  /* GPIO_NUM_NC：39 已作编码器 DT，勿再占 */
    Button timer_button_;        /* GPIO18 编码器 SW/C：短按模式 / 长按定时 */

    void InitializeDisplayI2c() {
        i2c_master_bus_config_t bus_config = {
            .i2c_port = (i2c_port_t)0,
            .sda_io_num = DISPLAY_SDA_PIN,
            .scl_io_num = DISPLAY_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &display_i2c_bus_));
    }

    void InitializeSsd1306Display() {
        // SSD1306 config
        esp_lcd_panel_io_i2c_config_t io_config = {
            .dev_addr = 0x3C,
            .scl_speed_hz = 400 * 1000,
            .control_phase_bytes = 1,
            .dc_bit_offset = 6,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
            .on_color_trans_done = nullptr,
            .user_ctx = nullptr,
            .flags = {
                .dc_low_on_data = 0,
                .disable_control_phase = 0,
            },
        };

        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(display_i2c_bus_, &io_config, &panel_io_));

        ESP_LOGI(TAG, "Install SSD1306 driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.bits_per_pixel = 1;

        esp_lcd_panel_ssd1306_config_t ssd1306_config = {
            .height = static_cast<uint8_t>(DISPLAY_HEIGHT),
        };
        panel_config.vendor_config = &ssd1306_config;

#ifdef SH1106
        ESP_ERROR_CHECK(esp_lcd_new_panel_sh1106(panel_io_, &panel_config, &panel_));
#else
        ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(panel_io_, &panel_config, &panel_));
#endif
        ESP_LOGI(TAG, "SSD1306 driver installed");

        // Reset the display
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        if (esp_lcd_panel_init(panel_) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize display");
            display_ = new NoDisplay();
            return;
        }
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_, false));

        // Set the display to on
        ESP_LOGI(TAG, "Turning display on");
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

        display_ = new OledDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            /* 短按：开/关摇头（语音故障时的备用演示方式） */
            if (g_stepper != nullptr) {
                if (g_stepper->IsRunning()) {
                    g_stepper->Stop();
                } else {
                    g_stepper->Start();
                }
            }
        });
        boot_button_.OnLongPress([this]() {
            /* 长按：切换对话状态（原短按功能） */
            Application::GetInstance().ToggleChatState();
        });
        /* TTP223：单击切换 UI 焦点（模式 → 定时 → 风速） */
        touch_button_.OnClick([this]() {
            if (g_fan_ui != nullptr) {
                g_fan_ui->CycleFocus();
            }
        });

        /* volume_up/down 为 GPIO_NUM_NC 空壳，不再注册回调（40/39 专供编码器） */

        /* 编码器 SW/C（GPIO18）：短按同触控切焦点（硬件按键可能损坏，逻辑预留） */
        timer_button_.OnClick([this]() {
            if (g_fan_ui != nullptr) {
                g_fan_ui->CycleFocus();
            }
        });
    }

    // 物联网初始化：注册设备能力为 MCP 工具，供云端大模型调用
    void InitializeTools() {
        static StepperMotor stepper(STEPPER_IN1_GPIO, STEPPER_IN2_GPIO,
                                    STEPPER_IN3_GPIO, STEPPER_IN4_GPIO);
        g_stepper = &stepper;
        /* 逐相自检已注释：诊断完成，避免每次开机多等 12 秒 */
        // stepper.SelfTest();
        static FanMotor fan(FAN_AIN1_GPIO, FAN_AIN2_GPIO);

        static SensorHub sensors(display_i2c_bus_, RADAR_UART_RX_GPIO,
                                 RADAR_UART_TX_GPIO, MQ135_ADC_CHANNEL);

        
        static DecisionEngine decision(&fan, &sensors);
        decision.Start();

        static FanModeController fan_ui(&fan, &decision);
        g_fan_ui = &fan_ui;
        fan_ui.Start();

        static RotaryEncoder encoder(ENCODER_A_GPIO, ENCODER_B_GPIO);
        encoder.Start();

        auto& mcp_server = McpServer::GetInstance();

        mcp_server.AddTool("self.decision.set_auto_mode",
            "开启或关闭自动模式。开启后风扇会根据人体存在、温湿度和睡眠状态自动调节档位。",
            PropertyList({Property("enable", kPropertyTypeBoolean)}),
            [](const PropertyList& properties) -> ReturnValue {
                bool on = properties["enable"].value<bool>();
                if (g_fan_ui != nullptr) {
                    if (on) {
                        g_fan_ui->EnterSmart();
                    } else {
                        g_fan_ui->EnterComfortKeepDuty();
                    }
                } else {
                    decision.SetAutoMode(on);
                }
                return std::string(on ? "{\"auto\":true}" : "{\"auto\":false}");
            });

        mcp_server.AddTool("self.decision.get_status",
            "查询自动决策状态：是否开启自动模式、当前建议档位、决策依据。",
            PropertyList(),
            [](const PropertyList&) -> ReturnValue {
                char buf[320];
                const char* mode = (g_fan_ui != nullptr) ? g_fan_ui->ModeId() : "unknown";
                int remain = (g_fan_ui != nullptr) ? g_fan_ui->TimerRemainS() : 0;
                snprintf(buf, sizeof(buf),
                         "{\"auto\":%s,\"level\":%d,\"reason\":\"%s\","
                         "\"fan_mode\":\"%s\",\"timer_remain_s\":%d}",
                         decision.auto_mode() ? "true" : "false",
                         decision.last_level(),
                         decision.last_reason(),
                         mode, remain);
                return std::string(buf);
            });

        mcp_server.AddTool("self.sleep.get_state",
            "判断用户当前是否已入睡。依据是雷达活动量在最近 10 分钟内的时间序列统计。",
            PropertyList(),
            [](const PropertyList&) -> ReturnValue {
                char buf[256];
                snprintf(buf, sizeof(buf),
                         "{\"sleeping\":%s,\"active_seconds_10min\":%d,"
                         "\"sampled_seconds\":%d,\"window_seconds\":%d}",
                         sensors.sleeping() ? "true" : "false",
                         sensors.active_seconds_10min(),
                         sensors.sampled_seconds(),
                         600);
                return std::string(buf);
            });

        mcp_server.AddTool("self.sensor.get_status",
            "获取当前环境与人体状态：室内温度、湿度、是否检测到人、目标距离、空气质量原始值。",
            PropertyList(),
            [](const PropertyList&) -> ReturnValue {
                /* 只读 DecisionEngine 维护的缓存，避免在 MCP 回调里做阻塞 I2C 事务 */
                float t = sensors.cached_temp();
                float h = sensors.cached_hum();
                bool th_ok = sensors.cached_valid();
                int st = sensors.radar_state();
                int raw = sensors.mq135_raw();
                char buf[256];
                snprintf(buf, sizeof(buf),
                         "{\"temperature\":%.1f,\"humidity\":%.1f,\"temp_hum_ok\":%s,"
                         "\"presence\":\"%s\",\"radar_online\":%s,\"distance_cm\":%d,"
                         "\"mq135_raw\":%d}",
                         t, h, th_ok ? "true" : "false",
                         (st == 0) ? "absent" : "present",
                         sensors.radar_online() ? "true" : "false",
                         sensors.radar_distance_cm(), raw);
                return std::string(buf);
            });

        mcp_server.AddTool("self.fan.set_oscillate",
            "控制风扇摇头（左右摆头）。enable=true 开始摇头，enable=false 停止摇头。"
            "开始前风扇头应处于中间位置。",
            PropertyList({Property("enable", kPropertyTypeBoolean)}),
            [](const PropertyList& properties) -> ReturnValue {
                bool enable = properties["enable"].value<bool>();
                if (enable) {
                    stepper.Start();
                } else {
                    stepper.Stop();
                }
                return std::string(enable ? "{\"oscillating\": true}" : "{\"oscillating\": false}");
            });

        mcp_server.AddTool("self.fan.calibrate_oscillate",
            "重新校准风扇摇头机构的中心位置。调用后下次开始摇头时会先转到一侧限位重新定位。",
            PropertyList(),
            [](const PropertyList&) -> ReturnValue {
                stepper.RequestCalibrate();
                return std::string("{\"calibrate\": true}");
            });

        mcp_server.AddTool("self.fan.get_oscillate_state",
            "查询风扇当前是否正在摇头。",
            PropertyList(),
            [](const PropertyList&) -> ReturnValue {
                return std::string(stepper.IsRunning() ? "{\"oscillating\": true}"
                                                       : "{\"oscillating\": false}");
            });

        mcp_server.AddTool("self.fan.set_speed",
            "设置风扇风量档位：1=睡眠静音档，2=低档，3=中档，4=高档，5=最大风。",
            PropertyList({Property("level", kPropertyTypeInteger, 1, 5)}),
            [](const PropertyList& properties) -> ReturnValue {
                int level = properties["level"].value<int>();
                static const int kDutyByLevel[6] = {0, 30, 45, 60, 80, 100};
                if (g_fan_ui != nullptr) {
                    g_fan_ui->SetComfortLevel(level);
                } else {
                    fan.SetPercent(kDutyByLevel[level]);
                }
                ESP_LOGI(TAG, "fan level %d -> duty %d%%", level, kDutyByLevel[level]);
                return std::string("{\"level\": ") + std::to_string(level) + ", \"duty\": " +
                       std::to_string(kDutyByLevel[level]) + ", \"fan_mode\": \"comfort\"}";
            });

        mcp_server.AddTool("self.fan.set_duty",
            "直接设置风扇转速百分比（0-100），0 表示停转。",
            PropertyList({Property("percent", kPropertyTypeInteger, 0, 100)}),
            [](const PropertyList& properties) -> ReturnValue {
                int percent = properties["percent"].value<int>();
                if (g_fan_ui != nullptr) {
                    g_fan_ui->SetComfortDuty(percent);
                } else {
                    fan.SetPercent(percent);
                }
                ESP_LOGI(TAG, "fan duty -> %d%%", percent);
                return std::string("{\"duty\": ") + std::to_string(percent) +
                       ", \"fan_mode\": \"comfort\"}";
            });

        mcp_server.AddTool("self.fan.stop",
            "关闭风扇（转速归零）。",
            PropertyList(),
            [](const PropertyList&) -> ReturnValue {
                if (g_fan_ui != nullptr) {
                    g_fan_ui->SetComfortDuty(0);
                } else {
                    fan.SetPercent(0);
                }
                ESP_LOGI(TAG, "fan stopped");
                return std::string("{\"duty\": 0, \"fan_mode\": \"comfort\"}");
            });

        mcp_server.AddTool("self.fan.set_mode",
            "设置风扇运行模式：comfort=舒适恒定档，natural=自然风起伏，"
            "sleep=睡觉渐降，smart=智能（雷达+温湿度自动）。",
            PropertyList({Property("mode", kPropertyTypeString)}),
            [](const PropertyList& properties) -> ReturnValue {
                auto mode = properties["mode"].value<std::string>();
                if (g_fan_ui == nullptr || !g_fan_ui->SetModeByName(mode.c_str())) {
                    return std::string("{\"ok\":false,\"error\":\"bad mode\"}");
                }
                char buf[128];
                snprintf(buf, sizeof(buf), "{\"ok\":true,\"fan_mode\":\"%s\"}",
                         g_fan_ui->ModeId());
                return std::string(buf);
            });

        mcp_server.AddTool("self.fan.get_state",
            "查询风扇当前转速、档位、模式、定时剩余和摇头状态。",
            PropertyList(),
            [](const PropertyList&) -> ReturnValue {
                char buf[256];
                const char* mode = (g_fan_ui != nullptr) ? g_fan_ui->ModeId() : "unknown";
                int level = (g_fan_ui != nullptr) ? g_fan_ui->manual_level()
                                                  : SleepfanDutyToLevel(fan.percent());
                int remain = (g_fan_ui != nullptr) ? g_fan_ui->TimerRemainS() : 0;
                snprintf(buf, sizeof(buf),
                         "{\"duty\":%d,\"level\":%d,\"oscillating\":%s,"
                         "\"fan_mode\":\"%s\",\"timer_remain_s\":%d}",
                         fan.percent(), level,
                         stepper.IsRunning() ? "true" : "false",
                         mode, remain);
                return std::string(buf);
            });

        /* 只创建上报任务。客户端在任务栈上、WiFi 有 IP 之后才初始化。 */
        static SleepfanReportCtx report_ctx{&fan, &sensors, &decision, &fan_ui};
        static MqttReporter mqtt_reporter(FillSleepfanStatus, &report_ctx);
        mqtt_reporter.Start();
    }

public:
    CompactWifiBoard() :
        boot_button_(BOOT_BUTTON_GPIO),
        /* TTP223 触摸模块 OUT 通常高有效，第二参 active_high=true */
        touch_button_(TOUCH_BUTTON_GPIO, true),
        volume_up_button_(GPIO_NUM_NC),
        volume_down_button_(GPIO_NUM_NC),
        timer_button_(ENCODER_SW_GPIO) {
        InitializeDisplayI2c();
        InitializeSsd1306Display();
        InitializeButtons();
        /* InitializeTools 末尾 Start() 上报任务，不在这条 app_main 调用栈上连接 broker。 */
        InitializeTools();
    }

    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
#ifdef AUDIO_I2S_METHOD_SIMPLEX
        static NoAudioCodecSimplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT, AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
#else
        static NoAudioCodecDuplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN);
#endif
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
};

DECLARE_BOARD(CompactWifiBoard);
