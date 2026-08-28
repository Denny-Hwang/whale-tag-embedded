//-----------------------------------------------------------------------------
// Unit tests for the recovery board protocol layer (recovery.c).
//
// The recovery source file is included directly into this translation unit so
// the static protocol functions (__recovery_get_packet, __ping, ...) can be
// exercised. The pigpio serial API is replaced by a scriptable in-memory
// serial port: bytes queued with script_rx() are what the parser "receives",
// and everything the code transmits is captured for inspection.
//-----------------------------------------------------------------------------
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unity.h>

#include "cetiTagApp/cetiTag.h"
#include "cetiTagApp/device/iox.h"
#include "cetiTagApp/utils/error.h"
#include "cetiTagApp/utils/timing.h"

/******************************** scripted serial port ***********************/
#define FAKE_SERIAL_BUFFER_SIZE 2048

static uint8_t s_rx_script[FAKE_SERIAL_BUFFER_SIZE];
static size_t s_rx_len = 0;
static size_t s_rx_pos = 0;

static uint8_t s_tx_capture[FAKE_SERIAL_BUFFER_SIZE];
static size_t s_tx_len = 0;

int serOpen(char *sertty, unsigned baud, unsigned serFlags) {
    (void)sertty;
    (void)baud;
    (void)serFlags;
    return 42;
}

int serClose(unsigned handle) {
    (void)handle;
    return 0;
}

int serWrite(unsigned handle, char *buf, unsigned count) {
    (void)handle;
    TEST_ASSERT_LESS_OR_EQUAL_size_t(FAKE_SERIAL_BUFFER_SIZE, s_tx_len + count);
    memcpy(&s_tx_capture[s_tx_len], buf, count);
    s_tx_len += count;
    return 0;
}

int serRead(unsigned handle, char *buf, unsigned count) {
    (void)handle;
    size_t available = s_rx_len - s_rx_pos;
    size_t to_copy = (count < available) ? count : available;
    memcpy(buf, &s_rx_script[s_rx_pos], to_copy);
    s_rx_pos += to_copy;
    return (int)to_copy;
}

int serDataAvailable(unsigned handle) {
    (void)handle;
    return (int)(s_rx_len - s_rx_pos);
}

static void script_rx(const void *data, size_t len) {
    TEST_ASSERT_LESS_OR_EQUAL_size_t(FAKE_SERIAL_BUFFER_SIZE, s_rx_len + len);
    memcpy(&s_rx_script[s_rx_len], data, len);
    s_rx_len += len;
}

static void reset_fake_serial(void) {
    s_rx_len = 0;
    s_rx_pos = 0;
    s_tx_len = 0;
}

/******************************** stubbed dependencies ***********************/
volatile int g_stopAcquisition = 0;
int g_stopLogging = 1; // keep the rx thread away from /data on the host
int g_recovery_rx_thread_tid = -1;

WTResult iox_init(void) { return WT_OK; }
WTResult iox_set_mode(int pin, WtIoxMode mode) {
    (void)pin;
    (void)mode;
    return WT_OK;
}
WTResult iox_write_pin(int pin, int value) {
    (void)pin;
    (void)value;
    return WT_OK;
}

int64_t get_monotonic_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((int64_t)ts.tv_sec * 1000000) + (int64_t)(ts.tv_nsec / 1000);
}

int64_t get_global_time_us(void) {
    return get_monotonic_time_us();
}

int getRtcCount(void) {
    return 0;
}

int timing_has_syncronized_to_ntp(void) {
    return 0;
}

/******************************** module under test **************************/
#include "cetiTagApp/recovery.c"

/******************************** helpers ************************************/
static void assert_single_tx_frame(uint8_t type, const void *payload, uint8_t length) {
    TEST_ASSERT_EQUAL_size_t(sizeof(RecPktHeader) + length, s_tx_len);
    TEST_ASSERT_EQUAL_UINT8(RECOVERY_PACKET_KEY_VALUE, s_tx_capture[0]);
    TEST_ASSERT_EQUAL_UINT8(type, s_tx_capture[1]);
    TEST_ASSERT_EQUAL_UINT8(length, s_tx_capture[2]);
    if (length != 0) {
        TEST_ASSERT_EQUAL_UINT8_ARRAY((const uint8_t *)payload, &s_tx_capture[sizeof(RecPktHeader)], length);
    }
}

static void script_rx_frame(uint8_t type, const void *payload, uint8_t length) {
    RecPktHeader header = {
        .key = RECOVERY_PACKET_KEY_VALUE,
        .type = type,
        .length = length,
    };
    script_rx(&header, sizeof(header));
    if (length != 0) {
        script_rx(payload, length);
    }
}

