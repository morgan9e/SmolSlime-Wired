/*
	SlimeVR Code is placed under the MIT license
	Copyright (c) 2025 SlimeVR Contributors

	Permission is hereby granted, free of charge, to any person obtaining a copy
	of this software and associated documentation files (the "Software"), to deal
	in the Software without restriction, including without limitation the rights
	to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
	copies of the Software, and to permit persons to whom the Software is
	furnished to do so, subject to the following conditions:

	The above copyright notice and this permission notice shall be included in
	all copies or substantial portions of the Software.

	THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
	IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
	FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
	AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
	LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
	OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
	THE SOFTWARE.
*/
#ifndef SLIMENRF_SENSOR_INTERFACE
#define SLIMENRF_SENSOR_INTERFACE

#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/i2c.h>

enum sensor_interface_dev
{
    SENSOR_INTERFACE_DEV_IMU,
    SENSOR_INTERFACE_DEV_MAG
};
#define SENSOR_INTERFACE_DEV_COUNT 2

enum sensor_interface_spec
{
    SENSOR_INTERFACE_SPEC_SPI,
    SENSOR_INTERFACE_SPEC_I2C,
    SENSOR_INTERFACE_SPEC_EXT
};

// TODO: move all sensor devices here?

struct spi_dt_spec *sensor_interface_dev_spi[SENSOR_INTERFACE_DEV_COUNT];
struct i2c_dt_spec *sensor_interface_dev_i2c[SENSOR_INTERFACE_DEV_COUNT];
enum sensor_interface_spec sensor_interface_dev_spec[SENSOR_INTERFACE_DEV_COUNT];

// TODO: only one active spi transaction at a time
// TODO: spi burst read multiple buffers

uint8_t rx_tmp[2] = {0};
struct spi_buf tx_buf = {.len = 1};
const struct spi_buf_set tx = {.buffers = &tx_buf, .count = 1};
struct spi_buf rx_bufs[2];
const struct spi_buf_set rx = {.buffers = rx_bufs, .count = 2};

// TODO: also keep reference to sensor device drivers (such as for ext mag)

void sensor_interface_register_sensor_imu_spi(struct spi_dt_spec *dev)
{
    sensor_interface_dev_spi[SENSOR_INTERFACE_DEV_IMU] = dev;
    sensor_interface_dev_spec[SENSOR_INTERFACE_DEV_IMU] = SENSOR_INTERFACE_SPEC_SPI;
}
void sensor_interface_register_sensor_imu_i2c(struct i2c_dt_spec *dev)
{
    sensor_interface_dev_i2c[SENSOR_INTERFACE_DEV_IMU] = dev;
    sensor_interface_dev_spec[SENSOR_INTERFACE_DEV_IMU] = SENSOR_INTERFACE_SPEC_I2C;
}

// void sensor_interface_register_sensor_mag_spi(struct spi_dt_spec *dev);
void sensor_interface_register_sensor_mag_i2c(struct i2c_dt_spec *dev)
{
    sensor_interface_dev_i2c[SENSOR_INTERFACE_DEV_MAG] = dev;
    sensor_interface_dev_spec[SENSOR_INTERFACE_DEV_MAG] = SENSOR_INTERFACE_SPEC_I2C;
}
void sensor_interface_register_sensor_mag_ext(struct i2c_dt_spec *dev) // only using mag i2c dev if needed
{
    // TODO: redirect to imu, need driver implementation
}

// TODO: spi config by device
//todo fix padding

