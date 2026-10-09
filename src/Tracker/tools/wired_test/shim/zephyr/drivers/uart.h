#pragma once
#include <stdint.h>
#include <stddef.h>
struct device;
enum uart_event_type { UART_TX_DONE, UART_TX_ABORTED, UART_RX_RDY, UART_RX_BUF_REQUEST, UART_RX_BUF_RELEASED, UART_RX_DISABLED, UART_RX_STOPPED };
struct uart_event { enum uart_event_type type; union { struct { uint8_t *buf; size_t offset; size_t len; } rx; } data; };
typedef void (*uart_callback_t)(const struct device *dev, struct uart_event *evt, void *user_data);
extern uart_callback_t sim_cb;
extern uint8_t sim_tx[4096]; extern size_t sim_tx_len;
static inline int uart_callback_set(const struct device *d, uart_callback_t cb, void *u) { (void)d; (void)u; sim_cb = cb; return 0; }
static inline int uart_tx(const struct device *d, const uint8_t *b, size_t n, int32_t t) { (void)d; (void)t; memcpy(sim_tx + sim_tx_len, b, n); sim_tx_len += n; return 0; }
static inline int uart_rx_enable(const struct device *d, uint8_t *b, size_t n, int32_t t) { (void)d; (void)b; (void)n; (void)t; return 0; }
static inline int uart_rx_disable(const struct device *d) { (void)d; return 0; }
static inline int uart_rx_buf_rsp(const struct device *d, uint8_t *b, size_t n) { (void)d; (void)b; (void)n; return 0; }
