#include "argus_mqtt_broker.h"

#include <errno.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

static const char *TAG = "argus_mqtt_broker";

#define ARGUS_MQTT_MAX_CLIENTS 10
#define ARGUS_MQTT_MAX_SUBS_PER_CLIENT 20
#define ARGUS_MQTT_MAX_RETAINED ARGUS_MQTT_BROKER_RETAINED_CAPACITY
#define ARGUS_MQTT_MAX_TOPIC_LEN ARGUS_MQTT_BROKER_TOPIC_CAP
#define ARGUS_MQTT_MAX_PAYLOAD_LEN ARGUS_MQTT_BROKER_PAYLOAD_CAP
#define ARGUS_MQTT_MAX_PACKET_LEN 1024
#define ARGUS_MQTT_CLIENT_TASK_STACK 8192
#define ARGUS_MQTT_SERVER_TASK_STACK 4096

/* ---------------------------------------------------------------------------
 * Lifecycle event bits  (waited on by start / stop callers)
 * -----------------------------------------------------------------------*/
#define BROKER_EVT_STARTED        (1 << 0)
#define BROKER_EVT_STOPPED        (1 << 1)
#define BROKER_EVT_CLIENTS_EXITED (1 << 2)

/* ---------------------------------------------------------------------------
 * Broker state machine
 * -----------------------------------------------------------------------*/
typedef struct {
    bool in_use;
    bool connected;
    int sock;
    uint64_t connection_id;
    uint32_t peer_key;
    uint8_t receiving_interface;
    char client_id[ARGUS_MQTT_BROKER_CLIENT_ID_CAP];
    argus_machine_principal_t principal;
    char subscriptions[ARGUS_MQTT_MAX_SUBS_PER_CLIENT][ARGUS_MQTT_MAX_TOPIC_LEN];
    size_t subscription_count;
} argus_mqtt_client_t;

typedef struct {
    bool in_use;
    char topic[ARGUS_MQTT_MAX_TOPIC_LEN];
    char payload[ARGUS_MQTT_MAX_PAYLOAD_LEN];
} argus_mqtt_retained_t;

typedef struct {
    /* ---- lifecycle objects (firmware-lifetime, never deleted) ---- */
    SemaphoreHandle_t lifecycle_mutex;
    SemaphoreHandle_t client_lock;
    EventGroupHandle_t lifecycle_event_group;

    /* ---- state ---- */
    argus_broker_state_t state;
    esp_err_t startup_error;

    /* ---- runtime ---- */
    uint16_t port;
    int listen_sock;
    TaskHandle_t server_task_handle;
    _Atomic int32_t active_client_count;
    _Atomic uint64_t next_connection_id;
    argus_mqtt_broker_message_cb_t on_message;
    argus_mqtt_broker_policy_cb_t publish_authorize;
    argus_mqtt_broker_policy_cb_t policy_check;
    argus_mqtt_broker_auth_cb_t authenticate;
    argus_mqtt_broker_revalidate_cb_t revalidate;
    argus_mqtt_broker_subscribe_policy_cb_t subscribe_policy;
    argus_mqtt_broker_client_cb_t on_client_event;
    void *user_ctx;
    argus_mqtt_client_t clients[ARGUS_MQTT_MAX_CLIENTS];
    argus_mqtt_retained_t retained[ARGUS_MQTT_MAX_RETAINED];
} argus_mqtt_broker_t;

static argus_mqtt_broker_t s_broker;

/* ===========================================================================
 * MQTT wire helpers  (unchanged from original)
 * =========================================================================*/

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
    if (*offset + string_len > len || out_len == 0U || string_len >= out_len) {
        return false;
    }

    memcpy(out, packet + *offset, string_len);
    out[string_len] = '\0';
    *offset += string_len;
    return true;
}

static bool argus_mqtt_read_binary(
    const uint8_t *packet, size_t length, size_t *offset,
    uint8_t *out, size_t capacity, size_t *out_length)
{
    if (packet == NULL || offset == NULL || out == NULL ||
        out_length == NULL || *offset + 2U > length) {
        return false;
    }
    size_t field_length =
        ((size_t)packet[*offset] << 8U) | packet[*offset + 1U];
    *offset += 2U;
    if (field_length >= capacity || *offset + field_length > length) {
        return false;
    }
    memcpy(out, packet + *offset, field_length);
    out[field_length] = 0U;
    *offset += field_length;
    *out_length = field_length;
    return true;
}

argus_mqtt_connect_parse_result_t argus_mqtt_broker_parse_connect(
    const uint8_t *packet, size_t length,
    argus_mqtt_connect_request_t *out)
{
    if (packet == NULL || out == NULL || length == 0U ||
        length > ARGUS_MQTT_MAX_PACKET_LEN) {
        return ARGUS_MQTT_CONNECT_PARSE_MALFORMED;
    }
    memset(out, 0, sizeof(*out));
    size_t offset = 0U;
    uint8_t protocol[8] = {0};
    size_t protocol_length = 0U;
    if (!argus_mqtt_read_binary(
            packet, length, &offset, protocol, sizeof(protocol),
            &protocol_length) ||
        protocol_length != 4U || memcmp(protocol, "MQTT", 4U) != 0 ||
        offset + 4U > length || packet[offset++] != 4U) {
        return ARGUS_MQTT_CONNECT_PARSE_PROTOCOL;
    }
    uint8_t flags = packet[offset++];
    out->keep_alive_s =
        (uint16_t)(((uint16_t)packet[offset] << 8U) | packet[offset + 1U]);
    offset += 2U;
    bool username = (flags & 0x80U) != 0U;
    bool password = (flags & 0x40U) != 0U;
    bool will_retain = (flags & 0x20U) != 0U;
    uint8_t will_qos = (flags >> 3U) & 0x03U;
    bool will = (flags & 0x04U) != 0U;
    bool clean_session = (flags & 0x02U) != 0U;
    if ((flags & 0x01U) != 0U || !clean_session || will ||
        will_retain || will_qos != 0U || !username || !password) {
        return ARGUS_MQTT_CONNECT_PARSE_FLAGS;
    }
    uint8_t client_id[ARGUS_MQTT_BROKER_CLIENT_ID_CAP] = {0};
    size_t client_id_length = 0U;
    if (!argus_mqtt_read_binary(
            packet, length, &offset, client_id, sizeof(client_id),
            &client_id_length) ||
        client_id_length == 0U ||
        memchr(client_id, '\0', client_id_length) != NULL) {
        return ARGUS_MQTT_CONNECT_PARSE_CLIENT_ID;
    }
    if (!argus_mqtt_read_binary(
            packet, length, &offset, out->username,
            sizeof(out->username), &out->username_len) ||
        out->username_len == 0U ||
        memchr(out->username, '\0', out->username_len) != NULL ||
        !argus_mqtt_read_binary(
            packet, length, &offset, out->password,
            sizeof(out->password), &out->password_len) ||
        out->password_len == 0U ||
        memchr(out->password, '\0', out->password_len) != NULL) {
        argus_password_zeroize(out, sizeof(*out));
        return ARGUS_MQTT_CONNECT_PARSE_CREDENTIALS;
    }
    if (offset != length) {
        argus_password_zeroize(out, sizeof(*out));
        return ARGUS_MQTT_CONNECT_PARSE_MALFORMED;
    }
    memcpy(out->client_id, client_id, client_id_length);
    out->client_id[client_id_length] = '\0';
    return ARGUS_MQTT_CONNECT_PARSE_OK;
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

/* ===========================================================================
 * Retained message & subscriber helpers  (unchanged, operate under client_lock)
 * =========================================================================*/

static esp_err_t argus_mqtt_store_retained_locked(const char *topic, const char *payload)
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
        ESP_LOGE(TAG, "retained capacity exhausted; refusing to evict authoritative state");
        return ESP_ERR_NO_MEM;
    }

    slot->in_use = true;
    strlcpy(slot->topic, topic, sizeof(slot->topic));
    strlcpy(slot->payload, payload, sizeof(slot->payload));
    return ESP_OK;
}

