#include "argus_mqtt_broker.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

static const char *TAG = "argus_mqtt_broker";

#define ARGUS_MQTT_MAX_CLIENTS 10
#define ARGUS_MQTT_MAX_SUBS_PER_CLIENT 20
#define ARGUS_MQTT_MAX_RETAINED 16
#define ARGUS_MQTT_MAX_TOPIC_LEN 128
#define ARGUS_MQTT_MAX_PAYLOAD_LEN 128
#define ARGUS_MQTT_MAX_PACKET_LEN 512
#define ARGUS_MQTT_CLIENT_TASK_STACK 4096
#define ARGUS_MQTT_SERVER_TASK_STACK 4096

typedef struct {
    bool in_use;
    int sock;
    char client_id[32];
    char subscriptions[ARGUS_MQTT_MAX_SUBS_PER_CLIENT][ARGUS_MQTT_MAX_TOPIC_LEN];
    size_t subscription_count;
} argus_mqtt_client_t;

typedef struct {
    bool in_use;
    char topic[ARGUS_MQTT_MAX_TOPIC_LEN];
    char payload[ARGUS_MQTT_MAX_PAYLOAD_LEN];
} argus_mqtt_retained_t;

typedef struct {
    uint16_t port;
    int listen_sock;
    SemaphoreHandle_t lock;
    bool started;
    argus_mqtt_broker_message_cb_t on_message;
    void *user_ctx;
    argus_mqtt_client_t clients[ARGUS_MQTT_MAX_CLIENTS];
    argus_mqtt_retained_t retained[ARGUS_MQTT_MAX_RETAINED];
} argus_mqtt_broker_t;

static argus_mqtt_broker_t s_broker;

static int argus_mqtt_read_exact(int sock, uint8_t *buffer, size_t len)
{
    size_t offset = 0;
    while (offset < len) {
        int got = recv(sock, buffer + offset, len - offset, 0);
        if (got <= 0) {
            return got;
        }
        offset += (size_t)got;
    }
    return (int)offset;
}

static int argus_mqtt_decode_remaining_length(int sock, uint32_t *remaining_len)
{
    uint32_t multiplier = 1;
    uint32_t value = 0;

    for (int i = 0; i < 4; ++i) {
        uint8_t encoded = 0;
        int got = argus_mqtt_read_exact(sock, &encoded, 1);
        if (got <= 0) {
            return got;
        }

        value += (uint32_t)(encoded & 127U) * multiplier;
        if ((encoded & 128U) == 0U) {
            *remaining_len = value;
            return 1;
        }
        multiplier *= 128U;
    }

    return -1;
}

static size_t argus_mqtt_encode_remaining_length(uint8_t *out, uint32_t len)
{
    size_t count = 0;
    do {
        uint8_t encoded = (uint8_t)(len % 128U);
        len /= 128U;
        if (len > 0U) {
            encoded |= 128U;
        }
        out[count++] = encoded;
    } while (len > 0U && count < 4U);
    return count;
}

static bool argus_mqtt_read_u16(const uint8_t *packet, uint32_t len, uint32_t *offset, uint16_t *value)
{
    if (*offset + 2U > len) {
        return false;
    }
    *value = (uint16_t)(((uint16_t)packet[*offset] << 8) | packet[*offset + 1U]);
    *offset += 2U;
    return true;
}

static bool argus_mqtt_read_string(const uint8_t *packet, uint32_t len, uint32_t *offset, char *out, size_t out_len)
{
    uint16_t string_len = 0;
    if (!argus_mqtt_read_u16(packet, len, offset, &string_len)) {
        return false;
    }
    if (*offset + string_len > len || out_len == 0U) {
        return false;
    }

    size_t copy_len = string_len;
    if (copy_len >= out_len) {
        copy_len = out_len - 1U;
    }

    memcpy(out, packet + *offset, copy_len);
    out[copy_len] = '\0';
    *offset += string_len;
    return true;
}

static bool argus_mqtt_topic_matches(const char *filter, const char *topic)
{
    while (*filter != '\0' && *topic != '\0') {
        if (*filter == '#') {
            return filter[1] == '\0';
        }

        if (*filter == '+') {
            while (*topic != '\0' && *topic != '/') {
                ++topic;
            }
            ++filter;
            continue;
        }

        if (*filter != *topic) {
            return false;
        }

        ++filter;
        ++topic;
    }

    if (*filter == '#' && filter[1] == '\0') {
        return true;
    }

    return *filter == '\0' && *topic == '\0';
}

