/**
 * @file        pifs_test.c
 * @brief       Test of Pi file system
 * @author      Copyright (C) Peter Ivanov, 2017
 *
 * Created:     2017-06-11 09:10:19
 * Last modify: 2017-06-15 14:34:46 ivanovp {Time-stamp}
 * Licence:     GPL
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>

#include "api_pifs.h"
#include "pifs.h"
#include "pifs_debug.h"
#include "buffer.h"

#define PIFS_TEST_ERROR_MSG(...)    do { \
        fprintf(stderr, "%s ERROR: ", __FUNCTION__); \
        fprintf(stderr, __VA_ARGS__); \
    } while (0);

uint8_t test_buf[3 * 256];

pifs_status_t pifs_test(void)
{
    pifs_status_t ret = PIFS_ERROR_FLASH_INIT;
    P_FILE * file;
    size_t   written_size;

    ret = pifs_init();
    PIFS_ASSERT(ret == PIFS_SUCCESS);

    file = pifs_fopen("test.dat", "w");
    if (file)
    {
        printf("File opened\r\n");
        fill_buffer(test_buf, sizeof(test_buf), FILL_TYPE_SEQUENCE_WORD, 1);
        written_size = pifs_fwrite(test_buf, 1, sizeof(test_buf), file);
    }

    file = pifs_fopen("test2.dat", "w");
    if (file)
    {
        printf("File opened\r\n");
        fill_buffer(test_buf, sizeof(test_buf), FILL_TYPE_SEQUENCE_WORD, 2);
        written_size = pifs_fwrite(test_buf, 1, sizeof(test_buf), file);
    }

    file = pifs_fopen("test.dat3", "w");
    if (file)
    {
        printf("File opened\r\n");
        fill_buffer(test_buf, sizeof(test_buf), FILL_TYPE_SEQUENCE_WORD, 3);
        written_size = pifs_fwrite(test_buf, 1, sizeof(test_buf), file);
    }

    ret = pifs_delete();
    PIFS_ASSERT(ret == PIFS_SUCCESS);

    return ret;
}

int main(void)
{
    srand(time(0));
    pifs_test();

    return 0;
}

