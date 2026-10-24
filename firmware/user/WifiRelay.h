#ifndef __WIFI_RELAY_H
#define __WIFI_RELAY_H

#include "stm32f10x.h"

/* Shared by the producer and transport; includes the terminating NUL. */
#define WIFI_TELEMETRY_SIZE 768U
#define WIFI_COMMAND_BUFFER_SIZE 256U

void WifiRelay_Init(void);
void WifiRelay_Service(const char *telemetry);
u8 WifiRelay_IsOnline(void);
u8 WifiRelay_GetCommand(char *buffer, u16 size, u32 *command_id);
void WifiRelay_SendAck(u32 command_id, const char *result);

typedef struct
{
    u8 state;
    u8 online;
    u8 got_ip;
    u8 tcp_connected;
    u8 last_event;
    u8 last_error;
    u16 last_http_status;
    u16 reset_count;
    u16 join_count;
    u16 tcp_count;
    u16 http_count;
    u16 http_ok_count;
    u16 http_error_count;
    u16 health_count;
    u16 health_ok_count;
    u16 health_error_count;
    u16 telemetry_count;
    u16 telemetry_ok_count;
    u16 telemetry_error_count;
    u16 uart_overflow_count;
} WifiRelayDiagnostics;

enum
{
    WIFI_DIAG_STATE_RESET = 0,
    WIFI_DIAG_STATE_READY,
    WIFI_DIAG_STATE_AT,
    WIFI_DIAG_STATE_ECHO,
    WIFI_DIAG_STATE_MODE,
    WIFI_DIAG_STATE_AUTOCONN,
    WIFI_DIAG_STATE_JOIN,
    WIFI_DIAG_STATE_CIPMODE,
    WIFI_DIAG_STATE_MUX,
    WIFI_DIAG_STATE_TCP,
    WIFI_DIAG_STATE_SETTLE,
    WIFI_DIAG_STATE_ONLINE,
    WIFI_DIAG_STATE_SEND_PROMPT,
    WIFI_DIAG_STATE_HTTP_RESPONSE,
    WIFI_DIAG_STATE_CLOSE,
    WIFI_DIAG_STATE_BACKOFF
};

enum
{
    WIFI_DIAG_EVENT_NONE = 0,
    WIFI_DIAG_EVENT_RESETTING,
    WIFI_DIAG_EVENT_AT_OK,
    WIFI_DIAG_EVENT_MODE_OK,
    WIFI_DIAG_EVENT_JOINING,
    WIFI_DIAG_EVENT_GOT_IP,
    WIFI_DIAG_EVENT_JOIN_FAIL,
    WIFI_DIAG_EVENT_TCP_CONNECTING,
    WIFI_DIAG_EVENT_TCP_CONNECTED,
    WIFI_DIAG_EVENT_TCP_FAIL,
    WIFI_DIAG_EVENT_HTTP_SENDING,
    WIFI_DIAG_EVENT_HTTP_OK,
    WIFI_DIAG_EVENT_HTTP_BAD_STATUS,
    WIFI_DIAG_EVENT_HTTP_TIMEOUT,
    WIFI_DIAG_EVENT_SOCKET_CLOSED,
    WIFI_DIAG_EVENT_WIFI_LOST,
    WIFI_DIAG_EVENT_AT_TIMEOUT,
    WIFI_DIAG_EVENT_CIPSEND_FAIL,
    WIFI_DIAG_EVENT_HEALTH_OK,
    WIFI_DIAG_EVENT_HEALTH_BAD,
    WIFI_DIAG_EVENT_TELEMETRY_OK,
    WIFI_DIAG_EVENT_TELEMETRY_BAD
};

void WifiRelay_GetDiagnostics(WifiRelayDiagnostics *diagnostics);

#endif
