/**
 * @file        pifs_wear.c
 * @brief       Pi file system
 * @author      Copyright (C) Peter Ivanov, 2017
 *
 * Created:     2017-06-11 09:10:19
 * Last modify: 2017-11-30 17:43:22 ivanovp {Time-stamp}
 * Licence:     GPL
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PIFS_DEBUG_LEVEL 2
#include "pifs_debug.h"

#include "api_pifs.h"
#include "flash.h"
#include "flash_config.h"
#include "pifs.h"
#include "pifs_fsbm.h"
#include "pifs_helper.h"
#include "pifs_delta.h"
#include "pifs_entry.h"
#include "pifs_map.h"
#include "pifs_merge.h"
#include "pifs_wear.h"
#include "pifs_file.h"
#include "pifs_dir.h"
#include "buffer.h" /* DEBUG */

/**
 * Structure used during emptying blocks.
 */
typedef struct
{
    pifs_block_address_t block_address;
    bool_t               is_block_emptied;
} pifs_empty_block_t;

/**
 * @brief pifs_wear_level_list_init Write initial wear level list with all
 * zeros.
 *
 * @return PIFS_SUCCESS if written successfully.
 */
pifs_status_t pifs_wear_level_list_init(void)
{
    pifs_status_t             ret = PIFS_SUCCESS;
    pifs_size_t               i;
    pifs_address_t            address;
    pifs_wear_level_entry_t * wear_level_entry = (pifs_wear_level_entry_t*) pifs.dmw_page_buf;

    memset(pifs.dmw_page_buf, PIFS_FLASH_ERASED_BYTE_VALUE, PIFS_LOGICAL_PAGE_SIZE_BYTE);
    address = pifs.header.wear_level_list_address;
    for (i = 0; i < PIFS_WEAR_LEVEL_ENTRY_PER_PAGE; i++)
    {
        wear_level_entry[i].wear_level_cntr = 0;
        wear_level_entry[i].wear_level_bits = PIFS_FLASH_ERASED_BYTE_VALUE;
    }

    for (i = 0; i < PIFS_WEAR_LEVEL_LIST_SIZE_PAGE && ret == PIFS_SUCCESS; i++)
    {
        //PIFS_WARNING_MSG("%s\r\n", pifs_address2str(&address));
        ret = pifs_write(address.block_address, address.page_address, 0,
                         pifs.dmw_page_buf, PIFS_LOGICAL_PAGE_SIZE_BYTE);
        if (ret == PIFS_SUCCESS)
        {
            (void)pifs_inc_address(&address);
        }
    }

    for (i = 0; i < PIFS_LEAST_WEARED_BLOCK_NUM && ret == PIFS_SUCCESS; i++)
    {
        pifs.header.least_weared_blocks[i].block_address = i;
        pifs.header.least_weared_blocks[i].wear_level_cntr = 0;
    }

    return ret;
}

/**
 * @brief pifs_get_wear_level Get wear level of a block.
 *
 * @param[in] a_block_address   Address of block.
 * @param[in] a_header          File system's header.
 * @param[out] a_wear_level     Erase count.
 * @return PIFS_SUCCESS if wear level successfully read.
 */
pifs_status_t pifs_get_wear_level(pifs_block_address_t a_block_address,
                                  pifs_header_t * a_header,
                                  pifs_wear_level_entry_t * a_wear_level)
{
    pifs_status_t        ret = PIFS_SUCCESS;
    pifs_address_t       address;
    pifs_size_t          po;
    pifs_size_t          i;

//    PIFS_NOTICE_MSG("Wear level list at %s\r\n", pifs_address2str(&a_header->wear_level_list_address));
    address = a_header->wear_level_list_address;
    po = (a_block_address % PIFS_WEAR_LEVEL_ENTRY_PER_PAGE) * PIFS_WEAR_LEVEL_ENTRY_SIZE_BYTE;
//    PIFS_WARNING_MSG("po: %i ba: %i\r\n", po, a_block_address / PIFS_WEAR_LEVEL_ENTRY_PER_PAGE);
    ret = pifs_add_address(&address, a_block_address / PIFS_WEAR_LEVEL_ENTRY_PER_PAGE);
    if (ret == PIFS_SUCCESS)
    {
        ret = pifs_read(address.block_address, address.page_address, po,
                        a_wear_level, PIFS_WEAR_LEVEL_ENTRY_SIZE_BYTE);
#if 0
        PIFS_WARNING_MSG("BA%i wear level counter: %i, bits: 0x%02X\r\n",
                         a_block_address,
                         a_wear_level->wear_level_cntr,
                         a_wear_level->wear_level_bits);
#endif
        /* Add wear_level_bits to wear_level_count! */
        for (i = 0; i < sizeof(a_wear_level->wear_level_bits) * PIFS_BYTE_BITS; i++)
        {
#if PIFS_FLASH_ERASED_BYTE_VALUE == 0xFF
            if (!(a_wear_level->wear_level_bits & 1))
#else
            if (a_wear_level->wear_level_bits & 1)
#endif
            {
                a_wear_level->wear_level_cntr++;
            }
            a_wear_level->wear_level_bits >>= 1;
        }
    }

    return ret;
}

