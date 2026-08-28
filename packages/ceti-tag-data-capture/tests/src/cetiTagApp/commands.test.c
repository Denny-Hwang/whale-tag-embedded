//-----------------------------------------------------------------------------
// Unit tests for the command dispatcher (commands.c).
//
// commands.c is included directly into this translation unit so the static
// response-pipe path can point at a plain file instead of the FIFO; each test
// writes a line into g_command, runs handle_command(), and asserts on the
// captured response text. The subcommand tables normally provided by
// subcommands/*.c are replaced with small fakes that record their invocation.
//-----------------------------------------------------------------------------
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unity.h>

#include "cetiTagApp/commands.h"
#include "cetiTagApp/commands_internal.h"

#define TEST_RSP_FILE "/tmp/ceti_commands_test_rsp.txt"

/******************************** stubbed dependencies ***********************/
#include "cetiTagApp/launcher.h"

int g_exit = 0;
int g_stopLogging = 0;
int g_command_thread_tid = -1;
char g_process_path[256] = "/tmp/";

static int s_start_acquisition_calls = 0;
static int s_stop_acquisition_calls = 0;
static int s_fpga_cam_calls = 0;
static unsigned int s_fpga_cam_last_opcode = 0;

void threadManager_start_acquisition(void) {
    s_start_acquisition_calls++;
}

void threadManager_stop_acquisition(void) {
    s_stop_acquisition_calls++;
}

void wt_fpga_cam(unsigned int opcode, unsigned int arg0, unsigned int arg1,
                 unsigned int pld0, unsigned int pld1, char *pResponse) {
    s_fpga_cam_calls++;
    s_fpga_cam_last_opcode = opcode;
}

/******************************** fake subcommand tables *********************/
static int s_fake_audio_start_calls = 0;
static char s_fake_audio_start_args[64] = "";

static int fake_audioCmd_start(const char *args) {
    s_fake_audio_start_calls++;
    snprintf(s_fake_audio_start_args, sizeof(s_fake_audio_start_args), "%s", args);
    fprintf(g_rsp_pipe, "fake audio started\n");
    return 0;
}

const CommandDescription audio_subcommand_list[] = {
    {.name = STR_FROM("start"), .description = "Fake start", .parse = fake_audioCmd_start},
};
const size_t audio_subcommand_list_size = sizeof(audio_subcommand_list) / sizeof(*audio_subcommand_list);

#define EMPTY_SUBCOMMAND_LIST(prefix)                            \
    const CommandDescription prefix##_subcommand_list[] = {      \
        {.name = STR_FROM("noop"), .description = "Fake noop", .parse = NULL}, \
    };                                                           \
    const size_t prefix##_subcommand_list_size = 1

EMPTY_SUBCOMMAND_LIST(battery);
EMPTY_SUBCOMMAND_LIST(burnwire);
EMPTY_SUBCOMMAND_LIST(fpga);
EMPTY_SUBCOMMAND_LIST(imu);
EMPTY_SUBCOMMAND_LIST(mission);
EMPTY_SUBCOMMAND_LIST(network);
EMPTY_SUBCOMMAND_LIST(recovery);

/******************************** module under test **************************/
#include "cetiTagApp/commands.c"

/******************************** helpers ************************************/
static char s_response[4096];

// place a command line in g_command, dispatch it, and capture the response
static int run_command(const char *line) {
    snprintf(g_command, sizeof(g_command), "%s\n", line);
    int result = handle_command();

    s_response[0] = '\0';
    FILE *f = fopen(TEST_RSP_FILE, "r");
    if (f != NULL) {
        size_t n = fread(s_response, 1, sizeof(s_response) - 1, f);
        s_response[n] = '\0';
        fclose(f);
    }
    return result;
}

/******************************** TESTS **************************************/
void test_ping_replies_pong(void) {
    TEST_ASSERT_EQUAL(0, run_command("ping"));
    TEST_ASSERT_NOT_NULL(strstr(s_response, "pong"));
}

void test_unknown_command_prints_help(void) {
    run_command("definitelyNotACommand");
    TEST_ASSERT_NOT_NULL(strstr(s_response, "Available Commands"));
    TEST_ASSERT_NOT_NULL(strstr(s_response, "ping"));
    TEST_ASSERT_NOT_NULL(strstr(s_response, "mission"));
}

void test_quit_stops_everything(void) {
    TEST_ASSERT_EQUAL(0, run_command("quit"));
    TEST_ASSERT_EQUAL(1, g_exit);
    TEST_ASSERT_EQUAL(1, g_stopLogging);
    TEST_ASSERT_EQUAL(1, s_stop_acquisition_calls);
}

void test_logging_toggle(void) {
    run_command("stopLogging");
    TEST_ASSERT_EQUAL(1, g_stopLogging);
    TEST_ASSERT_NOT_NULL(strstr(s_response, "stopped"));

    run_command("startLogging");
    TEST_ASSERT_EQUAL(0, g_stopLogging);
    TEST_ASSERT_NOT_NULL(strstr(s_response, "started"));
}

void test_acquisition_commands(void) {
    run_command("startDataAcq");
    TEST_ASSERT_EQUAL(1, s_start_acquisition_calls);
    run_command("stopDataAcq");
    TEST_ASSERT_EQUAL(1, s_stop_acquisition_calls);
}

void test_powerdown_delegates_to_fpga(void) {
    run_command("powerdown");
    TEST_ASSERT_TRUE(s_fpga_cam_calls > 0);
    TEST_ASSERT_EQUAL(0x0E, s_fpga_cam_last_opcode);
}

void test_subcommand_dispatch_and_args(void) {
    TEST_ASSERT_EQUAL(0, run_command("audio start 96"));
    TEST_ASSERT_EQUAL(1, s_fake_audio_start_calls);
    // the remainder of the line is handed to the subcommand handler
    TEST_ASSERT_NOT_NULL(strstr(s_fake_audio_start_args, "96"));
    TEST_ASSERT_NOT_NULL(strstr(s_response, "fake audio started"));
}

void test_unknown_subcommand_prints_subcommand_help(void) {
    run_command("audio bogus");
    TEST_ASSERT_EQUAL(0, s_fake_audio_start_calls);
    TEST_ASSERT_NOT_NULL(strstr(s_response, "`audio` Subcommands"));
    TEST_ASSERT_NOT_NULL(strstr(s_response, "start"));
}

/******************************** runner *************************************/
void setUp(void) {
    // route handle_command()'s response pipe to a plain file
    snprintf(rsp_pipe_path, sizeof(rsp_pipe_path), "%s", TEST_RSP_FILE);
    remove(TEST_RSP_FILE);
    g_exit = 0;
    g_stopLogging = 0;
    s_start_acquisition_calls = 0;
    s_stop_acquisition_calls = 0;
    s_fpga_cam_calls = 0;
    s_fpga_cam_last_opcode = 0;
    s_fake_audio_start_calls = 0;
    s_fake_audio_start_args[0] = '\0';
}

void tearDown(void) {
    remove(TEST_RSP_FILE);
}

int main(void) {
    UNITY_BEGIN();
    printf("Command dispatcher tests\n");
    RUN_TEST(test_ping_replies_pong);
    RUN_TEST(test_unknown_command_prints_help);
    RUN_TEST(test_quit_stops_everything);
    RUN_TEST(test_logging_toggle);
    RUN_TEST(test_acquisition_commands);
    RUN_TEST(test_powerdown_delegates_to_fpga);
    RUN_TEST(test_subcommand_dispatch_and_args);
    RUN_TEST(test_unknown_subcommand_prints_subcommand_help);
    return UNITY_END();
}
