#include "stm32f10x.h"
#include "stm32f10x_usart.h"
#include "misc.h"
#include "delay.h"
#include "WifiRelay.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define WIFI_SSID               "REPLACE_WITH_WIFI_SSID"
#define WIFI_PASSWORD           "REPLACE_WITH_WIFI_PASSWORD"
#define WIFI_SERVER_HOST        "server.example.com"
#define WIFI_SERVER_PORT        18127
#define WIFI_DEVICE_ID          "WIFI_AGRI_001"
#define WIFI_DEVICE_TOKEN       "REPLACE_WITH_WIFI_AGRI_DEVICE_TOKEN"
#define WIFI_PROTOCOL            "4"

#define WIFI_RX_SIZE            1024U
#define WIFI_TICK_JOIN          30000UL
#define WIFI_TICK_TCP           8000UL
#define WIFI_TICK_HTTP          5000UL
#define WIFI_TICK_RETRY         1000UL
/*
 * First communication isolation stage.  Do not send telemetry, command
 * polling, or ACKs here: a health request has no device payload/token and
 * therefore proves only STM32->ESP8266->TCP->HTTP->server->ESP8266->STM32.
 * Set to 0 only after the health counters stay stable on the OLED.
 */
#define WIFI_DIAGNOSTIC_HEALTH_ONLY  0U
#define WIFI_TICK_HEALTH_PROBE       3000UL

typedef enum {
    WR_RESET_LOW = 0, WR_RESET_HIGH, WR_AT, WR_ECHO,
    WR_MODE, WR_AUTOCONN, WR_JOIN, WR_CIPMODE, WR_MUX,
    WR_CONNECT, WR_SETTLE, WR_ONLINE, WR_SEND_PROMPT,
    WR_WAIT_RESPONSE, WR_CLOSE, WR_BACKOFF
} RelayState;

static volatile u8 rx_buf[WIFI_RX_SIZE];
static volatile u16 rx_head, rx_tail;
static char response[WIFI_RX_SIZE];
static u16 response_len;
static RelayState state;
static u32 tick_count, deadline, telemetry_tick, command_tick;
static u8 online;
/* Transparent TCP is entered once per connection.  After that point an
 * AT+CIPSEND string would be sent to the cloud as payload data. */
static u8 transparent_active;
static u8 http_kind;
static char latest_telemetry[WIFI_TELEMETRY_SIZE];
static char pending_command[WIFI_COMMAND_BUFFER_SIZE];
static u8 command_ready;
static u32 command_id;
static char pending_ack[160];
static u8 ack_ready;
static char request_buffer[WIFI_TELEMETRY_SIZE + 256U];
static volatile u8 diag_got_ip;
static volatile u8 diag_tcp_connected;
static volatile u8 diag_last_event;
static volatile u8 diag_last_error;
static volatile u16 diag_last_http_status;
static volatile u16 diag_reset_count;
static volatile u16 diag_join_count;
static volatile u16 diag_tcp_count;
static volatile u16 diag_http_count;
static volatile u16 diag_http_ok_count;
static volatile u16 diag_http_error_count;
static volatile u16 diag_health_count;
static volatile u16 diag_health_ok_count;
static volatile u16 diag_health_error_count;
static volatile u16 diag_telemetry_count;
static volatile u16 diag_telemetry_ok_count;
static volatile u16 diag_telemetry_error_count;
static volatile u16 diag_uart_overflow_count;
static u8 health_probe_pending;

static void start_tcp_connect(void);
static void recover_tcp(u8 error_code, u8 event_code);
static void recover_wifi(u8 error_code);

static void rx_clear(void)
{
    u32 mask = __get_PRIMASK();
    __disable_irq();
    rx_head = rx_tail = 0U;
    response_len = 0U;
    response[0] = '\0';
    if (!mask) __enable_irq();
}

static void response_drain(void)
{
    u8 ch;
    while (rx_tail != rx_head) {
        ch = rx_buf[rx_tail];
        rx_tail = (u16)((rx_tail + 1U) % WIFI_RX_SIZE);
        if (response_len < WIFI_RX_SIZE - 1U) {
            response[response_len++] = (char)ch;
            response[response_len] = '\0';
        } else {
            memmove(response, response + 256, WIFI_RX_SIZE - 257);
            response_len = WIFI_RX_SIZE - 257;
            response[response_len++] = (char)ch;
            response[response_len] = '\0';
        }
    }
}

