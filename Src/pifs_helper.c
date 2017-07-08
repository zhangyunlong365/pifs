/**
 * @file        pifs.c
 * @brief       Pi file system
 * @author      Copyright (C) Peter Ivanov, 2017
 *
 * Created:     2017-06-11 09:10:19
 * Last modify: 2017-07-06 19:12:58 ivanovp {Time-stamp}
 * Licence:     GPL
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api_pifs.h"
#include "flash.h"
#include "flash_config.h"
#include "pifs.h"
#include "pifs_helper.h"
#include "pifs_debug.h"
#include "buffer.h"

#if PIFS_DEBUG_LEVEL >= 1
/**
 * @brief pifs_address2str Convert address to human readable string.
 *
 * @param[in] a_address Address to convert.
 * @return The created string.
 */
char * pifs_address2str(pifs_address_t * a_address)
{
    static char str[32];

    snprintf(str, sizeof(str), "BA%i/PA%i @0x%X", a_address->block_address, a_address->page_address,
           a_address->block_address * PIFS_FLASH_BLOCK_SIZE_BYTE
           + a_address->page_address * PIFS_FLASH_PAGE_SIZE_BYTE);

    return str;
}

/**
 * @brief pifs_ba_pa2str Convert adress to human readable string.
 *
 * @param[in] a_block_address Block address.
 * @param[in] a_page_address Page address.
 * @return The created string.
 */
char * pifs_ba_pa2str(pifs_block_address_t a_block_address, pifs_page_address_t a_page_address)
{
    static char str[32];

    snprintf(str, sizeof(str), "BA%i/PA%i @0x%X", a_block_address, a_page_address,
           a_block_address * PIFS_FLASH_BLOCK_SIZE_BYTE
           + a_page_address * PIFS_FLASH_PAGE_SIZE_BYTE);

    return str;
}

/**
 * @brief pifs_byte2bin_str Convert a byte to binary string.
 *
 * @param[in] byte  Byte to convert.
 * @return Binary string.
 */
char * pifs_byte2bin_str(uint8_t byte)
{
    uint8_t i;
    static char s[12];

    s[0] = 0;
    for (i = 0; i < PIFS_BYTE_BITS; i++)
    {
        strncat(s, (byte & 0x80) ? "1" : "0", sizeof(s));
        byte <<= 1;
    }
    return s;
}
#endif

/**
 * @brief pifs_print_cache Print content of page buffer.
 */
void pifs_print_cache(void)
{
#if PIFS_DEBUG_LEVEL >= 5
    print_buffer(pifs.cache_page_buf, sizeof(pifs.cache_page_buf),
                 pifs.cache_page_buf_address.block_address * PIFS_FLASH_BLOCK_SIZE_BYTE
                 + pifs.cache_page_buf_address.page_address * PIFS_FLASH_PAGE_SIZE_BYTE);
#endif
}

bool_t pifs_is_address_valid(pifs_address_t * a_address)
{
    bool_t valid = (a_address->block_address < PIFS_FLASH_BLOCK_NUM_ALL)
            && (a_address->page_address < PIFS_FLASH_PAGE_PER_BLOCK);

    return valid;
}

/**
 * @brief pifs_is_block_type Checks if the given block address is block type.
 * @param[in] a_block_address Block address to check.
 * @param[in] a_block_type    Block type.
 * @param[in] a_header        Pointer to file system's header.
 * @return TRUE: If block address is equal to block type.
 */
bool_t pifs_is_block_type(pifs_block_address_t a_block_address,
                                 pifs_block_type_t a_block_type,
                                 pifs_header_t * a_header)
{
    pifs_size_t          i = 0;
    bool_t               is_block_type = TRUE;

    is_block_type = (a_block_type == PIFS_BLOCK_TYPE_DATA);
#if PIFS_MANAGEMENT_BLOCKS > 1
    for (i = 0; i < PIFS_MANAGEMENT_BLOCKS; i++)
#endif
    {
        if (a_header->management_blocks[i] == a_block_address)
        {
            is_block_type = (a_block_type == PIFS_BLOCK_TYPE_PRIMARY_MANAGEMENT);
        }
        if (a_header->next_management_blocks[i] == a_block_address)
        {
            is_block_type = (a_block_type == PIFS_BLOCK_TYPE_SECONDARY_MANAGEMENT);
        }
    }
#if PIFS_FLASH_BLOCK_RESERVED_NUM
    if (a_block_address < PIFS_FLASH_BLOCK_RESERVED_NUM)
    {
        is_block_type = (a_block_type == PIFS_BLOCK_TYPE_RESERVED);
    }
#endif

    return is_block_type;
}

