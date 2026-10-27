// Flipper Zero WiFi RPC bridge for the official WiFi Dev Board (ESP32-S2).
//
// Brings up a SoftAP, listens on TCP :2333, and tunnels the connection to the
// Flipper's RPC session over the GPIO UART using the Flipper Expansion Module
// protocol. RPC bytes ride inside expansion Data frames; the TCP side carries
// RAW RPC, which is exactly what flipper_rpc.py's --host (TcpTransport) expects.
//
// Hardware (official Flipper WiFi Dev Board, ESP32-S2-WROVER):
//   UART to Flipper : ESP GPIO43 = TX (-> Flipper RX pin14)
//                     ESP GPIO44 = RX (<- Flipper TX pin13)
//   RGB status LED  : GPIO4 blue, GPIO5 green, GPIO6 red  (board LEDs are active-LOW)
//   Console/logs    : native USB-CDC (USB-C), so UART0 pins stay free for the link
//
// REQUIRES on the Flipper: Settings -> Expansion Modules = ON (pins 13/14).

#include <string.h>
#include <sys/socket.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "lwip/sockets.h"

#define TAG "bridge"

// ---- config ----
#define AP_SSID       "flipper-bridge"
#define AP_PASS       "flipper1234"      // >=8 chars; change to taste
#define AP_CHANNEL    6
#define TCP_PORT      2333

#define LINK_UART     UART_NUM_1
#define PIN_TX        43
#define PIN_RX        44
#define BAUD_HANDSHAKE 9600
#define BAUD_RPC       115200

#define LED_BLUE      4
#define LED_GREEN     5
#define LED_RED       6

// ---- expansion protocol ----
enum { FRAME_HEARTBEAT = 1, FRAME_STATUS = 2, FRAME_BAUD = 3,
       FRAME_CONTROL = 4, FRAME_DATA = 5 };
enum { STATUS_OK = 0x00, STATUS_UNKNOWN = 0x01, STATUS_BAUD = 0x02 };
enum { CTRL_START_RPC = 0x00, CTRL_STOP_RPC = 0x01 };
#define EXP_MAX_DATA  64

static StreamBufferHandle_t s_tcp_to_flipper;   // raw RPC bytes from PC -> Flipper
static volatile int s_client = -1;              // connected TCP client fd, or -1

// ---------- LED (active-low) ----------
static void led_init(void) {
    gpio_config_t io = { .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL<<LED_BLUE)|(1ULL<<LED_GREEN)|(1ULL<<LED_RED) };
    gpio_config(&io);
    gpio_set_level(LED_BLUE, 1); gpio_set_level(LED_GREEN, 1); gpio_set_level(LED_RED, 1);
}
static void led(int b, int g, int r) {   // 1 = on
    gpio_set_level(LED_BLUE, !b); gpio_set_level(LED_GREEN, !g); gpio_set_level(LED_RED, !r);
}

// ---------- frame I/O ----------
static uint8_t xorsum(const uint8_t *d, int n) {
    uint8_t c = 0; for (int i = 0; i < n; i++) c ^= d[i]; return c;
}

// Build and send one expansion frame (content already includes any size byte).
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
static void send_baud(uint32_t b) { send_frame(FRAME_BAUD, (uint8_t*)&b, 4); } // LE both ends
static void send_control(uint8_t c) { send_frame(FRAME_CONTROL, &c, 1); }
static void send_data(const uint8_t *d, int n) {
    uint8_t c[1 + EXP_MAX_DATA]; c[0] = (uint8_t)n; memcpy(c+1, d, n);
    send_frame(FRAME_DATA, c, 1 + n);
}

// Read exactly n bytes with a deadline; returns 1 on success.
static int uart_read_exact(uint8_t *dst, int n, int to_ms) {
    int got = 0;
    while (got < n) {
        int r = uart_read_bytes(LINK_UART, dst + got, n - got, pdMS_TO_TICKS(to_ms));
        if (r <= 0) return 0;
        got += r;
    }
    return 1;
}

