// - Commands must be terminated with \r\n
// - response for everything except send command is in ascii (read until \r\n)
// - for send/photo command, stratolink first replies with:
//   FILE_OK\r\n  or  FILE_ERR\r\n
// - after FILE_OK, file transfer uses stop-and-wait packets:
//   <seq:2><payload_size:2><payload><hash16:2>
//   payload_size == 0 means end-of-file packet.
//   receiver replies with <ACK/NACK:1><seq:2>.

#include "e32.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <signal.h>

#include <stdint.h>
#include <stdbool.h>

// E32 config
#define E32_PORTNAME "/dev/serial0"
#define E32_M0_PIN 23
#define E32_M1_PIN 24
#define E32_AUX_PIN 25

#define E32_FALLBACK_CHANNEL 0x0F
#define E32_FALLBACK_SPEED 0x1C

#define TEST_BUILD

// buffer sizes for: maximum number of args in the command, storing the received command, storing the status response string
#define MAX_ARGS 10
#define COMMAND_BUFFER_SIZE 2048
#define LIST_RESPONSE_BUFFER_SIZE 2048
#define STATUS_RESPONSE_BUFFER_SIZE 1024
#define FILE_READ_CHUNK_SIZE 32

#define E32_CONFIG_CHANGE_DELAY_SEC 1
#define MAIN_LOOP_SLEEP_TIME_US (1 * 1000)

#define LOCAL_PHOTO_W 640
#define LOCAL_PHOTO_H 360
#define LOCAL_PHOTO_Q 95
#define TX_PHOTO_W 160
#define TX_PHOTO_H 120
#define TX_PHOTO_Q 15

static E32_Config config = {0};
static uint8_t command[COMMAND_BUFFER_SIZE]={0};
static size_t command_len;

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

static inline bool capture_photo_variant(const char* path, int width, int height, int quality) {
    char cmd[512] = {0};
    int written = snprintf(cmd, sizeof(cmd), "fswebcam -r %dx%d --jpeg %d --no-banner %s >/dev/null 2>&1", width, height, quality, path);
    if (written <= 0 || (size_t)written >= sizeof(cmd)) return false;
    return system(cmd) == 0;
}

static inline bool take_photo_pair(char* high_path, size_t high_size, bool tx_hq) {
    if (!build_timestamped_photo_name(high_path, high_size)) return false;
    if (!capture_photo_variant(high_path, LOCAL_PHOTO_W, LOCAL_PHOTO_H, LOCAL_PHOTO_Q)) return false;
    if (tx_hq) {if (!capture_photo_variant("stratolink_photo/tx_photo.jpg", LOCAL_PHOTO_W, LOCAL_PHOTO_H, LOCAL_PHOTO_Q)) return false;}
    else {if (!capture_photo_variant("stratolink_photo/tx_photo.jpg", TX_PHOTO_W, TX_PHOTO_H, TX_PHOTO_Q)) return false;}
    return true;
}

static inline bool change_exposure(int exposure_ms) {
    if (exposure_ms < 0 || exposure_ms > 1200) return false;
    if (exposure_ms == 0) {
        char cmd[256] = {0};
        int written = snprintf(cmd, sizeof(cmd), "v4l2-ctl -d /dev/video0 -c auto_exposure=3 -c exposure_dynamic_framerate=1 -c gain=8 -c backlight_compensation=1 >/dev/null 2>&1");
        if (written <= 0 || (size_t)written >= sizeof(cmd)) return false;
        return system(cmd) == 0;
    }
    char cmd[256] = {0};
    int written = snprintf(cmd, sizeof(cmd), "v4l2-ctl -d /dev/video0 -c auto_exposure=1 -c exposure_dynamic_framerate=0 -c gain=0 -c backlight_compensation=0 -c exposure_time_absolute=%d >/dev/null 2>&1", exposure_ms);
    if (written <= 0 || (size_t)written >= sizeof(cmd)) return false;
    return system(cmd) == 0;
}

static inline bool change_air_rate_and_channel(E32_Device* device, uint8_t air_rate, uint8_t channel) {
    if (air_rate < 2 || air_rate > 4) return false;
    if (!E32_read_config(device, &config)) {fprintf(stderr, "Failed to read config\n"); return false;};
    config.speed = ((config.speed & 0xF8) | (air_rate & 0x07));
    config.channel = channel;
    if (!E32_write_config(device, &config)) {fprintf(stderr, "Failed to write config\n"); return false;};
    sleep(E32_CONFIG_CHANGE_DELAY_SEC);
    if (!E32_read_config(device, &config)) {fprintf(stderr, "Failed to read config\n"); return false;};
    return config.speed == ((config.speed & 0xF8) | (air_rate & 0x07)) && config.channel == channel;
}

