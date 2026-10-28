// Flipper Zero WiFi RPC bridge for the official WiFi Dev Board (ESP32-S2).
//
// Brings up a SoftAP, listens on TCP :2333, and tunnels the connection to the
// Flipper's RPC session over the GPIO UART using the Flipper Expansion Module
// protocol. RPC bytes ride inside expansion Data frames; the TCP side carries
// RAW RPC, which is exactly what flipper_rpc.py's --host (TcpTransport) expects.
//
// A second TCP server on :2334 streams this firmware's debug log (the S2 USB-CDC
// console wouldn't enumerate reliably, so logs come out over WiFi instead):
//   python -c "import socket;s=socket.create_connection(('192.168.4.1',2334));
//              [print(s.recv(4096).decode('latin1'),end='') for _ in iter(int,1)]"
//
// Hardware (official Flipper WiFi Dev Board, ESP32-S2-WROVER):
//   UART to Flipper : GPIO43 = TX (-> Flipper RX pin14), GPIO44 = RX (<- pin13)
//   RGB status LED  : GPIO4 blue, GPIO5 green, GPIO6 red  (active-LOW)
// REQUIRES on the Flipper: Settings -> Expansion Modules = ON (pins 13/14).

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "lwip/sockets.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"

#define TAG "bridge"

#define AP_SSID       "flipper-bridge"
#define AP_PASS       "flipper1234"
#define AP_CHANNEL    6
#define TCP_PORT      2333
#define LOG_PORT      2334
#define CTRL_PORT     2335   // connect here to reboot into Marauder (ota_0)

#define LINK_UART     UART_NUM_1
#define PIN_TX        43
#define PIN_RX        44
#define BAUD_HANDSHAKE 9600
#define BAUD_RPC       115200

#define LED_BLUE      4
#define LED_GREEN     5
#define LED_RED       6

enum { FRAME_HEARTBEAT = 1, FRAME_STATUS = 2, FRAME_BAUD = 3,
       FRAME_CONTROL = 4, FRAME_DATA = 5 };
enum { STATUS_OK = 0x00 };
enum { CTRL_START_RPC = 0x00 };
#define EXP_MAX_DATA  64

static StreamBufferHandle_t s_tcp_to_flipper;   // raw RPC bytes PC -> Flipper
static StreamBufferHandle_t s_log_sb;           // debug log -> :2334
static SemaphoreHandle_t s_log_mtx;
static volatile int s_client = -1;              // connected RPC client fd, or -1
static volatile int s_client_gen = 0;           // bumped on each new RPC client

// ---------- debug log over WiFi ----------
static void logb(const char *fmt, ...) {
    if (!s_log_sb) return;
    char b[160];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n > (int)sizeof(b)) n = sizeof(b);
    if (xSemaphoreTake(s_log_mtx, 0) == pdTRUE) {
        xStreamBufferSend(s_log_sb, b, n, 0);     // drops if full; fine for debug
        xSemaphoreGive(s_log_mtx);
    }
}

// ---------- LED (active-low) ----------
static void led_init(void) {
    gpio_config_t io = { .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL<<LED_BLUE)|(1ULL<<LED_GREEN)|(1ULL<<LED_RED) };
    gpio_config(&io);
    gpio_set_level(LED_BLUE, 1); gpio_set_level(LED_GREEN, 1); gpio_set_level(LED_RED, 1);
}
static void led(int b, int g, int r) {
    gpio_set_level(LED_BLUE, !b); gpio_set_level(LED_GREEN, !g); gpio_set_level(LED_RED, !r);
}

// ---------- expansion frame I/O ----------
static uint8_t xorsum(const uint8_t *d, int n) {
    uint8_t c = 0; for (int i = 0; i < n; i++) c ^= d[i]; return c;
}
static void send_frame(uint8_t type, const uint8_t *content, int clen) {
    uint8_t buf[2 + EXP_MAX_DATA + 2];
    int n = 0;
    buf[n++] = type;
    for (int i = 0; i < clen; i++) buf[n++] = content[i];
    buf[n] = xorsum(buf, n); n++;
    uart_write_bytes(LINK_UART, (const char*)buf, n);
}
static void send_heartbeat(void) { send_frame(FRAME_HEARTBEAT, NULL, 0); }
static void send_status_ok(void) { uint8_t e = STATUS_OK; send_frame(FRAME_STATUS, &e, 1); }
static void send_baud(uint32_t b) { send_frame(FRAME_BAUD, (uint8_t*)&b, 4); }
static void send_control(uint8_t c) { send_frame(FRAME_CONTROL, &c, 1); }
static void send_data(const uint8_t *d, int n) {
    uint8_t c[1 + EXP_MAX_DATA]; c[0] = (uint8_t)n; memcpy(c+1, d, n);
    send_frame(FRAME_DATA, c, 1 + n);
}