/**
 * @brief pifs_inc_wear_level_level Increment wear level of a block.
 *
 * @param[in] a_block_address   Address of block.
 * @param[in] a_header          File system's header.
 * @return PIFS_SUCCESS if wear level successfully incremented.
 */
pifs_status_t pifs_inc_wear_level(pifs_block_address_t a_block_address,
                                  pifs_header_t * a_header)
{
    pifs_status_t           ret = PIFS_SUCCESS;
    pifs_address_t          address;
    pifs_size_t             po;
    pifs_size_t             i;
    pifs_wear_level_entry_t wear_level;

//    PIFS_NOTICE_MSG("Wear level list at %s\r\n", pifs_address2str(&a_header->wear_level_list_address));
    address = a_header->wear_level_list_address;
    po = (a_block_address % PIFS_WEAR_LEVEL_ENTRY_PER_PAGE) * PIFS_WEAR_LEVEL_ENTRY_SIZE_BYTE;
//    PIFS_WARNING_MSG("po: %i ba: %i\r\n", po, a_block_address / PIFS_WEAR_LEVEL_ENTRY_PER_PAGE);
    ret = pifs_add_address(&address, a_block_address / PIFS_WEAR_LEVEL_ENTRY_PER_PAGE);
    if (ret == PIFS_SUCCESS)
    {
        po %= PIFS_LOGICAL_PAGE_SIZE_BYTE;
        ret = pifs_read(address.block_address, address.page_address, po,
                        &wear_level, PIFS_WEAR_LEVEL_ENTRY_SIZE_BYTE);
        if (ret == PIFS_SUCCESS)
        {
            ret = PIFS_ERROR_NO_MORE_SPACE;
            for (i = 0; i < sizeof(wear_level.wear_level_bits) * PIFS_BYTE_BITS && ret != PIFS_SUCCESS; i++)
            {
#if PIFS_FLASH_ERASED_BYTE_VALUE == 0xFF
                if (wear_level.wear_level_bits & (1u << i))
#else
                if (!(wear_level.wear_level_bits & (1u << i)))
#endif
                {
                    ret = PIFS_SUCCESS;
                    wear_level.wear_level_bits ^= 1u << i;
                    PIFS_NOTICE_MSG("BA%i inverting bit %i\r\n", a_block_address, i);
                }
            }
            if (ret == PIFS_SUCCESS)
            {
                ret = pifs_write(address.block_address, address.page_address, po,
                                 &wear_level, PIFS_WEAR_LEVEL_ENTRY_SIZE_BYTE);
            }
        }
    }

    return ret;
}

/**
 * @brief pifs_write_wear_level_level Write wear level of a block.
 *
 * @param[in] a_block_address   Address of block.
 * @param[in] a_header          File system's header.
 * @param[out] a_wear_level     Erase count.
 * @return PIFS_SUCCESS if wear level successfully written.
 */