/**
 * @brief pifs_is_buffer_erased Check if buffer is erased or contains
 * programmed bytes.
 *
 * @param[in] a_buf[in]         Pointer to buffer.
 * @param[in] a_buf_size[in]    Size of buffer.
 * @return TRUE: if buffer is erased.
 * FALSE: if buffer contains at least one programmed bit.
 */
bool_t pifs_is_buffer_erased(const void * a_buf, pifs_size_t a_buf_size)
{
    uint8_t   * buf = (uint8_t*) a_buf;
    pifs_size_t i;
    bool_t      ret = TRUE;

    for (i = 0; i < a_buf_size && ret; i++)
    {
        if (buf[i] != PIFS_FLASH_ERASED_BYTE_VALUE)
        {
            ret = FALSE;
        }
    }

    return ret;
}

/**
 * @brief pifs_is_page_erased Checks if the given block address is block type.
 * @param[in] a_block_address Block address to check.
 * @param[in] a_page_address  Page address to check.
 * @return TRUE: If page is erased.
 */
bool_t pifs_is_page_erased(pifs_block_address_t a_block_address,
                           pifs_page_address_t a_page_address)
{
    pifs_status_t status;
    bool_t is_erased = FALSE;

    status = pifs_read(a_block_address, a_page_address, 0, NULL, 0);
    if (status == PIFS_SUCCESS)
    {
        is_erased = pifs_is_buffer_erased(pifs.cache_page_buf, PIFS_FLASH_PAGE_SIZE_BYTE);
    }
    return is_erased;
}

/**
 * @brief pifs_parse_open_mode Parse string of open mode.
 *
 * @param a_file[in]    Pointer to file's internal structure.
 * @param a_modes[in]   String of mode.
 */
void pifs_parse_open_mode(pifs_file_t * a_file, const char * a_modes)
{
    uint8_t i;

    /* Reset mode */
    a_file->mode_create_new_file = FALSE;
    a_file->mode_read = FALSE;
    a_file->mode_write = FALSE;
    a_file->mode_append = FALSE;
    a_file->mode_file_shall_exist = FALSE;
    for (i = 0; a_modes[i] && i < 4; i++)
    {
        switch (a_modes[i])
        {
            case 'r':
                /* Read */
                a_file->mode_read = TRUE;
                a_file->mode_file_shall_exist = TRUE;
                break;
            case 'w':
                /* Write */
                a_file->mode_write = TRUE;
                a_file->mode_create_new_file = TRUE;
                break;
            case '+':
                if (a_file->mode_write)
                {
                    /* mode "w+" */
                    a_file->mode_read = TRUE;
                    a_file->mode_create_new_file = TRUE;
                }
                else if (a_file->mode_read)
                {
                    /* mode "r+" */
                    a_file->mode_write = TRUE;
                    a_file->mode_file_shall_exist = TRUE;
                }
                else if (a_file->mode_append)
                {
                    /* mode "a+" */
                    a_file->mode_read = TRUE;
                }
                break;
            case 'a':
                a_file->mode_append = TRUE;
                break;
            case 'b':
                /* Binary, all operations are binary! */
                break;
            default:
                a_file->status = PIFS_ERROR_INVALID_OPEN_MODE;
                PIFS_ERROR_MSG("Invalid open mode '%s'\r\n", a_modes);
                break;
        }
    }

    PIFS_DEBUG_MSG("create_new_file: %i\r\n", a_file->mode_create_new_file);
    PIFS_DEBUG_MSG("read: %i\r\n", a_file->mode_read);
    PIFS_DEBUG_MSG("write: %i\r\n", a_file->mode_write);
    PIFS_DEBUG_MSG("append: %i\r\n", a_file->mode_append);
    PIFS_DEBUG_MSG("file_shall_exist: %i\r\n", a_file->mode_file_shall_exist);
}