static u8 has_text(const char *text)
{
    return strstr(response, text) != 0 ? 1U : 0U;
}

static u8 has_connect_event(void)
{
    const char *p = response;
    while ((p = strstr(p, "CONNECT\r\n")) != 0) {
        if (p == response || p[-1] == '\r' || p[-1] == '\n' ||
            p[-1] == ' ' || p[-1] == ',')
            return 1U;
        p += 7;
    }
    return has_text("ALREADY CONNECTED");
}

static u8 deadline_reached(void)
{
    return ((s32)(tick_count - deadline) >= 0) ? 1U : 0U;
}

static void send_raw(const char *text)
{
    while (*text) {
        while (USART_GetFlagStatus(USART2, USART_FLAG_TXE) == RESET) {}
        USART_SendData(USART2, (u16)(u8)*text++);
    }
    while (USART_GetFlagStatus(USART2, USART_FLAG_TC) == RESET) {}
}

static void send_at(const char *text, RelayState next, u32 timeout)
{
    rx_clear();
    send_raw(text);
    send_raw("\r\n");
    state = next;
    deadline = tick_count + timeout;
}

static void send_http_payload(void)
{
    rx_clear();
    diag_last_event = WIFI_DIAG_EVENT_HTTP_SENDING;
    if (diag_http_count != 65535U) diag_http_count++;
    send_raw(request_buffer);
    state = WR_WAIT_RESPONSE;
    deadline = tick_count + WIFI_TICK_HTTP;
}

static void reset_io(u8 high)
{
    GPIO_WriteBit(GPIOA, GPIO_Pin_4, high ? Bit_SET : Bit_RESET);
}

static void start_tcp_connect(void)
{
    char command[96];
    health_probe_pending = 1U;
    sprintf(command, "AT+CIPSTART=\"TCP\",\"%s\",%d",
            WIFI_SERVER_HOST, WIFI_SERVER_PORT);
    diag_last_event = WIFI_DIAG_EVENT_TCP_CONNECTING;
    send_at(command, WR_CONNECT, WIFI_TICK_TCP);
}

/*
 * Recover only the TCP socket after a cloud/HTTP failure. Resetting the
 * ESP8266 here would make a server-side failure appear to be a WiFi drop.
 */
static void recover_tcp(u8 error_code, u8 event_code)
{
    online = 0U;
    transparent_active = 0U;
    diag_tcp_connected = 0U;
    diag_last_error = error_code;
    diag_last_event = event_code;
    http_kind = 0U;
    /*
     * The validated transport is transparent TCP mode.  Once the ESP has
     * entered that mode, "AT+CIPCLOSE" would be sent as ordinary payload
     * bytes instead of an AT command.  Reset the module and rebuild the link
     * instead of leaving the state machine permanently stuck.
     */
    rx_clear();
    state = WR_BACKOFF;
    deadline = tick_count + 1000U;
}

static void recover_wifi(u8 error_code)
{
    online = 0U;
    transparent_active = 0U;
    diag_got_ip = 0U;
    diag_tcp_connected = 0U;
    diag_last_error = error_code;
    diag_last_event = WIFI_DIAG_EVENT_WIFI_LOST;
    http_kind = 0U;
    /*
     * A Wi-Fi loss is handled by the same hard recovery path.  In
     * transparent mode an AT+CWJAP string cannot be interpreted until the
     * module leaves transparent mode, so resetting is deterministic.
     */
    rx_clear();
    state = WR_BACKOFF;
    deadline = tick_count + 1000U;
}

static u8 extract_command(const char *body)
{
    const char *start = strstr(body, "\"data\":\"");
    const char *end;
    u16 length;
    char *id_text;
    if (!start) return 0U;
    start += 8;
    end = strchr(start, '"');
    if (!end) return 0U;
    length = (u16)(end - start);
    if (length == 0U || length >= WIFI_COMMAND_BUFFER_SIZE) return 0U;
    memcpy(pending_command, start, length);
    pending_command[length] = '\0';
    id_text = strstr(pending_command, "id=");
    if (!id_text) return 0U;
    command_id = (u32)strtoul(id_text + 3, 0, 10);
    if (!command_id) return 0U;
    command_ready = 1U;
    return 1U;
}