pifs_status_t pifs_write_wear_level(pifs_block_address_t a_block_address,
                                    pifs_header_t * a_header,
                                    pifs_wear_level_entry_t * a_wear_level)
{
    pifs_status_t        ret = PIFS_SUCCESS;
    pifs_address_t       address;
    pifs_size_t          po;

//    PIFS_NOTICE_MSG("Wear level list at %s\r\n", pifs_address2str(&a_header->wear_level_list_address));
    address = a_header->wear_level_list_address;
    po = (a_block_address % PIFS_WEAR_LEVEL_ENTRY_PER_PAGE) * PIFS_WEAR_LEVEL_ENTRY_SIZE_BYTE;
//    PIFS_WARNING_MSG("po: %i ba: %i\r\n", po, a_block_address / PIFS_WEAR_LEVEL_ENTRY_PER_PAGE);
    ret = pifs_add_address(&address, a_block_address / PIFS_WEAR_LEVEL_ENTRY_PER_PAGE);
    if (ret == PIFS_SUCCESS)
    {
        po %= PIFS_LOGICAL_PAGE_SIZE_BYTE;
        PIFS_NOTICE_MSG("BA%i wear level counter: %i, bits: 0x%02X\r\n",
                         a_block_address,
                         a_wear_level->wear_level_cntr,
                         a_wear_level->wear_level_bits);
        ret = pifs_write(address.block_address, address.page_address, po,
                         a_wear_level, PIFS_WEAR_LEVEL_ENTRY_SIZE_BYTE);
    }

    return ret;

}

/**
 * @brief pifs_wear_level_list_copy Copy wear level list.
 *
 * @return PIFS_SUCCESS if copied successfully.
 */
pifs_status_t pifs_copy_wear_level_list(pifs_header_t * a_old_header, pifs_header_t * a_new_header)
{
    pifs_status_t             ret = PIFS_SUCCESS;
    pifs_block_address_t      ba;
    pifs_wear_level_entry_t   wear_level_entry;

    for (ba = PIFS_FLASH_BLOCK_RESERVED_NUM; ba < PIFS_FLASH_BLOCK_NUM_FS && ret == PIFS_SUCCESS; ba++)
    {
        ret = pifs_get_wear_level(ba, a_old_header, &wear_level_entry);
        if (ret == PIFS_SUCCESS)
        {
            wear_level_entry.wear_level_bits = PIFS_FLASH_ERASED_BYTE_VALUE;
            ret = pifs_write_wear_level(ba, a_new_header, &wear_level_entry);
        }
    }

    return ret;
}

/**
 * @brief pifs_get_block_wear_stats Get least and most weared block.
 *
 * @param[in] a_block_type           Block type to take into account. Example: PIFS_BLOCK_TYPE_DATA
 * @param[in] a_header               Pointer to file system's header. Usually &pifs.header
 * @param[out] a_block_address_min   Pointer to block address of least weared block 
 *                                   or NULL if data is not relevant.
 * @param[out] a_block_address_max   Pointer to block address of most weared block
 *                                   or NULL if data is not relevant.
 * @param[out] a_wear_level_cntr_min Pointer to wear level counter of least weared block
 *                                   or NULL if data is not relevant.
 * @param[out] a_wear_level_cntr_max Pointer to wear level counter of most weared block
 *                                   or NULL if data is not relevant.
 *
 * @return PIFS_SUCCESS if get successfully.
 */
pifs_status_t pifs_get_block_wear_stats(pifs_block_type_t a_block_type,
                                        pifs_header_t * a_header,
                                        pifs_block_address_t * a_block_address_min,
                                        pifs_block_address_t * a_block_address_max,
                                        pifs_wear_level_cntr_t * a_wear_level_cntr_min,
                                        pifs_wear_level_cntr_t * a_wear_level_cntr_max)
{
    pifs_status_t             ret = PIFS_SUCCESS;
    pifs_block_address_t      ba;
    pifs_wear_level_entry_t   wear_level_entry;
    pifs_wear_level_cntr_t    wear_level_cntr_min = PIFS_WEAR_LEVEL_CNTR_MAX;
    pifs_wear_level_cntr_t    wear_level_cntr_max = 0;
    pifs_block_address_t      ba_min = PIFS_FLASH_BLOCK_RESERVED_NUM;
    pifs_block_address_t      ba_max = PIFS_FLASH_BLOCK_RESERVED_NUM;

    for (ba = PIFS_FLASH_BLOCK_RESERVED_NUM; ba < PIFS_FLASH_BLOCK_NUM_FS && ret == PIFS_SUCCESS; ba++)
    {
        if (pifs_is_block_type(ba, a_block_type, a_header))
        {
            ret = pifs_get_wear_level(ba, a_header, &wear_level_entry);
            if (ret == PIFS_SUCCESS)
            {
                if (wear_level_entry.wear_level_cntr < wear_level_cntr_min)
                {
                    ba_min = ba;
                    wear_level_cntr_min = wear_level_entry.wear_level_cntr;
                }
                if (wear_level_entry.wear_level_cntr > wear_level_cntr_max)
                {
                    ba_max = ba;
                    wear_level_cntr_max = wear_level_entry.wear_level_cntr;
                }
            }
        }
    }

    if (ret == PIFS_SUCCESS)
    {
//        PIFS_NOTICE_MSG("BA%i\r\n", ba_min);
        if (a_block_address_min)
        {
            *a_block_address_min = ba_min;
        }
        if (a_block_address_max)
        {
            *a_block_address_max = ba_max;
        }
        if (a_wear_level_cntr_min)
        {
            *a_wear_level_cntr_min = wear_level_cntr_min;
        }
        if (a_wear_level_cntr_max)
        {
            *a_wear_level_cntr_max = wear_level_cntr_max;
        }
    }

    return ret;
}

