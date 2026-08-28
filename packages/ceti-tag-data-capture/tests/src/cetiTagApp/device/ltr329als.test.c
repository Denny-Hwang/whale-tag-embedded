//-----------------------------------------------------------------------------
// Unit tests for the LTR-329ALS light sensor driver (device/ltr329als.c).
// Register addresses are datasheet constants (the driver keeps them private):
// 0x80 CONTRL, 0x85 MEAS_RATE, 0x86 PART_ID, 0x87 MANUFAC_ID,
// 0x88 DATA_CH1 (visible), 0x8A DATA_CH0 (infrared).
//-----------------------------------------------------------------------------
#include <unity.h>

#include "cetiTagApp/device/i2c.h"
#include "cetiTagApp/device/ltr329als.h"
#include "cetiTagApp/utils/error.h"
#include "pigpio_fake.h"

#define ALS_REG_CONTRL 0x80
#define ALS_REG_MEAS_RATE 0x85
#define ALS_REG_PART_ID 0x86
#define ALS_REG_MANUFAC_ID 0x87
#define ALS_REG_DATA_CH1 0x88
#define ALS_REG_DATA_CH0 0x8A

void test_wake_sets_active_and_meas_rate(void) {
    TEST_ASSERT_EQUAL(WT_OK, als_wake());

    // active bit set, gain 1 (bits 4:2 == 0)
    uint16_t contrl = fake_i2c_get_reg(ALS_I2C_DEV_ADDR, ALS_REG_CONTRL);
    TEST_ASSERT_EQUAL_HEX8(0x01, contrl & 0x1D);

    // regression for the "measurement rate never written" fix:
    // 100 ms integration (bits 5:3 == 0) + 500 ms repeat (bits 2:0 == 0b011)
    TEST_ASSERT_EQUAL_HEX8(0x03, fake_i2c_get_reg(ALS_I2C_DEV_ADDR, ALS_REG_MEAS_RATE));
}

void test_sleep_clears_active(void) {
    fake_i2c_set_reg(ALS_I2C_DEV_ADDR, ALS_REG_CONTRL, 0x01);
    TEST_ASSERT_EQUAL(WT_OK, als_sleep());
    TEST_ASSERT_EQUAL_HEX8(0x00, fake_i2c_get_reg(ALS_I2C_DEV_ADDR, ALS_REG_CONTRL) & 0x01);
}

void test_get_measurement(void) {
    fake_i2c_set_reg(ALS_I2C_DEV_ADDR, ALS_REG_DATA_CH1, 512); // visible
    fake_i2c_set_reg(ALS_I2C_DEV_ADDR, ALS_REG_DATA_CH0, 123); // infrared

    int visible = -1, infrared = -1;
    TEST_ASSERT_EQUAL(WT_OK, als_get_measurement(&visible, &infrared));
    TEST_ASSERT_EQUAL(512, visible);
    TEST_ASSERT_EQUAL(123, infrared);
}

void test_identity_registers(void) {
    fake_i2c_set_reg(ALS_I2C_DEV_ADDR, ALS_REG_MANUFAC_ID, 0x05);
    fake_i2c_set_reg(ALS_I2C_DEV_ADDR, ALS_REG_PART_ID, 0xA0);

    uint8_t manu = 0, part = 0, rev = 0xFF;
    TEST_ASSERT_EQUAL(WT_OK, als_get_manufacturer_id(&manu));
    TEST_ASSERT_EQUAL_HEX8(0x05, manu);
    TEST_ASSERT_EQUAL(WT_OK, als_get_part_id(&part, &rev));
    TEST_ASSERT_EQUAL_HEX8(0x0A, part);
    TEST_ASSERT_EQUAL_HEX8(0x00, rev);
}

void setUp(void) {
    fake_i2c_reset();
}

void tearDown(void) {
}

int main(void) {
    UNITY_BEGIN();
    printf("LTR-329ALS light sensor driver tests\n");
    RUN_TEST(test_wake_sets_active_and_meas_rate);
    RUN_TEST(test_sleep_clears_active);
    RUN_TEST(test_get_measurement);
    RUN_TEST(test_identity_registers);
    return UNITY_END();
}
