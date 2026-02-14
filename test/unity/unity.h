/* Minimal Unity-like test framework for host-based testing */
#ifndef UNITY_H
#define UNITY_H

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <setjmp.h>

/* Global counters - defined in test_main.c */
extern int unity_tests_run;
extern int unity_tests_passed;
extern int unity_tests_failed;
extern jmp_buf unity_jmp;
extern const char *unity_current_test;

#define TEST_ASSERT_TRUE(condition) do { \
    if (!(condition)) { \
        printf("  FAIL: %s:%d: '%s' was FALSE\n", __FILE__, __LINE__, #condition); \
        unity_tests_failed++; \
        longjmp(unity_jmp, 1); \
    } \
} while(0)

#define TEST_ASSERT_FALSE(condition) TEST_ASSERT_TRUE(!(condition))

#define TEST_ASSERT_EQUAL_INT(expected, actual) do { \
    int _e = (expected), _a = (actual); \
    if (_e != _a) { \
        printf("  FAIL: %s:%d: Expected %d, Got %d\n", __FILE__, __LINE__, _e, _a); \
        unity_tests_failed++; \
        longjmp(unity_jmp, 1); \
    } \
} while(0)

#define TEST_ASSERT_EQUAL_UINT8(expected, actual) TEST_ASSERT_EQUAL_INT((int)(expected), (int)(actual))
#define TEST_ASSERT_EQUAL_UINT16(expected, actual) TEST_ASSERT_EQUAL_INT((int)(expected), (int)(actual))

#define TEST_ASSERT_EQUAL_STRING(expected, actual) do { \
    const char *_e = (expected), *_a = (actual); \
    if (_e == NULL && _a == NULL) break; \
    if (_e == NULL || _a == NULL || strcmp(_e, _a) != 0) { \
        printf("  FAIL: %s:%d: Expected \"%s\", Got \"%s\"\n", __FILE__, __LINE__, \
               _e ? _e : "(null)", _a ? _a : "(null)"); \
        unity_tests_failed++; \
        longjmp(unity_jmp, 1); \
    } \
} while(0)

#define TEST_ASSERT_NOT_NULL(ptr) do { \
    if ((ptr) == NULL) { \
        printf("  FAIL: %s:%d: Expected non-NULL\n", __FILE__, __LINE__); \
        unity_tests_failed++; \
        longjmp(unity_jmp, 1); \
    } \
} while(0)

#define TEST_ASSERT_NULL(ptr) do { \
    if ((ptr) != NULL) { \
        printf("  FAIL: %s:%d: Expected NULL\n", __FILE__, __LINE__); \
        unity_tests_failed++; \
        longjmp(unity_jmp, 1); \
    } \
} while(0)

#define TEST_ASSERT_GREATER_THAN(threshold, actual) do { \
    if (!((actual) > (threshold))) { \
        printf("  FAIL: %s:%d: %d not greater than %d\n", __FILE__, __LINE__, (int)(actual), (int)(threshold)); \
        unity_tests_failed++; \
        longjmp(unity_jmp, 1); \
    } \
} while(0)

#define RUN_TEST(func) do { \
    unity_current_test = #func; \
    unity_tests_run++; \
    printf("  Running %s...", #func); \
    if (setjmp(unity_jmp) == 0) { \
        func(); \
        unity_tests_passed++; \
        printf(" PASS\n"); \
    } else { \
        /* already printed FAIL */ \
    } \
} while(0)

static inline void unity_print_summary(void) {
    printf("\n========================================\n");
    if (unity_tests_failed == 0) {
        printf("  ALL %d TESTS PASSED\n", unity_tests_run);
    } else {
        printf("  %d FAILED out of %d tests\n", unity_tests_failed, unity_tests_run);
    }
    printf("========================================\n");
}

#endif /* UNITY_H */