/**
 * @brief pifs_generate_least_weared_blocks Generate the list of least weared
 * blocks.
 *
 * @param[in] a_header Pointer to file system's header.
 * @return PIFS_SUCCESS if list successfully generated.
 */
pifs_status_t pifs_generate_least_weared_blocks(pifs_header_t * a_header)
{
    pifs_status_t             ret = PIFS_SUCCESS;
    pifs_block_address_t      ba;
    pifs_wear_level_entry_t   wear_level_entry;
    pifs_wear_level_cntr_t    wear_level_cntr_min;
    pifs_wear_level_cntr_t    last_wear_level_cntr;
    pifs_size_t               i;
    pifs_size_t               j;
    bool_t                    used = FALSE;
#if 0
    pifs_size_t               management_page_count;
    pifs_size_t               data_page_count;
#endif

    ret = pifs_get_block_wear_stats(PIFS_BLOCK_TYPE_DATA,
            a_header,
            &(a_header->least_weared_blocks[0].block_address),
            NULL,
            &wear_level_cntr_min,
            &(a_header->wear_level_cntr_max));

#if PIFS_LEAST_WEARED_BLOCK_NUM > 1
    for (i = 1; i < PIFS_LEAST_WEARED_BLOCK_NUM && ret == PIFS_SUCCESS; i++)
    {
        last_wear_level_cntr = PIFS_WEAR_LEVEL_CNTR_MAX;
        for (ba = PIFS_FLASH_BLOCK_RESERVED_NUM; ba < PIFS_FLASH_BLOCK_NUM_FS
             && ret == PIFS_SUCCESS; ba++)
        {
            if (pifs_is_block_type(ba, PIFS_BLOCK_TYPE_DATA, a_header))
            {
                /* Check if this block has not added already to the list */
                used = FALSE;
                for (j = 0; j < i && !used; j++)
                {
                    if (a_header->least_weared_blocks[j].block_address == ba)
                    {
                        used = TRUE;
                    }
                }
                if (!used)
                {
                    ret = pifs_get_wear_level(ba, a_header, &wear_level_entry);
                    if (ret == PIFS_SUCCESS
                            && wear_level_entry.wear_level_cntr >= wear_level_cntr_min
                            && wear_level_entry.wear_level_cntr < last_wear_level_cntr )
                    {
#if 1
                        a_header->least_weared_blocks[i].block_address = ba;
                        last_wear_level_cntr = wear_level_entry.wear_level_cntr;
#else
                        /* Get free pages in this block */
                        /* TODO Unfortunately this will get pages for the same block again and again */
                        /* Needs to be optimized */
                        ret = pifs_get_pages(TRUE, ba, 1, &management_page_count, &data_page_count);
                        if ( ret == PIFS_SUCCESS && data_page_count > 0)
                        {
                            a_header->least_weared_blocks[i].block_address = ba;
                            last_wear_level_cntr = wear_level_entry.wear_level_cntr;
                        }
#endif
                    }
                }
            }
        }
        wear_level_cntr_min = last_wear_level_cntr;
    }
#endif
    PIFS_WARNING_MSG("List: ");
    for (i = 0; i < PIFS_LEAST_WEARED_BLOCK_NUM && ret == PIFS_SUCCESS; i++)
    {
        printf("%i ", a_header->least_weared_blocks[i].block_address);
    }
    printf("\r\n");

    return ret;
}