// command -> argc, argv
static inline size_t chop_command(char* command, size_t len, char** argv, size_t max_args) {
    bool parsing_arg = false;
    size_t arg_count=0;
    for (size_t i = 0; i < len; ++i) {
        if (arg_count >= max_args) break;
        if (parsing_arg == false && command[i] != ' ') {argv[arg_count++] = command + i; parsing_arg = true;}
        else if (parsing_arg == true && command[i] == ' ' && command[i-1] != '\\') {command[i] = '\0'; parsing_arg = false;}
    }
    if (parsing_arg && len > 0) command[len] = '\0';
    return arg_count;
}

static inline void send_string(E32_Device* device, char* str){
    E32_write_bytes(device, (uint8_t*)str, strlen(str));
}

static inline void send_file_ok(E32_Device* device) {
    send_string(device, "FILE_OK\r\n");
}

static inline void send_file_err(E32_Device* device) {
    send_string(device, "FILE_ERR\r\n");
}

static inline bool send_packet_with_retry(E32_Device* device, uint16_t seq, const uint8_t* payload, uint16_t payload_size) {
    uint8_t packet[4 + FILE_READ_CHUNK_SIZE + 2] = {0};
    E32_u16_to_be(seq, &packet[0]);
    E32_u16_to_be(payload_size, &packet[2]);
    if (payload_size > 0) memcpy(&packet[4], payload, payload_size);
    const uint16_t hash = E32_hash16(packet, 4 + payload_size);
    E32_u16_to_be(hash, &packet[4 + payload_size]);
    const size_t packet_size = 4 + payload_size + 2;
    for (int attempt = 0; attempt < E32_FILE_MAX_RETRIES; ++attempt) {
        if (!E32_write_bytes(device, packet, packet_size)) return false;
        uint8_t ack[3] = {0};
        if (!E32_read_n_bytes(device, ack, sizeof(ack), sizeof(ack))) {
#ifdef TEST_BUILD
            fprintf(stderr, "send_packet_with_retry: timeout waiting for ACK for seq %u attempt %d\n", seq, attempt + 1);
#endif
            continue;
        }
        uint16_t ack_seq = E32_be_to_u16(&ack[1]);
        if (ack_seq != seq) {
#ifdef TEST_BUILD
            fprintf(stderr, "send_packet_with_retry: ACK seq mismatch got %u expected %u\n", ack_seq, seq);
#endif
            continue;
        }
        if (ack[0] == E32_FILE_ACK) return true;
        if (ack[0] == E32_FILE_NACK) {
#ifdef TEST_BUILD
            fprintf(stderr, "send_packet_with_retry: NACK for seq %u attempt %d\n", seq, attempt + 1);
#endif
            continue;
        }
#ifdef TEST_BUILD
        fprintf(stderr, "send_packet_with_retry: invalid ACK type 0x%02X\n", ack[0]);
#endif
    }
    return false;
}

static inline bool send_file(E32_Device* device, char* path){
    FILE* file_handle = fopen(path, "rb");
    if (!file_handle) return false;
    uint8_t file_chunk_buffer[FILE_READ_CHUNK_SIZE];
    size_t bytes_read;
    size_t total_sent = 0;
    uint16_t seq = 0;
    while ((bytes_read = fread(file_chunk_buffer, 1, FILE_READ_CHUNK_SIZE, file_handle)) > 0) {
        if (!send_packet_with_retry(device, seq, file_chunk_buffer, (uint16_t)bytes_read)) {fclose(file_handle); return false;}
        total_sent += bytes_read;
        seq++;
    }
    bool ok = !ferror(file_handle);
    fclose(file_handle);
    if (!ok) return false;
    if (!send_packet_with_retry(device, seq, NULL, 0)) return false;
#ifdef TEST_BUILD
    printf("send_file: sent %zu bytes\n", total_sent);
#endif
    return true;
}

