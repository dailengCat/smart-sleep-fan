#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

// 如果使用 Duplex I2S 模式，请注释下面一行
#define AUDIO_I2S_METHOD_SIMPLEX

#ifdef AUDIO_I2S_METHOD_SIMPLEX

#define AUDIO_I2S_MIC_GPIO_WS   GPIO_NUM_4
#define AUDIO_I2S_MIC_GPIO_SCK  GPIO_NUM_5
#define AUDIO_I2S_MIC_GPIO_DIN  GPIO_NUM_6
#define AUDIO_I2S_SPK_GPIO_DOUT GPIO_NUM_7
#define AUDIO_I2S_SPK_GPIO_BCLK GPIO_NUM_15
#define AUDIO_I2S_SPK_GPIO_LRCK GPIO_NUM_16

#else

#define AUDIO_I2S_GPIO_WS GPIO_NUM_4
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_5
#define AUDIO_I2S_GPIO_DIN  GPIO_NUM_6
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_7

#endif


#define BUILTIN_LED_GPIO        GPIO_NUM_NC  /* 原 48 已让给雷达 TX */
#define BOOT_BUTTON_GPIO        GPIO_NUM_0
#define TOUCH_BUTTON_GPIO       GPIO_NUM_14   /* TTP223 电容触摸：按住说话；模块 OUT 通常高有效 */
/* KY-040 旋转编码器（CLK/DT/SW）。SW 低有效接 GND，内部上拉。
 * 原 VOLUME_UP/DOWN 脚位改作 CLK/DT；勿再对 40/39 创建 Button。 */
#define ENCODER_A_GPIO          GPIO_NUM_40  /* CLK */
#define ENCODER_B_GPIO          GPIO_NUM_39  /* DT */
#define ENCODER_SW_GPIO         GPIO_NUM_18  /* SW/C：短按切模式 / 长按切定时 */
#define FAN_TIMER_BUTTON_GPIO   ENCODER_SW_GPIO  /* 兼容旧名：编码器按键 */
#define VOLUME_UP_BUTTON_GPIO   GPIO_NUM_NC  /* 已让给编码器 CLK，Button 置空 */
#define VOLUME_DOWN_BUTTON_GPIO GPIO_NUM_NC  /* 已让给编码器 DT，Button 置空 */

#define DISPLAY_SDA_PIN GPIO_NUM_10   /* OLED + SHT30 共用（与 11 同排） */
#define DISPLAY_SCL_PIN GPIO_NUM_11
#define DISPLAY_WIDTH   128

#if CONFIG_OLED_SSD1306_128X32
#define DISPLAY_HEIGHT  32
#elif CONFIG_OLED_SSD1306_128X64
#define DISPLAY_HEIGHT  64
#elif CONFIG_OLED_SH1106_128X64
#define DISPLAY_HEIGHT  64
#define SH1106
#else
#error "OLED display type is not selected"
#endif

#define DISPLAY_MIRROR_X true
#define DISPLAY_MIRROR_Y true


// =======================================================
// 睡眠风扇毕设扩展：步进电机（摇头）引脚
//   风扇摇头用的是 28BYJ-48 型 5 线步进电机，经 ULN2003 驱动
//   红线 = 公共端接 ULN2003 的 +VCC，橙黄粉蓝 = A B C D 四相
//   4096 步 = 1 圈（4 相 8 拍半步模式），64 步 ≈ 5.6°
// =======================================================
#define STEPPER_IN1_GPIO GPIO_NUM_1    /* 面包板一侧：1,2,21,47（38 常接 RGB，已避开） */
#define STEPPER_IN2_GPIO GPIO_NUM_2
#define STEPPER_IN3_GPIO GPIO_NUM_21
#define STEPPER_IN4_GPIO GPIO_NUM_47

// 步进动作参数
#define STEPPER_STEP_DELAY_US 3000  /* 每拍 3ms（333 步/秒），留出调度余量避免失步 */
#define STEPPER_HALF_STEPS    448   /* 中心到一侧 448 步 ≈ 39.4°，总行程 79° */
#define STEPPER_DWELL_MS      2500  /* 摆到两端各停 2.5 秒 */
#define STEPPER_HOME_STEPS    920   /* 归零步数：略大于总行程(896) */
#define STEPPER_HOME_DELAY_US 4000  /* 归零时慢速走 4ms/步，减小撞限位冲击 */

// 直流风扇电机（DRV8833 + LEDC PWM 调速）
//   AIN1 做 PWM 调速，AIN2 固定低电平实现单向驱动
#define FAN_AIN1_GPIO GPIO_NUM_12  /* L298N ENB PWM；原 21 改到这边接线方便 */
#define FAN_AIN2_GPIO GPIO_NUM_NC   /* L298N 的 IN2 直接接 GND，不再需要 GPIO */

// 雷达（LD2410/LD2420 兼容）串口
#define RADAR_UART_RX_GPIO GPIO_NUM_45   /* 雷达 TX → ESP RX */
#define RADAR_UART_TX_GPIO GPIO_NUM_48   /* 雷达 RX ← ESP TX；原 17/18 已腾出 */

// 空气质量 MQ-135：AO 经 10k/20k 分压后接 GPIO9（ADC1_CH8）
#define MQ135_ADC_CHANNEL 8   /* ADC1_CH8 = GPIO9 */

/*
 * 睡眠风扇状态上报。改 broker / 主题 / 周期只动下面这些宏。
 * 任务栈不要小于 8192。客户端在 mqtt_report 任务里创建；
 * 板子构造函数跑在 app_main 栈上，放在那里会栈溢出并无限重启。
 * URI 用 mqtt://（明文 1883）。改成 mqtts:// 时还要把 mqtt_reporter.cc 里
 * cfg.task.stack_size 提到至少 8192。
 */
#define SLEEPFAN_MQTT_BROKER_URI    "mqtt://broker.emqx.io:1883"
#define SLEEPFAN_MQTT_USERNAME      ""
#define SLEEPFAN_MQTT_PASSWORD      ""
#define SLEEPFAN_MQTT_TOPIC         "sleepfan/status"
#define SLEEPFAN_MQTT_QOS           0
#define SLEEPFAN_MQTT_INTERVAL_MS   10000
#define SLEEPFAN_MQTT_BOOT_DELAY_MS 20000
#define SLEEPFAN_MQTT_TASK_STACK    10240

#endif // _BOARD_CONFIG_H_