static inline int ssi_write(enum sensor_interface_dev dev, const uint8_t *buf, uint32_t num_bytes)
{
    switch (sensor_interface_dev_spec[dev])
    {
    case SENSOR_INTERFACE_SPEC_SPI:
        tx_buf.buf = buf;
        tx_buf.len = num_bytes;
        return spi_transceive_dt(sensor_interface_dev_spi[dev], &tx, NULL);
        break;
    case SENSOR_INTERFACE_SPEC_I2C:
        return i2c_write_dt(sensor_interface_dev_i2c[dev], buf, num_bytes);
        break;
    default:
        break;
    }
}
static inline int ssi_read(enum sensor_interface_dev dev, uint8_t *buf, uint32_t num_bytes)
{
    switch (sensor_interface_dev_spec[dev])
    {
    case SENSOR_INTERFACE_SPEC_SPI:
        // TODO: set length of rx_bufs[0] according to device? check bmi datasheet if needed
        // this may be zero!
        rx_bufs[0].buf = rx_tmp;
        rx_bufs[0].len = 1;
        rx_bufs[1].buf = buf;
        rx_bufs[1].len = num_bytes;
        return spi_transceive_dt(sensor_interface_dev_spi[dev], NULL, &rx);
        break;
    case SENSOR_INTERFACE_SPEC_I2C:
        return i2c_read_dt(sensor_interface_dev_i2c[dev], buf, num_bytes);
        break;
    default:
        break;
    }
}
static inline int ssi_write_read(enum sensor_interface_dev dev, const void *write_buf, size_t num_write, void *read_buf, size_t num_read)
{
    // TODO: is separate read/write better for spi?
    switch (sensor_interface_dev_spec[dev])
    {
    case SENSOR_INTERFACE_SPEC_SPI:
        // TODO: set length of rx_bufs[0] according to device + tx_buf len
        rx_bufs[0].buf = rx_tmp;
        rx_bufs[0].len = 1;
        tx_buf.buf = write_buf;
        tx_buf.len = num_write;
        rx_bufs[1].buf = read_buf;
        rx_bufs[1].len = num_read;
        return spi_transceive_dt(sensor_interface_dev_spi[dev], &tx, &rx);
        break;
    case SENSOR_INTERFACE_SPEC_I2C:
        return i2c_write_read(sensor_interface_dev_i2c[dev], write_buf, num_write, read_buf, num_read);
        break;
    default:
        break;
    }
}

static inline int ssi_burst_read(enum sensor_interface_dev dev, uint8_t start_addr, uint8_t *buf, uint32_t num_bytes)
{
    ssi_write_read(dev, &start_addr, 1, buf, num_bytes);
}
// static inline int ssi_burst_write(enum sensor_interface_dev dev, uint8_t start_addr, const uint8_t *buf, uint32_t num_bytes);
static inline int ssi_reg_read_byte(enum sensor_interface_dev dev, uint8_t reg_addr, uint8_t *value)
{
    return ssi_write_read(dev, &reg_addr, 1, value, 1);
}
static inline int ssi_reg_write_byte(enum sensor_interface_dev dev, uint8_t reg_addr, uint8_t value)
{
    uint8_t buf[2] = {reg_addr, value};
    return ssi_write(dev, buf, 2);
}
static inline int ssi_reg_update_byte(enum sensor_interface_dev dev, uint8_t reg_addr, uint8_t mask, uint8_t value)
{
    uint8_t old_value, new_value;
	int err = ssi_reg_read_byte(dev, reg_addr, &old_value);
    if (err)
        return err;
	new_value = (old_value & ~mask) | (value & mask);
	if (new_value == old_value) {
		return 0;
	}
    return ssi_reg_write_byte(dev, reg_addr, new_value);
}

static inline int ssi_reg_read_interval(enum sensor_interface_dev dev, uint8_t start_addr, uint8_t *buf, uint32_t num_bytes, uint32_t interval)
{
    // TODO: better way to handle with spi?
    int err = ssi_write(dev, &start_addr, 1); // Start read buffer
//    if (err)
//        return err;
    while (num_bytes > 0)
    {
        err |= ssi_read(dev, buf, interval);
//        if (err)
//            return err;
        buf += interval;
        num_bytes -= interval;
        if (interval > num_bytes)
            interval = num_bytes;
    }
    return err;
}
static inline int ssi_burst_read_interval(enum sensor_interface_dev dev, uint8_t start_addr, uint8_t *buf, uint32_t num_bytes, uint32_t interval)
{
    // TODO: better way to handle with spi?
    int err = 0;
    while (num_bytes > 0)
    {
        err |= ssi_burst_read(dev, start_addr, buf, interval);
//        if (err)
//            return err;
        buf += interval;
        num_bytes -= interval;
        if (interval > num_bytes)
            interval = num_bytes;
    }
    return err;
}

#endif