/**
 * @brief pifs_generate_most_weared_blocks Generate the list of most weared
 * blocks.
 *
 * @param[in] a_header Pointer to file system's header.
 * @return PIFS_SUCCESS if list successfully generated.
 */
pifs_status_t pifs_generate_most_weared_blocks(pifs_header_t * a_header)
{
    pifs_status_t             ret = PIFS_SUCCESS;
    pifs_block_address_t      ba;
    pifs_wear_level_entry_t   wear_level_entry;
    pifs_wear_level_cntr_t    wear_level_cntr_max;
    pifs_wear_level_cntr_t    last_wear_level_cntr;
    pifs_size_t               i;
    pifs_size_t               j;
    bool_t                    used = FALSE;
#if 0
    pifs_size_t               management_page_count;
    pifs_size_t               data_page_count;
#endif

    ret = pifs_get_block_wear_stats(PIFS_BLOCK_TYPE_DATA,
            a_header,
            NULL,
            &(a_header->most_weared_blocks[0].block_address),
            NULL,
            &wear_level_cntr_max);

#if PIFS_MOST_WEARED_BLOCK_NUM > 1
    for (i = 1; i < PIFS_MOST_WEARED_BLOCK_NUM && ret == PIFS_SUCCESS; i++)
    {
        last_wear_level_cntr = 0;
        for (ba = PIFS_FLASH_BLOCK_RESERVED_NUM; ba < PIFS_FLASH_BLOCK_NUM_FS
             && ret == PIFS_SUCCESS; ba++)
        {
            if (pifs_is_block_type(ba, PIFS_BLOCK_TYPE_DATA, a_header))
            {
                /* Check if this block has not added already to the list */
                used = FALSE;
                for (j = 0; j < i && !used; j++)
                {
                    if (a_header->most_weared_blocks[j].block_address == ba)
                    {
                        used = TRUE;
                    }
                }
                if (!used)
                {
                    ret = pifs_get_wear_level(ba, a_header, &wear_level_entry);
                    if (ret == PIFS_SUCCESS
                            && wear_level_entry.wear_level_cntr <= wear_level_cntr_max
                            && wear_level_entry.wear_level_cntr > last_wear_level_cntr )
                    {
#if 1
                        a_header->most_weared_blocks[i].block_address = ba;
                        last_wear_level_cntr = wear_level_entry.wear_level_cntr;
#else
                        /* Get free pages in this block */
                        /* TODO Unfortunately this will get pages for the same block again and again */
                        /* Needs to be optimized */
                        ret = pifs_get_pages(TRUE, ba, 1, &management_page_count, &data_page_count);
                        if ( ret == PIFS_SUCCESS && data_page_count > 0)
                        {
                            a_header->most_weared_blocks[i].block_address = ba;
                            last_wear_level_cntr = wear_level_entry.wear_level_cntr;
                        }
#endif
                    }
                }
            }
        }
        wear_level_cntr_max = last_wear_level_cntr;
    }
#endif

    return ret;
}

/**
 * @brief pifs_check_block Check if specified block is used by file as data
 * block.
 * This function is used for static wear leveling.
 *
 * @param[in] a_filename        File name to check.
 * @param[in] a_block_address   Block address to look for.
 * @param[out] a_is_block_used  TRUE: block address is used by file.
 * @return PIFS_SUCCESS if file was scanned successfully.
 */
pifs_status_t pifs_check_block(pifs_char_t * a_filename,
                               pifs_block_address_t a_block_address,
                               bool_t * a_is_block_used)
{
    pifs_status_t ret;
    bool_t        is_block_used = FALSE;

    PIFS_GET_MUTEX();

    ret = pifs_internal_open(&pifs.internal_file, a_filename, "r", FALSE);
    if (ret == PIFS_SUCCESS)
    {
        do
        {
            if (pifs.internal_file.rw_address.block_address == a_block_address)
            {
                is_block_used = TRUE;
            }
            ret = pifs_inc_rw_address(&pifs.internal_file, TRUE);
        } while (ret == PIFS_SUCCESS && !is_block_used);
        if (ret == PIFS_ERROR_END_OF_FILE)
        {
            /* Reaching end of file is not an error */
            ret = PIFS_SUCCESS;
        }
        ret = pifs_internal_fclose(&pifs.internal_file, FALSE, TRUE);
    }
    *a_is_block_used = is_block_used;

    PIFS_PUT_MUTEX();

    return ret;
}