static size_t argus_mqtt_collect_subscribers_locked(
    const char *topic, int *sockets, size_t capacity)
{
    size_t count = 0U;
    for (size_t i = 0; i < ARGUS_MQTT_MAX_CLIENTS; ++i) {
        argus_mqtt_client_t *client = &s_broker.clients[i];
        if (!client->in_use || !client->connected || client->sock < 0) {
            continue;
        }

        bool matched = false;
        for (size_t sub = 0; sub < client->subscription_count; ++sub) {
            if (argus_mqtt_topic_matches(client->subscriptions[sub], topic)) {
                matched = true;
                break;
            }
        }

        if (matched && count < capacity) {
            sockets[count++] = client->sock;
        }
    }
    return count;
}

static void argus_mqtt_deliver_to_sockets(
    const int *sockets, size_t count,
    const char *topic, const char *payload, bool retain)
{
    for (size_t i = 0U; i < count; ++i) {
        (void)argus_mqtt_send_publish(
            sockets[i], topic, payload, retain);
    }
}

/* ===========================================================================
 * MQTT packet handlers  (unchanged from original)
 * =========================================================================*/

static bool argus_mqtt_duplicate_client_id_locked(
    const argus_mqtt_client_t *requester, const char *client_id)
{
    for (size_t i = 0; i < ARGUS_MQTT_MAX_CLIENTS; ++i) {
        const argus_mqtt_client_t *other = &s_broker.clients[i];
        if (other != requester && other->in_use && other->connected &&
            strcmp(other->client_id, client_id) == 0) {
            return true;
        }
    }
    return false;
}

static void argus_mqtt_client_info(
    const argus_mqtt_client_t *client,
    argus_mqtt_broker_client_info_t *out)
{
    memset(out, 0, sizeof(*out));
    out->connection_id = client->connection_id;
    out->receiving_interface = client->receiving_interface;
    strlcpy(out->client_id, client->client_id, sizeof(out->client_id));
    out->principal = client->principal;
}

static esp_err_t argus_mqtt_handle_connect(
    argus_mqtt_client_t *client, const uint8_t *packet, uint32_t len)
{
    argus_mqtt_connect_request_t request;
    argus_mqtt_connect_parse_result_t parse =
        argus_mqtt_broker_parse_connect(packet, len, &request);
    if (parse != ARGUS_MQTT_CONNECT_PARSE_OK) {
        uint8_t code = parse == ARGUS_MQTT_CONNECT_PARSE_PROTOCOL
                           ? 0x01U
                           : 0x02U;
        const uint8_t connack[] = {0x20U, 0x02U, 0x00U, code};
        (void)send(client->sock, connack, sizeof(connack), 0);
        return ESP_ERR_INVALID_ARG;
    }
    if (s_broker.authenticate == NULL) {
        argus_password_zeroize(&request, sizeof(request));
        return ESP_ERR_INVALID_STATE;
    }
    argus_machine_auth_outcome_t auth = s_broker.authenticate(
        client->peer_key, &request, client->receiving_interface,
        s_broker.user_ctx);
    argus_password_zeroize(request.password, sizeof(request.password));
    if (auth.result != ARGUS_MACHINE_AUTH_SUCCESS) {
        const uint8_t connack_bad_auth[] =
            {0x20U, 0x02U, 0x00U, 0x04U};
        (void)send(client->sock, connack_bad_auth,
                   sizeof(connack_bad_auth), 0);
        argus_password_zeroize(&request, sizeof(request));
        return ESP_ERR_NOT_ALLOWED;
    }

    xSemaphoreTake(s_broker.client_lock, portMAX_DELAY);
    bool bind_allowed = client->in_use && !client->connected &&
        !argus_mqtt_duplicate_client_id_locked(client, request.client_id);
    if (bind_allowed) {
        strlcpy(client->client_id, request.client_id,
                sizeof(client->client_id));
        client->principal = auth.principal;
        client->subscription_count = 0U;
        client->connected = true;
    }
    xSemaphoreGive(s_broker.client_lock);
    argus_password_zeroize(&request, sizeof(request));
    if (!bind_allowed) {
        const uint8_t connack_bad_id[] =
            {0x20U, 0x02U, 0x00U, 0x02U};
        (void)send(client->sock, connack_bad_id,
                   sizeof(connack_bad_id), 0);
        return ESP_ERR_INVALID_STATE;
    }

    const uint8_t connack_ok[] = {0x20U, 0x02U, 0x00U, 0x00U};
    if (send(client->sock, connack_ok, sizeof(connack_ok), 0) !=
        sizeof(connack_ok)) {
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "authenticated machine connected: %s",
             client->client_id);
    return ESP_OK;
}

