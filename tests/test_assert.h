/* TEST_ASSERT(cond) — test assertion that stays live in every build
 * mode. Unlike assert(), the condition is always evaluated, so calls
 * carrying required side effects (bind/listen/pthread_create) survive
 * Release's -DNDEBUG instead of being compiled out. On failure it
 * prints the condition and aborts, like a real assert.
 */
#ifndef HVCAM_TEST_ASSERT_H
#define HVCAM_TEST_ASSERT_H

#include <stdio.h>
#include <stdlib.h>

#define TEST_ASSERT(cond)                                                    \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "%s:%d: TEST_ASSERT failed: %s\n", __FILE__,     \
                    __LINE__, #cond);                                        \
            abort();                                                         \
        }                                                                    \
    } while (0)

#endif