/**
 * @brief pifs_dir_walker_empty Callback function used during block emptying.
 * Not only pages found in the specified block are copied, but the whole file
 * in hope that it will be in one block.
 *
 * @param[in] a_dirent    Pointer to directory entry.
 * @param[in] a_func_data Internal data of function. @see pifs_empty_block_t
 *
 * @return PIFS_SUCCESS when file opened successfully.
 */
pifs_status_t pifs_dir_walker_empty(pifs_dirent_t * a_dirent, void * a_func_data)
{
    pifs_status_t        ret = PIFS_SUCCESS;
    pifs_empty_block_t * empty_block = (pifs_empty_block_t*) a_func_data;
    pifs_char_t          tmp_filename[PIFS_FILENAME_LEN_MAX];
    bool_t               is_block_used;

    PIFS_NOTICE_MSG("File '%s', attr: 0x%02X\r\n", a_dirent->d_name, a_dirent->d_attrib);
#if PIFS_ENABLE_DIRECTORIES
    if (!PIFS_IS_DIR(a_dirent->d_attrib))
#endif
    {
        ret = pifs_check_block(a_dirent->d_name, empty_block->block_address, &is_block_used);
        if (ret == PIFS_SUCCESS && is_block_used)
        {
            PIFS_NOTICE_MSG("File '%s' uses block %i\r\n", a_dirent->d_name, empty_block->block_address);
            pifs_tmpnamn(tmp_filename, PIFS_FILENAME_LEN_MAX);
            PIFS_NOTICE_MSG("Copy '%s' to '%s'...\r\n", a_dirent->d_name, tmp_filename);
            ret = pifs_copy(a_dirent->d_name, tmp_filename);
            if (ret == PIFS_SUCCESS)
            {
                PIFS_NOTICE_MSG("Done\r\n");
                PIFS_NOTICE_MSG("Rename '%s' to '%s'...\r\n", tmp_filename, a_dirent->d_name);
                ret = pifs_rename(tmp_filename, a_dirent->d_name);
                if (ret == PIFS_SUCCESS)
                {
                    PIFS_NOTICE_MSG("Done\r\n");
                }
                else
                {
                    PIFS_ERROR_MSG("Cannot rename '%s' to '%s'!\r\n",
                                   tmp_filename, a_dirent->d_name);
                }
            }
            else
            {
                PIFS_ERROR_MSG("Cannot copy '%s' to '%s'!\r\n",
                               a_dirent->d_name, tmp_filename);
            }

            empty_block->is_block_emptied = (ret == PIFS_SUCCESS);
        }
    }

    return ret;
}

/**
 * @brief pifs_empty_block Create copy of all files which found in
 * the specified block. The older files will be deleted, therefore the
 * specified block can be released.
 * Note: there shall be no free pages in the specified block!
 * This function is used for static wear leveling.
 *
 * @param[in] a_block_address Block address to find.
 * @param[out] a_is_emptied   TRUE: the files were moved from the block.
 * @return PIFS_SUCCESS if block was succuessfuly released.
 */
pifs_status_t pifs_empty_block(pifs_block_address_t a_block_address,
                               bool_t * a_is_emptied)
{
    pifs_status_t      ret = PIFS_ERROR_NO_MORE_RESOURCE;
    pifs_empty_block_t empty_block;

    empty_block.is_block_emptied = FALSE;
    empty_block.block_address = a_block_address;
    ret = pifs_walk_dir(PIFS_ROOT_STR, TRUE, TRUE, pifs_dir_walker_empty, &empty_block);

    if (ret == PIFS_SUCCESS)
    {
        *a_is_emptied = empty_block.is_block_emptied;
    }

    return ret;
}

