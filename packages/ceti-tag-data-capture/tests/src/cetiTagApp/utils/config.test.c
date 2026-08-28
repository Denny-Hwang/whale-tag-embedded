//-----------------------------------------------------------------------------
// Unit tests for the configuration parser (utils/config.c).
//
// Links the real config.o (plus str/aprs/logging) and drives the public API:
// config_parse_line(), config_read(), and strtotime_s(). Each test resets
// g_config to the compiled defaults first.
//-----------------------------------------------------------------------------
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unity.h>

#include "cetiTagApp/sensors/audio.h"
#include "cetiTagApp/utils/config.h"

#define TEST_CONFIG_FILE "/tmp/ceti_config_test.txt"

static const TagConfig default_config = {
    .audio = {
        .filter_type = CONFIG_DEFAULT_AUDIO_FILTER_TYPE,
        .sample_rate = CONFIG_DEFAULT_AUDIO_SAMPLE_RATE,
        .bit_depth = CONFIG_DEFAULT_AUDIO_BIT_DEPTH,
    },
    .surface_pressure = CONFIG_DEFAULT_SURFACE_PRESSURE_BAR,
    .dive_pressure = CONFIG_DEFAULT_DIVE_PRESSURE_BAR,
    .release_voltage_v = CONFIG_DEFAULT_RELEASE_VOLTAGE_V,
    .critical_voltage_v = CONFIG_DEFAULT_CRITICAL_VOLTAGE_V,
    .timeout_s = CONFIG_DEFAULT_TIMEOUT_S,
    .tod_release = {.valid = 0},
    .burn_interval_s = CONFIG_DEFAULT_BURN_INTERVAL_S,
    .recovery = {
        .enabled = CONFIG_DEFAULT_RECOVERY_ENABLED,
        .tx_on_whale = CONFIG_DEFAULT_RECOVERY_TX_ON_WHALE,
        .freq_MHz = CONFIG_DEFAULT_RECOVERY_FREQUENCY_MHZ,
        .callsign = {
            .callsign = CONFIG_DEFAULT_RECOVERY_CALLSIGN,
            .ssid = CONFIG_DEFAULT_RECOVERY_SSID,
        },
        .recipient = {
            .callsign = CONFIG_DEFAULT_RECOVERY_RECIPIENT_CALLSIGN,
            .ssid = CONFIG_DEFAULT_RECOVERY_RECIPIENT_SSID,
        },
    },
};

/******************************** TESTS **************************************/
void test_strtotime_s_suffixes(void) {
    TEST_ASSERT_EQUAL(90, strtotime_s("90s", NULL));
    TEST_ASSERT_EQUAL(5 * 60, strtotime_s("5m", NULL));
    TEST_ASSERT_EQUAL(2 * 60 * 60, strtotime_s("2h", NULL));
    TEST_ASSERT_EQUAL(24 * 60 * 60, strtotime_s("1d", NULL));
    // a bare number defaults to minutes
    TEST_ASSERT_EQUAL(7 * 60, strtotime_s("7", NULL));
}

void test_pressure_keys_and_aliases(void) {
    TEST_ASSERT_EQUAL(CONFIG_OK, config_parse_line("surface_pressure = 0.25"));
    TEST_ASSERT_EQUAL_FLOAT(0.25f, g_config.surface_pressure);

    TEST_ASSERT_EQUAL(CONFIG_OK, config_parse_line("P1 = 0.35"));
    TEST_ASSERT_EQUAL_FLOAT(0.35f, g_config.surface_pressure);

    TEST_ASSERT_EQUAL(CONFIG_OK, config_parse_line("dive_pressure = 0.6"));
    TEST_ASSERT_EQUAL_FLOAT(0.6f, g_config.dive_pressure);

    TEST_ASSERT_EQUAL(CONFIG_OK, config_parse_line("P2 = 0.7"));
    TEST_ASSERT_EQUAL_FLOAT(0.7f, g_config.dive_pressure);
}