// command logic, easy to add commands (with arg rules) in future
static inline void handle_command(E32_Device* device, char* command, size_t command_len){
    char* argv[MAX_ARGS]={0};
    size_t argc=chop_command(command, command_len, argv, MAX_ARGS);
    if (argc==0) {
#ifdef TEST_BUILD
        printf("no command given\n");
#endif
        send_string(device, "No command given\r\n");
        return;
    }
    else if (strcmp(argv[0], "photo")==0){
#ifdef TEST_BUILD
        printf("photo\n");
#endif
        char high_path[256] = {0};
        if (!take_photo_pair(high_path, sizeof(high_path), false)) {send_file_err(device); return;}
        send_file_ok(device);
        if (!send_file(device, "stratolink_photo/tx_photo.jpg")) {
#ifdef TEST_BUILD
            fprintf(stderr, "photo transfer failed\n");
#endif
        }
    }
    else if (strcmp(argv[0], "exposure")==0){
        if (argc!=2) {send_string(device, "Incorrect number of arguments\r\n"); return;}
#ifdef TEST_BUILD
        printf("exposure %s\n", argv[1]);
#endif
        int exposure = atoi(argv[1]);
        if (exposure < 0 || exposure > 1200) {send_string(device, "Invalid exposure value\r\n"); return;}
        if (!change_exposure(exposure)) {send_string(device, "Failed to change exposure\r\n"); return;}
        send_string(device, "Exposure changed successfully\r\n");
    }
    else if (strcmp(argv[0], "send")==0){
        if (argc!=2) {send_string(device, "Incorrect number of arguments\r\n"); return;}
#ifdef TEST_BUILD
        printf("send %s\n", argv[1]);
#endif
        FILE* test_handle = fopen(argv[1], "rb");
        if (!test_handle) {send_file_err(device); return;}
        fclose(test_handle);
        send_file_ok(device);
        if (!send_file(device, argv[1])) {
#ifdef TEST_BUILD
            fprintf(stderr, "file transfer failed for %s\n", argv[1]);
#endif
        }
    }
    else if (strcmp(argv[0], "list")==0){
        if (argc!=2) {send_string(device, "Incorrect number of arguments\r\n"); return;}
#ifdef TEST_BUILD
        printf("list %s\n", argv[1]);
#endif
        char ls_command_buffer[LIST_RESPONSE_BUFFER_SIZE];
        if (strlen(argv[1])+8>LIST_RESPONSE_BUFFER_SIZE) {send_string(device, "Path too long\r\n"); return;}
        sprintf(ls_command_buffer, "ls -l %s", argv[1]);
        FILE* ls_command_handle=popen(ls_command_buffer, "r");
        if (!ls_command_handle) {send_string(device, "Couldnt run ls\r\n"); return;}
        char ls_read_chunk_buffer[LIST_RESPONSE_BUFFER_SIZE];
        size_t bytes_read;
        while ((bytes_read = fread(ls_read_chunk_buffer, 1, LIST_RESPONSE_BUFFER_SIZE, ls_command_handle)) > 0) E32_write_bytes(device, (uint8_t*)ls_read_chunk_buffer, bytes_read);
        pclose(ls_command_handle);
        char crlf[2] = {'\r', '\n'};
        E32_write_bytes(device, (uint8_t*)crlf, 2);
    }
    else if (strcmp(argv[0], "config")==0){
        if (argc!=3) {send_string(device, "Incorrect number of arguments\r\n"); return;}
#ifdef TEST_BUILD
        printf("config %s %s\n", argv[1], argv[2]);
#endif
        uint8_t air_rate = (uint8_t)atoi(argv[1]);
        uint8_t channel = (uint8_t)atoi(argv[2]);
#ifdef TEST_BUILD
        printf("S");
#endif
        if (!change_air_rate_and_channel(device, air_rate, channel)) {
            fprintf(stderr, "Failed to change air rate and channel. Falling back to default config"); 
            change_air_rate_and_channel(device, E32_FALLBACK_SPEED & 0x07, E32_FALLBACK_CHANNEL);
            send_string(device, "NACK\r\n");
            return;
        }
#ifdef TEST_BUILD
        printf("C");
#endif
        send_string(device, "ACK\r\n");
#ifdef TEST_BUILD
        printf("R");
#endif
        memset(command, 0, COMMAND_BUFFER_SIZE);
        if (!E32_read_until_crlf(device, (uint8_t*)command, COMMAND_BUFFER_SIZE, &command_len)){
            fprintf(stderr, "Failed to read any response for config change. Falling back to default config"); 
            change_air_rate_and_channel(device, E32_FALLBACK_SPEED & 0x07, E32_FALLBACK_CHANNEL);
            send_string(device, "NACK\r\n");
            return;
        }
        if (strcmp((char*)command, "ACK") != 0) {
            fprintf(stderr, "Failed to read ACK response for config change. Falling back to default config");
            change_air_rate_and_channel(device, E32_FALLBACK_SPEED & 0x07, E32_FALLBACK_CHANNEL);
            send_string(device, "NACK\r\n");
            return;
        }
#ifdef TEST_BUILD
        printf("A\n");
#endif
        send_string(device, "ACK\r\n");
    }
    else if (strcmp(argv[0], "status")==0){
#ifdef TEST_BUILD
        printf("status\n");
#endif
        if (argc!=1) {send_string(device, "Incorrect number of arguments\r\n"); return;}
        char status_response_buffer[STATUS_RESPONSE_BUFFER_SIZE]={0};
        struct statvfs disk_info;
        struct sysinfo sys_info;
        if (statvfs("/", &disk_info)!=0 || sysinfo(&sys_info) != 0 || disk_info.f_blocks == 0 || disk_info.f_bfree > disk_info.f_blocks) {send_string(device, "Couldnt run status\r\n"); return;}
        uint8_t disk_usage=(uint8_t)(100 - ((uint64_t)disk_info.f_bfree * 100 / disk_info.f_blocks));
        uint8_t ram_usage=(uint8_t)(100 - ((uint64_t)sys_info.freeram * 100 / sys_info.totalram));
        E32_Config config={0};
        if (!E32_read_config(device, &config)) {send_string(device, "Couldnt read config\r\n"); return;}
        uint8_t channel = config.channel;
        float air_rate = 0.0f;
        switch (config.speed & 7) {
            case 0:
            case 1:
            case 2: air_rate = 2.4; break;
            case 3: air_rate = 4.8; break;
            case 4: air_rate = 9.6; break;
            case 5:
            case 6:
            case 7: air_rate = 19.2; break;
            default: break;
        }
        long status_response_buffer_len=snprintf(
            status_response_buffer, STATUS_RESPONSE_BUFFER_SIZE, "Disk usage: %d%%\nRAM usage: %d%%\nChannel: %d\nAir Rate: %.1f kbps\nConfig hexstring: %02x%02x%02x%02x%02x\r\n",
            disk_usage, ram_usage, channel, air_rate, config.addh, config.addl, config.speed, config.channel, config.option
        );
        if (status_response_buffer_len<0) {send_string(device, "Error occurred while generating status response\r\n"); return;}
        E32_write_bytes(device, (uint8_t*)status_response_buffer, (size_t)status_response_buffer_len);
    }
    else if (strcmp(argv[0], "restart")==0){
#ifdef TEST_BUILD
        printf("restart\n");
#endif
        if (argc!=1) {send_string(device, "Incorrect number of arguments\r\n"); return;}
        send_string(device, "Restarting E32\r\n");
        if (!E32_reset(device)) fprintf(stderr, "Failed to reset E32\n");
        if (system("reboot")!=0) fprintf(stderr, "Failed to reboot system\n");

    }
    else {
#ifdef TEST_BUILD
        printf("unknown command: %s\n", command);
#endif
        send_string(device, "Unknown command\r\n");
        return;
    }
}