static bool __script_exhausted(void) {
    return s_rx_pos >= s_rx_len;
}

/******************************** TESTS **************************************/
// hardware bring-up assigns a serial handle
void test_wt_recovery_init(void) {
    TEST_ASSERT_EQUAL(WT_OK, wt_recovery_init());
    TEST_ASSERT_TRUE(recovery_fd >= 0);
}

// TX framing: '$', type, length, reserved, payload
void test_message_framing(void) {
    TEST_ASSERT_EQUAL(0, recovery_message("HELLO"));
    assert_single_tx_frame(REC_CMD_MESSAGE, "HELLO", 5);
}

void test_message_max_length(void) {
    char msg[RECOVERY_BOARD_MAX_MSG_LENGTH + 1];
    memset(msg, 'A', RECOVERY_BOARD_MAX_MSG_LENGTH);
    msg[RECOVERY_BOARD_MAX_MSG_LENGTH] = '\0';
    TEST_ASSERT_EQUAL(0, recovery_message(msg));
    assert_single_tx_frame(REC_CMD_MESSAGE, msg, RECOVERY_BOARD_MAX_MSG_LENGTH);
}

void test_message_too_long(void) {
    char msg[RECOVERY_BOARD_MAX_MSG_LENGTH + 2];
    memset(msg, 'A', RECOVERY_BOARD_MAX_MSG_LENGTH + 1);
    msg[RECOVERY_BOARD_MAX_MSG_LENGTH + 1] = '\0';
    TEST_ASSERT_EQUAL(-1, recovery_message(msg));
    TEST_ASSERT_EQUAL_size_t(0, s_tx_len);
}

void test_critical_voltage_packing(void) {
    float voltage = 6.2f;
    TEST_ASSERT_EQUAL(WT_OK, recovery_set_critical_voltage(voltage));
    assert_single_tx_frame(REC_CMD_CONFIG_CRITICAL_VOLTAGE, &voltage, sizeof(float));
}

void test_state_commands(void) {
    TEST_ASSERT_EQUAL(0, recovery_wake());
    assert_single_tx_frame(REC_CMD_START, NULL, 0);

    reset_fake_serial();
    TEST_ASSERT_EQUAL(0, recovery_sleep());
    assert_single_tx_frame(REC_CMD_STOP, NULL, 0);

    reset_fake_serial();
    TEST_ASSERT_EQUAL(0, recovery_gps_only());
    assert_single_tx_frame(REC_CMD_COLLECT_ONLY, NULL, 0);
}

void test_sync_time_packing(void) {
    time_t before = time(NULL);
    TEST_ASSERT_EQUAL(WT_OK, recovery_sync_time());
    time_t after = time(NULL);

    TEST_ASSERT_EQUAL_size_t(sizeof(RecPktHeader) + 6, s_tx_len);
    TEST_ASSERT_EQUAL_UINT8(REC_CMD_SET_RTC_TIME_OF_DAY, s_tx_capture[1]);

    // The payload must match gmtime() of some instant between before and after.
    int matched = 0;
    for (time_t t = before; t <= after && !matched; t++) {
        struct tm utc;
        gmtime_r(&t, &utc);
        uint8_t expected[6] = {
            (uint8_t)(utc.tm_year - 100),
            (uint8_t)(utc.tm_mon + 1),
            (uint8_t)utc.tm_mday,
            (uint8_t)utc.tm_hour,
            (uint8_t)utc.tm_min,
            (uint8_t)utc.tm_sec,
        };
        matched = (memcmp(expected, &s_tx_capture[sizeof(RecPktHeader)], 6) == 0);
    }
    TEST_ASSERT_TRUE_MESSAGE(matched, "RTC payload does not match UTC time of the call");
}

// Argos configuration setters: strict validation before anything is sent
void test_argos_address_valid(void) {
    TEST_ASSERT_EQUAL(0, recovery_set_argos_address("0123ABCD", 8));
    assert_single_tx_frame(REC_CMD_CONFIG_ARGOS_ADDR, "0123ABCD", 8);
}

void test_argos_address_invalid(void) {
    TEST_ASSERT_EQUAL(-1, recovery_set_argos_address("0123ABC", 7));
    TEST_ASSERT_EQUAL(-1, recovery_set_argos_address("0123ABCG", 8));
    TEST_ASSERT_EQUAL_size_t(0, s_tx_len);
}

