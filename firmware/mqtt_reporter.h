#pragma once

#include "sleepfan_status.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

/*
 * 睡眠风扇 MQTT 状态上报。
 *
 * Start() 只创建 FreeRTOS 任务，可以在板子构造函数里调用。
 * 客户端的 init / start 只在该任务里执行（mqtt_reporter.cc 的 TaskLoop）。
 * 板对象是在 app_main 栈上构造的，不要把客户端创建挪回构造函数，
 * 否则 app_main 栈会溢出并无限重启。
 */
extern "C" void sleepfan_mqtt_event(void* handler_args, const char* base, int32_t event_id, void* event_data);

class MqttReporter {
public:
    using FillFn = void (*)(SleepfanStatus* out, void* ctx);

    explicit MqttReporter(FillFn fill, void* ctx);

    void Start();

private:
    friend void sleepfan_mqtt_event(void* handler_args, const char* base, int32_t event_id, void* event_data);

    static void TaskEntry(void* arg);

    void TaskLoop();
    bool InitClient();
    void PublishOnce();

    FillFn fill_;
    void* ctx_;
    TaskHandle_t task_ = nullptr;
    void* client_ = nullptr;
    char client_id_[32] = {};
    volatile bool connected_ = false;
};
