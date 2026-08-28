//-----------------------------------------------------------------------------
// Unit tests for the I/O expander driver (device/iox.c): read-modify-write of
// the configuration/output registers, pin reads, and argument validation.
//-----------------------------------------------------------------------------
#include <unity.h>

#include "cetiTagApp/device/i2c.h"
#include "cetiTagApp/device/iox.h"
#include "cetiTagApp/utils/error.h"
#include "pigpio_fake.h"

void test_set_mode_read_modify_write(void) {
    // all pins inputs initially (config bits = 1)
    fake_i2c_set_reg(IOX_I2C_DEV_ADDR, IOX_REG_CONFIGURATION, 0xFF);

    TEST_ASSERT_EQUAL(WT_OK, iox_set_mode(IOX_GPIO_BURNWIRE_ON, IOX_MODE_OUTPUT));
    // only the burnwire bit (pin 4) may be cleared
    TEST_ASSERT_EQUAL_HEX8(0xFF & ~(1 << IOX_GPIO_BURNWIRE_ON),
                           fake_i2c_get_reg(IOX_I2C_DEV_ADDR, IOX_REG_CONFIGURATION));

    TEST_ASSERT_EQUAL(WT_OK, iox_set_mode(IOX_GPIO_BURNWIRE_ON, IOX_MODE_INPUT));
    TEST_ASSERT_EQUAL_HEX8(0xFF, fake_i2c_get_reg(IOX_I2C_DEV_ADDR, IOX_REG_CONFIGURATION));
}

void test_get_mode(void) {
    fake_i2c_set_reg(IOX_I2C_DEV_ADDR, IOX_REG_CONFIGURATION, ~(1 << 2) & 0xFF);

    WtIoxMode mode = (WtIoxMode)-1;
    TEST_ASSERT_EQUAL(WT_OK, iox_get_mode(2, &mode));
    TEST_ASSERT_EQUAL(IOX_MODE_OUTPUT, mode);
    TEST_ASSERT_EQUAL(WT_OK, iox_get_mode(3, &mode));
    TEST_ASSERT_EQUAL(IOX_MODE_INPUT, mode);
}

void test_write_pin_preserves_other_bits(void) {
    fake_i2c_set_reg(IOX_I2C_DEV_ADDR, IOX_REG_OUTPUT, 0x81);

    TEST_ASSERT_EQUAL(WT_OK, iox_write_pin(4, 1));
    TEST_ASSERT_EQUAL_HEX8(0x91, fake_i2c_get_reg(IOX_I2C_DEV_ADDR, IOX_REG_OUTPUT));

    TEST_ASSERT_EQUAL(WT_OK, iox_write_pin(4, 0));
    TEST_ASSERT_EQUAL_HEX8(0x81, fake_i2c_get_reg(IOX_I2C_DEV_ADDR, IOX_REG_OUTPUT));
}

void test_read_pin(void) {
    fake_i2c_set_reg(IOX_I2C_DEV_ADDR, IOX_REG_INPUT, (1 << IOX_GPIO_ECG_LOD_N));

    int value = -1;
    TEST_ASSERT_EQUAL(WT_OK, iox_read_pin(IOX_GPIO_ECG_LOD_N, &value));
    TEST_ASSERT_EQUAL(1, value);
    TEST_ASSERT_EQUAL(WT_OK, iox_read_pin(IOX_GPIO_ECG_LOD_P, &value));
    TEST_ASSERT_EQUAL(0, value);
}

void test_invalid_arguments(void) {
    TEST_ASSERT_EQUAL(WT_RESULT(WT_DEV_IOX, WT_ERR_BAD_IOX_GPIO), iox_set_mode(-1, IOX_MODE_OUTPUT));
    TEST_ASSERT_EQUAL(WT_RESULT(WT_DEV_IOX, WT_ERR_BAD_IOX_GPIO), iox_set_mode(8, IOX_MODE_OUTPUT));
    TEST_ASSERT_EQUAL(WT_RESULT(WT_DEV_IOX, WT_ERR_BAD_IOX_GPIO), iox_write_pin(8, 1));
    TEST_ASSERT_EQUAL(WT_RESULT(WT_DEV_IOX, WT_ERR_BAD_IOX_GPIO), iox_read_pin(-1, NULL));
    TEST_ASSERT_EQUAL(WT_RESULT(WT_DEV_IOX, WT_ERR_BAD_IOX_MODE), iox_set_mode(0, (WtIoxMode)42));
}

void setUp(void) {
    fake_i2c_reset();
    iox_init(); // idempotent: keeps its handle after the first call
}

void tearDown(void) {
}

int main(void) {
    UNITY_BEGIN();
    printf("I/O expander driver tests\n");
    RUN_TEST(test_set_mode_read_modify_write);
    RUN_TEST(test_get_mode);
    RUN_TEST(test_write_pin_preserves_other_bits);
    RUN_TEST(test_read_pin);
    RUN_TEST(test_invalid_arguments);
    return UNITY_END();
}
