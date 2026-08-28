//-----------------------------------------------------------------------------
// Unit tests for the Keller 4LD pressure sensor driver (device/keller4ld.c),
// run against the fake pigpio I2C register map.
//-----------------------------------------------------------------------------
#include <unity.h>

#include "cetiTagApp/device/i2c.h"
#include "cetiTagApp/device/keller4ld.h"
#include "cetiTagApp/utils/error.h"
#include "pigpio_fake.h"

// status byte: (status & 0b11000100) == 0b01000000 means a valid packet
#define KELLER_STATUS_OK 0x40
#define KELLER_STATUS_BUSY 0x60

void test_valid_measurement_conversion(void) {
    // P raw = 0x5000 = 20480 -> (200/32768)*(20480-16384) = 25.0 bar
    // T raw = 0x4000 -> ((0x4000>>4)-24)*0.05-50 = 0.0 C
    const uint8_t raw[5] = {KELLER_STATUS_OK, 0x50, 0x00, 0x40, 0x00};
    fake_i2c_set_read_buffer(PRESSURE_I2C_DEV_ADDR, raw, sizeof(raw));

    double pressure_bar = -1000.0, temp_c = -1000.0;
    TEST_ASSERT_EQUAL(WT_OK, pressure_get_measurement(&pressure_bar, &temp_c));
    TEST_ASSERT_FLOAT_WITHIN(0.001, 25.0, pressure_bar);
    TEST_ASSERT_FLOAT_WITHIN(0.001, 0.0, temp_c);

    // the driver must have issued the measurement request command
    TEST_ASSERT_EQUAL_UINT8(0xAC, fake_i2c_last_command(PRESSURE_I2C_DEV_ADDR));
}

void test_raw_measurement_is_big_endian(void) {
    const uint8_t raw[5] = {KELLER_STATUS_OK, 0x12, 0x34, 0xAB, 0xCD};
    fake_i2c_set_read_buffer(PRESSURE_I2C_DEV_ADDR, raw, sizeof(raw));

    uint16_t pressure = 0, temp = 0;
    TEST_ASSERT_EQUAL(WT_OK, pressure_get_measurement_raw(&pressure, &temp));
    TEST_ASSERT_EQUAL_HEX16(0x1234, pressure);
    TEST_ASSERT_EQUAL_HEX16(0xABCD, temp);
}

void test_invalid_status_rejected(void) {
    const uint8_t raw[5] = {0x00, 0x50, 0x00, 0x40, 0x00};
    fake_i2c_set_read_buffer(PRESSURE_I2C_DEV_ADDR, raw, sizeof(raw));

    TEST_ASSERT_EQUAL(WT_RESULT(WT_DEV_PRESSURE, WT_ERR_PRESSURE_INVALID_RESPONSE),
                      pressure_get_measurement(NULL, NULL));
}

void test_busy_status_reported(void) {
    const uint8_t raw[5] = {KELLER_STATUS_BUSY, 0x50, 0x00, 0x40, 0x00};
    fake_i2c_set_read_buffer(PRESSURE_I2C_DEV_ADDR, raw, sizeof(raw));

    TEST_ASSERT_EQUAL(WT_RESULT(WT_DEV_PRESSURE, WT_ERR_PRESSURE_BUSY),
                      pressure_get_measurement(NULL, NULL));
}

void setUp(void) {
    fake_i2c_reset();
}

void tearDown(void) {
}

int main(void) {
    UNITY_BEGIN();
    printf("Keller 4LD pressure driver tests\n");
    RUN_TEST(test_valid_measurement_conversion);
    RUN_TEST(test_raw_measurement_is_big_endian);
    RUN_TEST(test_invalid_status_rejected);
    RUN_TEST(test_busy_status_reported);
    return UNITY_END();
}