static esp_err_t argus_mqtt_handle_subscribe(argus_mqtt_client_t *client, const uint8_t *packet, uint32_t len)
{
    typedef struct {
        char filters[ARGUS_MQTT_MAX_SUBS_PER_CLIENT][ARGUS_MQTT_MAX_TOPIC_LEN];
        size_t count;
    } subscribe_request_t;
    subscribe_request_t *request = calloc(1U, sizeof(*request));
    if (request == NULL) return ESP_ERR_NO_MEM;
    uint32_t offset = 0;
    uint16_t packet_id = 0;
    uint8_t suback[64] = {0};
    size_t suback_len = 0;

    if (!argus_mqtt_read_u16(packet, len, &offset, &packet_id)) {
        free(request);
        return ESP_ERR_INVALID_ARG;
    }
    while (offset < len) {
        if (request->count >= ARGUS_MQTT_MAX_SUBS_PER_CLIENT ||
            !argus_mqtt_read_string(
                packet, len, &offset, request->filters[request->count],
                sizeof(request->filters[request->count])) ||
            request->filters[request->count][0] == '\0' ||
            offset >= len) {
            free(request);
            return ESP_ERR_INVALID_ARG;
        }
        uint8_t requested_qos = packet[offset++];
        if (requested_qos > 1U) {
            free(request);
            return ESP_ERR_NOT_SUPPORTED;
        }
        request->count++;
    }
    if (request->count == 0U || offset != len) {
        free(request);
        return ESP_ERR_INVALID_ARG;
    }
    argus_mqtt_broker_client_info_t info;
    argus_mqtt_client_info(client, &info);
    for (size_t i = 0U; i < request->count; ++i) {
        if (s_broker.subscribe_policy == NULL ||
            s_broker.subscribe_policy(
                &info, request->filters[i], s_broker.user_ctx) != ESP_OK) {
            suback[suback_len++] = 0x90U;
            suback[suback_len++] = (uint8_t)(2U + request->count);
            suback[suback_len++] = (uint8_t)(packet_id >> 8);
            suback[suback_len++] = (uint8_t)(packet_id & 0xffU);
            for (size_t denied = 0U;
                 denied < request->count; ++denied) {
                suback[suback_len++] = 0x80U;
            }
            (void)send(client->sock, suback, suback_len, 0);
            free(request);
            return ESP_OK;
        }
    }
    argus_mqtt_client_info(client, &info);
    if (s_broker.revalidate == NULL ||
        s_broker.revalidate(&info, s_broker.user_ctx) != ESP_OK) {
        free(request);
        return ESP_ERR_NOT_ALLOWED;
    }
    xSemaphoreTake(s_broker.client_lock, portMAX_DELAY);
    bool capacity =
        client->connected &&
        client->subscription_count + request->count <=
            ARGUS_MQTT_MAX_SUBS_PER_CLIENT;
    if (capacity) {
        for (size_t i = 0U; i < request->count; ++i) {
            strlcpy(client->subscriptions[client->subscription_count++],
                    request->filters[i], ARGUS_MQTT_MAX_TOPIC_LEN);
        }
    }
    xSemaphoreGive(s_broker.client_lock);
    if (!capacity) {
        free(request);
        return ESP_ERR_NO_MEM;
    }

    suback[suback_len++] = 0x90U;
    suback[suback_len++] = (uint8_t)(2U + request->count);
    suback[suback_len++] = (uint8_t)(packet_id >> 8);
    suback[suback_len++] = (uint8_t)(packet_id & 0xFFU);
    for (size_t i = 0U; i < request->count; ++i) {
        suback[suback_len++] = 0x00U;
    }
    if (send(client->sock, suback, suback_len, 0) != (int)suback_len) {
        free(request);
        return ESP_FAIL;
    }
    for (size_t filter = 0U; filter < request->count; ++filter) {
        for (size_t retained = 0U;
             retained < ARGUS_MQTT_MAX_RETAINED; ++retained) {
            argus_mqtt_retained_t copy = {0};
            xSemaphoreTake(s_broker.client_lock, portMAX_DELAY);
            if (s_broker.retained[retained].in_use &&
                argus_mqtt_topic_matches(
                    request->filters[filter],
                    s_broker.retained[retained].topic)) {
                copy = s_broker.retained[retained];
            }
            xSemaphoreGive(s_broker.client_lock);
            if (copy.in_use) {
                (void)argus_mqtt_send_publish(
                    client->sock, copy.topic, copy.payload, true);
            }
        }
    }
    ESP_LOGI(TAG, "machine %s added %u authorized subscription(s)",
             client->client_id, (unsigned)request->count);
    free(request);
    return ESP_OK;
}

