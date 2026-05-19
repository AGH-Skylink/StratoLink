//================================================================
// E32
// Ported to libgpiod v2.x
//================================================================

#ifndef _E32_H
#define _E32_H

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>

#include <stdint.h>
#include <stdbool.h>

#include <gpiod.h>

#include <sys/select.h>
#include <time.h>

#define E32_WAIT_FOR_AUX_DELAY_US (5 * 1000)
#define E32_AUX_TIMEOUT_MS (20 * 1000)
#define E32_READ_TIMEOUT_SEC 200

#define E32_SAFE_UART_CHUNK 256

#define E32_FILE_ACK 0x06
#define E32_FILE_NACK 0x15
#define E32_FILE_MAX_RETRIES 10

typedef struct {
    int fd;
    struct gpiod_chip* chip;
    struct gpiod_line_request* gpio_request;
    unsigned int m0_pin;
    unsigned int m1_pin;
    unsigned int aux_pin;
} E32_Device;

static inline uint16_t E32_hash16(const uint8_t* data, size_t len) {
    uint32_t hash = 2166136261u; // FNV-1a 32-bit
    for (size_t i = 0; i < len; ++i) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return (uint16_t)((hash >> 16) ^ (hash & 0xFFFFu));
}

static inline void E32_u16_to_be(uint16_t value, uint8_t out[2]) {
    out[0] = (uint8_t)((value >> 8) & 0xFFu);
    out[1] = (uint8_t)(value & 0xFFu);
}

static inline uint16_t E32_be_to_u16(const uint8_t in[2]) {
    return (uint16_t)(((uint16_t)in[0] << 8) | (uint16_t)in[1]);
}

bool E32_init(const char* portname, unsigned int m0_pin, unsigned int m1_pin, unsigned int aux_pin, E32_Device* device) {
    device->fd = open(portname, O_RDWR | O_NOCTTY | O_SYNC);
    if (device->fd < 0) {perror("open serial"); return false;}
    struct termios tty;
    if (tcgetattr(device->fd, &tty) != 0) {perror("tcgetattr"); close(device->fd); return false;}
    cfsetospeed(&tty, B9600);
    cfsetispeed(&tty, B9600);
    cfmakeraw(&tty);
    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 10;
    if (tcsetattr(device->fd, TCSANOW, &tty) != 0) {perror("tcsetattr"); close(device->fd); return false;}
    tcflush(device->fd, TCIOFLUSH);
    device->chip = gpiod_chip_open("/dev/gpiochip0");
    if (!device->chip) {perror("gpiod_chip_open"); close(device->fd); return false;}
    device->m0_pin = m0_pin;
    device->m1_pin = m1_pin;
    device->aux_pin = aux_pin;
    struct gpiod_line_settings* settings = gpiod_line_settings_new();
    if (!settings) {perror("gpiod_line_settings_new"); gpiod_chip_close(device->chip); close(device->fd); return false;}
    struct gpiod_request_config* req_config = gpiod_request_config_new();
    if (!req_config) {perror("gpiod_request_config_new"); gpiod_line_settings_free(settings); gpiod_chip_close(device->chip); close(device->fd); return false;}
    gpiod_request_config_set_consumer(req_config, "e32_module");
    struct gpiod_line_config* line_config = gpiod_line_config_new();
    if (!line_config) {perror("gpiod_line_config_new"); gpiod_request_config_free(req_config); gpiod_line_settings_free(settings); gpiod_chip_close(device->chip); close(device->fd); return false;}
    gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_INPUT);
    gpiod_line_config_add_line_settings(line_config, &aux_pin, 1, settings);
    gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_OUTPUT);
    gpiod_line_settings_set_output_value(settings, GPIOD_LINE_VALUE_INACTIVE);
    unsigned int output_pins[] = {m0_pin, m1_pin};
    gpiod_line_config_add_line_settings(line_config, output_pins, 2, settings);
    device->gpio_request = gpiod_chip_request_lines(device->chip, req_config, line_config);
    gpiod_line_config_free(line_config);
    gpiod_request_config_free(req_config);
    gpiod_line_settings_free(settings);
    if (!device->gpio_request) {perror("gpiod_chip_request_lines"); gpiod_chip_close(device->chip); close(device->fd); return false;}
    return true;
}

bool E32_wait_for_aux(E32_Device* device) {
    usleep(E32_WAIT_FOR_AUX_DELAY_US);
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    while (gpiod_line_request_get_value(device->gpio_request, device->aux_pin) == GPIOD_LINE_VALUE_INACTIVE) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed_ms = (now.tv_sec  - start.tv_sec)  * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000L;
        if (elapsed_ms >= E32_AUX_TIMEOUT_MS) {fprintf(stderr, "E32_wait_for_aux: timed out after %d ms\n", E32_AUX_TIMEOUT_MS); return false;}
        usleep(1000);
    }
    return true;
}

bool E32_write_bytes(E32_Device* device, uint8_t* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        size_t chunk = len - sent;
        if (chunk > E32_SAFE_UART_CHUNK) chunk = E32_SAFE_UART_CHUNK;
        size_t chunk_sent = 0;
        while (chunk_sent < chunk) {
            ssize_t written = write(device->fd, data + sent + chunk_sent, chunk - chunk_sent);
            if (written < 0) {if (errno == EINTR) continue; perror("write"); return false;}
            if (written == 0) {fprintf(stderr, "write returned 0\n"); return false;}
            chunk_sent += (size_t)written;
        }
        if (tcdrain(device->fd) != 0) {perror("tcdrain"); return false;}
        if (!E32_wait_for_aux(device)) return false;
        sent += chunk;
    }
    return true;
}

