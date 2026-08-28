//-----------------------------------------------------------------------------
// Control interface for the fake pigpio I2C implementation
// (tests/fakes/pigpio.fake.c). The fake models each 7-bit device address as a
// bank of 256 sixteen-bit registers plus an optional raw read buffer (for
// devices like the Keller 4LD that stream bytes without register addressing).
//-----------------------------------------------------------------------------
#ifndef FAKE_PIGPIO_CONTROL_H
#define FAKE_PIGPIO_CONTROL_H

#include <stddef.h>
#include <stdint.h>

// wipe every register, read buffer, and captured command
void fake_i2c_reset(void);

// register map (byte accessors use the low 8 bits of the same slot)
void fake_i2c_set_reg(unsigned addr, unsigned reg, uint16_t value);
uint16_t fake_i2c_get_reg(unsigned addr, unsigned reg);

// raw read stream returned by i2cReadDevice()
void fake_i2c_set_read_buffer(unsigned addr, const uint8_t *data, size_t len);

// last byte sent with i2cWriteByte() (e.g. the Keller measurement request)
uint8_t fake_i2c_last_command(unsigned addr);

#endif // FAKE_PIGPIO_CONTROL_H
