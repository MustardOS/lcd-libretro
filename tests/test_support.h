#ifndef LCD_TEST_SUPPORT_H
#define LCD_TEST_SUPPORT_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned test_checks;
static unsigned test_failures;

#define CHECK(cond)                                                                                               \
    do {                                                                                                          \
        test_checks++;                                                                                            \
        if (!(cond)) {                                                                                            \
            test_failures++;                                                                                      \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                            \
        }                                                                                                         \
    } while (0)

#define CHECK_EQ(a, b)                                                                                            \
    do {                                                                                                          \
        long long va_ = (long long)(a), vb_ = (long long)(b);                                                     \
        test_checks++;                                                                                            \
        if (va_ != vb_) {                                                                                         \
            test_failures++;                                                                                      \
            fprintf(stderr, "%s:%d: CHECK_EQ failed: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, va_, \
                    vb_);                                                                                         \
        }                                                                                                         \
    } while (0)

static inline int test_finish(const char *name)
{
    if (test_failures) {
        printf("FAIL: %s: %u of %u checks failed\n", name, test_failures, test_checks);
        return 1;
    }
    printf("PASS: %s: %u checks\n", name, test_checks);
    return 0;
}

static inline unsigned char *test_read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf;
    long n;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc(n > 0 ? (size_t)n : 1u);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    *size = (size_t)n;
    return buf;
}

#endif
