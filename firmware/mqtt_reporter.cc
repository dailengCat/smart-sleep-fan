#include "mqtt_reporter.h"

#include "config.h"

#include <mqtt_client.h>

#include <esp_err.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <esp_wifi.h>

#include <cstring>

static const char* TAG = "MqttReporter";

static_assert(SLEEPFAN_MQTT_TASK_STACK >= 8192,
              "MQTT task stack must be >= 8192 so esp_mqtt_client_init does not overflow");
static_assert(SLEEPFAN_MQTT_QOS >= 0 && SLEEPFAN_MQTT_QOS <= 2, "MQTT QoS must be 0, 1, or 2");
static_assert(SLEEPFAN_MQTT_INTERVAL_MS >= 1000, "publish interval must be at least 1s");

namespace {

bool CopyIpIfReady(esp_netif_t* netif, esp_netif_ip_info_t* out)
{
    if (netif == nullptr) {
        return false;
    }
    esp_netif_ip_info_t ip = {};
    if (esp_netif_get_ip_info(netif, &ip) != ESP_OK || ip.ip.addr == 0) {
        return false;
    }
    if (out != nullptr) {
        *out = ip;
    }
    return true;
}

bool ReadStaIp(esp_netif_ip_info_t* out)
{
    /* 先确认 STA 已经连上 AP，再要一个非 0 的 IPv4。默认网卡键是 WIFI_STA_DEF。 */
    wifi_ap_record_t ap = {};
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return false;
    }
    /* IDF 6.x 已移除 esp_netif_next，STA 场景用默认 ifkey 即可 */
    return CopyIpIfReady(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), out);
}

}  // namespace

MqttReporter::MqttReporter(FillFn fill, void* ctx) : fill_(fill), ctx_(ctx) {}

void MqttReporter::Start()
{
    if (task_ != nullptr) {
        ESP_LOGW(TAG, "mqtt task already running");
        return;
    }

    /*
     * 这里只创建任务。esp_mqtt_client_init 在 TaskLoop 里，
     * 用的是本任务的栈（SLEEPFAN_MQTT_TASK_STACK），不是 app_main。
     */
    BaseType_t ok = xTaskCreatePinnedToCore(&MqttReporter::TaskEntry,
                                            "mqtt_report",
                                            SLEEPFAN_MQTT_TASK_STACK,
                                            this,
                                            3,
                                            &task_,
                                            1);
    if (ok != pdPASS) {
        task_ = nullptr;
        ESP_LOGE(TAG, "[1/4] mqtt task create FAILED. MQTT stays uninitialized.");
        return;
    }
    ESP_LOGI(TAG, "[1/4] task created stack=%d priority=3 core=1", SLEEPFAN_MQTT_TASK_STACK);
}

void MqttReporter::TaskEntry(void* arg)
{
    static_cast<MqttReporter*>(arg)->TaskLoop();
}

extern "C" void sleepfan_mqtt_event(void* handler_args, const char* base, int32_t event_id, void* event_data)
{
    (void)base;
    auto* self = static_cast<MqttReporter*>(handler_args);
    if (self == nullptr) {
        return;
    }
    auto* event = static_cast<esp_mqtt_event_handle_t>(event_data);

    switch (static_cast<esp_mqtt_event_id_t>(event_id)) {
    case MQTT_EVENT_BEFORE_CONNECT:
        ESP_LOGI(TAG, "MQTT connecting client_id=%s broker=%s",
                 self->client_id_, SLEEPFAN_MQTT_BROKER_URI);
        break;
    case MQTT_EVENT_CONNECTED:
        self->connected_ = true;
        ESP_LOGI(TAG, "[4/4] MQTT connected broker=%s client_id=%s",
                 SLEEPFAN_MQTT_BROKER_URI, self->client_id_);
        break;
    case MQTT_EVENT_DISCONNECTED:
        self->connected_ = false;
        ESP_LOGW(TAG, "MQTT disconnected broker=%s (auto reconnect stays on)",
                 SLEEPFAN_MQTT_BROKER_URI);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT publish acked msg_id=%d", event != nullptr ? event->msg_id : -1);
        break;
    case MQTT_EVENT_ERROR:
        self->connected_ = false;
        if (event != nullptr && event->error_handle != nullptr) {
            ESP_LOGE(TAG, "MQTT error type=%d tls_err=%d sock_errno=%d",
                     static_cast<int>(event->error_handle->error_type),
                     event->error_handle->esp_tls_last_esp_err,
                     static_cast<int>(event->error_handle->esp_transport_sock_errno));
        } else {
            ESP_LOGE(TAG, "MQTT error event");
        }
        break;
    default:
        break;
    }
}

