/**
 * @file test_main.c
 * @brief Main test runner - defines unity global counters
 */

#include <setjmp.h>

/* Unity global counters (extern'd in unity.h) */
int unity_tests_run = 0;
int unity_tests_passed = 0;
int unity_tests_failed = 0;
jmp_buf unity_jmp;
const char *unity_current_test = "";

#include "unity.h"

extern void run_pdu_decoder_tests(void);
extern void run_sms_assembly_tests(void);

int main(void) {
    printf("========================================\n");
    printf("  Sim_Message_Receiver Unit Tests\n");
    printf("========================================\n");

    run_pdu_decoder_tests();
    run_sms_assembly_tests();

    unity_print_summary();

    return unity_tests_failed > 0 ? 1 : 0;
}