void test_argos_id(void) {
    TEST_ASSERT_EQUAL(0, recovery_set_argos_id("123456", 6));
    assert_single_tx_frame(REC_CMD_CONFIG_ARGOS_ID, "123456", 6);

    reset_fake_serial();
    TEST_ASSERT_EQUAL(-1, recovery_set_argos_id("12A456", 6));
    TEST_ASSERT_EQUAL_size_t(0, s_tx_len);
}

void test_argos_modulation(void) {
    TEST_ASSERT_EQUAL(0, recovery_set_argos_modulation(ARGOS_MOD_LDK));
    uint8_t expected = (uint8_t)ARGOS_MOD_LDK;
    assert_single_tx_frame(REC_CMD_CONFIG_ARGOS_MODULATION, &expected, 1);
}

void test_argos_secret_key(void) {
    const char *key = "0123456789abcdef0123456789ABCDEF";
    TEST_ASSERT_EQUAL(0, recovery_set_argos_secret_key(key, 32));
    assert_single_tx_frame(REC_CMD_CONFIG_ARGOS_SECKEY, key, 32);

    reset_fake_serial();
    TEST_ASSERT_EQUAL(-1, recovery_set_argos_secret_key(key, 31));
    TEST_ASSERT_EQUAL(-1, recovery_set_argos_secret_key("0123456789abcdef0123456789ABCDEg", 32));
    TEST_ASSERT_EQUAL_size_t(0, s_tx_len);
}

// RX parser: resynchronizes on the '$' start byte, skipping leading garbage
void test_get_packet_resync(void) {
    script_rx("XYZ", 3);
    script_rx_frame(REC_CMD_PONG, NULL, 0);

    RecoveryPacket pkt;
    TEST_ASSERT_EQUAL(WT_OK, __recovery_get_packet(&pkt, __script_exhausted));
    TEST_ASSERT_EQUAL_UINT8(REC_CMD_PONG, pkt.header.type);
    TEST_ASSERT_EQUAL_UINT8(0, pkt.header.length);
}

void test_get_packet_payload(void) {
    const char *nmea = "$GPGGA,123519,4807.038,N";
    uint8_t nmea_len = (uint8_t)strlen(nmea);
    script_rx_frame(REC_CMD_NMEA_PACKET, nmea, nmea_len);

    RecoveryPacket pkt;
    TEST_ASSERT_EQUAL(WT_OK, __recovery_get_packet(&pkt, __script_exhausted));
    TEST_ASSERT_EQUAL_UINT8(REC_CMD_NMEA_PACKET, pkt.header.type);
    TEST_ASSERT_EQUAL_UINT8(nmea_len, pkt.header.length);
    TEST_ASSERT_EQUAL_UINT8_ARRAY((const uint8_t *)nmea, pkt.data.raw, nmea_len);
}

void test_get_packet_timeout(void) {
    RecoveryPacket pkt;
    TEST_ASSERT_EQUAL(WT_RESULT(WT_DEV_RECOVERY, WT_ERR_RECOVERY_TIMEOUT),
                      __recovery_get_packet(&pkt, __script_exhausted));
}

// ping/pong round trip
void test_ping_pong(void) {
    script_rx_frame(REC_CMD_PONG, NULL, 0);
    TEST_ASSERT_TRUE(__ping());
    assert_single_tx_frame(REC_CMD_PING, NULL, 0);
}

void test_ping_skips_other_packets(void) {
    const char *nmea = "$GPGGA,000000";
    script_rx_frame(REC_CMD_NMEA_PACKET, nmea, (uint8_t)strlen(nmea));
    script_rx_frame(REC_CMD_PONG, NULL, 0);
    TEST_ASSERT_TRUE(__ping());
}

void test_ping_timeout(void) {
    // no scripted response: __ping gives up after RECOVERY_UART_TIMEOUT_US (~0.5 s)
    TEST_ASSERT_FALSE(__ping());
}

/******************************** RX thread tests ****************************/
// The rx thread consumes scripted NMEA packets, timestamps them into shared
// memory, and posts the semaphore. Each test creates fresh SHM/semaphore
// objects (the thread tears its handles down on exit), reads its assertions
// from the shared sample BEFORE stopping the thread, then joins it.

#define RX_TEST_SHM_NAME "/test_recovery_shm"
#define RX_TEST_SEM_NAME "/test_recovery_sem"

static pthread_t s_rx_thread;

static void rx_thread_start(void) {
    g_stopAcquisition = 0;
    shm_unlink(RX_TEST_SHM_NAME);
    sem_unlink(RX_TEST_SEM_NAME);
    shm_nmea_sentence = create_shared_memory_region(RX_TEST_SHM_NAME, sizeof(CetiRecoverySample));
    TEST_ASSERT_NOT_NULL(shm_nmea_sentence);
    sem_nmea_sentence_ready = sem_open(RX_TEST_SEM_NAME, O_CREAT, 0644, 0);
    TEST_ASSERT_NOT_EQUAL(SEM_FAILED, sem_nmea_sentence_ready);
    TEST_ASSERT_EQUAL(0, pthread_create(&s_rx_thread, NULL, recovery_rx_thread, NULL));
}

