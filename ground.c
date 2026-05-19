#include "e32.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sys/stat.h>

#include <stdbool.h>

// E32 config
#define E32_PORTNAME "/dev/serial0"
#define E32_M0_PIN 23
#define E32_M1_PIN 24
#define E32_AUX_PIN 25

#define E32_FALLBACK_CHANNEL 0x0F
#define E32_FALLBACK_SPEED 0x1C

#define E32_CONFIG_CHANGE_DELAY_SEC 1

#define INPUT_BUFFER_SIZE 2048
#define RESPONSE_BUFFER_SIZE 4096

#define FILE_READ_CHUNK_SIZE 32

#define MAX_FILE_SIZE (2 * 1024 * 1024)

static E32_Config config={0};

static inline void print_gse(const char* error_msg) {
    printf("UNEXPECTED GROUND SIDE ERROR: %s\n", error_msg);
}

static inline void print_sse(const char* error_msg) {
    printf("STRATOLINK SIDE ERROR: %s\n", error_msg);
}

static inline void send_string(E32_Device* device, char* str){
    if (!E32_write_bytes(device, (uint8_t*)str, strlen(str))) print_gse("TX failed");
}

static inline void send_ack(E32_Device* device, uint8_t status, uint16_t seq) {
    uint8_t ack[3] = {0};
    ack[0] = status;
    E32_u16_to_be(seq, &ack[1]);
    if (!E32_write_bytes(device, ack, sizeof(ack))) print_gse("Failed to send ACK/NACK");
}

static inline char* path_basename(char* path) {
    char* p = strrchr(path, '/');
    return p ? p + 1 : path;
}

static inline bool build_timestamped_photo_name(char* out, size_t out_size) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return false;
    struct tm tm_buf;
    if (!localtime_r(&ts.tv_sec, &tm_buf)) return false;
    char stamp[64] = {0};
    if (strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tm_buf) == 0) return false;
    int written = snprintf(out, out_size, "stratolink_photo/photo_%s.jpg", stamp);
    return written > 0 && (size_t)written < out_size;
}

static inline bool read_file_transfer_status(E32_Device* device, uint8_t* response, size_t response_size) {
    size_t len = 0;
    if (!E32_read_until_crlf(device, response, response_size, &len)) {print_gse("Failed to read file transfer status"); return false;}
    if (strcmp((char*)response, "FILE_OK") == 0) return true;
    if (strcmp((char*)response, "FILE_ERR") == 0) {print_sse("No such file exists or photo capture failed"); return false;}
    print_gse("Unexpected file transfer status response");
    return false;
}

static inline bool fetch_file(E32_Device* device, const char* path) {
    FILE* file_handle = fopen(path, "wb");
    if (!file_handle) {print_gse("Failed to open file for writing"); return false;}
    size_t total_received = 0;
    size_t total_corrupted = 0;
    uint16_t expected_seq = 0;
    bool done = false;
    while (!done) {
        uint8_t header[4] = {0};
        if (!E32_read_n_bytes(device, header, sizeof(header), sizeof(header))) {print_gse("Failed to read packet header"); fclose(file_handle); return false;}
        const uint16_t seq = E32_be_to_u16(&header[0]);
        const uint16_t payload_size = E32_be_to_u16(&header[2]);
        if (payload_size > FILE_READ_CHUNK_SIZE) {print_gse("Invalid payload size received"); send_ack(device, E32_FILE_NACK, seq); fclose(file_handle); return false;}
        uint8_t payload[FILE_READ_CHUNK_SIZE] = {0};
        if (payload_size > 0 && !E32_read_n_bytes(device, payload, sizeof(payload), payload_size)) {print_gse("Failed to read packet payload"); send_ack(device, E32_FILE_NACK, seq); fclose(file_handle); return false;}
        uint8_t hash_bytes[2] = {0};
        if (!E32_read_n_bytes(device, hash_bytes, sizeof(hash_bytes), sizeof(hash_bytes))) {print_gse("Failed to read packet hash"); send_ack(device, E32_FILE_NACK, seq); fclose(file_handle); return false;}
        uint8_t hash_input[4 + FILE_READ_CHUNK_SIZE] = {0};
        memcpy(hash_input, header, sizeof(header));
        if (payload_size > 0) memcpy(hash_input + sizeof(header), payload, payload_size);
        const uint16_t expected_hash = E32_hash16(hash_input, sizeof(header) + payload_size);
        const uint16_t received_hash = E32_be_to_u16(hash_bytes);
        if (received_hash != expected_hash) {
            print_gse("Packet hash mismatch"); 
            send_ack(device, E32_FILE_NACK, seq); 
            total_corrupted += payload_size;
            continue;
        }
        if (seq == expected_seq) {
            if (payload_size == 0) {send_ack(device, E32_FILE_ACK, seq); done = true; break;}
            if (total_received + payload_size > MAX_FILE_SIZE) {print_gse("File too large to receive. Check ground side config."); send_ack(device, E32_FILE_NACK, seq); fclose(file_handle); return false;}
            if (fwrite(payload, 1, payload_size, file_handle) != payload_size) {print_gse("Failed to write file data"); send_ack(device, E32_FILE_NACK, seq); fclose(file_handle); return false;}
            total_received += payload_size;
            send_ack(device, E32_FILE_ACK, seq);
            expected_seq++;
            printf("\x1b[2J\x1b[H");
            printf("Total received: %zu        Corrupted: %zu\n\n", total_received, total_corrupted);
            continue;
        }
        if (expected_seq > 0 && (uint16_t)(expected_seq - 1) == seq) {
            send_ack(device, E32_FILE_ACK, seq);
            continue;
        }
        print_gse("Unexpected sequence number");
        send_ack(device, E32_FILE_NACK, seq);
        total_corrupted += payload_size;
    }
    fclose(file_handle);
    printf("File (%zu bytes) received and saved as %s\n", total_received, path);
    return true;
}

