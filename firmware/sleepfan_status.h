#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

/*
 * 睡眠风扇上报快照。MQTT 任务只填这份结构再序列化，不访问 I2C。
 * decision_reason / client_id 由调用方保证以 NUL 结尾。
 */
struct SleepfanStatus {
    char client_id[32];
    float temperature;
    float humidity;
    bool temp_hum_ok;
    bool presence;
    int distance_cm;
    bool radar_online;
    bool sleeping;
    int fan_level;
    int duty;
    bool auto_mode;
    int decision_level;
    char decision_reason[48];
    char fan_mode[16];      /* comfort/natural/sleep/smart */
    int timer_remain_s;     /* 定时剩余秒，0=未开启 */
    int mq135_raw;
    int64_t uptime_ms;
};

/* 与 DecisionEngine::Evaluate、self.fan.set_speed 的占空比表保持一致。
 * 0/30/45/60/80/100 → 0..5 档；其它占空比返回 -1（自定义转速，看 duty）。 */
inline int SleepfanDutyToLevel(int duty)
{
    static const int kDutyByLevel[6] = {0, 30, 45, 60, 80, 100};
    for (int level = 0; level < 6; ++level) {
        if (kDutyByLevel[level] == duty) {
            return level;
        }
    }
    return -1;
}

inline void SleepfanJsonEscape(const char* in, char* out, size_t out_len)
{
    if (out == nullptr || out_len == 0) {
        return;
    }
    if (in == nullptr) {
        out[0] = '\0';
        return;
    }
    size_t j = 0;
    for (size_t i = 0; in[i] != '\0'; ++i) {
        unsigned char c = static_cast<unsigned char>(in[i]);
        if (c == '"' || c == '\\') {
            if (j + 2 >= out_len) {
                break;
            }
            out[j++] = '\\';
            out[j++] = static_cast<char>(c);
            continue;
        }
        if (c < 0x20) {
            continue;
        }
        if (j + 1 >= out_len) {
            break;
        }
        out[j++] = static_cast<char>(c);
    }
    out[j] = '\0';
}

/* 成功返回写入字符数（不含 NUL）。缓冲区不够或参数为空返回 -1。 */
inline int SleepfanStatusToJson(const SleepfanStatus* status, char* out, size_t out_len)
{
    if (status == nullptr || out == nullptr || out_len == 0) {
        return -1;
    }

    char reason[96];
    char client_id[64];
    char fan_mode[32];
    SleepfanJsonEscape(status->decision_reason, reason, sizeof(reason));
    SleepfanJsonEscape(status->client_id, client_id, sizeof(client_id));
    SleepfanJsonEscape(status->fan_mode, fan_mode, sizeof(fan_mode));
    const char* sleep_state = status->sleeping ? "asleep" : "awake";

    int n = snprintf(
        out,
        out_len,
        "{\"client_id\":\"%s\",\"temperature\":%.1f,\"humidity\":%.1f,\"temp_hum_ok\":%s,"
        "\"presence\":%s,\"distance_cm\":%d,\"radar_online\":%s,"
        "\"sleeping\":%s,\"sleep_state\":\"%s\","
        "\"fan_level\":%d,\"duty\":%d,"
        "\"auto_mode\":%s,\"decision_level\":%d,\"decision_reason\":\"%s\","
        "\"fan_mode\":\"%s\",\"timer_remain_s\":%d,"
        "\"mq135_raw\":%d,\"uptime_ms\":%lld}",
        client_id,
        static_cast<double>(status->temperature),
        static_cast<double>(status->humidity),
        status->temp_hum_ok ? "true" : "false",
        status->presence ? "true" : "false",
        status->distance_cm,
        status->radar_online ? "true" : "false",
        status->sleeping ? "true" : "false",
        sleep_state,
        status->fan_level,
        status->duty,
        status->auto_mode ? "true" : "false",
        status->decision_level,
        reason,
        fan_mode,
        status->timer_remain_s,
        status->mq135_raw,
        static_cast<long long>(status->uptime_ms));

    if (n < 0 || static_cast<size_t>(n) >= out_len) {
        out[out_len - 1] = '\0';
        return -1;
    }
    return n;
}