bool MqttReporter::InitClient()
{
    if (client_ != nullptr) {
        return true;
    }
    if (client_id_[0] == '\0') {
        snprintf(client_id_, sizeof(client_id_), "sleepfan-%08lx",
                 static_cast<unsigned long>(esp_random()));
    }

    /*
     * 配置体放静态区，避免和 init 内部调用叠在任务栈上。
     * 明文 mqtt:// 不走 TLS，组件内部任务 6144 够用。
     * 若把 URI 改成 mqtts://，把 cfg.task.stack_size 提到至少 8192。
     */
    static esp_mqtt_client_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.broker.address.uri = SLEEPFAN_MQTT_BROKER_URI;
    cfg.credentials.client_id = client_id_;
    if (SLEEPFAN_MQTT_USERNAME[0] != '\0') {
        cfg.credentials.username = SLEEPFAN_MQTT_USERNAME;
        cfg.credentials.authentication.password = SLEEPFAN_MQTT_PASSWORD;
    }
    cfg.session.keepalive = 60;
    cfg.session.protocol_ver = MQTT_PROTOCOL_V_3_1_1;
    cfg.buffer.size = 1024;
    cfg.network.disable_auto_reconnect = false;
    cfg.network.reconnect_timeout_ms = 10000;
    cfg.network.timeout_ms = 10000;
    cfg.task.stack_size = 6144;
    cfg.task.priority = 5;

    esp_netif_ip_info_t ip = {};
    ReadStaIp(&ip);
    ESP_LOGI(TAG, "[3/4] WiFi ready " IPSTR ", esp_mqtt_client_init client_id=%s uri=%s",
             IP2STR(&ip.ip), client_id_, SLEEPFAN_MQTT_BROKER_URI);

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&cfg);
    if (client == nullptr) {
        ESP_LOGE(TAG, "esp_mqtt_client_init returned null");
        return false;
    }

    esp_err_t err = esp_mqtt_client_register_event(client, MQTT_EVENT_ANY,
                                                   &sleepfan_mqtt_event, this);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_mqtt_client_register_event failed: %s", esp_err_to_name(err));
        esp_mqtt_client_destroy(client);
        return false;
    }

    err = esp_mqtt_client_start(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_mqtt_client_start failed: %s", esp_err_to_name(err));
        esp_mqtt_client_destroy(client);
        return false;
    }

    client_ = client;
    ESP_LOGI(TAG, "esp_mqtt_client_start ok, stack watermark=%u",
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    return true;
}

void MqttReporter::PublishOnce()
{
    if (client_ == nullptr) {
        ESP_LOGW(TAG, "publish skipped, client not started");
        return;
    }
    if (!connected_) {
        ESP_LOGW(TAG, "publish skipped, MQTT not connected");
        return;
    }
    if (!ReadStaIp(nullptr)) {
        ESP_LOGW(TAG, "publish skipped, WiFi STA has no IP");
        return;
    }
    if (fill_ == nullptr) {
        ESP_LOGE(TAG, "publish skipped, status callback missing");
        return;
    }

    SleepfanStatus status = {};
    fill_(&status, ctx_);
    snprintf(status.client_id, sizeof(status.client_id), "%s", client_id_);
    status.uptime_ms = esp_timer_get_time() / 1000;

    char payload[768];
    int n = SleepfanStatusToJson(&status, payload, sizeof(payload));
    if (n < 0) {
        ESP_LOGE(TAG, "status JSON did not fit in %u bytes", static_cast<unsigned>(sizeof(payload)));
        return;
    }

    auto client = static_cast<esp_mqtt_client_handle_t>(client_);
    int msg_id = esp_mqtt_client_publish(client, SLEEPFAN_MQTT_TOPIC, payload, n, SLEEPFAN_MQTT_QOS, 0);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "publish failed topic=%s payload=%s", SLEEPFAN_MQTT_TOPIC, payload);
        return;
    }
    /* QoS 0 成功时 msg_id 为 0，小于 0 才是失败。 */
    ESP_LOGI(TAG, "publish ok msg_id=%d topic=%s payload=%s", msg_id, SLEEPFAN_MQTT_TOPIC, payload);
}

void MqttReporter::TaskLoop()
{
    ESP_LOGI(TAG, "[2/4] task running, wait %d ms, then WiFi STA IP, before esp_mqtt_client_init",
             SLEEPFAN_MQTT_BOOT_DELAY_MS);
    vTaskDelay(pdMS_TO_TICKS(SLEEPFAN_MQTT_BOOT_DELAY_MS));

    while (!ReadStaIp(nullptr)) {
        ESP_LOGI(TAG, "WiFi STA has no IP yet; esp_mqtt_client_init still pending");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }

    while (!InitClient()) {
        ESP_LOGE(TAG, "MQTT client init failed, retry in 5s");
        vTaskDelay(pdMS_TO_TICKS(5000));
        while (!ReadStaIp(nullptr)) {
            ESP_LOGW(TAG, "WiFi dropped while starting MQTT, still waiting for STA IP");
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }

    while (true) {
        PublishOnce();
        vTaskDelay(pdMS_TO_TICKS(SLEEPFAN_MQTT_INTERVAL_MS));
    }
}
