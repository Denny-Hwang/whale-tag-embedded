//-----------------------------------------------------------------------------
// Fake pigpio I2C implementation for host-side unit tests.
// i2cOpen() returns the device address itself as the handle, so one register
// bank per address is shared by every "open" of that device.
//-----------------------------------------------------------------------------
#include "pigpio.h"
#include "pigpio_fake.h"

#include <string.h>

#define FAKE_I2C_ADDR_COUNT 128
#define FAKE_I2C_REG_COUNT 256
#define FAKE_I2C_READ_BUFFER_SIZE 32

static uint16_t s_regs[FAKE_I2C_ADDR_COUNT][FAKE_I2C_REG_COUNT];
static uint8_t s_read_buffer[FAKE_I2C_ADDR_COUNT][FAKE_I2C_READ_BUFFER_SIZE];
static size_t s_read_len[FAKE_I2C_ADDR_COUNT];
static uint8_t s_last_command[FAKE_I2C_ADDR_COUNT];

void fake_i2c_reset(void) {
    memset(s_regs, 0, sizeof(s_regs));
    memset(s_read_buffer, 0, sizeof(s_read_buffer));
    memset(s_read_len, 0, sizeof(s_read_len));
    memset(s_last_command, 0, sizeof(s_last_command));
}

void fake_i2c_set_reg(unsigned addr, unsigned reg, uint16_t value) {
    s_regs[addr % FAKE_I2C_ADDR_COUNT][reg % FAKE_I2C_REG_COUNT] = value;
}

uint16_t fake_i2c_get_reg(unsigned addr, unsigned reg) {
    return s_regs[addr % FAKE_I2C_ADDR_COUNT][reg % FAKE_I2C_REG_COUNT];
}

void fake_i2c_set_read_buffer(unsigned addr, const uint8_t *data, size_t len) {
    if (len > FAKE_I2C_READ_BUFFER_SIZE) {
        len = FAKE_I2C_READ_BUFFER_SIZE;
    }
    memcpy(s_read_buffer[addr % FAKE_I2C_ADDR_COUNT], data, len);
    s_read_len[addr % FAKE_I2C_ADDR_COUNT] = len;
}

uint8_t fake_i2c_last_command(unsigned addr) {
    return s_last_command[addr % FAKE_I2C_ADDR_COUNT];
}

/******************************** pigpio API *********************************/
int i2cOpen(unsigned i2cBus, unsigned i2cAddr, unsigned i2cFlags) {
    (void)i2cBus;
    (void)i2cFlags;
    return (int)(i2cAddr % FAKE_I2C_ADDR_COUNT);
}

int i2cClose(unsigned handle) {
    (void)handle;
    return 0;
}

int i2cWriteByte(unsigned handle, unsigned bVal) {
    s_last_command[handle % FAKE_I2C_ADDR_COUNT] = (uint8_t)bVal;
    return 0;
}

int i2cReadByteData(unsigned handle, unsigned i2cReg) {
    return (int)(s_regs[handle % FAKE_I2C_ADDR_COUNT][i2cReg % FAKE_I2C_REG_COUNT] & 0xFF);
}

int i2cWriteByteData(unsigned handle, unsigned i2cReg, unsigned bVal) {
    s_regs[handle % FAKE_I2C_ADDR_COUNT][i2cReg % FAKE_I2C_REG_COUNT] = (uint16_t)(bVal & 0xFF);
    return 0;
}

int i2cReadWordData(unsigned handle, unsigned i2cReg) {
    return (int)s_regs[handle % FAKE_I2C_ADDR_COUNT][i2cReg % FAKE_I2C_REG_COUNT];
}

int i2cWriteWordData(unsigned handle, unsigned i2cReg, unsigned wVal) {
    s_regs[handle % FAKE_I2C_ADDR_COUNT][i2cReg % FAKE_I2C_REG_COUNT] = (uint16_t)(wVal & 0xFFFF);
    return 0;
}

int i2cReadDevice(unsigned handle, char *buf, unsigned count) {
    unsigned addr = handle % FAKE_I2C_ADDR_COUNT;
    unsigned to_copy = (count < s_read_len[addr]) ? count : (unsigned)s_read_len[addr];
    memcpy(buf, s_read_buffer[addr], to_copy);
    return (int)count;
}