static esp_err_t argus_mqtt_handle_publish(argus_mqtt_client_t *client,
                                           uint8_t fixed_header,
                                           const uint8_t *packet,
                                           uint32_t len,
                                           argus_mqtt_broker_message_t *callback_message,
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

    const size_t payload_len = len - offset;
    if (payload_len >= sizeof(payload)) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(payload, packet + offset, payload_len);
    payload[payload_len] = '\0';

    *callback_message = (argus_mqtt_broker_message_t) {
        .connection_id = client->connection_id,
        .payload_len = payload_len,
        .qos = qos,
        .retain = retain,
        .dup = (fixed_header & 0x08U) != 0U,
        .policy_admitted = true,
        .receiving_interface = client->receiving_interface,
        .principal = client->principal,
    };
    strlcpy(callback_message->client_id, client->client_id,
            sizeof(callback_message->client_id));
    strlcpy(callback_message->topic, topic, sizeof(callback_message->topic));
    memcpy(callback_message->payload, payload, payload_len + 1U);

    if (s_broker.publish_authorize == NULL ||
        s_broker.publish_authorize(
            callback_message, s_broker.user_ctx) != ESP_OK) {
        ESP_LOGW(TAG, "machine publish authorization rejected: topic=%s",
                 topic);
        if (qos == 1U) {
            const uint8_t puback[] = {
                0x40U, 0x02U, (uint8_t)(packet_id >> 8),
                (uint8_t)(packet_id & 0xFFU)};
            (void)send(client->sock, puback, sizeof(puback), 0);
        }
        return ESP_OK;
    }
    if (s_broker.policy_check != NULL &&
        s_broker.policy_check(callback_message, s_broker.user_ctx) != ESP_OK) {
        callback_message->policy_admitted = false;
        *has_callback = s_broker.on_message != NULL;
        ESP_LOGW(TAG, "broker policy rejected external publish: topic=%s qos=%u retain=%d",
                 topic, qos, retain);
        if (qos == 1U) {
            const uint8_t puback[] = {0x40U, 0x02U, (uint8_t)(packet_id >> 8), (uint8_t)(packet_id & 0xFFU)};
            (void)send(client->sock, puback, sizeof(puback), 0);
        }
        return ESP_OK;
    }
    argus_mqtt_broker_client_info_t info;
    argus_mqtt_client_info(client, &info);
    if (s_broker.revalidate == NULL ||
        s_broker.revalidate(&info, s_broker.user_ctx) != ESP_OK) {
        return ESP_ERR_NOT_ALLOWED;
    }

    if (retain) {
        xSemaphoreTake(s_broker.client_lock, portMAX_DELAY);
        esp_err_t retain_err = argus_mqtt_store_retained_locked(topic, payload);
        xSemaphoreGive(s_broker.client_lock);
        if (retain_err != ESP_OK) {
            return retain_err;
        }
    }
    int sockets[ARGUS_MQTT_MAX_CLIENTS];
    xSemaphoreTake(s_broker.client_lock, portMAX_DELAY);
    size_t socket_count = argus_mqtt_collect_subscribers_locked(
        topic, sockets, ARGUS_MQTT_MAX_CLIENTS);
    xSemaphoreGive(s_broker.client_lock);
    argus_mqtt_deliver_to_sockets(
        sockets, socket_count, topic, payload, retain);

    ESP_LOGI(TAG, "client %s publish accepted: topic=%s qos=%u retain=%d payload_len=%u",
             client->client_id, topic, qos, retain, (unsigned)payload_len);

    if (s_broker.on_message != NULL) {
        *has_callback = true;
    }

    if (qos == 1U) {
        uint8_t puback[] = {0x40U, 0x02U, (uint8_t)(packet_id >> 8), (uint8_t)(packet_id & 0xFFU)};
        send(client->sock, puback, sizeof(puback), 0);
    }

    return ESP_OK;
}

/* ===========================================================================
 * Client slot release  (hardened: atomic count + event signal)
 * =========================================================================*/

static void argus_mqtt_close_client(argus_mqtt_client_t *client)
{
    argus_mqtt_broker_client_info_t info = {
        .connection_id = client->connection_id,
        .receiving_interface = client->receiving_interface,
        .principal = client->principal,
    };
    strlcpy(info.client_id, client->client_id, sizeof(info.client_id));

    xSemaphoreTake(s_broker.client_lock, portMAX_DELAY);
    int sock = client->sock;
    bool notify_disconnect = client->connected;
    client->sock = -1;
    client->connected = false;
    client->in_use = false;
    client->subscription_count = 0;
    argus_password_zeroize(&client->principal, sizeof(client->principal));
    xSemaphoreGive(s_broker.client_lock);

    if (sock >= 0) {
        shutdown(sock, 0);
        close(sock);
    }

    if (notify_disconnect && s_broker.on_client_event != NULL) {
        s_broker.on_client_event(ARGUS_MQTT_BROKER_CLIENT_DISCONNECTED,
                                 &info, s_broker.user_ctx);
    }

    int32_t prev = atomic_fetch_sub(&s_broker.active_client_count, 1);
    if (prev <= 0) {
        ESP_LOGE(TAG, "active_client_count underflow! (prev=%d)", (int)prev);
    } else if (prev == 1) {
        /* Last client exited -- signal any waiter in stop(). */
        xEventGroupSetBits(s_broker.lifecycle_event_group, BROKER_EVT_CLIENTS_EXITED);
    }
}

/* ===========================================================================
 * Per-client task  (protocol loop unchanged; exit path hardened)
 * =========================================================================*/

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

        if (remaining_len > 0) {
            got = argus_mqtt_read_exact(client->sock, packet, remaining_len);
            if (got <= 0) {
                break;
            }
        }

        uint8_t packet_type = fixed_header >> 4;
        bool has_callback = false;
        bool notify_connect = false;
        argus_mqtt_broker_message_t callback_message = {0};

        esp_err_t err = ESP_OK;
        if (!client->connected && packet_type != 1U) {
            err = ESP_ERR_INVALID_STATE;
        } else if (client->connected && packet_type == 1U) {
            err = ESP_ERR_INVALID_STATE;
        } else if (client->connected &&
                   (packet_type == 3U || packet_type == 8U)) {
            argus_mqtt_broker_client_info_t info;
            argus_mqtt_client_info(client, &info);
            if (s_broker.revalidate == NULL ||
                s_broker.revalidate(&info, s_broker.user_ctx) != ESP_OK) {
                err = ESP_ERR_NOT_ALLOWED;
            }
        }
        if (err == ESP_OK) switch (packet_type) {
        case 1:
            err = (fixed_header & 0x0fU) == 0U
                ? argus_mqtt_handle_connect(client, packet, remaining_len)
                : ESP_ERR_INVALID_ARG;
            notify_connect = err == ESP_OK;
            break;
        case 3:
            err = argus_mqtt_handle_publish(client,
                                            fixed_header,
                                            packet,
                                            remaining_len,
                                            &callback_message,
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
            goto done;
        default:
            ESP_LOGW(TAG, "unsupported packet type: %u", packet_type);
            break;
        }

        if (notify_connect && s_broker.on_client_event != NULL) {
            argus_mqtt_broker_client_info_t info;
            argus_mqtt_client_info(client, &info);
            s_broker.on_client_event(ARGUS_MQTT_BROKER_CLIENT_CONNECTED,
                                     &info, s_broker.user_ctx);
        }

        if (has_callback && s_broker.on_message != NULL) {
            s_broker.on_message(&callback_message, s_broker.user_ctx);
        }

        if (err != ESP_OK) {
            ESP_LOGW(TAG, "client packet handling failed: %s", esp_err_to_name(err));
            break;
        }
    }