static int uart_read_exact(uint8_t *dst, int n, int to_ms) {
    int got = 0;
    while (got < n) {
        int r = uart_read_bytes(LINK_UART, dst + got, n - got, pdMS_TO_TICKS(to_ms));
        if (r <= 0) return 0;
        got += r;
    }
    return 1;
}
static int read_frame(uint8_t *payload, int *plen, int to_ms) {
    uint8_t type;
    if (!uart_read_exact(&type, 1, to_ms)) return -1;
    uint8_t content[1 + EXP_MAX_DATA]; int clen = 0;
    switch (type) {
        case FRAME_HEARTBEAT: clen = 0; break;
        case FRAME_STATUS:    clen = 1; break;
        case FRAME_CONTROL:   clen = 1; break;
        case FRAME_BAUD:      clen = 4; break;
        case FRAME_DATA: {
            uint8_t sz; if (!uart_read_exact(&sz, 1, 50)) return -1;
            if (sz > EXP_MAX_DATA) return -1;
            content[0] = sz; clen = 1 + sz;
            if (sz && !uart_read_exact(content + 1, sz, 100)) return -1;
            break;
        }
        default: return -1;
    }
    if (clen && type != FRAME_DATA && !uart_read_exact(content, clen, 50)) return -1;
    uint8_t chk; if (!uart_read_exact(&chk, 1, 50)) return -1;
    uint8_t calc = type; for (int i = 0; i < clen; i++) calc ^= content[i];
    if (calc != chk) { logb("bad checksum type=%d\n", type); return -1; }
    if (type == FRAME_DATA && payload && plen) {
        *plen = content[0]; memcpy(payload, content + 1, content[0]);
    } else if (type == FRAME_STATUS && payload && plen) {
        *plen = 1; payload[0] = content[0];
    }
    return type;
}

// ---------- handshake to RpcActive ----------
static int handshake(void) {
    uart_set_baudrate(LINK_UART, BAUD_HANDSHAKE);
    uart_flush_input(LINK_UART);
    int seen = 0;
    for (int i = 0; i < 40 && !seen; i++) {
        send_heartbeat();
        uint8_t p[EXP_MAX_DATA]; int pl;
        TickType_t until = xTaskGetTickCount() + pdMS_TO_TICKS(300);
        while (xTaskGetTickCount() < until) {
            if (read_frame(p, &pl, 50) == FRAME_HEARTBEAT) { seen = 1; break; }
        }
    }
    if (!seen) { logb("handshake: no heartbeat (Expansion Modules on?)\n"); return 0; }
    send_baud(BAUD_RPC);
    uint8_t p[EXP_MAX_DATA]; int pl;
    int t = read_frame(p, &pl, 300);
    if (t != FRAME_STATUS || p[0] != STATUS_OK) { logb("handshake: baud rejected (%d)\n", t); return 0; }
    vTaskDelay(pdMS_TO_TICKS(30));
    uart_set_baudrate(LINK_UART, BAUD_RPC);
    send_control(CTRL_START_RPC);
    t = read_frame(p, &pl, 300);
    if (t != FRAME_STATUS || p[0] != STATUS_OK) { logb("handshake: StartRpc refused (%d)\n", t); return 0; }
    logb("handshake: RPC active\n");
    return 1;
}