/**
 * @brief pifs_static_wear_leveling Do static wear leveling by moving files
 * from least weared blocks to most weared blocks.
 *
 * @param[in] a_max_block_num Maximum number of blocks to process.
 * @return PIFS_SUCCESS if blocks were processed successfully.
 */
pifs_status_t pifs_static_wear_leveling(pifs_size_t a_max_block_num)
{
    pifs_status_t           ret = PIFS_SUCCESS;
    pifs_size_t             cntr = PIFS_LEAST_WEARED_BLOCK_NUM;
    pifs_size_t             free_data_pages;
    pifs_size_t             free_management_pages;
    pifs_block_address_t    ba;
    bool_t                  is_emptied;
    pifs_wear_level_cntr_t  diff;
    bool_t                  is_data_block;

    PIFS_GET_MUTEX();

    PIFS_ASSERT(!pifs.is_merging);
    if (!pifs.is_wear_leveling)
    {
        PIFS_WARNING_MSG("Static wear leveling started\r\n");

        pifs.is_wear_leveling = TRUE;

        PIFS_NOTICE_MSG("Wear level counter maximum: %i\r\n",
                        pifs.header.wear_level_cntr_max);
        PIFS_NOTICE_MSG("Static wear level limit:    %i\r\n",
                        PIFS_STATIC_WEAR_LEVEL_LIMIT);

        do
        {
            ba = pifs.header.least_weared_blocks[pifs.last_static_wear_block_idx].block_address;
            diff = pifs.header.wear_level_cntr_max - pifs.header.least_weared_blocks[pifs.last_static_wear_block_idx].wear_level_cntr;
            ret = pifs_get_pages(TRUE, ba,
                                 1, &free_management_pages, &free_data_pages);
            is_data_block = pifs_is_block_type(ba, PIFS_BLOCK_TYPE_DATA, &pifs.header);
            PIFS_NOTICE_MSG("Block %3i, free data pages: %3i, diff: %i\r\n", ba, free_data_pages, diff);
            if (ret == PIFS_SUCCESS && !free_data_pages
                    && diff >= PIFS_STATIC_WEAR_LEVEL_LIMIT
                    && is_data_block)
            {
                PIFS_NOTICE_MSG("Empty block %i... \r\n", ba);
                is_emptied = FALSE;
                PIFS_PUT_MUTEX();
                ret = pifs_empty_block(ba, &is_emptied);
                PIFS_GET_MUTEX();
                if (ret == PIFS_SUCCESS)
                {
                    if (is_emptied)
                    {
                        PIFS_NOTICE_MSG("Block %i was emptied\r\n", ba);
                    }
                    else
                    {
                        PIFS_NOTICE_MSG("Block %i was not emptied\r\n", ba);
                    }
                }
                else
                {
                    PIFS_ERROR_MSG("Cannot empty block %i: %i\r\n", ba, ret);
                }
                if (is_emptied)
                {
                    a_max_block_num--;
                }
            }
            pifs.last_static_wear_block_idx++;
            if (pifs.last_static_wear_block_idx == PIFS_LEAST_WEARED_BLOCK_NUM)
            {
                pifs.last_static_wear_block_idx = 0;
            }
        } while (ret == PIFS_SUCCESS && a_max_block_num && cntr--);

        pifs.is_wear_leveling = FALSE;
        PIFS_WARNING_MSG("Static wear leveling exiting\r\n");
    }

    PIFS_PUT_MUTEX();

    return ret;
}

/**
 * @brief pifs_auto_static_wear_leveling Automatic static wear leveling.
 *
 * @return PIFS_SUCCESS if blocks were processed successfully.
 */
pifs_status_t pifs_auto_static_wear_leveling(void)
{
    pifs_status_t ret = PIFS_SUCCESS;

#if PIFS_ENABLE_AUTO_STATIC_WEAR
    PIFS_GET_MUTEX();
    if (pifs.auto_static_wear_cntr == 0)
    {
        PIFS_PUT_MUTEX();
        ret = pifs_static_wear_leveling(PIFS_STATIC_WEAR_LEVEL_BLOCKS);
        PIFS_GET_MUTEX();
        pifs.auto_static_wear_cntr = 100; /* TODO move this number to pifs_config.h */
    }
    else
    {
        pifs.auto_static_wear_cntr--;
    }
    PIFS_PUT_MUTEX();
#endif

    return ret;
}