void test_release_voltage_halved_and_range_checked(void) {
    // pack voltage is stored halved (per cell)
    TEST_ASSERT_EQUAL(CONFIG_OK, config_parse_line("release_voltage = 6.6"));
    TEST_ASSERT_EQUAL_FLOAT(3.3f, g_config.release_voltage_v);

    TEST_ASSERT_EQUAL(CONFIG_OK, config_parse_line("critical_voltage = 6.4"));
    TEST_ASSERT_EQUAL_FLOAT(3.2f, g_config.critical_voltage_v);

    // out of the 6.2-8.4 V pack range: rejected, value unchanged
    TEST_ASSERT_EQUAL(CONFIG_ERR_INVALID_VALUE, config_parse_line("release_voltage = 5.0"));
    TEST_ASSERT_EQUAL_FLOAT(3.3f, g_config.release_voltage_v);
    TEST_ASSERT_EQUAL(CONFIG_ERR_INVALID_VALUE, config_parse_line("critical_voltage = 9.0"));
    TEST_ASSERT_EQUAL_FLOAT(3.2f, g_config.critical_voltage_v);
}

void test_timeout_and_burn_interval(void) {
    TEST_ASSERT_EQUAL(CONFIG_OK, config_parse_line("timeout_release = 4d"));
    TEST_ASSERT_EQUAL(4 * 24 * 60 * 60, g_config.timeout_s);

    TEST_ASSERT_EQUAL(CONFIG_OK, config_parse_line("T0 = 30m"));
    TEST_ASSERT_EQUAL(30 * 60, g_config.timeout_s);

    TEST_ASSERT_EQUAL(CONFIG_OK, config_parse_line("burn_interval = 20m"));
    TEST_ASSERT_EQUAL(20 * 60, g_config.burn_interval_s);

    TEST_ASSERT_EQUAL(CONFIG_OK, config_parse_line("BT = 45s"));
    TEST_ASSERT_EQUAL(45, g_config.burn_interval_s);
}

void test_time_of_day_release(void) {
    TEST_ASSERT_EQUAL(0, g_config.tod_release.valid);
    TEST_ASSERT_EQUAL(CONFIG_OK, config_parse_line("time_of_day_release = 20:15"));
    TEST_ASSERT_EQUAL(1, g_config.tod_release.valid);
    TEST_ASSERT_EQUAL(20, g_config.tod_release.value.tm_hour);
    TEST_ASSERT_EQUAL(15, g_config.tod_release.value.tm_min);

    TEST_ASSERT_EQUAL(CONFIG_ERR_INVALID_VALUE, config_parse_line("time_of_day_release = banana"));
}

void test_audio_sample_rate_mapping(void) {
    TEST_ASSERT_EQUAL(CONFIG_OK, config_parse_line("audio_sample_rate = 48"));
    TEST_ASSERT_EQUAL(AUDIO_SAMPLE_RATE_48KHZ, g_config.audio.sample_rate);

    TEST_ASSERT_EQUAL(CONFIG_OK, config_parse_line("audio_sample_rate = 96"));
    TEST_ASSERT_EQUAL(AUDIO_SAMPLE_RATE_96KHZ, g_config.audio.sample_rate);

    TEST_ASSERT_EQUAL(CONFIG_OK, config_parse_line("audio_sample_rate = 192"));
    TEST_ASSERT_EQUAL(AUDIO_SAMPLE_RATE_192KHZ, g_config.audio.sample_rate);

    TEST_ASSERT_EQUAL(CONFIG_OK, config_parse_line("audio_sample_rate = 0"));
    TEST_ASSERT_EQUAL(AUDIO_SAMPLE_RATE_DEFAULT, g_config.audio.sample_rate);

    // above 192 kHz is rejected
    TEST_ASSERT_EQUAL(CONFIG_ERR_INVALID_VALUE, config_parse_line("audio_sample_rate = 250"));
}

void test_audio_bitdepth_and_filter(void) {
    TEST_ASSERT_EQUAL(CONFIG_OK, config_parse_line("audio_bitdepth = 16"));
    TEST_ASSERT_EQUAL(AUDIO_BIT_DEPTH_16, g_config.audio.bit_depth);

    TEST_ASSERT_EQUAL(CONFIG_OK, config_parse_line("audio_bitdepth = 24"));
    TEST_ASSERT_EQUAL(AUDIO_BIT_DEPTH_24, g_config.audio.bit_depth);

    TEST_ASSERT_EQUAL(CONFIG_OK, config_parse_line("audio_filter = sinc5"));
    TEST_ASSERT_EQUAL(AUDIO_FILTER_SINC5, g_config.audio.filter_type);

    // case-insensitive
    TEST_ASSERT_EQUAL(CONFIG_OK, config_parse_line("audio_filter = WideBand"));
    TEST_ASSERT_EQUAL(AUDIO_FILTER_WIDEBAND, g_config.audio.filter_type);

    TEST_ASSERT_EQUAL(CONFIG_ERR_INVALID_VALUE, config_parse_line("audio_filter = lowpass"));
}