// handle signals
volatile bool running = true;
void signal_handler(int sig) {(void)sig; running = false;}

int main() {
    mkdir("./stratolink_photo", 0777);

    E32_Device device = {0};
    if (!E32_init(E32_PORTNAME, E32_M0_PIN, E32_M1_PIN, E32_AUX_PIN, &device)) {fprintf(stderr, "Failed to initialize E32 module\n"); return 1;}
    if (!E32_set_mode(&device, 0, 0)) {fprintf(stderr, "Failed to set mode\n"); E32_close(&device); return 1;}

    if (!E32_read_config(&device, &config)) {fprintf(stderr, "Failed to read config1\n"); E32_close(&device); return 1;};
    config.addh = 0x00;
    config.addl = 0x00;
    config.speed = 0x1A; // default
    config.speed = E32_FALLBACK_SPEED;
    config.channel = 0x0F; // default
    config.channel = E32_FALLBACK_CHANNEL;
    config.option = 0x44;
    if (!E32_write_config(&device, &config)) {fprintf(stderr, "Failed to write config\n"); E32_close(&device); return 1;}
    sleep(E32_CONFIG_CHANGE_DELAY_SEC);
    if (!E32_read_config(&device, &config)) {fprintf(stderr, "Failed to read config2\n"); E32_close(&device); return 1;}
#ifdef TEST_BUILD
    printf("Config: ADDH=%02x ADDL=%02x SPEED=%02x CH=%02x OPT=%02x\n", config.addh, config.addl, config.speed, config.channel, config.option);
#endif

    // handle signals and enter main loop
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    while(running){
        memset(command, 0, COMMAND_BUFFER_SIZE);
        if (!E32_read_until_crlf(&device, (uint8_t*)command, COMMAND_BUFFER_SIZE, &command_len)) {
#ifdef TEST_BUILD
            printf("No command\n");
#endif
            usleep(MAIN_LOOP_SLEEP_TIME_US); continue;
        }
        handle_command(&device, (char*)command, command_len);
    }

    E32_close(&device);

    return 0;
}