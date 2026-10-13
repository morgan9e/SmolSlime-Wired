/*
 * IMU probe for the ProMicro stacked SPI tracker: is there an IMU answering on SPI?
 *
 * Uses the pins of promicro_uf2_stackedspi.dts, driven directly (bit-banged SPI, so no driver or
 * devicetree can hide a fault).
 *
 * Starts in hold mode: IMU powered (VCC P0.17, GND P0.20), CS high (I2C mode), SCK/MOSI/MISO
 * released, so another board can talk I2C on the SCL/SDA pads. Console keys on USB serial:
 * 'p' run the SPI probe once (drives SCK/MOSI, disconnect the other board first), 'h' back to
 * hold, 'd' UF2 bootloader. Any reset or crash also lands in the UF2 bootloader.
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/fatal.h>
#include <hal/nrf_gpio.h>

#define PIN_MISO NRF_GPIO_PIN_MAP(0, 11)
#define PIN_MOSI NRF_GPIO_PIN_MAP(0, 24)
#define PIN_SCK  NRF_GPIO_PIN_MAP(0, 22)
#define PIN_CS   NRF_GPIO_PIN_MAP(1, 0)
#define PIN_VCC  NRF_GPIO_PIN_MAP(0, 17)
#define PIN_GND  NRF_GPIO_PIN_MAP(0, 20)
#define PIN_INT  NRF_GPIO_PIN_MAP(0, 2)

static const struct {
	uint8_t reg, id;
	const char *name;
} known[] = {
	{0x0F, 0x70, "LSM6DSV"},       {0x0F, 0x71, "LSM6DSV16B"},  {0x0F, 0x6C, "LSM6DSO/ISM330DHCX"},
	{0x0F, 0x6A, "LSM6DS3TR-C/LSM6DSL/ISM330DLC"}, {0x0F, 0x6B, "ISM330DHCX/LSM6DSOX"},
	{0x0F, 0x22, "LSM6DSR (0x6B alt)"},
	{0x75, 0x47, "ICM-42688-P"},   {0x75, 0x67, "ICM-42686"},   {0x75, 0x6F, "ICM-42670"},
	{0x72, 0xE9, "ICM-45686"},     {0x72, 0xE5, "ICM-45605"},
	{0x00, 0x24, "BMI270"},
};

static int mode; /* SPI mode 0 or 3 */

static void delay(void) { k_busy_wait(5); } /* ~100 kHz */

static uint8_t xfer(uint8_t out)
{
	uint8_t in = 0;

	for (int i = 7; i >= 0; i--) {
		nrf_gpio_pin_write(PIN_MOSI, (out >> i) & 1);
		if (mode == 0) {
			delay();
			nrf_gpio_pin_set(PIN_SCK);
			in |= nrf_gpio_pin_read(PIN_MISO) << i;
			delay();
			nrf_gpio_pin_clear(PIN_SCK);
		} else {
			nrf_gpio_pin_clear(PIN_SCK);
			delay();
			nrf_gpio_pin_set(PIN_SCK);
			in |= nrf_gpio_pin_read(PIN_MISO) << i;
			delay();
		}
	}
	return in;
}

/* BMI270 returns a dummy byte before the data, so read two and keep both */
static void read_reg(uint8_t reg, uint8_t out[2])
{
	nrf_gpio_pin_write(PIN_SCK, mode == 3);
	nrf_gpio_pin_clear(PIN_CS);
	delay();
	xfer(0x80 | reg);
	out[0] = xfer(0);
	out[1] = xfer(0);
	nrf_gpio_pin_set(PIN_CS);
	delay();
}

static void miso_level(const char *label)
{
	nrf_gpio_cfg_input(PIN_MISO, NRF_GPIO_PIN_PULLDOWN);
	k_busy_wait(200);
	int down = nrf_gpio_pin_read(PIN_MISO);
	nrf_gpio_cfg_input(PIN_MISO, NRF_GPIO_PIN_PULLUP);
	k_busy_wait(200);
	int up = nrf_gpio_pin_read(PIN_MISO);
	nrf_gpio_cfg_input(PIN_MISO, NRF_GPIO_PIN_NOPULL);
	printk("  MISO with %s: pull-down reads %d, pull-up reads %d -> %s\n", label, down, up,
	       down == up ? (down ? "driven HIGH" : "driven LOW") : "floating (nothing drives it)");
}