bool E32_read_until_crlf(E32_Device* device, uint8_t* buffer, size_t buffer_size, size_t* len) {
    size_t total = 0;
    bool got_cr = false;
    struct timespec deadline, now;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += E32_READ_TIMEOUT_SEC;
    while (total < buffer_size - 1) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        long remaining_ms = (deadline.tv_sec - now.tv_sec) * 1000 + (deadline.tv_nsec - now.tv_nsec) / 1000000L;
        if (remaining_ms <= 0) {fprintf(stderr, "E32_read_until_crlf: timed out before CRLF, got %zu bytes\n", total); return false;}
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(device->fd, &fds);
        struct timeval tv = {.tv_sec = remaining_ms / 1000, .tv_usec = (remaining_ms % 1000) * 1000};
        int ready = select(device->fd + 1, &fds, NULL, NULL, &tv);
        if (ready == 0) {fprintf(stderr, "E32_read_until_crlf: select timeout before CRLF, got %zu bytes\n", total); return false;}
        if (ready < 0) {perror("select"); return false;}
        uint8_t c;
        ssize_t r = read(device->fd, &c, 1);
        if (r <= 0) {perror("read"); return false;}
        buffer[total++] = c;
        if (got_cr && c == '\n') {
            buffer[total - 2] = '\0';
            *len = total - 2;
            return true;
        }
        got_cr = (c == '\r');
    }
    fprintf(stderr, "E32_read_until_crlf: buffer too small before CRLF\n");
    return false;
}

bool E32_read_n_bytes(E32_Device* device, uint8_t* buffer, size_t buffer_size, size_t n) {
    if (n > buffer_size) return false;
    size_t received = 0;
    struct timespec deadline, now;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += E32_READ_TIMEOUT_SEC;
    while (received < n) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        long remaining_ms = (deadline.tv_sec  - now.tv_sec)  * 1000 + (deadline.tv_nsec - now.tv_nsec) / 1000000L;
        if (remaining_ms <= 0) {fprintf(stderr, "E32_read_n_bytes: timed out waiting for byte %zu/%zu\n", received, n); return false;}
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(device->fd, &fds);
        struct timeval tv = { .tv_sec = remaining_ms / 1000, .tv_usec = (remaining_ms % 1000) * 1000 };
        int ready = select(device->fd + 1, &fds, NULL, NULL, &tv);
        if (ready == 0) {fprintf(stderr, "E32_read_n_bytes: select timed out at byte %zu/%zu\n", received, n); return false;}
        if (ready < 0) { perror("select"); return false; }
        ssize_t r = read(device->fd, buffer + received, n - received);
        if (r <= 0) { perror("read"); return false; }
        received += (size_t)r;
    }
    return true;
}

bool E32_read_bytes(E32_Device* device, uint8_t* buffer, size_t buffer_size, size_t* len) {
    ssize_t n = read(device->fd, buffer, buffer_size);
    if (n <= 0) return false;
    *len = n;
    return true;
}

bool E32_set_mode(E32_Device* device, int m0_value, int m1_value) {
    enum gpiod_line_value m0_val = m0_value ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE;
    enum gpiod_line_value m1_val = m1_value ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE;
    if (gpiod_line_request_set_value(device->gpio_request, device->m0_pin, m0_val) < 0 || gpiod_line_request_set_value(device->gpio_request, device->m1_pin, m1_val) < 0) {perror("gpiod_line_request_set_value"); return false;}
    return E32_wait_for_aux(device);
}

bool E32_reset(E32_Device* device) {
    if (!E32_set_mode(device, 1, 1)) return false;
    uint8_t cmd[3] = {0xC4, 0xC4, 0xC4};
    if (!E32_write_bytes(device, cmd, 3)) {E32_set_mode(device, 0, 0); return false;}
    return E32_set_mode(device, 0, 0);
}

typedef struct __attribute__((packed)) {
    uint8_t addh;
    uint8_t addl;
    uint8_t speed;
    uint8_t channel;
    uint8_t option;
} E32_Config;

bool E32_read_config(E32_Device* device, E32_Config* config) {
    if (!E32_set_mode(device, 1, 1)) return false;
    tcflush(device->fd, TCIFLUSH);
    uint8_t cmd[3] = {0xC1, 0xC1, 0xC1};
    if (!E32_write_bytes(device, cmd, 3)) {E32_set_mode(device, 0, 0); return false;}
    uint8_t response[6] = {0};
    if (!E32_read_n_bytes(device, response, sizeof(response), 6) || response[0] != 0xC0) {
        fprintf(stderr, "E32_read_config: bad read or header\n");
        fprintf(stderr, "%02x, %02x, %02x, %02x, %02x, %02x\n", response[0], response[1], response[2], response[3], response[4], response[5]);
        E32_set_mode(device, 0, 0);
        return false;
    }
    memcpy(config, &response[1], 5);
    return E32_set_mode(device, 0, 0);
}

bool E32_write_config(E32_Device* device, E32_Config* config) {
    if (!E32_set_mode(device, 1, 1)) return false;
    tcflush(device->fd, TCIFLUSH);
    uint8_t cmd[6] = {0xC0};
    memcpy(&cmd[1], config, 5);
    if (!E32_write_bytes(device, cmd, 6)) {E32_set_mode(device, 0, 0); return false;}
    return E32_set_mode(device, 0, 0);
}

void E32_close(E32_Device* device) {
    if (device->gpio_request) gpiod_line_request_release(device->gpio_request);
    if (device->chip) gpiod_chip_close(device->chip);
    if (device->fd >= 0) close(device->fd);
}

#endif // _E32_H