static void rx_thread_wait_for_sample(void) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 2;
    TEST_ASSERT_EQUAL_MESSAGE(0, sem_timedwait(sem_nmea_sentence_ready, &deadline),
                              "rx thread did not deliver a sample in time");
}

static void rx_thread_stop(void) {
    g_stopAcquisition = 1;
    pthread_join(s_rx_thread, NULL);
    shm_unlink(RX_TEST_SHM_NAME);
    sem_unlink(RX_TEST_SEM_NAME);
    g_stopAcquisition = 0;
}

void test_rx_thread_nmea_to_shm(void) {
    const char *nmea = "$GPGGA,123519,4807.038,N,01131.000,E\r\n";
    script_rx_frame(REC_CMD_NMEA_PACKET, nmea, (uint8_t)strlen(nmea));

    rx_thread_start();
    rx_thread_wait_for_sample();

    // trailing \r\n must be trimmed, timestamps stamped
    TEST_ASSERT_EQUAL_STRING("$GPGGA,123519,4807.038,N,01131.000,E", shm_nmea_sentence->nmea_sentence);
    TEST_ASSERT_TRUE(shm_nmea_sentence->sys_time_us > 0);

    rx_thread_stop();
}

void test_rx_thread_oversized_nmea_is_clamped(void) {
    char oversized[201];
    memset(oversized, 'A', 200);
    oversized[200] = '\0';
    script_rx_frame(REC_CMD_NMEA_PACKET, oversized, 200);

    rx_thread_start();
    rx_thread_wait_for_sample();

    // clamped to the 96-byte shared-memory buffer (95 chars + NUL)
    size_t max_len = sizeof(shm_nmea_sentence->nmea_sentence) - 1;
    TEST_ASSERT_EQUAL_size_t(max_len, strlen(shm_nmea_sentence->nmea_sentence));

    rx_thread_stop();
}

void test_rx_thread_all_whitespace_nmea(void) {
    // a packet that is nothing but line endings must not underflow the trim loop
    script_rx_frame(REC_CMD_NMEA_PACKET, "\r\n\r\n", 4);

    rx_thread_start();
    rx_thread_wait_for_sample();

    TEST_ASSERT_EQUAL_STRING("", shm_nmea_sentence->nmea_sentence);

    rx_thread_stop();
}

void test_rx_thread_pong_updates_liveness(void) {
    script_rx_frame(REC_CMD_PONG, NULL, 0);
    recovery_board.pong = 0;

    rx_thread_start();
    // wait until the pong flag is cached by the rx thread
    for (int i = 0; i < 200 && !recovery_board.pong; i++) {
        usleep(10000);
    }
    TEST_ASSERT_TRUE(recovery_board.pong);

    rx_thread_stop();
}

/******************************** runner *************************************/
void setUp(void) {
    reset_fake_serial();
    recovery_fd = 42; // pretend the serial port is already open
}

void tearDown(void) {
}

int main(void) {
    UNITY_BEGIN();
    printf("Recovery board protocol tests\n");
    RUN_TEST(test_wt_recovery_init);
    RUN_TEST(test_message_framing);
    RUN_TEST(test_message_max_length);
    RUN_TEST(test_message_too_long);
    RUN_TEST(test_critical_voltage_packing);
    RUN_TEST(test_state_commands);
    RUN_TEST(test_sync_time_packing);
    RUN_TEST(test_argos_address_valid);
    RUN_TEST(test_argos_address_invalid);
    RUN_TEST(test_argos_id);
    RUN_TEST(test_argos_modulation);
    RUN_TEST(test_argos_secret_key);
    RUN_TEST(test_get_packet_resync);
    RUN_TEST(test_get_packet_payload);
    RUN_TEST(test_get_packet_timeout);
    RUN_TEST(test_ping_pong);
    RUN_TEST(test_ping_skips_other_packets);
    RUN_TEST(test_ping_timeout);

    printf("Recovery rx thread tests\n");
    RUN_TEST(test_rx_thread_nmea_to_shm);
    RUN_TEST(test_rx_thread_oversized_nmea_is_clamped);
    RUN_TEST(test_rx_thread_all_whitespace_nmea);
    RUN_TEST(test_rx_thread_pong_updates_liveness);
    return UNITY_END();
}