done:
    ESP_LOGI(TAG, "client disconnected: %s", client->client_id[0] ? client->client_id : "(unknown)");
    argus_mqtt_close_client(client);
    vTaskDelete(NULL);
}

/* ===========================================================================
 * Client slot allocation  (hardened: atomic count tracking)
 * =========================================================================*/

static argus_mqtt_client_t *argus_mqtt_alloc_client_locked(
    int sock, uint32_t peer_key, uint8_t receiving_interface)
{
    for (size_t i = 0; i < ARGUS_MQTT_MAX_CLIENTS; ++i) {
        if (!s_broker.clients[i].in_use) {
            argus_mqtt_client_t *client = &s_broker.clients[i];
            memset(client, 0, sizeof(*client));
            client->in_use = true;
            client->sock = sock;
            client->connection_id = atomic_fetch_add(&s_broker.next_connection_id, 1U) + 1U;
            client->peer_key = peer_key;
            client->receiving_interface = receiving_interface;
            return client;
        }
    }
    return NULL;
}

static uint8_t argus_mqtt_receiving_interface(int sock)
{
    struct sockaddr_in local = {0};
    socklen_t length = sizeof(local);
    if (getsockname(sock, (struct sockaddr *)&local, &length) != 0 ||
        local.sin_family != AF_INET) {
        return 0U;
    }
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip = {0};
    if (ap != NULL && esp_netif_get_ip_info(ap, &ip) == ESP_OK &&
        ip.ip.addr != 0U && local.sin_addr.s_addr == ip.ip.addr) {
        return ARGUS_MACHINE_INTERFACE_SOFTAP;
    }
    memset(&ip, 0, sizeof(ip));
    if (sta != NULL && esp_netif_get_ip_info(sta, &ip) == ESP_OK &&
        ip.ip.addr != 0U && local.sin_addr.s_addr == ip.ip.addr) {
        return ARGUS_MACHINE_INTERFACE_STA;
    }
    return 0U;
}

/* ===========================================================================
 * Server (accept-loop) task  -- socket creation moved here, lifecycle signals
 * =========================================================================*/

static void argus_mqtt_server_task(void *arg)
{
    (void)arg;

    /* --- Create listening socket inside the task -------------------------*/
    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "unable to create socket: errno=%d", errno);
        xSemaphoreTake(s_broker.lifecycle_mutex, portMAX_DELAY);
        s_broker.startup_error = ESP_FAIL;
        s_broker.server_task_handle = NULL;
        xSemaphoreGive(s_broker.lifecycle_mutex);
        xEventGroupSetBits(s_broker.lifecycle_event_group, BROKER_EVT_STOPPED);
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in dest_addr = {
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_family = AF_INET,
        .sin_port = htons(s_broker.port),
    };

    if (bind(listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) != 0) {
        ESP_LOGE(TAG, "bind failed: errno=%d", errno);
        close(listen_sock);
        xSemaphoreTake(s_broker.lifecycle_mutex, portMAX_DELAY);
        s_broker.startup_error = ESP_FAIL;
        s_broker.server_task_handle = NULL;
        xSemaphoreGive(s_broker.lifecycle_mutex);
        xEventGroupSetBits(s_broker.lifecycle_event_group, BROKER_EVT_STOPPED);
        vTaskDelete(NULL);
        return;
    }

    if (listen(listen_sock, ARGUS_MQTT_MAX_CLIENTS) != 0) {
        ESP_LOGE(TAG, "listen failed: errno=%d", errno);
        close(listen_sock);
        xSemaphoreTake(s_broker.lifecycle_mutex, portMAX_DELAY);
        s_broker.startup_error = ESP_FAIL;
        s_broker.server_task_handle = NULL;
        xSemaphoreGive(s_broker.lifecycle_mutex);
        xEventGroupSetBits(s_broker.lifecycle_event_group, BROKER_EVT_STOPPED);
        vTaskDelete(NULL);
        return;
    }

    /* Publish the socket and transition to RUNNING. */
    xSemaphoreTake(s_broker.lifecycle_mutex, portMAX_DELAY);
    if (s_broker.state == BROKER_STATE_STARTING) {
        s_broker.listen_sock = listen_sock;
        s_broker.state = BROKER_STATE_RUNNING;
        s_broker.startup_error = ESP_OK;
        xSemaphoreGive(s_broker.lifecycle_mutex);

        ESP_LOGI(TAG, "local MQTT broker listening on port %u", s_broker.port);
        xEventGroupSetBits(s_broker.lifecycle_event_group, BROKER_EVT_STARTED);
    } else {
        s_broker.server_task_handle = NULL;
        xSemaphoreGive(s_broker.lifecycle_mutex);
        close(listen_sock);
        xEventGroupSetBits(s_broker.lifecycle_event_group, BROKER_EVT_STOPPED);
        vTaskDelete(NULL);
        return;
    }

    /* --- Accept loop --------------------------------------------------- */
    while (1) {
        xSemaphoreTake(s_broker.lifecycle_mutex, portMAX_DELAY);
        bool running = (s_broker.state == BROKER_STATE_RUNNING);
        int current_listen_sock = s_broker.listen_sock;
        xSemaphoreGive(s_broker.lifecycle_mutex);

        if (!running || current_listen_sock < 0) {
            break;
        }

        struct sockaddr_in source_addr = {0};
        socklen_t addr_len = sizeof(source_addr);
        int sock = accept(current_listen_sock, (struct sockaddr *)&source_addr, &addr_len);

        if (sock < 0) {
            xSemaphoreTake(s_broker.lifecycle_mutex, portMAX_DELAY);
            running = (s_broker.state == BROKER_STATE_RUNNING);
            current_listen_sock = s_broker.listen_sock;
            xSemaphoreGive(s_broker.lifecycle_mutex);

            if (!running || current_listen_sock < 0) {
                break;
            }
            ESP_LOGW(TAG, "accept failed: errno=%d", errno);
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }

        /* Check broker is still running before allocating a client. */
        xSemaphoreTake(s_broker.lifecycle_mutex, portMAX_DELAY);
        running = (s_broker.state == BROKER_STATE_RUNNING);
        xSemaphoreGive(s_broker.lifecycle_mutex);

        if (!running) {
            close(sock);
            break;
        }

        /* Reserve the active-client count BEFORE creating the task. */
        atomic_fetch_add(&s_broker.active_client_count, 1);

        uint8_t receiving_interface =
            argus_mqtt_receiving_interface(sock);
        xSemaphoreTake(s_broker.client_lock, portMAX_DELAY);
        argus_mqtt_client_t *client = argus_mqtt_alloc_client_locked(
            sock, source_addr.sin_addr.s_addr,
            receiving_interface);
        xSemaphoreGive(s_broker.client_lock);

        if (client == NULL) {
            ESP_LOGW(TAG, "rejecting MQTT client: client limit reached");
            atomic_fetch_sub(&s_broker.active_client_count, 1);
            close(sock);
            continue;
        }

        BaseType_t created = xTaskCreate(argus_mqtt_client_task, "mqtt_client", ARGUS_MQTT_CLIENT_TASK_STACK, client, 5, NULL);
        if (created != pdPASS) {
            ESP_LOGE(TAG, "failed to create MQTT client task");
            /* Release slot and decrement count. */
            xSemaphoreTake(s_broker.client_lock, portMAX_DELAY);
            client->sock = -1;
            client->in_use = false;
            client->subscription_count = 0;
            xSemaphoreGive(s_broker.client_lock);
            atomic_fetch_sub(&s_broker.active_client_count, 1);
            close(sock);
        }
    }

    ESP_LOGI(TAG, "MQTT broker server task exiting cleanly.");
    xSemaphoreTake(s_broker.lifecycle_mutex, portMAX_DELAY);
    s_broker.server_task_handle = NULL;
    xSemaphoreGive(s_broker.lifecycle_mutex);
    xEventGroupSetBits(s_broker.lifecycle_event_group, BROKER_EVT_STOPPED);
    vTaskDelete(NULL);
}