static u8 http_ready(void)
{
    char *http;
    char *header = strstr(response, "\r\n\r\n");
    char *length = strstr(response, "Content-Length:");
    u32 body_offset, body_len;
    http = strstr(response, "HTTP/1.1 ");
    if (!http) http = strstr(response, "HTTP/1.0 ");
    if (!http) return 0U;
    header = strstr(http, "\r\n\r\n");
    length = strstr(http, "Content-Length:");
    if (!header || !length || length > header) return 0U;
    body_len = (u32)strtoul(length + 15, 0, 10);
    body_offset = (u32)(header + 4 - response);
    return response_len >= body_offset + body_len ? 1U : 0U;
}

static void process_http(void)
{
    char *http;
    char *header, *body;
    if (!http_ready()) return;
    http = strstr(response, "HTTP/1.1 ");
    if (!http) http = strstr(response, "HTTP/1.0 ");
    if (!http) return;
    diag_last_http_status = (u16)atoi(http + 9);
    if (http[9] != '2') {
        if (diag_http_error_count != 65535U) diag_http_error_count++;
        if (http_kind == 4U &&
            diag_health_error_count != 65535U)
            diag_health_error_count++;
        if (http_kind == 1U &&
            diag_telemetry_error_count != 65535U)
            diag_telemetry_error_count++;
        if (http_kind == 4U)
            diag_last_event = WIFI_DIAG_EVENT_HEALTH_BAD;
        else if (http_kind == 1U)
            diag_last_event = WIFI_DIAG_EVENT_TELEMETRY_BAD;
        recover_tcp(11U, http_kind == 4U ?
                    WIFI_DIAG_EVENT_HEALTH_BAD :
                    (http_kind == 1U ?
                     WIFI_DIAG_EVENT_TELEMETRY_BAD :
                     WIFI_DIAG_EVENT_HTTP_BAD_STATUS));
        return;
    }
    header = strstr(http, "\r\n\r\n");
    body = header + 4;
    if (http_kind == 2U) extract_command(body);
    if (http_kind == 4U) {
        if (diag_health_ok_count != 65535U) diag_health_ok_count++;
        diag_last_event = WIFI_DIAG_EVENT_HEALTH_OK;
        health_probe_pending = 0U;
        command_tick = tick_count;
    }
    else if (http_kind == 1U) {
        if (diag_telemetry_ok_count != 65535U) diag_telemetry_ok_count++;
        diag_last_event = WIFI_DIAG_EVENT_TELEMETRY_OK;
    }
    /*
     * Mark the link online only after a complete 2xx HTTP response.  This is
     * deliberately stricter than treating a TCP CONNECT or a CIPSEND prompt
     * as "通讯正常": the cloud relay must have received and parsed a
     * request.
     */
    online = 1U;
    if (http_kind != 4U)
        diag_last_event = WIFI_DIAG_EVENT_HTTP_OK;
    if (diag_http_ok_count != 65535U) diag_http_ok_count++;
    rx_clear();
    http_kind = 0U;
    state = WR_ONLINE;
}

void WifiRelay_Init(void)
{
    GPIO_InitTypeDef gpio;
    USART_InitTypeDef usart;
    NVIC_InitTypeDef nvic;
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART2, ENABLE);
    gpio.GPIO_Pin = GPIO_Pin_4;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_Init(GPIOA, &gpio);
    reset_io(0U);
    gpio.GPIO_Pin = GPIO_Pin_2;
    gpio.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_Init(GPIOA, &gpio);
    gpio.GPIO_Pin = GPIO_Pin_3;
    gpio.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOA, &gpio);
    USART_StructInit(&usart);
    usart.USART_BaudRate = 115200U;
    usart.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;
    USART_Init(USART2, &usart);
    USART_Cmd(USART2, ENABLE);
    USART_ITConfig(USART2, USART_IT_RXNE, ENABLE);
    nvic.NVIC_IRQChannel = USART2_IRQn;
    nvic.NVIC_IRQChannelPreemptionPriority = 1;
    nvic.NVIC_IRQChannelSubPriority = 1;
    nvic.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&nvic);
    rx_head = rx_tail = 0U;
    response_len = 0U;
    latest_telemetry[0] = '\0';
    pending_command[0] = '\0';
    pending_ack[0] = '\0';
    request_buffer[0] = '\0';
    command_ready = ack_ready = online = 0U;
    transparent_active = 0U;
    diag_got_ip = 0U;
    diag_tcp_connected = 0U;
    diag_last_event = WIFI_DIAG_EVENT_RESETTING;
    diag_last_error = 0U;
    diag_last_http_status = 0U;
    diag_reset_count = 0U;
    diag_join_count = 0U;
    diag_tcp_count = 0U;
    diag_http_count = 0U;
    diag_http_ok_count = 0U;
    diag_http_error_count = 0U;
    diag_health_count = 0U;
    diag_health_ok_count = 0U;
    diag_health_error_count = 0U;
    diag_telemetry_count = 0U;
    diag_telemetry_ok_count = 0U;
    diag_telemetry_error_count = 0U;
    diag_uart_overflow_count = 0U;
    health_probe_pending = 1U;
    tick_count = telemetry_tick = command_tick = 0U;
    state = WR_RESET_LOW;
    deadline = 30U;
}

