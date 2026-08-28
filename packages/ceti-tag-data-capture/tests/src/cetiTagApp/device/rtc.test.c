//-----------------------------------------------------------------------------
// Unit tests for the RTC driver (device/rtc.c): registers 0..3 hold a
// little-endian 32-bit second counter.
//-----------------------------------------------------------------------------
#include <unity.h>

#include "cetiTagApp/device/i2c.h"
#include "cetiTagApp/device/rtc.h"
#include "cetiTagApp/utils/error.h"
#include "pigpio_fake.h"

void test_get_count_little_endian(void) {
    fake_i2c_set_reg(RTC_I2C_DEV_ADDR, 0, 0x78);
    fake_i2c_set_reg(RTC_I2C_DEV_ADDR, 1, 0x56);
    fake_i2c_set_reg(RTC_I2C_DEV_ADDR, 2, 0x34);
    fake_i2c_set_reg(RTC_I2C_DEV_ADDR, 3, 0x12);

    uint32_t count = 0;
    TEST_ASSERT_EQUAL(WT_OK, rtc_get_count(&count));
    TEST_ASSERT_EQUAL_HEX32(0x12345678, count);
}

void test_set_count_little_endian(void) {
    TEST_ASSERT_EQUAL(WT_OK, rtc_set_count(0xAABBCCDD));

    TEST_ASSERT_EQUAL_HEX8(0xDD, fake_i2c_get_reg(RTC_I2C_DEV_ADDR, 0));
    TEST_ASSERT_EQUAL_HEX8(0xCC, fake_i2c_get_reg(RTC_I2C_DEV_ADDR, 1));
    TEST_ASSERT_EQUAL_HEX8(0xBB, fake_i2c_get_reg(RTC_I2C_DEV_ADDR, 2));
    TEST_ASSERT_EQUAL_HEX8(0xAA, fake_i2c_get_reg(RTC_I2C_DEV_ADDR, 3));
}

void test_round_trip(void) {
    TEST_ASSERT_EQUAL(WT_OK, rtc_set_count(1735689600)); // 2025-01-01 UTC
    uint32_t count = 0;
    TEST_ASSERT_EQUAL(WT_OK, rtc_get_count(&count));
    TEST_ASSERT_EQUAL_UINT32(1735689600, count);
}

void setUp(void) {
    fake_i2c_reset();
}

void tearDown(void) {
}

int main(void) {
    UNITY_BEGIN();
    printf("RTC driver tests\n");
    RUN_TEST(test_get_count_little_endian);
    RUN_TEST(test_set_count_little_endian);
    RUN_TEST(test_round_trip);
    return UNITY_END();
}
