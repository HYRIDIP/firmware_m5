/*
 * M5StickC Plus 1.1 firmware base platform (ESP-IDF, no Arduino IDE required)
 * Build:
 *   . $HOME/esp/esp-idf/export.sh
 *   idf.py set-target esp32
 *   idf.py build
 *   idf.py -p /dev/ttyUSB0 flash monitor
 *
 * Security policy:
 *   This firmware provides defensive/diagnostic Wi-Fi tooling (scanning, inventory).
 *   Offensive RF actions (deauth, jamming, disruption) are explicitly blocked.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_chip_info.h"
#include "esp_timer.h"
#include "driver/uart.h"

#define UART_PORT_NUM      UART_NUM_0
#define UART_RX_BUF        2048
#define UART_TX_BUF        0
#define MAX_SCRIPT_LINES   128
#define MAX_LINE_LEN       160
#define MAX_VARS           64
#define MAX_APS            32

static const char *TAG = "M5_PLATFORM";

typedef enum {
    SCREEN_HOME = 0,
    SCREEN_INFO,
    SCREEN_WIFI,
    SCREEN_SCRIPT,
    SCREEN_HELP,
} screen_id_t;

typedef struct {
    char name[24];
    int value;
} script_var_t;

typedef struct {
    char lines[MAX_SCRIPT_LINES][MAX_LINE_LEN];
    int count;
} script_program_t;

typedef struct {
    char ssid[33];
    int rssi;
    uint8_t channel;
    wifi_auth_mode_t auth;
} ap_entry_t;

static screen_id_t g_screen = SCREEN_HOME;
static script_var_t g_vars[MAX_VARS];
static int g_var_count = 0;
static ap_entry_t g_aps[MAX_APS];
static uint16_t g_ap_count = 0;

static void ui_print_banner(void) {
    printf("\n=============================================\n");
    printf(" M5 Platform FW (ESP-IDF)\n");
    printf(" Minimal core for community extension\n");
    printf("=============================================\n\n");
}

static void ui_print_home(void) {
    g_screen = SCREEN_HOME;
    printf("\n[HOME]\n");
    printf("  info        - device neofetch-style info\n");
    printf("  wifi scan   - passive Wi-Fi scan\n");
    printf("  script demo - run JS-like script demo\n");
    printf("  script run  - enter multi-line script mode\n");
    printf("  help        - all commands\n");
    printf("  clear       - clear terminal\n\n");
}

static const char *auth_to_str(wifi_auth_mode_t auth) {
    switch (auth) {
        case WIFI_AUTH_OPEN: return "OPEN";
        case WIFI_AUTH_WEP: return "WEP";
        case WIFI_AUTH_WPA_PSK: return "WPA";
        case WIFI_AUTH_WPA2_PSK: return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
        case WIFI_AUTH_WPA3_PSK: return "WPA3";
        default: return "OTHER";
    }
}

static void print_ascii_m5(void) {
    printf("    __  __  ____       \n");
    printf("   /  \\/  \\/ ___|      \n");
    printf("  / /\\  /\\\\__ \\      \n");
    printf(" / /  \\/  \\___) |     \n");
    printf(" \\/        \\____/      \n");
}

static void show_system_info(void) {
    g_screen = SCREEN_INFO;
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    printf("\n[NEOFETCH]\n");
    print_ascii_m5();
    printf(" device : M5StickC Plus 1.1\n");
    printf(" chip   : ESP32 cores=%d rev=%d\n", chip.cores, chip.revision);
    printf(" flash  : %lu MB\n", (unsigned long)(spi_flash_get_chip_size() / (1024 * 1024)));
    printf(" heap   : %lu bytes\n", (unsigned long)esp_get_free_heap_size());
    printf(" uptime : %llu ms\n", (unsigned long long)(esp_timer_get_time() / 1000ULL));
    printf(" sdk    : %s\n\n", esp_get_idf_version());
}

static esp_err_t wifi_init_sta_mode(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    return ESP_OK;
}

static void wifi_scan_passive(void) {
    g_screen = SCREEN_WIFI;

    wifi_scan_config_t scan_cfg = {
        .ssid = 0,
        .bssid = 0,
        .channel = 0,
        .show_hidden = true,
    };

    printf("\n[WIFI] Passive scan started...\n");
    if (esp_wifi_scan_start(&scan_cfg, true) != ESP_OK) {
        printf("[WIFI] scan failed\n");
        return;
    }

    uint16_t ap_count = MAX_APS;
    wifi_ap_record_t records[MAX_APS];
    memset(records, 0, sizeof(records));

    if (esp_wifi_scan_get_ap_records(&ap_count, records) != ESP_OK) {
        printf("[WIFI] failed to fetch AP list\n");
        return;
    }

    g_ap_count = ap_count;
    for (int i = 0; i < ap_count; i++) {
        snprintf(g_aps[i].ssid, sizeof(g_aps[i].ssid), "%s", (char*)records[i].ssid);
        g_aps[i].rssi = records[i].rssi;
        g_aps[i].channel = records[i].primary;
        g_aps[i].auth = records[i].authmode;
    }

    printf("[WIFI] Found %u APs\n", g_ap_count);
    for (int i = 0; i < g_ap_count; i++) {
        printf("  %02d | %-24s | RSSI %-4d | CH %-2d | %s\n",
               i,
               g_aps[i].ssid[0] ? g_aps[i].ssid : "<hidden>",
               g_aps[i].rssi,
               g_aps[i].channel,
               auth_to_str(g_aps[i].auth));
    }
    printf("\n");
}

static bool is_space_only(const char *s) {
    while (*s) {
        if (!isspace((unsigned char)*s)) return false;
        s++;
    }
    return true;
}

static int var_find(const char *name) {
    for (int i = 0; i < g_var_count; i++) {
        if (strcmp(g_vars[i].name, name) == 0) return i;
    }
    return -1;
}

static int var_get(const char *name) {
    int idx = var_find(name);
    if (idx < 0) return 0;
    return g_vars[idx].value;
}

static void var_set(const char *name, int v) {
    int idx = var_find(name);
    if (idx >= 0) {
        g_vars[idx].value = v;
        return;
    }
    if (g_var_count < MAX_VARS) {
        snprintf(g_vars[g_var_count].name, sizeof(g_vars[g_var_count].name), "%s", name);
        g_vars[g_var_count].value = v;
        g_var_count++;
    }
}

static void var_dump(void) {
    printf("[SCRIPT] vars: ");
    if (g_var_count == 0) {
        printf("<empty>\n");
        return;
    }
    for (int i = 0; i < g_var_count; i++) {
        printf("%s=%d", g_vars[i].name, g_vars[i].value);
        if (i + 1 < g_var_count) printf(", ");
    }
    printf("\n");
}

static int parse_int_or_var(const char *token) {
    if (!token || !token[0]) return 0;
    bool numeric = true;
    int i = 0;
    if (token[0] == '-' || token[0] == '+') i = 1;
    for (; token[i]; i++) {
        if (!isdigit((unsigned char)token[i])) {
            numeric = false;
            break;
        }
    }
    if (numeric) return atoi(token);
    return var_get(token);
}

static void trim(char *s) {
    char *start = s;
    while (*start && isspace((unsigned char)*start)) start++;
    if (start != s) memmove(s, start, strlen(start) + 1);

    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[len - 1] = '\0';
        len--;
    }
}

static bool starts_with(const char *s, const char *prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static void script_exec_line(char *line, int *pc, script_program_t *prog) {
    trim(line);
    if (!line[0] || starts_with(line, "//")) return;

    if (strstr(line, "deauth") != NULL || strstr(line, "jam") != NULL) {
        printf("[SCRIPT] blocked: offensive RF actions are not allowed\n");
        return;
    }

    if (starts_with(line, "print(")) {
        char *p1 = strchr(line, '"');
        char *p2 = strrchr(line, '"');
        if (p1 && p2 && p2 > p1) {
            char msg[96] = {0};
            size_t n = (size_t)(p2 - p1 - 1);
            if (n >= sizeof(msg)) n = sizeof(msg) - 1;
            memcpy(msg, p1 + 1, n);
            printf("%s\n", msg);
            return;
        }

        char expr[64] = {0};
        sscanf(line, "print(%63[^)])", expr);
        trim(expr);
        if (expr[0]) {
            printf("%d\n", parse_int_or_var(expr));
            return;
        }
    }

    if (starts_with(line, "let ")) {
        char name[24] = {0};
        char rhs[32] = {0};
        if (sscanf(line, "let %23[^=]=%31[^;]", name, rhs) == 2) {
            trim(name);
            trim(rhs);
            var_set(name, parse_int_or_var(rhs));
            return;
        }
    }

    if (starts_with(line, "add(")) {
        char a[24] = {0};
        char b[24] = {0};
        char out[24] = {0};
        if (sscanf(line, "add(%23[^,],%23[^,],%23[^)])", a, b, out) == 3) {
            trim(a); trim(b); trim(out);
            int res = parse_int_or_var(a) + parse_int_or_var(b);
            var_set(out, res);
            return;
        }
    }

    if (starts_with(line, "sub(")) {
        char a[24] = {0};
        char b[24] = {0};
        char out[24] = {0};
        if (sscanf(line, "sub(%23[^,],%23[^,],%23[^)])", a, b, out) == 3) {
            trim(a); trim(b); trim(out);
            int res = parse_int_or_var(a) - parse_int_or_var(b);
            var_set(out, res);
            return;
        }
    }

    if (starts_with(line, "sleep_ms(")) {
        char ms[16] = {0};
        if (sscanf(line, "sleep_ms(%15[^)])", ms) == 1) {
            int delay_ms = parse_int_or_var(ms);
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
            return;
        }
    }

    if (starts_with(line, "wifi.scan()")) {
        wifi_scan_passive();
        return;
    }

    if (starts_with(line, "goto_if_gt(")) {
        char lhs[24] = {0};
        char rhs[24] = {0};
        int target = 0;
        if (sscanf(line, "goto_if_gt(%23[^,],%23[^,],%d)", lhs, rhs, &target) == 3) {
            trim(lhs); trim(rhs);
            if (parse_int_or_var(lhs) > parse_int_or_var(rhs)) {
                if (target >= 0 && target < prog->count) {
                    *pc = target - 1;
                }
            }
            return;
        }
    }

    if (strcmp(line, "vars()") == 0) {
        var_dump();
        return;
    }

    if (strcmp(line, "help()") == 0) {
        printf("[SCRIPT] Supported JS-like API:\n");
        printf("  print(\"text\"); print(var);\n");
        printf("  let x = 5;\n");
        printf("  add(a,b,out); sub(a,b,out);\n");
        printf("  sleep_ms(250); wifi.scan(); vars();\n");
        printf("  goto_if_gt(a,b,lineIndex);\n");
        return;
    }

    printf("[SCRIPT] unknown: %s\n", line);
}

static void script_run(script_program_t *prog) {
    printf("\n[SCRIPT] Running %d lines\n", prog->count);
    for (int pc = 0; pc < prog->count; pc++) {
        char line[MAX_LINE_LEN];
        snprintf(line, sizeof(line), "%s", prog->lines[pc]);
        script_exec_line(line, &pc, prog);
    }
    printf("[SCRIPT] done\n");
    var_dump();
}

static void script_demo(void) {
    script_program_t p = {0};
    const char *demo[] = {
        "print(\"hello from js-like vm\");",
        "let a = 7;",
        "let b = 5;",
        "add(a,b,sum);",
        "print(sum);",
        "wifi.scan();",
        "print(\"demo complete\");",
    };

    p.count = sizeof(demo) / sizeof(demo[0]);
    for (int i = 0; i < p.count; i++) {
        snprintf(p.lines[i], sizeof(p.lines[i]), "%s", demo[i]);
    }
    script_run(&p);
}

static void script_interactive_mode(void) {
    g_screen = SCREEN_SCRIPT;
    printf("\n[SCRIPT EDITOR] Enter up to %d lines.\n", MAX_SCRIPT_LINES);
    printf("Type ':run' to execute, ':cancel' to exit.\n");

    script_program_t p = {0};
    uint8_t rxbuf[256];

    while (1) {
        printf("js[%02d]> ", p.count);
        int len = uart_read_bytes(UART_PORT_NUM, rxbuf, sizeof(rxbuf) - 1, pdMS_TO_TICKS(60000));
        if (len <= 0) continue;
        rxbuf[len] = '\0';

        char line[MAX_LINE_LEN] = {0};
        snprintf(line, sizeof(line), "%s", (char*)rxbuf);
        trim(line);

        if (!strcmp(line, ":cancel")) {
            printf("[SCRIPT EDITOR] canceled\n");
            return;
        }
        if (!strcmp(line, ":run")) {
            script_run(&p);
            return;
        }
        if (is_space_only(line)) continue;

        if (p.count < MAX_SCRIPT_LINES) {
            snprintf(p.lines[p.count], sizeof(p.lines[p.count]), "%s", line);
            p.count++;
        } else {
            printf("[SCRIPT EDITOR] buffer full\n");
        }
    }
}

static void print_help(void) {
    g_screen = SCREEN_HELP;
    printf("\n[HELP]\n");
    printf("Commands:\n");
    printf("  home                - show home\n");
    printf("  info                - neofetch style info\n");
    printf("  wifi scan           - passive scan only\n");
    printf("  wifi deauth         - blocked (not supported)\n");
    printf("  script demo         - run built-in JS-like demo\n");
    printf("  script run          - open interactive script input\n");
    printf("  vars                - show VM variables\n");
    printf("  clear               - ANSI clear\n");
    printf("\nSafety:\n");
    printf("  This project intentionally excludes disruption attacks.\n\n");
}

static void ansi_clear(void) {
    printf("\033[2J\033[H");
}

static void exec_command(char *cmd) {
    trim(cmd);
    if (!cmd[0]) return;

    if (!strcmp(cmd, "home")) {
        ui_print_home();
        return;
    }
    if (!strcmp(cmd, "info")) {
        show_system_info();
        return;
    }
    if (!strcmp(cmd, "wifi scan")) {
        wifi_scan_passive();
        return;
    }
    if (!strcmp(cmd, "wifi deauth") || strstr(cmd, "deauth") != NULL) {
        printf("[SECURITY] denied: deauth/jamming features are not implemented.\n");
        printf("[SECURITY] available alternative: use 'wifi scan' for diagnostics.\n");
        return;
    }
    if (!strcmp(cmd, "script demo")) {
        script_demo();
        return;
    }
    if (!strcmp(cmd, "script run")) {
        script_interactive_mode();
        return;
    }
    if (!strcmp(cmd, "help")) {
        print_help();
        return;
    }
    if (!strcmp(cmd, "vars")) {
        var_dump();
        return;
    }
    if (!strcmp(cmd, "clear")) {
        ansi_clear();
        return;
    }

    printf("[CLI] unknown command: %s\n", cmd);
}

static void cli_task(void *arg) {
    (void)arg;

    uint8_t rxbuf[256];
    while (1) {
        printf("m5> ");
        int len = uart_read_bytes(UART_PORT_NUM, rxbuf, sizeof(rxbuf) - 1, pdMS_TO_TICKS(600000));
        if (len <= 0) {
            continue;
        }
        rxbuf[len] = '\0';
        char cmd[200] = {0};
        snprintf(cmd, sizeof(cmd), "%s", (char*)rxbuf);
        exec_command(cmd);
    }
}

static void uart_init_console(void) {
    const uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };

    ESP_ERROR_CHECK(uart_driver_install(UART_PORT_NUM, UART_RX_BUF, UART_TX_BUF, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT_NUM, &uart_config));
}

void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    uart_init_console();

    ESP_LOGI(TAG, "Booting M5 platform firmware (ESP-IDF)");
    wifi_init_sta_mode();

    ui_print_banner();
    ui_print_home();
    print_help();

    xTaskCreate(cli_task, "cli_task", 8192, NULL, 5, NULL);
}