// ---------- main bridge loop ----------
static void expansion_task(void *arg) {
    led(1, 0, 0);
    while (1) {
        if (!handshake()) { led(0,0,1); vTaskDelay(pdMS_TO_TICKS(500)); continue; }
        led(0, 1, 0);
        TickType_t last_hb = xTaskGetTickCount();
        int awaiting_ack = 0, last_gen = s_client_gen;
        uint8_t out[EXP_MAX_DATA];

        while (1) {
            // a fresh TCP client -> clear any half-finished send state
            if (s_client_gen != last_gen) { last_gen = s_client_gen; awaiting_ack = 0; logb("exp: new client, ack reset\n"); }

            if (!awaiting_ack && s_client >= 0) {
                size_t got = xStreamBufferReceive(s_tcp_to_flipper, out, EXP_MAX_DATA, 0);
                if (got > 0) { send_data(out, got); awaiting_ack = 1; logb("exp: ->flipper %dB\n", (int)got); }
            }
            uint8_t p[EXP_MAX_DATA]; int pl = 0;
            int t = read_frame(p, &pl, 20);
            if (t == FRAME_DATA) {
                send_status_ok();
                if (s_client >= 0) send(s_client, (char*)p, pl, 0);
                logb("exp: <-flipper %dB\n", pl);
            } else if (t == FRAME_STATUS) {
                awaiting_ack = 0;                 // our last Data was acked
            } else if (t == FRAME_HEARTBEAT) {
                /* Flipper's reply to our keepalive -- just consume it (no ping-pong) */
            }
            if (xTaskGetTickCount() - last_hb > pdMS_TO_TICKS(100)) {
                send_heartbeat(); last_hb = xTaskGetTickCount();
            }
        }
    }
}

// ---------- TCP RPC server ----------
static void tcp_task(void *arg) {
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(TCP_PORT),
                             .sin_addr.s_addr = htonl(INADDR_ANY) };
    int one = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    bind(ls, (struct sockaddr*)&a, sizeof(a));
    listen(ls, 1);
    logb("tcp: listening on %d\n", TCP_PORT);
    while (1) {
        int c = accept(ls, NULL, NULL);
        if (c < 0) continue;
        xStreamBufferReset(s_tcp_to_flipper);
        s_client = c;
        s_client_gen++;
        logb("tcp: client connected (gen %d)\n", s_client_gen);
        uint8_t buf[256];
        while (1) {
            int r = recv(c, (char*)buf, sizeof(buf), 0);
            if (r <= 0) break;
            xStreamBufferSend(s_tcp_to_flipper, buf, r, portMAX_DELAY);
        }
        s_client = -1; close(c);
        logb("tcp: client disconnected\n");
    }
}

// ---------- log server ----------
static void log_task(void *arg) {
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(LOG_PORT),
                             .sin_addr.s_addr = htonl(INADDR_ANY) };
    int one = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    bind(ls, (struct sockaddr*)&a, sizeof(a));
    listen(ls, 1);
    while (1) {
        int c = accept(ls, NULL, NULL);
        if (c < 0) continue;
        logb("log: client attached\n");
        char buf[256];
        while (1) {
            size_t n = xStreamBufferReceive(s_log_sb, buf, sizeof(buf), pdMS_TO_TICKS(1000));
            if (n > 0 && send(c, buf, n, 0) < 0) break;
        }
        close(c);
    }
}

// ---------- control server: hop back to Marauder (ota_0) ----------
// Any TCP connect to :2335 points otadata at ota_0 and reboots, so you can leave
// the bridge and return to Marauder WIRELESSLY (no USB, no download mode).
//   python -c "import socket; socket.create_connection(('192.168.4.1',2335)).recv(64)"
static void ctrl_task(void *arg) {
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(CTRL_PORT),
                             .sin_addr.s_addr = htonl(INADDR_ANY) };
    int one = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    bind(ls, (struct sockaddr*)&a, sizeof(a));
    listen(ls, 1);
    logb("ctrl: listening on %d (connect = boot Marauder)\n", CTRL_PORT);
    while (1) {
        int c = accept(ls, NULL, NULL);
        if (c < 0) continue;
        const esp_partition_t *m = esp_partition_find_first(
            ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
        if (!m) { const char *e = "no ota_0 partition\n"; send(c, e, strlen(e), 0); close(c); continue; }
        esp_err_t r = esp_ota_set_boot_partition(m);
        char msg[96];
        int n = snprintf(msg, sizeof(msg), "boot->ota_0 (Marauder) %s; rebooting\n",
                         r == ESP_OK ? "OK" : esp_err_to_name(r));
        send(c, msg, n, 0);
        logb("ctrl: %s", msg);
        close(c);
        if (r == ESP_OK) { vTaskDelay(pdMS_TO_TICKS(300)); esp_restart(); }
    }
}