static inline bool change_air_rate_and_channel(E32_Device* device, uint8_t air_rate, uint8_t channel) {
    if (air_rate < 2 || air_rate > 5) return false;
    if (!E32_read_config(device, &config)) {fprintf(stderr, "Failed to read config\n"); return false;};
    config.speed = ((config.speed & 0xF8) | (air_rate & 0x07));
    config.channel = channel;
    if (!E32_write_config(device, &config)) {fprintf(stderr, "Failed to write config\n"); return false;};
    sleep(E32_CONFIG_CHANGE_DELAY_SEC);
    if (!E32_read_config(device, &config)) {fprintf(stderr, "Failed to read config\n"); return false;};
    return config.speed == ((config.speed & 0xF8) | (air_rate & 0x07)) && config.channel == channel;
}

int main() {
    mkdir("./stratolink_photo", 0777);

    E32_Device device = {0};
    if (!E32_init(E32_PORTNAME, E32_M0_PIN, E32_M1_PIN, E32_AUX_PIN, &device)) {printf("Failed to initialize E32 module\n"); return 1;}
    if (!E32_set_mode(&device, 0, 0)) {printf("Failed to set mode\n"); E32_close(&device); return 1;}

    if (!E32_read_config(&device, &config)) {printf("Failed to read config\n"); E32_close(&device); return 1;};
    config.addh = 0x00;
    config.addl = 0x00;
    config.speed = 0x1A; // default
    config.speed = E32_FALLBACK_SPEED;
    config.channel = 0x0F; // default
    config.channel = E32_FALLBACK_CHANNEL;
    config.option = 0x44;
    if (!E32_write_config(&device, &config)) {printf("Failed to write config\n"); E32_close(&device); return 1;}
    sleep(E32_CONFIG_CHANGE_DELAY_SEC);
    if (!E32_read_config(&device, &config)) {printf("Failed to read config\n"); E32_close(&device); return 1;}

    uint8_t response[RESPONSE_BUFFER_SIZE]={0};
    size_t len;
    char input[INPUT_BUFFER_SIZE];

    system("clear");
    while(true){
        printf("====================================================================\n");
        printf("[0] Photo\n[1] Camera Exposure\n[2] Get file\n[3] List\n[4] Air rate and channel\n[5] Status\n[6] Restart\n[7] Exit\n");
        printf("====================================================================\n");
        scanf("%2047s", input);
        system("clear");
        printf("Please wait...\n\n");
        if (strcmp(input, "0")==0) {
            char photo_name[256] = {0};
            if (!build_timestamped_photo_name(photo_name, sizeof(photo_name))) {print_gse("Failed to generate local photo filename"); continue;}
            send_string(&device, "photo\r\n");
            memset(response, 0, sizeof(response));
            if (!read_file_transfer_status(&device, response, sizeof(response))) continue;
            if (!fetch_file(&device, photo_name)) continue;
        }
        else if (strcmp(input, "1")==0) {
            printf("Exposure (0-1200ms) (0 for auto exposure): ");
            char exposure_str[16]={0};
            scanf("%15s", exposure_str);
            int exposure = atoi(exposure_str);
            if (exposure < 0 || exposure > 1200) {print_gse("Invalid exposure value"); continue;}
            char command[32]={0};
            snprintf(command, sizeof(command), "exposure %d\r\n", exposure);
            send_string(&device, command);
            if (!E32_read_until_crlf(&device, response, sizeof(response), &len)) {print_gse("Failed to read response"); continue;}
            printf("%.*s\n", (int)len, response);
        }
        else if (strcmp(input, "2")==0) {
            printf("Path: ");
            char path[INPUT_BUFFER_SIZE-10]={0};
            scanf("%2037s", path);
            snprintf(input, INPUT_BUFFER_SIZE, "send %s\r\n", path);
            send_string(&device, input);
            memset(response, 0, sizeof(response));
            if (!read_file_transfer_status(&device, response, sizeof(response))) continue;
            if (!fetch_file(&device, path_basename(path))) continue;
        }
        else if (strcmp(input, "3")==0) {
            printf("Path: ");
            char path[INPUT_BUFFER_SIZE-10]={0};
            scanf("%2037s", path);
            snprintf(input, INPUT_BUFFER_SIZE, "list %s\r\n", path);
            send_string(&device, input);
            if (!E32_read_until_crlf(&device, response, sizeof(response), &len)) {print_gse("Failed to read list response"); continue;}
            printf("Files on device:\n%.*s\n", (int)len, response);
        }
        else if (strcmp(input, "4")==0) {
            printf("Air rate (2:2.4k, 3:4.8k, 4:9.6k): ");
            char air_rate_str[16]={0};
            scanf("%15s", air_rate_str);
            uint8_t air_rate = (uint8_t)atoi(air_rate_str);
            if (air_rate < 2 || air_rate > 4) {print_gse("Invalid air rate value"); continue;}
            printf("Channel (0-255) (Default %d): ", E32_FALLBACK_CHANNEL);
            char channel_str[16]={0};
            scanf("%15s", channel_str);
            uint8_t channel = (uint8_t)atoi(channel_str);
            char command[32]={0};
            snprintf(command, sizeof(command), "config %d %d\r\n", air_rate, channel);
            send_string(&device, command);

            printf("S");
            if (!change_air_rate_and_channel(&device, air_rate, channel)) {
                print_gse("Failed to change air rate and channel. Falling back to default config"); 
                change_air_rate_and_channel(&device, E32_FALLBACK_SPEED & 0x07, E32_FALLBACK_CHANNEL);
                send_string(&device, "NACK\r\n");
                continue;
            }
            printf("C");
            memset(response, 0, sizeof(response));
            printf("R");
            if (!E32_read_until_crlf(&device, response, sizeof(response), &len)) {
                print_gse("Failed to read any response for config change. Falling back to default config"); 
                change_air_rate_and_channel(&device, E32_FALLBACK_SPEED & 0x07, E32_FALLBACK_CHANNEL);
                send_string(&device, "NACK\r\n");
                continue;
            }
            if (strcmp((char*)response, "ACK") != 0) {
                print_gse("Failed to read ACK response for config change. Falling back to default config");
                change_air_rate_and_channel(&device, E32_FALLBACK_SPEED & 0x07, E32_FALLBACK_CHANNEL);
                send_string(&device, "NACK\r\n");
                continue;
            }
            printf("A");
            send_string(&device, "ACK\r\n");
            printf("R");
            if (!E32_read_until_crlf(&device, response, sizeof(response), &len)) {
                print_gse("Failed to read final response for config change. Falling back to default config");
                change_air_rate_and_channel(&device, E32_FALLBACK_SPEED & 0x07, E32_FALLBACK_CHANNEL);
                send_string(&device, "NACK\r\n");
                continue;
            }
            if (strcmp((char*)response, "ACK") != 0) {
                print_gse("Failed to read ACK response for config change. Falling back to default config");
                change_air_rate_and_channel(&device, E32_FALLBACK_SPEED & 0x07, E32_FALLBACK_CHANNEL);
                send_string(&device, "NACK\r\n");
                continue;
            }
            printf("A\n");
            printf("Config changed successfully\n");
        }
        else if (strcmp(input, "5")==0) {
            send_string(&device, "status\r\n");
            if (!E32_read_until_crlf(&device, response, sizeof(response), &len)) {print_gse("Failed to read status response"); continue;}
            printf("Status of device:\n%.*s\n", (int)len, response);
        }
        else if (strcmp(input, "6")==0) {
            send_string(&device, "restart\r\n");
            if (!E32_read_until_crlf(&device, response, sizeof(response), &len)) {print_gse("Failed to read restart response"); continue;}
            printf("Restart response:\n%.*s\n", (int)len, response);
        }
        else if (strcmp(input, "7")==0) break;
        else printf("Invalid menu option\n");
    }

    E32_close(&device);

    return 0;
}