// Read one whole frame. Returns type (>0) or -1 on timeout/bad checksum.
// *data/*dlen receive the payload of a Data frame (dlen must point to a 64-byte buf cap).
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
    if (calc != chk) return -1;
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

    // Announce presence at 9600 until the Flipper's worker heartbeats back.
    // Send one heartbeat, then listen a while before resending: the worker enters
    // Handshake expecting a BaudRate next, and an extra heartbeat there could end it.
    int seen = 0;
    for (int i = 0; i < 40 && !seen; i++) {
        send_heartbeat();
        uint8_t p[EXP_MAX_DATA]; int pl;
        TickType_t until = xTaskGetTickCount() + pdMS_TO_TICKS(300);
        while (xTaskGetTickCount() < until) {
            if (read_frame(p, &pl, 50) == FRAME_HEARTBEAT) { seen = 1; break; }
        }
    }
    if (!seen) { ESP_LOGE(TAG, "no heartbeat from Flipper (Expansion Modules on?)"); return 0; }
    // Request the faster RPC baud; Flipper acks Status at the OLD rate, then switches.
    send_baud(BAUD_RPC);
    uint8_t p[EXP_MAX_DATA]; int pl;
    int t = read_frame(p, &pl, 300);
    if (t != FRAME_STATUS || p[0] != STATUS_OK) { ESP_LOGE(TAG, "baud not accepted (%d)", t); return 0; }
    vTaskDelay(pdMS_TO_TICKS(30));              // >= 25ms dead time
    uart_set_baudrate(LINK_UART, BAUD_RPC);

    send_control(CTRL_START_RPC);
    t = read_frame(p, &pl, 300);
    if (t != FRAME_STATUS || p[0] != STATUS_OK) { ESP_LOGE(TAG, "StartRpc refused (%d)", t); return 0; }
    ESP_LOGI(TAG, "RPC session active");
    return 1;
}

// ---------- main bridge loop ----------
static void expansion_task(void *arg) {
    led(1, 0, 0);                                // blue: waiting
    while (1) {
        if (!handshake()) { led(0,0,1); vTaskDelay(pdMS_TO_TICKS(500)); continue; }
        led(0, 1, 0);                            // green: RPC up
        TickType_t last_hb = xTaskGetTickCount();
        int awaiting_ack = 0;
        uint8_t out[EXP_MAX_DATA];

        while (1) {
            // 1) push queued PC->Flipper RPC bytes, one acked chunk at a time
            if (!awaiting_ack && s_client >= 0) {
                size_t got = xStreamBufferReceive(s_tcp_to_flipper, out, EXP_MAX_DATA, 0);
                if (got > 0) { send_data(out, got); awaiting_ack = 1; }
            }
            // 2) service one inbound frame
            uint8_t p[EXP_MAX_DATA]; int pl = 0;
            int t = read_frame(p, &pl, 20);
            if (t == FRAME_DATA) {
                send_status_ok();                // ack Flipper's data
                if (s_client >= 0) send(s_client, (char*)p, pl, 0);
            } else if (t == FRAME_STATUS) {
                awaiting_ack = 0;                // our last Data was acked
            } else if (t == FRAME_HEARTBEAT) {
                send_heartbeat();
            }
            // 3) keepalive (< 250ms timeout)
            if (xTaskGetTickCount() - last_hb > pdMS_TO_TICKS(150)) {
                send_heartbeat(); last_hb = xTaskGetTickCount();
            }
        }
    }
}

// ---------- TCP server ----------
static void tcp_task(void *arg) {
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(TCP_PORT),
                             .sin_addr.s_addr = htonl(INADDR_ANY) };
    int one = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    bind(ls, (struct sockaddr*)&a, sizeof(a));
    listen(ls, 1);
    ESP_LOGI(TAG, "TCP listening on %d", TCP_PORT);
    while (1) {
        int c = accept(ls, NULL, NULL);
        if (c < 0) continue;
        ESP_LOGI(TAG, "client connected");
        xStreamBufferReset(s_tcp_to_flipper);
        s_client = c;
        uint8_t buf[256];
        while (1) {
            int r = recv(c, (char*)buf, sizeof(buf), 0);
            if (r <= 0) break;
            xStreamBufferSend(s_tcp_to_flipper, buf, r, portMAX_DELAY);
        }
        s_client = -1; close(c);
        ESP_LOGI(TAG, "client disconnected");
    }
}

// ---------- WiFi SoftAP ----------
static void wifi_softap(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    wifi_config_t wc = { 0 };
    strcpy((char*)wc.ap.ssid, AP_SSID); wc.ap.ssid_len = strlen(AP_SSID);
    strcpy((char*)wc.ap.password, AP_PASS);
    wc.ap.channel = AP_CHANNEL; wc.ap.max_connection = 2;
    wc.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "SoftAP '%s' up; connect and reach 192.168.4.1:%d", AP_SSID, TCP_PORT);
}

void app_main(void) {
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase(); nvs_flash_init();
    }
    led_init(); led(1, 0, 0);

    uart_config_t uc = { .baud_rate = BAUD_HANDSHAKE, .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT };
    uart_driver_install(LINK_UART, 2048, 2048, 0, NULL, 0);
    uart_param_config(LINK_UART, &uc);
    uart_set_pin(LINK_UART, PIN_TX, PIN_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    s_tcp_to_flipper = xStreamBufferCreate(4096, 1);

    wifi_softap();
    xTaskCreate(tcp_task, "tcp", 4096, NULL, 5, NULL);
    xTaskCreate(expansion_task, "exp", 4096, NULL, 6, NULL);
}