// ---------- WiFi SoftAP ----------
// Returns 0 on success, else the 1-based index of the call that failed, so
// app_main can BLINK that number on the LED (no console or WiFi needed to report
// it). Step map: 1 netif_init, 2 event_loop, 3 wifi_init, 4 set_mode,
// 5 set_config, 6 wifi_start.
#define WCHK(x, step) do { esp_err_t _r = (x); \
    if (_r != ESP_OK) { logb("wifi: step %d fail: %s\n", step, esp_err_to_name(_r)); return step; } } while (0)
static int wifi_softap(void) {
    WCHK(esp_netif_init(), 1);
    WCHK(esp_event_loop_create_default(), 2);
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    WCHK(esp_wifi_init(&cfg), 3);
    wifi_config_t wc = { 0 };
    strcpy((char*)wc.ap.ssid, AP_SSID); wc.ap.ssid_len = strlen(AP_SSID);
    strcpy((char*)wc.ap.password, AP_PASS);
    wc.ap.channel = AP_CHANNEL; wc.ap.max_connection = 2;
    wc.ap.authmode = WIFI_AUTH_WPA2_PSK;
    WCHK(esp_wifi_set_mode(WIFI_MODE_AP), 4);
    WCHK(esp_wifi_set_config(WIFI_IF_AP, &wc), 5);
    WCHK(esp_wifi_start(), 6);
    logb("wifi: SoftAP '%s' up at 192.168.4.1 (rpc:%d log:%d)\n", AP_SSID, TCP_PORT, LOG_PORT);
    return 0;
}

// Boot LED breadcrumbs (each held ~0.6s so they're visible). The LAST colour you
// see before it settles tells you exactly how far boot got:
//   (dark, nothing)        crash BEFORE app_main (bootloader / early startup)
//   WHITE then frozen      died in nvs_flash_init
//   WHITE->BLUE frozen     died creating buffers / UART driver
//   WHITE->BLUE->GREEN     reached WiFi; GREEN frozen = wifi aborted internally
//   ...->MAGENTA blinking  wifi_softap() returned an error (caught, see :2334 log)
//   ...->YELLOW->steady    SUCCESS: AP up; expansion_task then drives blue/green/red
void app_main(void) {
    led_init();
    led(1, 1, 1);                              // WHITE: app_main entered
    vTaskDelay(pdMS_TO_TICKS(600));

    // Marauder (ota_0) shares this NVS partition; its stored data is the prime
    // suspect for breaking our WiFi init, so start from a clean NVS every boot.
    nvs_flash_erase();
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase(); nvs_flash_init();
    }
    led(1, 0, 0);                              // BLUE: nvs ok
    vTaskDelay(pdMS_TO_TICKS(600));

    s_log_mtx = xSemaphoreCreateMutex();
    s_log_sb = xStreamBufferCreate(4096, 1);
    s_tcp_to_flipper = xStreamBufferCreate(4096, 1);

    uart_config_t uc = { .baud_rate = BAUD_HANDSHAKE, .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT };
    uart_driver_install(LINK_UART, 2048, 2048, 0, NULL, 0);
    uart_param_config(LINK_UART, &uc);
    uart_set_pin(LINK_UART, PIN_TX, PIN_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    led(0, 1, 0);                              // GREEN: uart ok
    vTaskDelay(pdMS_TO_TICKS(600));

    int werr = wifi_softap();
    if (werr != 0) {
        // WiFi failed: blink RED <werr> times, pause, repeat. Count the blinks to
        // see which call failed (1 netif,2 event,3 wifi_init,4 mode,5 cfg,6 start).
        while (1) {
            for (int i = 0; i < werr; i++) {
                led(0,0,1); vTaskDelay(pdMS_TO_TICKS(250));
                led(0,0,0); vTaskDelay(pdMS_TO_TICKS(250));
            }
            vTaskDelay(pdMS_TO_TICKS(1500));
        }
    }
    led(1, 1, 0);                              // YELLOW: wifi up, starting tasks
    vTaskDelay(pdMS_TO_TICKS(600));

    const esp_partition_t *run = esp_ota_get_running_partition();
    if (run) logb("boot: running from %s @ 0x%06lx\n", run->label, (unsigned long)run->address);
    xTaskCreate(log_task,  "log",  4096, NULL, 4, NULL);
    xTaskCreate(tcp_task,  "tcp",  8192, NULL, 6, NULL);
    xTaskCreate(ctrl_task, "ctrl", 4096, NULL, 4, NULL);
    xTaskCreate(expansion_task, "exp", 8192, NULL, 5, NULL);
}