void WifiRelay_Service(const char *telemetry)
{
    /* Only holds AT+CWJAP, never HTTP data. Avoid a 1 KB stack allocation. */
    char request[128];
    response_drain();
    if (telemetry && telemetry[0]) {
        strncpy(latest_telemetry, telemetry, sizeof(latest_telemetry) - 1U);
        latest_telemetry[sizeof(latest_telemetry) - 1U] = '\0';
    }
    tick_count++;
    if (state == WR_RESET_LOW) {
        if (deadline_reached()) {
            if (diag_reset_count != 65535U) diag_reset_count++;
            diag_last_event = WIFI_DIAG_EVENT_RESETTING;
            diag_got_ip = 0U;
            diag_tcp_connected = 0U;
            reset_io(1U);
            state = WR_RESET_HIGH;
            deadline = tick_count + 800U;
        }
        return;
    }
    if (state == WR_RESET_HIGH) {
        if (has_text("ready") || deadline_reached())
            send_at("AT", WR_AT, 500U);
        return;
    }
    if (state == WR_AT) {
        if (has_text("OK")) {
            diag_last_event = WIFI_DIAG_EVENT_AT_OK;
            send_at("ATE0", WR_ECHO, 500U);
        }
        else if (deadline_reached()) {
            diag_last_error = 1U;
            diag_last_event = WIFI_DIAG_EVENT_AT_TIMEOUT;
            state = WR_BACKOFF; deadline = tick_count + 1000U;
        }
        return;
    }
    if (state == WR_ECHO) {
        if (has_text("OK")) send_at("AT+CWMODE=1", WR_MODE, 500U);
        else if (deadline_reached()) {
            diag_last_error = 2U;
            diag_last_event = WIFI_DIAG_EVENT_AT_TIMEOUT;
            state = WR_BACKOFF; deadline = tick_count + 1000U;
        }
        return;
    }
    if (state == WR_MODE) {
        if (has_text("OK")) {
            diag_last_event = WIFI_DIAG_EVENT_MODE_OK;
            send_at("AT+CWAUTOCONN=0", WR_AUTOCONN, 500U);
        }
        else if (has_text("ERROR")) {
            /*
             * Older ESP-AT builds do not implement CWAUTOCONN.  It is an
             * optional setting, so continue with the explicit CWJAP instead
             * of resetting forever.
            */
            snprintf(request, sizeof(request), "AT+CWJAP=\"%s\",\"%s\"", WIFI_SSID, WIFI_PASSWORD);
            diag_last_event = WIFI_DIAG_EVENT_JOINING;
            if (diag_join_count != 65535U) diag_join_count++;
            send_at(request, WR_JOIN, WIFI_TICK_JOIN);
        } else if (deadline_reached()) {
            diag_last_error = 3U;
            diag_last_event = WIFI_DIAG_EVENT_AT_TIMEOUT;
            state = WR_BACKOFF; deadline = tick_count + 1000U;
        }
        return;
    }
    if (state == WR_AUTOCONN) {
        if (has_text("OK")) {
            snprintf(request, sizeof(request), "AT+CWJAP=\"%s\",\"%s\"", WIFI_SSID, WIFI_PASSWORD);
            diag_last_event = WIFI_DIAG_EVENT_JOINING;
            if (diag_join_count != 65535U) diag_join_count++;
            send_at(request, WR_JOIN, WIFI_TICK_JOIN);
        } else if (has_text("ERROR")) {
            /* CWAUTOCONN is optional on older ESP-AT builds. */
            snprintf(request, sizeof(request), "AT+CWJAP=\"%s\",\"%s\"", WIFI_SSID, WIFI_PASSWORD);
            diag_last_event = WIFI_DIAG_EVENT_JOINING;
            if (diag_join_count != 65535U) diag_join_count++;
            send_at(request, WR_JOIN, WIFI_TICK_JOIN);
        } else if (deadline_reached()) state = WR_BACKOFF, deadline = tick_count + 1000U;
        return;
    }
    if (state == WR_JOIN) {
        if (has_text("WIFI GOT IP")) {
            diag_got_ip = 1U;
            diag_last_event = WIFI_DIAG_EVENT_GOT_IP;
        }
        if (has_text("OK")) {
            diag_got_ip = 1U;
            diag_last_event = WIFI_DIAG_EVENT_GOT_IP;
            send_at("AT+CIPMODE=1", WR_CIPMODE, 500U);
        }
        else if (has_text("FAIL") || has_text("ERROR") || deadline_reached()) {
            diag_last_error = has_text("FAIL") ? 4U : 5U;
            diag_last_event = WIFI_DIAG_EVENT_JOIN_FAIL;
            state = WR_BACKOFF, deadline = tick_count + 1000U;
        }
        return;
    }
    if (state == WR_CIPMODE) {
        if (has_text("OK")) send_at("AT+CIPMUX=0", WR_MUX, 500U);
        else if (has_text("ERROR") || has_text("FAIL") ||
                 deadline_reached()) {
            diag_last_error = 6U;
            diag_last_event = WIFI_DIAG_EVENT_AT_TIMEOUT;
            state = WR_BACKOFF, deadline = tick_count + 1000U;
        }
        return;
    }
    if (state == WR_MUX) {
        if (has_text("OK")) start_tcp_connect();
        else if (has_text("ERROR") || has_text("FAIL") ||
                   deadline_reached()) {
            diag_last_error = 7U;
            diag_last_event = WIFI_DIAG_EVENT_AT_TIMEOUT;
            state = WR_BACKOFF, deadline = tick_count + 1000U;
        }
        return;
    }
    if (state == WR_CONNECT) {
        if (has_text("WIFI DISCONNECT")) {
            recover_wifi(8U);
        } else if (has_connect_event()) {
            diag_tcp_connected = 1U;
            diag_last_event = WIFI_DIAG_EVENT_TCP_CONNECTED;
            if (diag_tcp_count != 65535U) diag_tcp_count++;
            state = WR_SETTLE;
            deadline = tick_count + 20U;
        } else if (has_text("ERROR") || has_text("FAIL") ||
                   has_text("CLOSED") || deadline_reached()) {
            recover_tcp(8U, WIFI_DIAG_EVENT_TCP_FAIL);
        }
        return;
    }
    if (state == WR_SETTLE) {
        if (has_text("WIFI DISCONNECT")) {
            recover_wifi(9U);
        } else if (has_text("ERROR") || has_text("FAIL") ||
                   has_text("CLOSED")) {
            recover_tcp(9U, WIFI_DIAG_EVENT_TCP_FAIL);
        } else if (deadline_reached()) {
            /*
             * Enter transparent TCP mode once for this socket.  The
             * AT+CIPSEND prompt only switches the ESP8266 data path; the
             * actual HTTP request is sent after the prompt below.
             */
            online = 0U;
            /*
             * The TCP response is no longer relevant once the socket is
             * established.  Keeping it in the parser made a later diagnostic
             * page look as if an old CONNECT/CLOSED event was still active.
             */
            rx_clear();
            transparent_active = 0U;
            send_at("AT+CIPSEND", WR_SEND_PROMPT, 1500U);
        }
        return;
    }
    if (state == WR_SEND_PROMPT) {
        if (has_text(">")) {
            rx_clear();
            transparent_active = 1U;
            /*
             * A '>' prompt only proves that ESP-AT entered transparent
             * mode.  The cloud has not seen a request yet, so keep the
             * device offline until process_http() receives a complete 2xx
             * response.  This prevents the OLED/APP from showing
             * "通讯正常" while telemetry is still being rejected or timed
             * out.
             */
            online = 0U;
            diag_last_event = WIFI_DIAG_EVENT_TCP_CONNECTED;
            telemetry_tick = tick_count - 2000U;
            command_tick = tick_count;
            state = WR_ONLINE;
        } else if (has_text("WIFI DISCONNECT")) {
            recover_wifi(9U);
        } else if (has_text("ERROR") || has_text("FAIL") ||
                   has_text("CLOSED") || deadline_reached()) {
            recover_tcp(10U, WIFI_DIAG_EVENT_CIPSEND_FAIL);
        }
        return;
    }
    if (state == WR_WAIT_RESPONSE) {
        if (has_text("WIFI DISCONNECT")) {
            recover_wifi(11U);
            return;
        }
        /*
         * Parse a complete HTTP response before reacting to a trailing
         * CLOSED/SEND FAIL line. Some ESP-AT firmware appends CLOSED after
         * the server response even when the response itself is valid. The
         * old order treated that suffix as a failure and discarded a valid
         * health/telemetry result, causing an endless reconnect loop.
         */
        if (http_kind) {
            process_http();
            if (!http_kind) return;
            if (http_kind && deadline_reached()) {
                if (diag_http_error_count != 65535U)
                    diag_http_error_count++;
                recover_tcp(13U, WIFI_DIAG_EVENT_HTTP_TIMEOUT);
                return;
            }
        }
        if (has_text("CLOSED") || has_text("SEND FAIL") ||
            has_text("ERROR")) {
            recover_tcp(12U, has_text("CLOSED") ?
                        WIFI_DIAG_EVENT_SOCKET_CLOSED :
                        WIFI_DIAG_EVENT_TCP_FAIL);
            return;
        }
        return;
    }
    if (state == WR_CLOSE) {
        if (has_text("WIFI DISCONNECT")) {
            recover_wifi(14U);
        } else if (has_text("OK") || has_text("CLOSED") ||
                   has_text("ERROR") || deadline_reached()) {
            start_tcp_connect();
        }
        return;
    }
    if (state == WR_BACKOFF) {
        if (deadline_reached()) {
            reset_io(0U);
            rx_clear();
            http_kind = 0U;
            state = WR_RESET_LOW;
            deadline = tick_count + 30U;
        }
        return;
    }
    if (state != WR_ONLINE) return;

    if (has_text("WIFI DISCONNECT")) {
        recover_wifi(15U);
        return;
    }
    if (has_text("CLOSED") || has_text("SEND FAIL") ||
        has_text("link is not valid")) {
        recover_tcp(16U, has_text("CLOSED") || has_text("link is not valid") ?
                    WIFI_DIAG_EVENT_SOCKET_CLOSED :
                    WIFI_DIAG_EVENT_TCP_FAIL);
        return;
    }
    if (http_kind) return;
    /*
     * Probe the public health endpoint first after every TCP connection.
     * This endpoint needs no device token and separates:
     *   AP/TCP failure  -> no health response
     *   server reachable -> HTTP 200 on /health
     *   device auth/payload failure -> health 200, telemetry 401/400
     */
    if (health_probe_pending) {
        sprintf(request_buffer,
                "GET /health HTTP/1.1\r\n"
                "Host: %s:%d\r\nConnection: keep-alive\r\n\r\n",
                WIFI_SERVER_HOST, WIFI_SERVER_PORT);
        http_kind = 4U;
        if (diag_health_count != 65535U) diag_health_count++;
        if (transparent_active) send_http_payload();
        else recover_tcp(17U, WIFI_DIAG_EVENT_CIPSEND_FAIL);
    }
#if !WIFI_DIAGNOSTIC_HEALTH_ONLY
    else if (ack_ready) {
        sprintf(request_buffer,
                "GET /lora/device/ack?did=%s&token=%s&data=%s HTTP/1.1\r\n"
                "Host: %s:%d\r\nConnection: keep-alive\r\n\r\n",
                WIFI_DEVICE_ID, WIFI_DEVICE_TOKEN, pending_ack,
                WIFI_SERVER_HOST, WIFI_SERVER_PORT);
        http_kind = 3U; ack_ready = 0U;
        if (transparent_active) send_http_payload();
        else recover_tcp(17U, WIFI_DIAG_EVENT_CIPSEND_FAIL);
    } else if (tick_count - telemetry_tick >= 2000U &&
               latest_telemetry[0]) {
        sprintf(request_buffer,
                "GET /lora/device/telemetry?did=%s&token=%s&data=%s HTTP/1.1\r\n"
                "Host: %s:%d\r\nConnection: keep-alive\r\n\r\n",
                WIFI_DEVICE_ID, WIFI_DEVICE_TOKEN, latest_telemetry,
                WIFI_SERVER_HOST, WIFI_SERVER_PORT);
        diag_last_http_status = 0U;
        if (diag_telemetry_count != 65535U) diag_telemetry_count++;
        http_kind = 1U;
        telemetry_tick = tick_count; deadline = tick_count + WIFI_TICK_HTTP;
        if (transparent_active) send_http_payload();
        else recover_tcp(17U, WIFI_DIAG_EVENT_CIPSEND_FAIL);
    } else if (tick_count - command_tick >= 500U) {
        sprintf(request_buffer,
                "GET /lora/device/command?did=%s&token=%s HTTP/1.1\r\n"
                "Host: %s:%d\r\nConnection: keep-alive\r\n\r\n",
                WIFI_DEVICE_ID, WIFI_DEVICE_TOKEN,
                WIFI_SERVER_HOST, WIFI_SERVER_PORT);
        diag_last_http_status = 0U;
        http_kind = 2U;
        command_tick = tick_count; deadline = tick_count + WIFI_TICK_HTTP;
        if (transparent_active) send_http_payload();
        else recover_tcp(17U, WIFI_DIAG_EVENT_CIPSEND_FAIL);
    }
#else
    else if (!health_probe_pending &&
             tick_count - command_tick >= WIFI_TICK_HEALTH_PROBE) {
        health_probe_pending = 1U;
    }
#endif
}

