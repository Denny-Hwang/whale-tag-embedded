//-----------------------------------------------------------------------------
// Unit tests for the MAX17320 battery gauge driver (device/max17320.c):
// dual-address register routing, unit conversions, and the COMM_STAT FET
// enable/disable read-modify-write behavior.
//-----------------------------------------------------------------------------
#include <unity.h>

#include "cetiTagApp/device/i2c.h"
#include "cetiTagApp/device/max17320.h"
#include "cetiTagApp/utils/error.h"
#include "pigpio_fake.h"

void test_register_routing_by_address_range(void) {
    // registers <= 0xFF go to the lower device address
    TEST_ASSERT_EQUAL(WT_OK, max17320_write(MAX17320_REG_COMM_STAT, 0x1234));
    TEST_ASSERT_EQUAL_HEX16(0x1234, fake_i2c_get_reg(BMS_I2C_DEV_ADDR_LOWER, 0x61));

    // registers > 0xFF go to the upper device address, masked to 8 bits
    TEST_ASSERT_EQUAL(WT_OK, max17320_write(MAX17320_REG_NUVPRTTH, 0xA002)); // 0x1D0
    TEST_ASSERT_EQUAL_HEX16(0xA002, fake_i2c_get_reg(BMS_I2C_DEV_ADDR_UPPER, 0xD0));
    TEST_ASSERT_EQUAL_HEX16(0x0000, fake_i2c_get_reg(BMS_I2C_DEV_ADDR_LOWER, 0xD0));

    uint16_t read = 0;
    fake_i2c_set_reg(BMS_I2C_DEV_ADDR_UPPER, 0x3A, 0xBEEF); // 0x13A
    TEST_ASSERT_EQUAL(WT_OK, max17320_read(0x13A, &read));
    TEST_ASSERT_EQUAL_HEX16(0xBEEF, read);
}

void test_cell_voltage_conversion(void) {
    // LSB = 78.125 uV: raw 51200 -> 4.0 V. Cell 0 = reg 0xD8, cell 1 = 0xD7.
    fake_i2c_set_reg(BMS_I2C_DEV_ADDR_LOWER, 0xD8, 51200);
    fake_i2c_set_reg(BMS_I2C_DEV_ADDR_LOWER, 0xD7, 44800); // 3.5 V

    double v = 0.0;
    TEST_ASSERT_EQUAL(WT_OK, max17320_get_cell_voltage_v(0, &v));
    TEST_ASSERT_FLOAT_WITHIN(0.0001, 4.0, v);
    TEST_ASSERT_EQUAL(WT_OK, max17320_get_cell_voltage_v(1, &v));
    TEST_ASSERT_FLOAT_WITHIN(0.0001, 3.5, v);

    TEST_ASSERT_EQUAL(WT_RESULT(WT_DEV_BMS, WT_ERR_BMS_BAD_CELL_INDEX),
                      max17320_get_cell_voltage_v(MAX17320_CELL_COUNT, &v));
}

void test_current_conversion_signed(void) {
    // LSB = 1.5625 uV across R_sense (10 mOhm) -> raw * 0.15625 mA
    fake_i2c_set_reg(BMS_I2C_DEV_ADDR_LOWER, MAX17320_REG_BATT_CURRENT, 6400);
    double i_mA = 0.0;
    TEST_ASSERT_EQUAL(WT_OK, max17320_get_current_mA(&i_mA));
    TEST_ASSERT_FLOAT_WITHIN(0.001, 1000.0, i_mA);

    // negative (discharge) current: two's complement
    fake_i2c_set_reg(BMS_I2C_DEV_ADDR_LOWER, MAX17320_REG_BATT_CURRENT, (uint16_t)(-6400));
    TEST_ASSERT_EQUAL(WT_OK, max17320_get_current_mA(&i_mA));
    TEST_ASSERT_FLOAT_WITHIN(0.001, -1000.0, i_mA);
}

void test_temperature_and_soc_conversion(void) {
    // temperature LSB = 1/256 C, signed. Cell 0 temp = reg 0x13A (upper bank).
    fake_i2c_set_reg(BMS_I2C_DEV_ADDR_UPPER, 0x3A, 6400); // 25.0 C
    double t = 0.0;
    TEST_ASSERT_EQUAL(WT_OK, max17320_get_cell_temperature_c(0, &t));
    TEST_ASSERT_FLOAT_WITHIN(0.001, 25.0, t);

    fake_i2c_set_reg(BMS_I2C_DEV_ADDR_UPPER, 0x3A, (uint16_t)(-3200)); // -12.5 C
    TEST_ASSERT_EQUAL(WT_OK, max17320_get_cell_temperature_c(0, &t));
    TEST_ASSERT_FLOAT_WITHIN(0.001, -12.5, t);

    // state of charge LSB = 1/256 %
    fake_i2c_set_reg(BMS_I2C_DEV_ADDR_LOWER, MAX17320_REG_REP_SOC, 25600);
    double soc = 0.0;
    TEST_ASSERT_EQUAL(WT_OK, max17320_get_state_of_charge(&soc));
    TEST_ASSERT_FLOAT_WITHIN(0.001, 100.0, soc);
}

void test_fet_control_preserves_other_bits(void) {
    // regression for the disable_discharging clobber fix: a latched CHARGE_OFF
    // must survive disabling discharge
    fake_i2c_set_reg(BMS_I2C_DEV_ADDR_LOWER, 0x61, CHARGE_OFF);
    TEST_ASSERT_EQUAL(WT_OK, max17320_disable_discharging());
    TEST_ASSERT_EQUAL_HEX16(CHARGE_OFF | DISCHARGE_OFF,
                            fake_i2c_get_reg(BMS_I2C_DEV_ADDR_LOWER, 0x61));

    // re-enabling charge must keep DISCHARGE_OFF latched
    TEST_ASSERT_EQUAL(WT_OK, max17320_enable_charging());
    TEST_ASSERT_EQUAL_HEX16(DISCHARGE_OFF, fake_i2c_get_reg(BMS_I2C_DEV_ADDR_LOWER, 0x61));

    TEST_ASSERT_EQUAL(WT_OK, max17320_enable_discharging());
    TEST_ASSERT_EQUAL_HEX16(0x0000, fake_i2c_get_reg(BMS_I2C_DEV_ADDR_LOWER, 0x61));
}

void test_clear_write_protection(void) {
    // the write itself lands in the fake register, so the read-back matches
    TEST_ASSERT_EQUAL(WT_OK, max17320_clear_write_protection());
    TEST_ASSERT_EQUAL_HEX16(CLEAR_WRITE_PROT, fake_i2c_get_reg(BMS_I2C_DEV_ADDR_LOWER, 0x61));
}

void setUp(void) {
    fake_i2c_reset();
}

void tearDown(void) {
}

int main(void) {
    UNITY_BEGIN();
    printf("MAX17320 battery gauge driver tests\n");
    RUN_TEST(test_register_routing_by_address_range);
    RUN_TEST(test_cell_voltage_conversion);
    RUN_TEST(test_current_conversion_signed);
    RUN_TEST(test_temperature_and_soc_conversion);
    RUN_TEST(test_fet_control_preserves_other_bits);
    RUN_TEST(test_clear_write_protection);
    return UNITY_END();
}
