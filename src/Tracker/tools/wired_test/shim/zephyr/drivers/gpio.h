#pragma once
#include <stdint.h>
#include <stdbool.h>
struct gpio_dt_spec { const struct device *port; uint8_t pin; };
struct gpio_callback { int dummy; };
typedef void (*gpio_callback_handler_t)(const struct device *, struct gpio_callback *, uint32_t);
#define GPIO_DT_SPEC_GET(n, p) {0, 8}
#define GPIO_INPUT 0
#define GPIO_INT_DISABLE 0
#define GPIO_INT_EDGE_TO_ACTIVE 1
static inline bool gpio_is_ready_dt(const struct gpio_dt_spec *s) { (void)s; return true; }
static inline int gpio_pin_configure_dt(const struct gpio_dt_spec *s, int f) { (void)s; (void)f; return 0; }
static inline int gpio_pin_interrupt_configure_dt(const struct gpio_dt_spec *s, int f) { (void)s; (void)f; return 0; }
static inline void gpio_init_callback(struct gpio_callback *c, gpio_callback_handler_t h, uint32_t m) { (void)c; (void)h; (void)m; }
static inline int gpio_add_callback(const struct device *p, struct gpio_callback *c) { (void)p; (void)c; return 0; }
static inline int gpio_pin_get_raw(const struct device *p, uint8_t pin) { (void)p; (void)pin; return 1; }