/* ===========================================================================
 * Public API
 * =========================================================================*/

esp_err_t argus_mqtt_broker_init(void)
{
    ESP_RETURN_ON_FALSE(s_broker.lifecycle_mutex == NULL, ESP_ERR_INVALID_STATE,
                        TAG, "broker already initialised");

    s_broker.lifecycle_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_broker.lifecycle_mutex != NULL, ESP_ERR_NO_MEM,
                        TAG, "failed to create lifecycle_mutex");

    s_broker.client_lock = xSemaphoreCreateMutex();
    if (s_broker.client_lock == NULL) {
        vSemaphoreDelete(s_broker.lifecycle_mutex);
        s_broker.lifecycle_mutex = NULL;
        ESP_LOGE(TAG, "failed to create client_lock");
        return ESP_ERR_NO_MEM;
    }

    s_broker.lifecycle_event_group = xEventGroupCreate();
    if (s_broker.lifecycle_event_group == NULL) {
        vSemaphoreDelete(s_broker.client_lock);
        s_broker.client_lock = NULL;
        vSemaphoreDelete(s_broker.lifecycle_mutex);
        s_broker.lifecycle_mutex = NULL;
        ESP_LOGE(TAG, "failed to create lifecycle_event_group");
        return ESP_ERR_NO_MEM;
    }

    s_broker.state = BROKER_STATE_STOPPED;
    atomic_store(&s_broker.active_client_count, 0);
    atomic_store(&s_broker.next_connection_id, 0);
    s_broker.listen_sock = -1;
    s_broker.server_task_handle = NULL;

    ESP_LOGI(TAG, "broker lifecycle objects initialised");
    return ESP_OK;
}