static esp_err_t argus_mqtt_send_publish(int sock, const char *topic, const char *payload, bool retain)
{
    uint8_t packet[ARGUS_MQTT_MAX_PACKET_LEN];
    const size_t topic_len = strnlen(topic, ARGUS_MQTT_MAX_TOPIC_LEN - 1U);
    const size_t payload_len = strnlen(payload, ARGUS_MQTT_MAX_PAYLOAD_LEN - 1U);
    const uint32_t remaining_len = (uint32_t)(2U + topic_len + payload_len);

    if (remaining_len + 5U > sizeof(packet)) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t offset = 0;
    packet[offset++] = retain ? 0x31U : 0x30U;
    offset += argus_mqtt_encode_remaining_length(packet + offset, remaining_len);
    packet[offset++] = (uint8_t)(topic_len >> 8);
    packet[offset++] = (uint8_t)(topic_len & 0xFFU);
    memcpy(packet + offset, topic, topic_len);
    offset += topic_len;
    memcpy(packet + offset, payload, payload_len);
    offset += payload_len;

    int sent = send(sock, packet, offset, 0);
    return (sent == (int)offset) ? ESP_OK : ESP_FAIL;
}

static void argus_mqtt_store_retained_locked(const char *topic, const char *payload)
{
    argus_mqtt_retained_t *slot = NULL;

    for (size_t i = 0; i < ARGUS_MQTT_MAX_RETAINED; ++i) {
        if (s_broker.retained[i].in_use && strcmp(s_broker.retained[i].topic, topic) == 0) {
            slot = &s_broker.retained[i];
            break;
        }
    }

    if (slot == NULL) {
        for (size_t i = 0; i < ARGUS_MQTT_MAX_RETAINED; ++i) {
            if (!s_broker.retained[i].in_use) {
                slot = &s_broker.retained[i];
                break;
            }
        }
    }

    if (slot == NULL) {
        slot = &s_broker.retained[0];
    }

    slot->in_use = true;
    strlcpy(slot->topic, topic, sizeof(slot->topic));
    strlcpy(slot->payload, payload, sizeof(slot->payload));
}

static void argus_mqtt_deliver_to_subscribers_locked(const char *topic, const char *payload, bool retain)
{
    for (size_t i = 0; i < ARGUS_MQTT_MAX_CLIENTS; ++i) {
        argus_mqtt_client_t *client = &s_broker.clients[i];
        if (!client->in_use || client->sock < 0) {
            continue;
        }

        bool matched = false;
        for (size_t sub = 0; sub < client->subscription_count; ++sub) {
            if (argus_mqtt_topic_matches(client->subscriptions[sub], topic)) {
                matched = true;
                break;
            }
        }

        if (matched) {
            (void)argus_mqtt_send_publish(client->sock, topic, payload, retain);
        }
    }
}

static void argus_mqtt_send_retained_for_filter_locked(argus_mqtt_client_t *client, const char *filter)
{
    for (size_t i = 0; i < ARGUS_MQTT_MAX_RETAINED; ++i) {
        if (!s_broker.retained[i].in_use) {
            continue;
        }
        if (argus_mqtt_topic_matches(filter, s_broker.retained[i].topic)) {
            (void)argus_mqtt_send_publish(client->sock, s_broker.retained[i].topic, s_broker.retained[i].payload, true);
        }
    }
}