u8 WifiRelay_IsOnline(void) { return online; }

void WifiRelay_GetDiagnostics(WifiRelayDiagnostics *diagnostics)
{
    u32 mask;
    if (diagnostics == 0) return;
    mask = __get_PRIMASK();
    __disable_irq();
    diagnostics->state = (u8)state;
    diagnostics->online = online;
    diagnostics->got_ip = diag_got_ip;
    diagnostics->tcp_connected = diag_tcp_connected;
    diagnostics->last_event = diag_last_event;
    diagnostics->last_error = diag_last_error;
    diagnostics->last_http_status = diag_last_http_status;
    diagnostics->reset_count = diag_reset_count;
    diagnostics->join_count = diag_join_count;
    diagnostics->tcp_count = diag_tcp_count;
    diagnostics->http_count = diag_http_count;
    diagnostics->http_ok_count = diag_http_ok_count;
    diagnostics->http_error_count = diag_http_error_count;
    diagnostics->health_count = diag_health_count;
    diagnostics->health_ok_count = diag_health_ok_count;
    diagnostics->health_error_count = diag_health_error_count;
    diagnostics->telemetry_count = diag_telemetry_count;
    diagnostics->telemetry_ok_count = diag_telemetry_ok_count;
    diagnostics->telemetry_error_count = diag_telemetry_error_count;
    diagnostics->uart_overflow_count = diag_uart_overflow_count;
    if (!mask) __enable_irq();
}

u8 WifiRelay_GetCommand(char *buffer, u16 size, u32 *id)
{
    u16 length;
    if (!command_ready || !buffer || size == 0U) return 0U;
    length = (u16)strlen(pending_command);
    if (length >= size) length = size - 1U;
    memcpy(buffer, pending_command, length);
    buffer[length] = '\0';
    if (id) *id = command_id;
    command_ready = 0U;
    return 1U;
}

void WifiRelay_SendAck(u32 id, const char *result)
{
    sprintf(pending_ack, "v=%s;id=%lu;slave=1;result=%s",
            WIFI_PROTOCOL, (unsigned long)id, result ? result : "failed");
    ack_ready = 1U;
}

void USART2_IRQHandler(void)
{
    u8 data;
    u16 next;
    if (USART_GetITStatus(USART2, USART_IT_RXNE) == RESET) return;
    data = (u8)USART_ReceiveData(USART2);
    next = (u16)((rx_head + 1U) % WIFI_RX_SIZE);
    if (next != rx_tail) {
        rx_buf[rx_head] = data;
        rx_head = next;
    } else if (diag_uart_overflow_count != 65535U)
        diag_uart_overflow_count++;
}