esp_err_t argus_mqtt_broker_start(const argus_mqtt_broker_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "config is null");
    ESP_RETURN_ON_FALSE(
        config->authenticate != NULL && config->revalidate != NULL &&
        config->subscribe_policy != NULL &&
        config->publish_authorize != NULL &&
        config->policy_check != NULL,
        ESP_ERR_INVALID_ARG, TAG, "security callbacks are required");
    ESP_RETURN_ON_FALSE(s_broker.lifecycle_mutex != NULL, ESP_ERR_INVALID_STATE,
                        TAG, "broker not initialised (call argus_mqtt_broker_init first)");

    xSemaphoreTake(s_broker.lifecycle_mutex, portMAX_DELAY);

    if (s_broker.state != BROKER_STATE_STOPPED) {
        xSemaphoreGive(s_broker.lifecycle_mutex);
        ESP_LOGE(TAG, "broker not in STOPPED state (current=%d)", (int)s_broker.state);
        return ESP_ERR_INVALID_STATE;
    }

    /* Verify no residual task handles remain from a previous cycle. */
    if (s_broker.server_task_handle != NULL) {
        xSemaphoreGive(s_broker.lifecycle_mutex);
        ESP_LOGE(TAG, "stale server_task_handle detected");
        return ESP_ERR_INVALID_STATE;
    }

    s_broker.state = BROKER_STATE_STARTING;

    /* Zero only runtime fields -- lifecycle objects are preserved. */
    s_broker.port = config->port;
    s_broker.on_message = config->on_message;
    s_broker.publish_authorize = config->publish_authorize;
    s_broker.policy_check = config->policy_check;
    s_broker.authenticate = config->authenticate;
    s_broker.revalidate = config->revalidate;
    s_broker.subscribe_policy = config->subscribe_policy;
    s_broker.on_client_event = config->on_client_event;
    s_broker.user_ctx = config->user_ctx;
    s_broker.listen_sock = -1;
    s_broker.startup_error = ESP_OK;
    atomic_store(&s_broker.active_client_count, 0);
    memset(s_broker.clients, 0, sizeof(s_broker.clients));
    memset(s_broker.retained, 0, sizeof(s_broker.retained));

    /* Clear event bits before launching the task. */
    xEventGroupClearBits(s_broker.lifecycle_event_group,
                         BROKER_EVT_STARTED | BROKER_EVT_STOPPED | BROKER_EVT_CLIENTS_EXITED);

    BaseType_t created = xTaskCreate(argus_mqtt_server_task, "mqtt_broker",
                                     ARGUS_MQTT_SERVER_TASK_STACK, NULL, 5,
                                     &s_broker.server_task_handle);
    if (created != pdPASS) {
        s_broker.state = BROKER_STATE_STOPPED;
        s_broker.server_task_handle = NULL;
        xSemaphoreGive(s_broker.lifecycle_mutex);
        ESP_LOGE(TAG, "failed to create broker task");
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreGive(s_broker.lifecycle_mutex);

    /* --- Wait bounded for the server task to report outcome ------------ */
    EventBits_t bits = xEventGroupWaitBits(s_broker.lifecycle_event_group,
                                           BROKER_EVT_STARTED | BROKER_EVT_STOPPED,
                                           pdFALSE,   /* don't clear */
                                           pdFALSE,   /* wait for ANY */
                                           pdMS_TO_TICKS(2000));

    if (bits & BROKER_EVT_STARTED) {
        return ESP_OK;
    }

    if (bits & BROKER_EVT_STOPPED) {
        /* Server task reported failure and already exited. */
        xSemaphoreTake(s_broker.lifecycle_mutex, portMAX_DELAY);
        s_broker.state = BROKER_STATE_STOPPED;
        xSemaphoreGive(s_broker.lifecycle_mutex);
        ESP_LOGE(TAG, "server task failed during startup: %s",
                 esp_err_to_name(s_broker.startup_error));
        return (s_broker.startup_error != ESP_OK) ? s_broker.startup_error : ESP_FAIL;
    }

    /* Timeout -- task neither started nor stopped within the window. */
    ESP_LOGE(TAG, "broker start timed out after 2000 ms");
    xSemaphoreTake(s_broker.lifecycle_mutex, portMAX_DELAY);
    s_broker.state = BROKER_STATE_STOPPING;
    int timed_out_listener = s_broker.listen_sock;
    s_broker.listen_sock = -1;
    xSemaphoreGive(s_broker.lifecycle_mutex);
    if (timed_out_listener >= 0) {
        shutdown(timed_out_listener, SHUT_RDWR);
        close(timed_out_listener);
    }

    bits = xEventGroupWaitBits(s_broker.lifecycle_event_group,
                               BROKER_EVT_STOPPED,
                               pdFALSE, pdFALSE,
                               pdMS_TO_TICKS(2000));

    if (bits & BROKER_EVT_STOPPED) {
        xSemaphoreTake(s_broker.lifecycle_mutex, portMAX_DELAY);
        s_broker.state = BROKER_STATE_STOPPED;
        xSemaphoreGive(s_broker.lifecycle_mutex);
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t argus_mqtt_broker_publish(const char *topic, const char *payload, bool retain)
{
    ESP_RETURN_ON_FALSE(topic != NULL && payload != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid publish args");
    ESP_RETURN_ON_FALSE(strnlen(topic, ARGUS_MQTT_MAX_TOPIC_LEN) < ARGUS_MQTT_MAX_TOPIC_LEN,
                        ESP_ERR_INVALID_SIZE, TAG, "publish topic too long");
    ESP_RETURN_ON_FALSE(strnlen(payload, ARGUS_MQTT_MAX_PAYLOAD_LEN) < ARGUS_MQTT_MAX_PAYLOAD_LEN,
                        ESP_ERR_INVALID_SIZE, TAG, "publish payload too long");

    xSemaphoreTake(s_broker.lifecycle_mutex, portMAX_DELAY);
    if (s_broker.state != BROKER_STATE_RUNNING) {
        xSemaphoreGive(s_broker.lifecycle_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_broker.client_lock, portMAX_DELAY);
    xSemaphoreGive(s_broker.lifecycle_mutex);

    esp_err_t retain_err = ESP_OK;
    if (retain) {
        retain_err = argus_mqtt_store_retained_locked(topic, payload);
    }
    int sockets[ARGUS_MQTT_MAX_CLIENTS];
    size_t socket_count = retain_err == ESP_OK
        ? argus_mqtt_collect_subscribers_locked(
              topic, sockets, ARGUS_MQTT_MAX_CLIENTS)
        : 0U;
    xSemaphoreGive(s_broker.client_lock);
    if (retain_err != ESP_OK) return retain_err;
    argus_mqtt_deliver_to_sockets(
        sockets, socket_count, topic, payload, retain);
    return ESP_OK;
}

esp_err_t argus_mqtt_broker_disconnect_machine(const char *identifier)
{
    if (identifier == NULL || identifier[0] == '\0' ||
        s_broker.client_lock == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    int sockets[ARGUS_MQTT_MAX_CLIENTS];
    size_t count = 0U;
    xSemaphoreTake(s_broker.client_lock, portMAX_DELAY);
    for (size_t i = 0U; i < ARGUS_MQTT_MAX_CLIENTS; ++i) {
        argus_mqtt_client_t *client = &s_broker.clients[i];
        if (client->in_use && client->connected && client->sock >= 0 &&
            strcmp(client->principal.identifier, identifier) == 0) {
            sockets[count++] = client->sock;
        }
    }
    xSemaphoreGive(s_broker.client_lock);
    for (size_t i = 0U; i < count; ++i) {
        (void)shutdown(sockets[i], SHUT_RDWR);
    }
    return count > 0U ? ESP_OK : ESP_ERR_NOT_FOUND;
}

bool argus_mqtt_broker_is_running(void)
{
    argus_mqtt_broker_lifecycle_obs_t obs = {0};
    return argus_mqtt_broker_get_lifecycle_obs(&obs) == ESP_OK && obs.running;
}

esp_err_t argus_mqtt_broker_stop(void)
{
    ESP_RETURN_ON_FALSE(s_broker.lifecycle_mutex != NULL, ESP_ERR_INVALID_STATE,
                        TAG, "broker not initialised");

    xSemaphoreTake(s_broker.lifecycle_mutex, portMAX_DELAY);

    argus_mqtt_broker_lifecycle_obs_t initial = {
        .state = s_broker.state,
        .active_client_count = atomic_load(&s_broker.active_client_count),
        .has_server_task = s_broker.server_task_handle != NULL,
        .has_listener = s_broker.listen_sock >= 0,
    };
    if (argus_mqtt_broker_observation_is_stopped(&initial)) {
        xSemaphoreGive(s_broker.lifecycle_mutex);
        return ESP_OK;
    }

    /* Clear event bits so we can wait for fresh signals. */
    xEventGroupClearBits(s_broker.lifecycle_event_group, BROKER_EVT_STARTED | BROKER_EVT_STOPPED | BROKER_EVT_CLIENTS_EXITED);

    s_broker.state = BROKER_STATE_STOPPING;
    bool wait_for_server_task = s_broker.server_task_handle != NULL;

    int listener = s_broker.listen_sock;
    s_broker.listen_sock = -1;
    xSemaphoreGive(s_broker.lifecycle_mutex);

    /* Close the listener socket outside the lifecycle lock to break accept(). */
    if (listener >= 0) {
        shutdown(listener, SHUT_RDWR);
        close(listener);
    }

    /* Snapshot and retire sockets under the lock, then close them unlocked. */
    int sockets[ARGUS_MQTT_MAX_CLIENTS];
    size_t socket_count = 0U;
    xSemaphoreTake(s_broker.client_lock, portMAX_DELAY);
    for (size_t i = 0; i < ARGUS_MQTT_MAX_CLIENTS; ++i) {
        if (s_broker.clients[i].in_use && s_broker.clients[i].sock >= 0) {
            sockets[socket_count++] = s_broker.clients[i].sock;
            s_broker.clients[i].sock = -1;
        }
    }
    xSemaphoreGive(s_broker.client_lock);
    for (size_t i = 0U; i < socket_count; ++i) {
        shutdown(sockets[i], SHUT_RDWR);
        close(sockets[i]);
    }

    /* --- Wait for the server task to exit (bounded 2 s) ---------------- */
    EventBits_t bits = 0;
    if (wait_for_server_task) {
        bits = xEventGroupWaitBits(s_broker.lifecycle_event_group,
                                   BROKER_EVT_STOPPED,
                                   pdFALSE, pdFALSE,
                                   pdMS_TO_TICKS(2000));

        if (!(bits & BROKER_EVT_STOPPED)) {
            ESP_LOGW(TAG, "server task did not exit within 2000 ms");
            return ESP_ERR_TIMEOUT;
        }
    }

    /* --- Wait for all client tasks to drain (bounded 2 s) -------------- */
    if (atomic_load(&s_broker.active_client_count) > 0) {
        bits = xEventGroupWaitBits(s_broker.lifecycle_event_group,
                                   BROKER_EVT_CLIENTS_EXITED,
                                   pdFALSE, pdFALSE,
                                   pdMS_TO_TICKS(2000));

        if (!(bits & BROKER_EVT_CLIENTS_EXITED) && atomic_load(&s_broker.active_client_count) > 0) {
            ESP_LOGW(TAG, "client tasks did not drain within 2000 ms (%d remaining)",
                     (int)atomic_load(&s_broker.active_client_count));
            return ESP_ERR_TIMEOUT;
        }
    }

    /* Publish STOPPED only after every lifecycle resource has converged. */
    xSemaphoreTake(s_broker.lifecycle_mutex, portMAX_DELAY);
    bool resources_absent = s_broker.server_task_handle == NULL &&
                            s_broker.listen_sock < 0 &&
                            atomic_load(&s_broker.active_client_count) == 0;
    if (resources_absent) {
        s_broker.state = BROKER_STATE_STOPPED;
    }
    xSemaphoreGive(s_broker.lifecycle_mutex);

    if (!resources_absent) {
        ESP_LOGW(TAG, "broker stop did not reach full lifecycle convergence");
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "local MQTT broker stopped cleanly");
    return ESP_OK;
}

esp_err_t argus_mqtt_broker_get_lifecycle_obs(argus_mqtt_broker_lifecycle_obs_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_broker.lifecycle_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_broker.lifecycle_mutex, portMAX_DELAY);
    out->state = s_broker.state;
    out->active_client_count = atomic_load(&s_broker.active_client_count);
    out->has_server_task = (s_broker.server_task_handle != NULL);
    out->has_listener = (s_broker.listen_sock >= 0);
    out->running = argus_mqtt_broker_observation_is_running(out);
    out->stopped = argus_mqtt_broker_observation_is_stopped(out);
    xSemaphoreGive(s_broker.lifecycle_mutex);
    return ESP_OK;
}

bool argus_mqtt_broker_observation_is_stopped(
    const argus_mqtt_broker_lifecycle_obs_t *obs)
{
    return obs && obs->state == BROKER_STATE_STOPPED &&
           !obs->has_server_task && !obs->has_listener &&
           obs->active_client_count == 0;
}

bool argus_mqtt_broker_observation_is_running(
    const argus_mqtt_broker_lifecycle_obs_t *obs)
{
    return obs && obs->state == BROKER_STATE_RUNNING &&
           obs->has_server_task && obs->has_listener;
}

esp_err_t argus_mqtt_broker_request_stop_converged(
    const argus_mqtt_broker_convergence_ops_t *ops)
{
    if (!ops || !ops->observe || !ops->stop) {
        return ESP_ERR_INVALID_ARG;
    }
    argus_mqtt_broker_lifecycle_obs_t obs = {0};
    esp_err_t err = ops->observe(ops->ctx, &obs);
    if (err != ESP_OK) {
        return err;
    }
    if (argus_mqtt_broker_observation_is_stopped(&obs)) {
        return ESP_OK;
    }
    return ops->stop(ops->ctx);
}

esp_err_t argus_mqtt_broker_verify_stopped_converged(
    const argus_mqtt_broker_convergence_ops_t *ops,
    uint32_t observation_attempts,
    uint32_t delay_ms)
{
    if (!ops || !ops->observe || observation_attempts == 0 ||
        (observation_attempts > 1 && delay_ms > 0 && !ops->wait)) {
        return ESP_ERR_INVALID_ARG;
    }

    for (uint32_t attempt = 0; attempt < observation_attempts; ++attempt) {
        argus_mqtt_broker_lifecycle_obs_t obs = {0};
        esp_err_t err = ops->observe(ops->ctx, &obs);
        if (err != ESP_OK) {
            return err;
        }
        if (argus_mqtt_broker_observation_is_stopped(&obs)) {
            return ESP_OK;
        }
        if (attempt + 1 < observation_attempts && delay_ms > 0) {
            ops->wait(ops->ctx, delay_ms);
        }
    }
    return ESP_ERR_TIMEOUT;
}