static esp_err_t argus_mqtt_handle_connect(argus_mqtt_client_t *client, const uint8_t *packet, uint32_t len)
{
    uint32_t offset = 0;
    char protocol_name[8] = {0};
    char client_id[sizeof(client->client_id)] = {0};

    if (!argus_mqtt_read_string(packet, len, &offset, protocol_name, sizeof(protocol_name)) ||
        offset + 4U > len) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t protocol_level = packet[offset++];
    uint8_t connect_flags = packet[offset++];
    offset += 2U;

    if (strcmp(protocol_name, "MQTT") != 0 || protocol_level != 4U) {
        const uint8_t connack_bad_protocol[] = {0x20U, 0x02U, 0x00U, 0x01U};
        send(client->sock, connack_bad_protocol, sizeof(connack_bad_protocol), 0);
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (!argus_mqtt_read_string(packet, len, &offset, client_id, sizeof(client_id))) {
        strlcpy(client_id, "argus-client", sizeof(client_id));
    }

    if (connect_flags & 0x04U) {
        char ignored[ARGUS_MQTT_MAX_TOPIC_LEN];
        if (!argus_mqtt_read_string(packet, len, &offset, ignored, sizeof(ignored)) ||
            !argus_mqtt_read_string(packet, len, &offset, ignored, sizeof(ignored))) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    if (connect_flags & 0x80U) {
        char ignored[ARGUS_MQTT_MAX_PAYLOAD_LEN];
        if (!argus_mqtt_read_string(packet, len, &offset, ignored, sizeof(ignored))) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    if (connect_flags & 0x40U) {
        char ignored[ARGUS_MQTT_MAX_PAYLOAD_LEN];
        if (!argus_mqtt_read_string(packet, len, &offset, ignored, sizeof(ignored))) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    strlcpy(client->client_id, client_id, sizeof(client->client_id));
    client->subscription_count = 0;

    const uint8_t connack_ok[] = {0x20U, 0x02U, 0x00U, 0x00U};
    if (send(client->sock, connack_ok, sizeof(connack_ok), 0) != sizeof(connack_ok)) {
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "client connected: %s", client->client_id);
    return ESP_OK;
}

static esp_err_t argus_mqtt_handle_subscribe(argus_mqtt_client_t *client, const uint8_t *packet, uint32_t len)
{
    uint32_t offset = 0;
    uint16_t packet_id = 0;
    uint8_t suback[64] = {0};
    size_t suback_len = 0;
    uint8_t granted_count = 0;
    size_t first_new_sub = client->subscription_count;

    if (!argus_mqtt_read_u16(packet, len, &offset, &packet_id)) {
        return ESP_ERR_INVALID_ARG;
    }

    suback[suback_len++] = 0x90U;
    size_t remaining_len_offset = suback_len++;
    suback[suback_len++] = (uint8_t)(packet_id >> 8);
    suback[suback_len++] = (uint8_t)(packet_id & 0xFFU);

    while (offset < len && suback_len < sizeof(suback)) {
        char filter[ARGUS_MQTT_MAX_TOPIC_LEN] = {0};
        if (!argus_mqtt_read_string(packet, len, &offset, filter, sizeof(filter)) ||
            offset >= len) {
            break;
        }

        uint8_t requested_qos = packet[offset++];
        (void)requested_qos;

        if (client->subscription_count < ARGUS_MQTT_MAX_SUBS_PER_CLIENT) {
            strlcpy(client->subscriptions[client->subscription_count++], filter, ARGUS_MQTT_MAX_TOPIC_LEN);
            suback[suback_len++] = 0x00U;
            granted_count++;
            ESP_LOGI(TAG, "client %s subscribed: %s", client->client_id, filter);
        } else {
            suback[suback_len++] = 0x80U;
            granted_count++;
        }
    }

    suback[remaining_len_offset] = (uint8_t)(2U + granted_count);
    if (send(client->sock, suback, suback_len, 0) != (int)suback_len) {
        return ESP_FAIL;
    }

    for (size_t i = first_new_sub; i < client->subscription_count; ++i) {
        argus_mqtt_send_retained_for_filter_locked(client, client->subscriptions[i]);
    }

    return ESP_OK;
}

static esp_err_t argus_mqtt_handle_publish(argus_mqtt_client_t *client,
                                           uint8_t fixed_header,
                                           const uint8_t *packet,
                                           uint32_t len,
                                           char *callback_topic,
                                           size_t callback_topic_len,
                                           char *callback_payload,
                                           size_t callback_payload_len,
                                           bool *has_callback)
{
    uint32_t offset = 0;
    char topic[ARGUS_MQTT_MAX_TOPIC_LEN] = {0};
    char payload[ARGUS_MQTT_MAX_PAYLOAD_LEN] = {0};
    uint16_t packet_id = 0;
    uint8_t qos = (fixed_header >> 1) & 0x03U;
    bool retain = (fixed_header & 0x01U) != 0U;

    if (qos > 1U) {
        ESP_LOGW(TAG, "QoS %u publish is not supported", qos);
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (!argus_mqtt_read_string(packet, len, &offset, topic, sizeof(topic))) {
        return ESP_ERR_INVALID_ARG;
    }

    if (qos > 0U && !argus_mqtt_read_u16(packet, len, &offset, &packet_id)) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t payload_len = len - offset;
    if (payload_len >= sizeof(payload)) {
        payload_len = sizeof(payload) - 1U;
    }
    memcpy(payload, packet + offset, payload_len);
    payload[payload_len] = '\0';

    if (retain) {
        argus_mqtt_store_retained_locked(topic, payload);
    }
    argus_mqtt_deliver_to_subscribers_locked(topic, payload, retain);

    ESP_LOGI(TAG, "client %s publish: %s=%s", client->client_id, topic, payload);

    if (s_broker.on_message != NULL) {
        strlcpy(callback_topic, topic, callback_topic_len);
        strlcpy(callback_payload, payload, callback_payload_len);
        *has_callback = true;
    }

    if (qos == 1U) {
        uint8_t puback[] = {0x40U, 0x02U, (uint8_t)(packet_id >> 8), (uint8_t)(packet_id & 0xFFU)};
        send(client->sock, puback, sizeof(puback), 0);
    }

    return ESP_OK;
}

static void argus_mqtt_close_client(argus_mqtt_client_t *client)
{
    xSemaphoreTake(s_broker.lock, portMAX_DELAY);
    int sock = client->sock;
    client->sock = -1;
    client->in_use = false;
    client->subscription_count = 0;
    xSemaphoreGive(s_broker.lock);

    if (sock >= 0) {
        shutdown(sock, 0);
        close(sock);
    }
}

static void argus_mqtt_client_task(void *arg)
{
    argus_mqtt_client_t *client = (argus_mqtt_client_t *)arg;
    uint8_t packet[ARGUS_MQTT_MAX_PACKET_LEN];

    while (true) {
        uint8_t fixed_header = 0;
        uint32_t remaining_len = 0;

        int got = argus_mqtt_read_exact(client->sock, &fixed_header, 1);
        if (got <= 0) {
            break;
        }

        got = argus_mqtt_decode_remaining_length(client->sock, &remaining_len);
        if (got <= 0 || remaining_len > sizeof(packet)) {
            break;
        }

        got = argus_mqtt_read_exact(client->sock, packet, remaining_len);
        if (got <= 0) {
            break;
        }

        uint8_t packet_type = fixed_header >> 4;
        bool has_callback = false;
        char callback_topic[ARGUS_MQTT_MAX_TOPIC_LEN] = {0};
        char callback_payload[ARGUS_MQTT_MAX_PAYLOAD_LEN] = {0};

        xSemaphoreTake(s_broker.lock, portMAX_DELAY);

        esp_err_t err = ESP_OK;
        switch (packet_type) {
        case 1:
            err = argus_mqtt_handle_connect(client, packet, remaining_len);
            break;
        case 3:
            err = argus_mqtt_handle_publish(client,
                                            fixed_header,
                                            packet,
                                            remaining_len,
                                            callback_topic,
                                            sizeof(callback_topic),
                                            callback_payload,
                                            sizeof(callback_payload),
                                            &has_callback);
            break;
        case 8:
            err = argus_mqtt_handle_subscribe(client, packet, remaining_len);
            break;
        case 12: {
            const uint8_t pingresp[] = {0xD0U, 0x00U};
            send(client->sock, pingresp, sizeof(pingresp), 0);
            break;
        }
        case 14:
            xSemaphoreGive(s_broker.lock);
            goto done;
        default:
            ESP_LOGW(TAG, "unsupported packet type: %u", packet_type);
            break;
        }

        xSemaphoreGive(s_broker.lock);

        if (has_callback && s_broker.on_message != NULL) {
            s_broker.on_message(callback_topic, callback_payload, s_broker.user_ctx);
        }

        if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGW(TAG, "client packet handling failed: %s", esp_err_to_name(err));
        }
        if (err == ESP_ERR_NOT_SUPPORTED) {
            break;
        }
    }

done:
    ESP_LOGI(TAG, "client disconnected: %s", client->client_id[0] ? client->client_id : "(unknown)");
    argus_mqtt_close_client(client);
    vTaskDelete(NULL);
}

static argus_mqtt_client_t *argus_mqtt_alloc_client_locked(int sock)
{
    for (size_t i = 0; i < ARGUS_MQTT_MAX_CLIENTS; ++i) {
        if (!s_broker.clients[i].in_use) {
            argus_mqtt_client_t *client = &s_broker.clients[i];
            memset(client, 0, sizeof(*client));
            client->in_use = true;
            client->sock = sock;
            return client;
        }
    }
    return NULL;
}

static void argus_mqtt_server_task(void *arg)
{
    (void)arg;

    while (true) {
        struct sockaddr_in source_addr = {0};
        socklen_t addr_len = sizeof(source_addr);
        int sock = accept(s_broker.listen_sock, (struct sockaddr *)&source_addr, &addr_len);

        if (sock < 0) {
            ESP_LOGW(TAG, "accept failed: errno=%d", errno);
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }

        xSemaphoreTake(s_broker.lock, portMAX_DELAY);
        argus_mqtt_client_t *client = argus_mqtt_alloc_client_locked(sock);
        xSemaphoreGive(s_broker.lock);

        if (client == NULL) {
            ESP_LOGW(TAG, "rejecting MQTT client: client limit reached");
            close(sock);
            continue;
        }

        BaseType_t created = xTaskCreate(argus_mqtt_client_task, "mqtt_client", ARGUS_MQTT_CLIENT_TASK_STACK, client, 5, NULL);
        if (created != pdPASS) {
            ESP_LOGE(TAG, "failed to create MQTT client task");
            argus_mqtt_close_client(client);
        }
    }
}

esp_err_t argus_mqtt_broker_start(const argus_mqtt_broker_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "config is null");
    ESP_RETURN_ON_FALSE(!s_broker.started, ESP_ERR_INVALID_STATE, TAG, "broker already started");

    memset(&s_broker, 0, sizeof(s_broker));
    s_broker.port = config->port;
    s_broker.on_message = config->on_message;
    s_broker.user_ctx = config->user_ctx;
    s_broker.listen_sock = -1;
    s_broker.lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_broker.lock != NULL, ESP_ERR_NO_MEM, TAG, "failed to create lock");

    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    ESP_RETURN_ON_FALSE(listen_sock >= 0, ESP_FAIL, TAG, "unable to create socket");

    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in dest_addr = {
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_family = AF_INET,
        .sin_port = htons(s_broker.port),
    };

    if (bind(listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) != 0) {
        close(listen_sock);
        ESP_LOGE(TAG, "bind failed: errno=%d", errno);
        return ESP_FAIL;
    }

    if (listen(listen_sock, ARGUS_MQTT_MAX_CLIENTS) != 0) {
        close(listen_sock);
        ESP_LOGE(TAG, "listen failed: errno=%d", errno);
        return ESP_FAIL;
    }

    s_broker.listen_sock = listen_sock;
    s_broker.started = true;

    BaseType_t created = xTaskCreate(argus_mqtt_server_task, "mqtt_broker", ARGUS_MQTT_SERVER_TASK_STACK, NULL, 5, NULL);
    ESP_RETURN_ON_FALSE(created == pdPASS, ESP_ERR_NO_MEM, TAG, "failed to create broker task");

    ESP_LOGI(TAG, "local MQTT broker listening on port %u", s_broker.port);
    return ESP_OK;
}

esp_err_t argus_mqtt_broker_publish(const char *topic, const char *payload, bool retain)
{
    ESP_RETURN_ON_FALSE(topic != NULL && payload != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid publish args");
    ESP_RETURN_ON_FALSE(s_broker.started, ESP_ERR_INVALID_STATE, TAG, "broker not started");

    xSemaphoreTake(s_broker.lock, portMAX_DELAY);
    if (retain) {
        argus_mqtt_store_retained_locked(topic, payload);
    }
    argus_mqtt_deliver_to_subscribers_locked(topic, payload, retain);
    xSemaphoreGive(s_broker.lock);

    return ESP_OK;
}