void test_recovery_keys(void) {
    TEST_ASSERT_EQUAL(CONFIG_OK, config_parse_line("rec_enabled = true"));
    TEST_ASSERT_EQUAL(1, g_config.recovery.enabled);
    TEST_ASSERT_EQUAL(CONFIG_OK, config_parse_line("rec_enabled = false"));
    TEST_ASSERT_EQUAL(0, g_config.recovery.enabled);

    TEST_ASSERT_EQUAL(CONFIG_OK, config_parse_line("rec_tx_on_whale = true"));
    TEST_ASSERT_EQUAL(1, g_config.recovery.tx_on_whale);

    TEST_ASSERT_EQUAL(CONFIG_OK, config_parse_line("rec_freq = 145.050"));
    TEST_ASSERT_EQUAL_FLOAT(145.050f, g_config.recovery.freq_MHz);
    // outside the 134.0-174.0 MHz range
    TEST_ASSERT_EQUAL(CONFIG_ERR_INVALID_VALUE, config_parse_line("rec_freq = 500.0"));
    TEST_ASSERT_EQUAL_FLOAT(145.050f, g_config.recovery.freq_MHz);

    TEST_ASSERT_EQUAL(CONFIG_OK, config_parse_line("rec_callsign = KC1TUJ-3"));
    TEST_ASSERT_EQUAL_STRING("KC1TUJ", g_config.recovery.callsign.callsign);
    TEST_ASSERT_EQUAL(3, g_config.recovery.callsign.ssid);

    TEST_ASSERT_EQUAL(CONFIG_OK, config_parse_line("rec_recipient = J75Z-2"));
    TEST_ASSERT_EQUAL_STRING("J75Z", g_config.recovery.recipient.callsign);
    TEST_ASSERT_EQUAL(2, g_config.recovery.recipient.ssid);
}

void test_parse_errors(void) {
    TEST_ASSERT_EQUAL(CONFIG_ERR_UNKNOWN_KEY, config_parse_line("no_such_key = 1"));
    TEST_ASSERT_EQUAL(CONFIG_ERR_MISSING_ASSIGN_OP, config_parse_line("surface_pressure 0.3"));
    // comments and blank lines are silently accepted
    TEST_ASSERT_EQUAL(CONFIG_OK, config_parse_line("# just a comment"));
    TEST_ASSERT_EQUAL(CONFIG_OK, config_parse_line(""));
}

void test_config_read_file(void) {
    FILE *f = fopen(TEST_CONFIG_FILE, "w");
    TEST_ASSERT_NOT_NULL(f);
    fprintf(f, "# deployment test config\n");
    fprintf(f, "surface_pressure = 0.31\n");
    fprintf(f, "dive_pressure = 0.51\n");
    fprintf(f, "this_line_is_garbage\n"); // must be skipped, not fatal
    fprintf(f, "timeout_release = 2d\n");
    fprintf(f, "rec_enabled = true\n");
    fclose(f);

    TEST_ASSERT_EQUAL(0, config_read(TEST_CONFIG_FILE));
    TEST_ASSERT_EQUAL_FLOAT(0.31f, g_config.surface_pressure);
    TEST_ASSERT_EQUAL_FLOAT(0.51f, g_config.dive_pressure);
    TEST_ASSERT_EQUAL(2 * 24 * 60 * 60, g_config.timeout_s);
    TEST_ASSERT_EQUAL(1, g_config.recovery.enabled);

    remove(TEST_CONFIG_FILE);
}

/******************************** runner *************************************/
void setUp(void) {
    g_config = default_config;
}

void tearDown(void) {
}

int main(void) {
    UNITY_BEGIN();
    printf("Configuration parser tests\n");
    RUN_TEST(test_strtotime_s_suffixes);
    RUN_TEST(test_pressure_keys_and_aliases);
    RUN_TEST(test_release_voltage_halved_and_range_checked);
    RUN_TEST(test_timeout_and_burn_interval);
    RUN_TEST(test_time_of_day_release);
    RUN_TEST(test_audio_sample_rate_mapping);
    RUN_TEST(test_audio_bitdepth_and_filter);
    RUN_TEST(test_recovery_keys);
    RUN_TEST(test_parse_errors);
    RUN_TEST(test_config_read_file);
    return UNITY_END();
}
