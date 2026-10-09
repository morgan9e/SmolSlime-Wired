#pragma once
static inline int gpio_set_level(int pin, int level) { (void)pin; (void)level; return 0; }
static inline int gpio_reset_pin(int pin) { (void)pin; return 0; }
#define GPIO_MODE_OUTPUT 0
static inline int gpio_set_direction(int pin, int mode) { (void)pin; (void)mode; return 0; }