static void power_imu(bool on)
{
	/* stackedspi: IMU VCC from P0.17 (high drive), IMU GND through P0.20 */
	nrf_gpio_cfg(PIN_GND, NRF_GPIO_PIN_DIR_OUTPUT, NRF_GPIO_PIN_INPUT_DISCONNECT,
		     NRF_GPIO_PIN_NOPULL, NRF_GPIO_PIN_H0H1, NRF_GPIO_PIN_NOSENSE);
	nrf_gpio_pin_clear(PIN_GND);
	nrf_gpio_cfg(PIN_VCC, NRF_GPIO_PIN_DIR_OUTPUT, NRF_GPIO_PIN_INPUT_DISCONNECT,
		     NRF_GPIO_PIN_NOPULL, NRF_GPIO_PIN_H0H1, NRF_GPIO_PIN_NOSENSE);
	nrf_gpio_pin_write(PIN_VCC, on);
}

static void probe(void)
{
	bool found = false;

	printk("\n=== IMU probe (uptime %lld s) ===\n", k_uptime_get() / 1000);
	printk("  pins: MISO P0.11  MOSI P0.24  SCK P0.22  CS P1.00  VCC P0.17  GND P0.20\n");

	nrf_gpio_pin_set(PIN_CS);
	miso_level("CS high (IMU deselected)");
	nrf_gpio_pin_clear(PIN_CS);
	miso_level("CS low (IMU selected)");
	nrf_gpio_pin_set(PIN_CS);
	printk("  INT1 (P0.02) reads %d\n", nrf_gpio_pin_read(PIN_INT));

	for (mode = 3; mode >= 0; mode -= 3) {
		uint8_t r0f[2], r75[2], r72[2], r00[2];

		read_reg(0x0F, r0f);
		read_reg(0x75, r75);
		read_reg(0x72, r72);
		read_reg(0x00, r00);
		printk("  SPI mode %d: reg 0x0F=%02X  0x75=%02X  0x72=%02X  0x00=%02X %02X\n", mode,
		       r0f[0], r75[0], r72[0], r00[0], r00[1]);
		for (int i = 0; i < ARRAY_SIZE(known); i++) {
			uint8_t got = known[i].reg == 0x0F ? r0f[0] : known[i].reg == 0x75 ? r75[0]
				    : known[i].reg == 0x72 ? r72[0] : r00[1];
			if (got == known[i].id) {
				printk("  -> FOUND %s (reg 0x%02X = 0x%02X) in mode %d\n", known[i].name,
				       known[i].reg, got, mode);
				found = true;
			}
		}
	}
	if (!found) {
		printk("  -> NO IMU answered. All FF = MISO stuck high/no chip; all 00 = MISO stuck low.\n");
	}
}

static void release_bus(void)
{
	nrf_gpio_cfg_default(PIN_SCK);
	nrf_gpio_cfg_default(PIN_MOSI);
	nrf_gpio_cfg_default(PIN_MISO);
	nrf_gpio_cfg_output(PIN_CS);
	nrf_gpio_pin_set(PIN_CS); /* CS high: I2C mode */
}

static void drive_bus(void)
{
	nrf_gpio_cfg_output(PIN_SCK);
	nrf_gpio_pin_set(PIN_SCK);
	nrf_gpio_cfg_output(PIN_MOSI);
	nrf_gpio_cfg_input(PIN_MISO, NRF_GPIO_PIN_NOPULL);
}

/* A crash reboots into the UF2 bootloader, never back into this app */
void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	ARG_UNUSED(reason);
	ARG_UNUSED(esf);
	NRF_POWER->GPREGRET = 0x57;
	NVIC_SystemReset();
}

int main(void)
{
	const struct device *con = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
	bool hold = true;

	/* Adafruit bootloader: any later reset (button, crash) stays in UF2 mode */
	NRF_POWER->GPREGRET = 0x57;
	usb_enable(NULL);

	release_bus();
	nrf_gpio_cfg_input(PIN_INT, NRF_GPIO_PIN_NOPULL);

	/* fresh power-on reset of the IMU */
	power_imu(false);
	k_msleep(200);
	power_imu(true);
	k_msleep(100);

	while (1) {
		if (hold) {
			printk("HOLD: IMU powered (VCC P0.17 high, GND P0.20 low), CS high, "
			       "SCK/MOSI/MISO released. Keys: p=SPI probe, h=hold, d=bootloader\n");
		}
		for (int i = 0; i < 20; i++) {
			unsigned char c;

			if (uart_poll_in(con, &c) != 0) {
				k_msleep(100);
				continue;
			}
			if (c == 'd' || c == 'D') {
				printk("Rebooting into UF2 bootloader\n");
				k_msleep(100);
				sys_reboot(SYS_REBOOT_COLD);
			} else if (c == 'p' || c == 'P') {
				drive_bus();
				probe();
				release_bus();
				hold = false;
			} else if (c == 'h' || c == 'H') {
				hold = true;
			}
		}
	}
}
