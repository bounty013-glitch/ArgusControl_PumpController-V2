#include "argus_console_helpers.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "linenoise/linenoise.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

esp_err_t argus_console_transport_init(void)
{
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
        esp_err_t err = usb_serial_jtag_driver_install(&config);
        if (err != ESP_OK) {
            return err;
        }
    }

    usb_serial_jtag_vfs_use_driver();

    linenoiseSetMultiLine(0);
    linenoiseAllowEmpty(false);

    int line_len_result = linenoiseSetMaxLineLen(96);
    if (line_len_result != 0) {
        return ESP_FAIL;
    }

    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t argus_console_read_line(const char *prompt, char *buffer, size_t buffer_size)
{
    if (prompt == NULL || buffer == NULL || buffer_size < 2) {
        return ESP_ERR_INVALID_ARG;
    }

    buffer[0] = '\0';

    char *line = linenoise(prompt);
    if (line == NULL) {
        return ESP_FAIL;
    }

    size_t len = strlen(line);

    if (len == 0) {
        linenoiseFree(line);
        return ESP_ERR_INVALID_ARG;
    }

    if (len >= buffer_size) {
        linenoiseFree(line);
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(buffer, line, len + 1);
    linenoiseFree(line);

    return ESP_OK;
}

esp_err_t argus_console_read_password(const char *prompt, char *buffer, size_t buffer_size)
{
    if (prompt == NULL || buffer == NULL || buffer_size < 9) {
        return ESP_ERR_INVALID_ARG;
    }

    buffer[0] = '\0';
    write(STDOUT_FILENO, prompt, strlen(prompt));

    size_t idx = 0;
    bool cancel = false;
    bool overflow = false;

    while (true) {
        uint8_t ch = 0;
        int n = read(STDIN_FILENO, &ch, 1);
        if (n <= 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (ch == 0x03) {
            cancel = true;
            write(STDOUT_FILENO, "^C\r\n", 4);
            break;
        }

        if (ch == '\b' || ch == 0x7F) {
            if (idx > 0) {
                idx--;
                buffer[idx] = '\0';
                write(STDOUT_FILENO, "\b \b", 3);
            }
            continue;
        }

        if (ch == '\r' || ch == '\n') {
            write(STDOUT_FILENO, "\r\n", 2);
            break;
        }

        if (ch < 32 || ch > 126) {
            continue;
        }

        if (idx >= buffer_size - 1) {
            overflow = true;
            while (true) {
                uint8_t drain_ch = 0;
                if (read(STDIN_FILENO, &drain_ch, 1) > 0) {
                    if (drain_ch == '\r' || drain_ch == '\n') {
                        write(STDOUT_FILENO, "\r\n", 2);
                        break;
                    }
                } else {
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
            }
            break;
        }

        buffer[idx++] = (char)ch;
        buffer[idx] = '\0';
        write(STDOUT_FILENO, "*", 1);
    }

    if (cancel) {
        memset(buffer, 0, buffer_size);
        printf("[CANCELLED] Password entry cancelled.\n");
        return ESP_ERR_INVALID_STATE;
    }

    if (overflow) {
        memset(buffer, 0, buffer_size);
        printf("[ERROR] Password exceeds maximum capacity (%u chars).\n", (unsigned int)(buffer_size - 1));
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t val_err = argus_console_validate_password(buffer);
    if (val_err != ESP_OK) {
        printf("[STAGING FAILED] Password must be 8-63 characters (got %u chars).\n", (unsigned int)strlen(buffer));
        memset(buffer, 0, buffer_size);
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

char *argus_console_trim_whitespace(char *str)
{
    if (str == NULL) return NULL;

    while (isspace((unsigned char)*str)) {
        str++;
    }

    if (*str == '\0') {
        return str;
    }

    char *end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) {
        end--;
    }

    end[1] = '\0';
    return str;
}

esp_err_t argus_console_parse_menu_key(const char *line, char *out_key)
{
    if (line == NULL || out_key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    while (isspace((unsigned char)*line)) {
        line++;
    }

    if (*line == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    char candidate = *line++;

    while (*line != '\0') {
        if (!isspace((unsigned char)*line)) {
            return ESP_ERR_INVALID_ARG;
        }
        line++;
    }

    *out_key = candidate;
    return ESP_OK;
}

esp_err_t argus_console_validate_ssid(const char *ssid)
{
    if (ssid == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t len = strlen(ssid);
    if (len < 1 || len > 32) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

esp_err_t argus_console_validate_password(const char *pass)
{
    if (pass == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t len = strlen(pass);
    if (len < 8 || len > 63) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}
