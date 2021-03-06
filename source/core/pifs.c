/**
 * @file        pifs.c
 * @brief       Pi file system
 * @author      Copyright (C) Peter Ivanov, 2017
 *
 * Created:     2017-06-11 09:10:19
 * Last modify: 2017-11-30 17:42:26 ivanovp {Time-stamp}
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

#define PIFS_DEBUG_LEVEL 3
#include "pifs_debug.h"

#include "api_pifs.h"
#include "flash.h"
#include "flash_config.h"
#include "pifs.h"
#include "pifs_file.h"
#include "pifs_fsbm.h"
#include "pifs_helper.h"
#include "pifs_delta.h"
#include "pifs_entry.h"
#include "pifs_map.h"
#include "pifs_merge.h"
#include "pifs_wear.h"
#include "pifs_dir.h"
#include "pifs_os.h"
#include "buffer.h" /* DEBUG */

bool_t pifs_initialized = FALSE;
pifs_t pifs;
int pifs_errno = PIFS_SUCCESS;
#if PIFS_ENABLE_OS
PIFS_OS_DEFINE_MUTEX(pifs_mutex);
PIFS_OS_MUTEX_TYPE pifs_mutex;
#endif



/**
 * @brief pifs_calc_header_checksum Calculate checksum of the file system header.
 *
 * @param[in] a_pifs_header Pointer to the file system header.
 * @return The calculated checksum.
 */
static pifs_checksum_t pifs_calc_header_checksum(pifs_header_t * a_pifs_header)
{
    pifs_checksum_t checksum;

    checksum = pifs_calc_checksum(a_pifs_header, sizeof(pifs_header_t) - sizeof(pifs_checksum_t));

    return checksum;
}

/**
 * @brief pifs_flush  Flush cache.
 *
 * @return PIFS_SUCCESS if data written successfully.
 */
pifs_status_t pifs_flush(void)
{
    pifs_status_t ret = PIFS_SUCCESS;
#if PIFS_LOGICAL_PAGE_ENABLED
    pifs_size_t   i;
#endif

    if (pifs.cache_page_buf_is_dirty)
    {
#if PIFS_LOGICAL_PAGE_ENABLED
        for (i = 0; i < PIFS_FLASH_PAGE_PER_LOGICAL_PAGE && ret == PIFS_SUCCESS; i++)
#endif
        {
            ret = pifs_flash_write(pifs.cache_page_buf_address.block_address,
                                   PIFS_LP2FP(pifs.cache_page_buf_address.page_address) + PIFS_LOGICAL_PAGE_IDX(i),
                                   0,
                                   pifs.cache_page_buf + PIFS_LOGICAL_PAGE_IDX(i * PIFS_FLASH_PAGE_SIZE_BYTE),
                                   PIFS_FLASH_PAGE_SIZE_BYTE);
            if (ret == PIFS_SUCCESS)
            {
                pifs.cache_page_buf_is_dirty = FALSE;
            }
            else
            {
                PIFS_ERROR_MSG("Cannot flush buffer %s\r\n",
                               pifs_address2str(&pifs.cache_page_buf_address));
            }
        }
    }

    return ret;
}

/**
 * @brief pifs_read  Cached read.
 *
 * @param[in] a_block_address   Block address of page to read.
 * @param[in] a_page_address    Page address of page to read.
 * @param[in] a_page_offset     Offset in page.
 * @param[out] a_buf            Pointer to buffer to fill or NULL if
 *                              pifs.cache_page_buf is used.
 * @param[in] a_buf_size        Size of buffer. Ignored if a_buf is NULL.
 * @return PIFS_SUCCESS if data read successfully.
 */
pifs_status_t pifs_read(pifs_block_address_t a_block_address,
                        pifs_page_address_t a_page_address,
                        pifs_page_offset_t a_page_offset,
                        void * const a_buf,
                        pifs_size_t a_buf_size)
{
    pifs_status_t ret = PIFS_ERROR_GENERAL;
#if PIFS_LOGICAL_PAGE_ENABLED
    pifs_size_t   i;
#endif

    if (a_block_address == pifs.cache_page_buf_address.block_address
            && a_page_address == pifs.cache_page_buf_address.page_address)
    {
        /* Cache hit */
        if (a_buf)
        {
            memcpy(a_buf, &pifs.cache_page_buf[a_page_offset], a_buf_size);
        }
        ret = PIFS_SUCCESS;
    }
    else
    {
        /* Cache miss, first flush cache */
        ret = pifs_flush();

        if (ret == PIFS_SUCCESS)
        {
#if PIFS_LOGICAL_PAGE_ENABLED
            for (i = 0; i < PIFS_FLASH_PAGE_PER_LOGICAL_PAGE && ret == PIFS_SUCCESS; i++)
#endif
            {
                ret = pifs_flash_read(a_block_address,
                                      PIFS_LP2FP(a_page_address) + PIFS_LOGICAL_PAGE_IDX(i),
                                      0,
                                      pifs.cache_page_buf + PIFS_LOGICAL_PAGE_IDX(i * PIFS_FLASH_PAGE_SIZE_BYTE),
                                      PIFS_FLASH_PAGE_SIZE_BYTE);
            }
        }

        if (ret == PIFS_SUCCESS)
        {
            if (a_buf)
            {
                memcpy(a_buf, &pifs.cache_page_buf[a_page_offset], a_buf_size);
            }
            pifs.cache_page_buf_address.block_address = a_block_address;
            pifs.cache_page_buf_address.page_address = a_page_address;
        }
    }

    return ret;
}

/**
 * @brief pifs_write  Cached write.
 *
 * @param[in] a_block_address   Block address of page to write.
 * @param[in] a_page_address    Page address of page to write.
 * @param[in] a_page_offset     Offset in page.
 * @param[in] a_buf             Pointer to buffer to write or NULL if
 *                              pifs.cache_page_buf is directly written.
 * @param[in] a_buf_size        Size of buffer. Ignored if a_buf is NULL.
 * @return PIFS_SUCCESS if data write successfully.
 */
pifs_status_t pifs_write(pifs_block_address_t a_block_address,
                         pifs_page_address_t a_page_address,
                         pifs_page_offset_t a_page_offset,
                         const void * const a_buf,
                         pifs_size_t a_buf_size)
{
    pifs_status_t ret = PIFS_ERROR_GENERAL;
#if PIFS_LOGICAL_PAGE_ENABLED
    pifs_size_t   i;
#endif

    if (a_block_address == pifs.cache_page_buf_address.block_address
            && a_page_address == pifs.cache_page_buf_address.page_address)
    {
        /* Cache hit */
        if (a_buf)
        {
            memcpy(&pifs.cache_page_buf[a_page_offset], a_buf, a_buf_size);
        }
        pifs.cache_page_buf_is_dirty = TRUE;
        ret = PIFS_SUCCESS;
    }
    else
    {
        /* Cache miss, first flush cache */
        ret = pifs_flush();

        if (ret == PIFS_SUCCESS)
        {
            if (a_page_offset != 0 || a_buf_size != PIFS_LOGICAL_PAGE_SIZE_BYTE)
            {
#if PIFS_LOGICAL_PAGE_ENABLED
                for (i = 0; i < PIFS_FLASH_PAGE_PER_LOGICAL_PAGE && ret == PIFS_SUCCESS; i++)
#endif
                {
                    /* Only part of page is written */
                    ret = pifs_flash_read(a_block_address,
                                          PIFS_LP2FP(a_page_address) + PIFS_LOGICAL_PAGE_IDX(i),
                                          0,
                                          pifs.cache_page_buf + PIFS_LOGICAL_PAGE_IDX(i * PIFS_FLASH_PAGE_SIZE_BYTE),
                                          PIFS_FLASH_PAGE_SIZE_BYTE);
                }
            }

            if (a_buf)
            {
                memcpy(&pifs.cache_page_buf[a_page_offset], a_buf, a_buf_size);
            }
            pifs.cache_page_buf_address.block_address = a_block_address;
            pifs.cache_page_buf_address.page_address = a_page_address;
            pifs.cache_page_buf_is_dirty = TRUE;
        }
    }

    return ret;
}

/**
 * @brief pifs_erase  Cached erase.
 *
 * @param[in] a_block_address   Block address of page to erase.
 * @param[in] a_old_header      Old file system's header.
 * @param[in] a_new_header      New (not yet used) file system's header.
 * @return PIFS_SUCCESS if data erased successfully.
 */
pifs_status_t pifs_erase(pifs_block_address_t a_block_address, pifs_header_t * a_old_header, pifs_header_t * a_new_header)
{
    pifs_status_t           ret = PIFS_ERROR_GENERAL;

    (void) a_old_header;

    PIFS_DEBUG_MSG("Erasing block %i\r\n", a_block_address)
    ret = pifs_flash_erase(a_block_address);

    if (a_block_address == pifs.cache_page_buf_address.block_address)
    {
        /* If the block was erased which contains the cached page, simply forget it */
        pifs.cache_page_buf_address.block_address = PIFS_BLOCK_ADDRESS_INVALID;
        pifs.cache_page_buf_address.page_address = PIFS_PAGE_ADDRESS_INVALID;
        pifs.cache_page_buf_is_dirty = FALSE;
    }

    if (ret == PIFS_SUCCESS && a_new_header)
    {
        /* Increase wear level */
        ret = pifs_inc_wear_level(a_block_address, a_new_header);
    }

    return ret;
}

/**
 * @brief pifs_header_init Initialize file system's header.
 *
 * @param[in] a_block_address           Block address of header.
 * @param[in] a_page_address            Page address of header.
 * @param[in] a_next_mgmt_block_address Index of next management block to use.
 * @param[out] a_header                 Pointer to the header to be initialized.
 */
pifs_status_t pifs_header_init(pifs_block_address_t a_block_address,
                               pifs_page_address_t a_page_address,
                               pifs_block_address_t a_next_mgmt_block_address,
                               pifs_header_t * a_header)
{
    pifs_status_t        ret = PIFS_SUCCESS;
    pifs_block_address_t ba = a_block_address;
    pifs_address_t       address;
    pifs_size_t          i;

    PIFS_DEBUG_MSG("Creating managamenet block %s\r\n", pifs_ba_pa2str(a_block_address, a_page_address));
    a_header->magic = PIFS_MAGIC;
#if PIFS_ENABLE_VERSION
    a_header->majorVersion = PIFS_MAJOR_VERSION;
    a_header->minorVersion = PIFS_MINOR_VERSION;
#endif
    if (a_next_mgmt_block_address == PIFS_BLOCK_ADDRESS_ERASED)
    {
        a_header->counter++;
        for (i = 0; i < PIFS_LEAST_WEARED_BLOCK_NUM; i++)
        {
            a_header->least_weared_blocks[i].block_address = PIFS_BLOCK_ADDRESS_ERASED;
            a_header->least_weared_blocks[i].wear_level_cntr = PIFS_WEAR_LEVEL_CNTR_MAX;
        }
        for (i = 0; i < PIFS_MOST_WEARED_BLOCK_NUM; i++)
        {
            a_header->most_weared_blocks[i].block_address = PIFS_BLOCK_ADDRESS_ERASED;
            a_header->most_weared_blocks[i].wear_level_cntr = PIFS_WEAR_LEVEL_CNTR_MAX;
        }
        a_header->wear_level_cntr_max = PIFS_WEAR_LEVEL_CNTR_MAX;
    }
#if PIFS_ENABLE_CONFIG_IN_FLASH
    /* Flash configuration */
    a_header->flash_block_num_all = PIFS_FLASH_BLOCK_NUM_ALL;
    a_header->flash_block_reserved_num = PIFS_FLASH_BLOCK_RESERVED_NUM;
    a_header->flash_page_per_block = PIFS_FLASH_PAGE_PER_BLOCK;
    a_header->flash_page_size_byte = PIFS_FLASH_PAGE_SIZE_BYTE;
    /* File system configuration */
    a_header->logical_page_size_byte = PIFS_LOGICAL_PAGE_SIZE_BYTE;
    a_header->filename_len_max = PIFS_FILENAME_LEN_MAX;
    a_header->entry_num_max = PIFS_ENTRY_NUM_MAX;
    a_header->user_data_size_byte = PIFS_USER_DATA_SIZE_BYTE;
    a_header->management_block_num = PIFS_MANAGEMENT_BLOCK_NUM;
    a_header->least_weared_block_num = PIFS_LEAST_WEARED_BLOCK_NUM;
    a_header->most_weared_block_num = PIFS_MOST_WEARED_BLOCK_NUM;
    a_header->delta_map_page_num = PIFS_DELTA_MAP_PAGE_NUM;
    a_header->map_page_count_size = PIFS_MAP_PAGE_COUNT_SIZE;
    a_header->use_delta_for_entries = PIFS_USE_DELTA_FOR_ENTRIES;
    a_header->enable_directories = PIFS_ENABLE_DIRECTORIES;
    a_header->enable_crc = PIFS_ENABLE_CRC;
#endif
    address.block_address = a_block_address;
    address.page_address = a_page_address;
    pifs_add_address(&address, PIFS_HEADER_SIZE_PAGE);
    a_header->root_entry_list_address = address;
    pifs_add_address(&address, PIFS_ENTRY_LIST_SIZE_PAGE);
    a_header->free_space_bitmap_address = address;
    pifs_add_address(&address, PIFS_FREE_SPACE_BITMAP_SIZE_PAGE);
    a_header->delta_map_address = address;
    pifs_add_address(&address, PIFS_DELTA_MAP_PAGE_NUM);
    a_header->wear_level_list_address = address;
    if ((address.block_address - a_block_address) > (pifs_block_address_t)PIFS_MANAGEMENT_BLOCK_NUM)
    {
        /* Not enough space for management pages */
        ret = PIFS_ERROR_CONFIGURATION;
    }
    a_header->management_block_address = ba;
    a_header->next_management_block_address = a_next_mgmt_block_address;
    if (a_next_mgmt_block_address != PIFS_BLOCK_ADDRESS_ERASED)
    {
        a_header->checksum = pifs_calc_header_checksum(a_header);
    }
    else
    {
        a_header->checksum = PIFS_CHECKSUM_ERASED;
    }

    return ret;
}


/**
 * @brief pifs_header_write Write file system header.
 *
 * @param[in] a_block_address   Block address of header.
 * @param[in] a_page_address    Page address of header.
 * @param[in] a_header          Pointer to the header.
 * @param[in] a_mark_pages      TRUE: mark management pages as used ones.
 * @return PIFS_SUCCESS if header successfully written.
 */
pifs_status_t pifs_header_write(pifs_block_address_t a_block_address,
                                pifs_page_address_t a_page_address,
                                pifs_header_t * a_header,
                                bool_t a_mark_pages)
{
    pifs_status_t ret = PIFS_ERROR_GENERAL;
    pifs_size_t   free_management_bytes = 0;
    pifs_size_t   free_data_bytes = 0;
    pifs_size_t   free_management_pages = 0;
    pifs_size_t   free_data_pages = 0;
    pifs_size_t   free_entries = 0;
    pifs_size_t   to_be_released_entries = 0;

    ret = pifs_write(a_block_address, a_page_address, 0, a_header, sizeof(pifs_header_t));
    if (ret == PIFS_SUCCESS)
    {
        pifs.is_header_found = TRUE;
        pifs.header_address.block_address = a_block_address;
        pifs.header_address.page_address = a_page_address;
        if (a_header->counter == 0)
        {
            /* Initialize wear level list for the very first header */
            ret = pifs_wear_level_list_init();
        }
    }
    if (a_mark_pages)
    {
        if (ret == PIFS_SUCCESS)
        {
            /* Mark file system header as used */
            ret = pifs_mark_page(a_block_address,
                                 a_page_address,
                                 PIFS_HEADER_SIZE_PAGE, TRUE, FALSE);
        }
        if (ret == PIFS_SUCCESS)
        {
            PIFS_DEBUG_MSG("Marking entry list %s, %i pages\r\n",
                           pifs_address2str(&a_header->root_entry_list_address),
                           PIFS_ENTRY_LIST_SIZE_PAGE);
            /* Mark entry list as used */
            ret = pifs_mark_page(a_header->root_entry_list_address.block_address,
                                 a_header->root_entry_list_address.page_address,
                                 PIFS_ENTRY_LIST_SIZE_PAGE, TRUE, FALSE);
        }
        if (ret == PIFS_SUCCESS)
        {
            /* Mark free space bitmap as used */
            ret = pifs_mark_page(a_header->free_space_bitmap_address.block_address,
                                 a_header->free_space_bitmap_address.page_address,
                                 PIFS_FREE_SPACE_BITMAP_SIZE_PAGE, TRUE, FALSE);
        }
        if (ret == PIFS_SUCCESS)
        {
            /* Mark delta page map as used */
            ret = pifs_mark_page(a_header->delta_map_address.block_address,
                                 a_header->delta_map_address.page_address,
                                 PIFS_DELTA_MAP_PAGE_NUM, TRUE, FALSE);
        }
        if (ret == PIFS_SUCCESS)
        {
            /* Mark wear level list as used */
            ret = pifs_mark_page(a_header->wear_level_list_address.block_address,
                                 a_header->wear_level_list_address.page_address,
                                 PIFS_WEAR_LEVEL_LIST_SIZE_PAGE, TRUE, FALSE);
        }
    }
    PIFS_INFO_MSG("Counter: %i\r\n",
                  a_header->counter);
    PIFS_INFO_MSG("Entry list at %s\r\n",
                  pifs_address2str(&a_header->root_entry_list_address));
    PIFS_INFO_MSG("Free space bitmap at %s\r\n",
                  pifs_address2str(&a_header->free_space_bitmap_address));
    PIFS_INFO_MSG("Delta page map at %s\r\n",
                  pifs_address2str(&a_header->delta_map_address));
    PIFS_INFO_MSG("Wear level list at %s\r\n",
                  pifs_address2str(&a_header->wear_level_list_address));
    if (ret == PIFS_SUCCESS)
    {
        /* Deliberately avoiding return code, it is not an error */
        /* if there is no more space */
        PIFS_PUT_MUTEX();
        (void)pifs_get_free_space(&free_management_bytes, &free_data_bytes,
                                  &free_management_pages, &free_data_pages);
        PIFS_GET_MUTEX();
        PIFS_INFO_MSG("Free data area:                     %lu bytes, %lu pages\r\n",
                      free_data_bytes, free_data_pages);
        PIFS_INFO_MSG("Free management area:               %lu bytes, %lu pages\r\n",
                      free_management_bytes, free_management_pages);
        /* Deliberately avoiding return code, it is not an error */
        /* if there is no more space */
        (void)pifs_count_entries(&free_entries, &to_be_released_entries,
                                 pifs.header.root_entry_list_address.block_address,
                                 pifs.header.root_entry_list_address.page_address);
        PIFS_NOTICE_MSG("free_entries: %lu, to_be_released_entries: %lu\r\n",
                        free_entries, to_be_released_entries);
    }

    return ret;
}

/**
 * @brief pifs_fs_info Print information about flash memory and filesystem.
 */
void pifs_print_fs_info(void)
{
    PIFS_PRINT_MSG("Geometry of flash memory\r\n");
    PIFS_PRINT_MSG("------------------------\r\n");
    PIFS_PRINT_MSG("Size of flash memory (all):         %i bytes, %i KiB\r\n", PIFS_FLASH_SIZE_BYTE_ALL, PIFS_FLASH_SIZE_BYTE_ALL / 1024);
    PIFS_PRINT_MSG("Size of flash memory (used by FS):  %i bytes, %i KiB\r\n", PIFS_FLASH_SIZE_BYTE_FS, PIFS_FLASH_SIZE_BYTE_FS / 1024);
    PIFS_PRINT_MSG("Size of block:                      %i bytes\r\n", PIFS_FLASH_BLOCK_SIZE_BYTE);
    PIFS_PRINT_MSG("Size of page:                       %i bytes\r\n", PIFS_FLASH_PAGE_SIZE_BYTE);
    PIFS_PRINT_MSG("Number of blocks (all):             %i\r\n", PIFS_FLASH_BLOCK_NUM_ALL);
    PIFS_PRINT_MSG("Number of blocks (used by FS)):     %i\r\n", PIFS_FLASH_BLOCK_NUM_FS);
    PIFS_PRINT_MSG("Number of pages/block:              %i\r\n", PIFS_FLASH_PAGE_PER_BLOCK);
    PIFS_PRINT_MSG("Number of pages (all):              %i\r\n", PIFS_FLASH_PAGE_NUM_ALL);
    PIFS_PRINT_MSG("Number of pages (used by FS)):      %i\r\n", PIFS_FLASH_PAGE_NUM_FS);
    PIFS_PRINT_MSG("\r\n");
    PIFS_PRINT_MSG("Geometry of file system\r\n");
    PIFS_PRINT_MSG("-----------------------\r\n");
    PIFS_PRINT_MSG("Size of logical page:               %i bytes\r\n", PIFS_LOGICAL_PAGE_SIZE_BYTE);
    PIFS_PRINT_MSG("Block address size:                 %lu bytes\r\n", sizeof(pifs_block_address_t));
    PIFS_PRINT_MSG("Page address size:                  %lu bytes\r\n", sizeof(pifs_page_address_t));
    PIFS_PRINT_MSG("Header size:                        %lu bytes, %lu logical pages\r\n", PIFS_HEADER_SIZE_BYTE, PIFS_HEADER_SIZE_PAGE);
    PIFS_PRINT_MSG("Entry size:                         %lu bytes\r\n", PIFS_ENTRY_SIZE_BYTE);
    PIFS_PRINT_MSG("Entry size in a page:               %lu bytes\r\n", PIFS_ENTRY_SIZE_BYTE * PIFS_ENTRY_PER_PAGE);
    PIFS_PRINT_MSG("Entry list size:                    %lu bytes, %lu logical pages\r\n", PIFS_ENTRY_LIST_SIZE_BYTE, PIFS_ENTRY_LIST_SIZE_PAGE);
    PIFS_PRINT_MSG("Free space bitmap size:             %u bytes, %u logical pages\r\n", PIFS_FREE_SPACE_BITMAP_SIZE_BYTE, PIFS_FREE_SPACE_BITMAP_SIZE_PAGE);
    PIFS_PRINT_MSG("Map header size:                    %lu bytes\r\n", PIFS_MAP_HEADER_SIZE_BYTE);
    PIFS_PRINT_MSG("Map entry size:                     %lu bytes\r\n", PIFS_MAP_ENTRY_SIZE_BYTE);
    PIFS_PRINT_MSG("Number of map entries/page:         %lu\r\n", PIFS_MAP_ENTRY_PER_PAGE);
    PIFS_PRINT_MSG("Delta entry size:                   %lu bytes\r\n", PIFS_DELTA_ENTRY_SIZE_BYTE);
    PIFS_PRINT_MSG("Number of delta entries/page:       %lu\r\n", PIFS_DELTA_ENTRY_PER_PAGE);
    PIFS_PRINT_MSG("Number of delta entries:            %lu\r\n", PIFS_DELTA_ENTRY_PER_PAGE * PIFS_DELTA_MAP_PAGE_NUM);
    PIFS_PRINT_MSG("Delta map size:                     %u bytes, %u logical pages\r\n", PIFS_DELTA_MAP_PAGE_NUM * PIFS_LOGICAL_PAGE_SIZE_BYTE, PIFS_DELTA_MAP_PAGE_NUM);
    PIFS_PRINT_MSG("Wear level entry size:              %lu bytes\r\n", PIFS_WEAR_LEVEL_ENTRY_SIZE_BYTE);
    PIFS_PRINT_MSG("Number of wear level entries/page:  %lu\r\n", PIFS_WEAR_LEVEL_ENTRY_PER_PAGE);
    PIFS_PRINT_MSG("Number of wear level entries:       %lu\r\n", PIFS_FLASH_BLOCK_NUM_FS);
    PIFS_PRINT_MSG("Wear level map size:                %u bytes, %u logical pages\r\n", PIFS_WEAR_LEVEL_LIST_SIZE_BYTE, PIFS_WEAR_LEVEL_LIST_SIZE_PAGE);
    PIFS_PRINT_MSG("Minimum management area:            %lu logical pages, %lu blocks\r\n", PIFS_MANAGEMENT_PAGE_NUM_MIN,
           PIFS_MANAGEMENT_BLOCK_NUM_MIN);
    PIFS_PRINT_MSG("Recommended management area:        %lu logical pages, %lu blocks\r\n", PIFS_MANAGEMENT_PAGE_NUM_RECOMM,
           PIFS_MANAGEMENT_BLOCK_NUM_RECOMM);
    PIFS_PRINT_MSG("Full reserved area for management:  %i bytes, %i logical pages\r\n",
           PIFS_MANAGEMENT_BLOCK_NUM * 2 * PIFS_FLASH_BLOCK_SIZE_BYTE,
           PIFS_MANAGEMENT_BLOCK_NUM * 2 * PIFS_LOGICAL_PAGE_PER_BLOCK);
    PIFS_PRINT_MSG("Size of management area:            %i bytes, %i logical pages\r\n",
           PIFS_MANAGEMENT_BLOCK_NUM * PIFS_FLASH_BLOCK_SIZE_BYTE,
           PIFS_MANAGEMENT_BLOCK_NUM * PIFS_LOGICAL_PAGE_PER_BLOCK);
    PIFS_PRINT_MSG("\r\n");
    PIFS_PRINT_MSG("File system in RAM:                 %lu bytes\r\n", sizeof(pifs_t));
}

/**
 * @brief pifs_print_header_info Print information about file system's header.
 */
void pifs_print_header_info(void)
{
    PIFS_PRINT_MSG("Counter: %i\r\n",
           pifs.header.counter);
    PIFS_PRINT_MSG("Entry list at %s\r\n",
           pifs_address2str(&pifs.header.root_entry_list_address));
    PIFS_PRINT_MSG("Free space bitmap at %s\r\n",
           pifs_address2str(&pifs.header.free_space_bitmap_address));
    PIFS_PRINT_MSG("Delta page map at %s\r\n",
           pifs_address2str(&pifs.header.delta_map_address));
    PIFS_PRINT_MSG("Wear level list at %s\r\n",
           pifs_address2str(&pifs.header.wear_level_list_address));
}

/**
 * @brief pifs_print_free_space_info Print information about free space.
 */
void pifs_print_free_space_info(void)
{
    size_t        free_management_bytes = 0;
    size_t        free_data_bytes = 0;
    size_t        free_management_pages = 0;
    size_t        free_data_pages = 0;
    size_t        to_be_released_management_bytes = 0;
    size_t        to_be_released_data_bytes = 0;
    size_t        to_be_released_management_pages = 0;
    size_t        to_be_released_data_pages = 0;
    pifs_size_t   free_entries = 0;
    pifs_size_t   to_be_released_entries = 0;
    pifs_status_t ret;

    ret = pifs_get_free_space(&free_management_bytes, &free_data_bytes,
                              &free_management_pages, &free_data_pages);
    if (ret == PIFS_SUCCESS || ret == PIFS_ERROR_NO_MORE_SPACE)
    {
        PIFS_PRINT_MSG("Free data area:                     %lu bytes, %lu pages\r\n",
               free_data_bytes, free_data_pages);
        PIFS_PRINT_MSG("Free management area:               %lu bytes, %lu pages\r\n",
               free_management_bytes, free_management_pages);
    }
    ret = pifs_get_to_be_released_space(&to_be_released_management_bytes, &to_be_released_data_bytes,
                                        &to_be_released_management_pages, &to_be_released_data_pages);
    if (ret == PIFS_SUCCESS || ret == PIFS_ERROR_NO_MORE_SPACE)
    {
        PIFS_PRINT_MSG("To be released data area:           %lu bytes, %lu pages\r\n",
               to_be_released_data_bytes, to_be_released_data_pages);
        PIFS_PRINT_MSG("To be released management area:     %lu bytes, %lu pages\r\n",
               to_be_released_management_bytes, to_be_released_management_pages);
    }
    if (ret == PIFS_SUCCESS || ret == PIFS_ERROR_NO_MORE_SPACE)
    {
        ret = pifs_count_entries(&free_entries, &to_be_released_entries,
                                 pifs.header.root_entry_list_address.block_address,
                                 pifs.header.root_entry_list_address.page_address);
    }
    if (ret == PIFS_SUCCESS || ret == PIFS_ERROR_NO_MORE_ENTRY)
    {
        PIFS_PRINT_MSG("Free entries:                       %lu\r\n", free_entries);
        PIFS_PRINT_MSG("To be released entries:             %lu\r\n", to_be_released_entries);
    }
}


/**
 * @brief pifs_init Initialize flash driver and file system.
 *
 * @return PIFS_SUCCESS if FS successfully initialized.
 */
pifs_status_t pifs_init(void)
{
    pifs_status_t        ret = PIFS_SUCCESS;
    pifs_block_address_t ba;
    pifs_page_address_t  pa;
    pifs_header_t        header;
    pifs_header_t        prev_header;
    pifs_checksum_t      checksum;
    pifs_size_t          i;
    uint8_t              retry_cntr = 5;

#if PIFS_ENABLE_OS
    pifs_mutex = PIFS_OS_CREATE_MUTEX(pifs_mutex);
#endif

    PIFS_GET_MUTEX();

    pifs_initialized = FALSE;
    pifs.header_address.block_address = PIFS_BLOCK_ADDRESS_INVALID;
    pifs.header_address.page_address = PIFS_PAGE_ADDRESS_INVALID;
    pifs.is_header_found = FALSE;
    pifs.is_merging = FALSE;
    pifs.is_wear_leveling = FALSE;
    memset(&pifs.header, 0, PIFS_HEADER_SIZE_BYTE);
    memset(pifs.cache_page_buf, 0, PIFS_LOGICAL_PAGE_SIZE_BYTE);
    pifs.cache_page_buf_address.block_address = PIFS_BLOCK_ADDRESS_INVALID;
    pifs.cache_page_buf_address.page_address = PIFS_PAGE_ADDRESS_INVALID;
    pifs.cache_page_buf_is_dirty = FALSE;
    memset(pifs.file, 0, sizeof(pifs.file));
    memset(&pifs.internal_file, 0, sizeof(pifs.internal_file));
    memset(pifs.dir, 0, sizeof(pifs.dir));
    memset(pifs.delta_map_page_buf, 0, sizeof(pifs.delta_map_page_buf));
    pifs.delta_map_page_is_read = FALSE;
    pifs.delta_map_page_is_dirty = FALSE;
    memset(pifs.dmw_page_buf, 0, sizeof(pifs.dmw_page_buf));
    memset(pifs.sc_page_buf, 0, sizeof(pifs.sc_page_buf));
    pifs.error_cntr = 0;
    pifs.last_static_wear_block_idx = 0;
    pifs.auto_static_wear_cntr = 0;
#if PIFS_ENABLE_DIRECTORIES
    for (i = 0; i < PIFS_TASK_COUNT_MAX; i++)
    {
        pifs.cwd[i][0] = PIFS_ROOT_CHAR;
        pifs.cwd[i][1] = PIFS_EOS;
#if PIFS_OS_TASK_ID_IS_SEQUENTIAL == 0 && PIFS_SEPARATE_WORKDIR_FOR_TASKS
        pifs.task_ids[i] = PIFS_OS_TASK_ID_NULL;
#endif
    }
#endif

#if PIFS_DEBUG_LEVEL >= 5
    pifs_print_fs_info();
#endif

    if (PIFS_HEADER_SIZE_BYTE > PIFS_LOGICAL_PAGE_SIZE_BYTE)
    {
        PIFS_ERROR_MSG("Header size (%lu) is larger than logical page (%u)!\r\n"
                       "Decrease PIFS_LEAST_WEARED_BLOCK_NUM or increase PIFS_LOGICAL_PAGE_SIZE_BYTE!\r\n",
                       PIFS_HEADER_SIZE_BYTE, PIFS_LOGICAL_PAGE_SIZE_BYTE);
        ret = PIFS_ERROR_CONFIGURATION;
    }


    if (PIFS_ENTRY_SIZE_BYTE > PIFS_LOGICAL_PAGE_SIZE_BYTE)
    {
        PIFS_ERROR_MSG("Entry size (%lu) is larger than logical page (%u)!\r\n"
                       "Change PIFS_FILENAME_LEN_MAX to %lu or increase PIFS_LOGICAL_PAGE_SIZE_BYTE!\r\n",
                       PIFS_ENTRY_SIZE_BYTE, PIFS_LOGICAL_PAGE_SIZE_BYTE,
                       PIFS_FILENAME_LEN_MAX - (PIFS_ENTRY_SIZE_BYTE - PIFS_LOGICAL_PAGE_SIZE_BYTE));
        ret = PIFS_ERROR_CONFIGURATION;
    }

    if (PIFS_MANAGEMENT_BLOCK_NUM_MIN > PIFS_MANAGEMENT_BLOCK_NUM)
    {
        PIFS_ERROR_MSG("Cannot fit data in management block!\r\n");
        PIFS_ERROR_MSG("Decrease PIFS_ENTRY_NUM_MAX or PIFS_FILENAME_LEN_MAX or PIFS_DELTA_PAGES_NUM!\r\n");
        PIFS_ERROR_MSG("Or increase PIFS_MANAGEMENT_BLOCK_NUM to %lu!\r\n",
                       PIFS_MANAGEMENT_BLOCK_NUM_MIN);
        ret = PIFS_ERROR_CONFIGURATION;
    }

    if (PIFS_MANAGEMENT_BLOCK_NUM_RECOMM > PIFS_MANAGEMENT_BLOCK_NUM)
    {
        PIFS_WARNING_MSG("Recommended PIFS_MANAGEMENT_BLOCK_NUM is %lu!\r\n", PIFS_MANAGEMENT_BLOCK_NUM_RECOMM);
    }

    if (((PIFS_LOGICAL_PAGE_SIZE_BYTE - (PIFS_ENTRY_PER_PAGE * PIFS_ENTRY_SIZE_BYTE)) / PIFS_ENTRY_PER_PAGE) > 0)
    {
        PIFS_NOTICE_MSG("PIFS_FILENAME_LEN_MAX can be increased by %lu with same entry list size.\r\n",
                        (PIFS_LOGICAL_PAGE_SIZE_BYTE - (PIFS_ENTRY_PER_PAGE * PIFS_ENTRY_SIZE_BYTE)) / PIFS_ENTRY_PER_PAGE
                        );
    }

    if (ret == PIFS_SUCCESS)
    {
        do
        {
            ret = pifs_flash_init();
            if (ret != PIFS_SUCCESS)
            {
                PIFS_OS_DELAY_MS(250);
            }
        } while (ret != PIFS_SUCCESS && retry_cntr--);
    }

    if (ret == PIFS_SUCCESS)
    {
        /* Find latest management block */
        for (ba = PIFS_FLASH_BLOCK_RESERVED_NUM; ba < PIFS_FLASH_BLOCK_NUM_ALL && ret == PIFS_SUCCESS; ba++)
        {
            pa = 0;
            ret = pifs_read(ba, pa, 0, &header, sizeof(header));
            if (ret == PIFS_SUCCESS && header.magic == PIFS_MAGIC
#if PIFS_ENABLE_VERSION
                    && header.majorVersion == PIFS_MAJOR_VERSION
                    && header.minorVersion == PIFS_MINOR_VERSION
#endif
               )
            {
                PIFS_DEBUG_MSG("Management page found: %s\r\n", pifs_ba_pa2str(ba, pa));
                checksum = pifs_calc_header_checksum(&header);
                if (checksum == header.checksum)
                {
                    PIFS_DEBUG_MSG("Checksum is valid\r\n");
                    if (pifs.is_header_found)
                    {
                        PIFS_WARNING_MSG("Previous management page was not erased! Erasing...\r\n");
                        /* This can happen when pifs_merge() was interrupted before step #11 */
                        /* Erase old management area */
                        for (i = 0; i < PIFS_MANAGEMENT_BLOCK_NUM && ret == PIFS_SUCCESS; i++)
                        {
                            ret = pifs_erase(prev_header.management_block_address + i, &prev_header, &header);
                        }
                        if (ret == PIFS_SUCCESS)
                        {
                            PIFS_WARNING_MSG("Done.\r\n");
                        }
                    }
                    if (!pifs.is_header_found || prev_header.counter < pifs.header.counter)
                    {
#if PIFS_ENABLE_CONFIG_IN_FLASH
                        /* Check flash and file system configuration */
                        if (header.flash_block_num_all == PIFS_FLASH_BLOCK_NUM_ALL
                                && header.flash_block_reserved_num == PIFS_FLASH_BLOCK_RESERVED_NUM
                                && header.flash_page_per_block == PIFS_FLASH_PAGE_PER_BLOCK
                                && header.flash_page_size_byte == PIFS_FLASH_PAGE_SIZE_BYTE
                                && header.logical_page_size_byte == PIFS_LOGICAL_PAGE_SIZE_BYTE
                                && header.filename_len_max == PIFS_FILENAME_LEN_MAX
                                && header.entry_num_max == PIFS_ENTRY_NUM_MAX
                                && header.user_data_size_byte == PIFS_USER_DATA_SIZE_BYTE
                                && header.management_block_num == PIFS_MANAGEMENT_BLOCK_NUM
                                && header.least_weared_block_num == PIFS_LEAST_WEARED_BLOCK_NUM
                                && header.most_weared_block_num == PIFS_MOST_WEARED_BLOCK_NUM
                                && header.delta_map_page_num == PIFS_DELTA_MAP_PAGE_NUM
                                && header.map_page_count_size == PIFS_MAP_PAGE_COUNT_SIZE
                                && header.use_delta_for_entries == PIFS_USE_DELTA_FOR_ENTRIES
                                && header.enable_directories == PIFS_ENABLE_DIRECTORIES
                                && header.enable_crc == PIFS_ENABLE_CRC)
#endif
                        {
                            pifs.is_header_found = TRUE;
                            pifs.header_address.block_address = ba;
                            pifs.header_address.page_address = pa;
                            memcpy(&prev_header, &header, sizeof(prev_header));
                        }
#if PIFS_ENABLE_CONFIG_IN_FLASH
                        else
                        {
                            PIFS_WARNING_MSG("Invalid flash/file system configuration!\r\n");
                        }
#endif
                    }
                }
                else
                {
                    PIFS_WARNING_MSG("Checksum is invalid! Calculated: 0x%02X, read: 0x%02X\r\n",
                            checksum, pifs.header.checksum);
                }
            }
        }

        if (pifs.is_header_found)
        {
            memcpy(&pifs.header, &prev_header, sizeof(pifs.header));
        }
        else
        {
            /* No file system header found, so create brand new one */
            PIFS_WARNING_MSG("No file system header found, creating...\r\n");
            pifs.header.counter = 0;
            ba = PIFS_FLASH_BLOCK_RESERVED_NUM;
            pa = 0;
            ret = pifs_header_init(ba, pa, ba + PIFS_MANAGEMENT_BLOCK_NUM, &pifs.header);
            if (ret == PIFS_SUCCESS)
            {
                PIFS_WARNING_MSG("Erasing all blocks...\r\n");
                for (i = PIFS_FLASH_BLOCK_RESERVED_NUM; i < PIFS_FLASH_BLOCK_NUM_ALL; i++)
                {
                    ret = pifs_flash_erase(i);
                    /* TODO mark bad blocks */
                }
                PIFS_WARNING_MSG("Done.\r\n");
            }
            if (ret == PIFS_SUCCESS)
            {
                ret = pifs_header_write(ba, pa, &pifs.header, TRUE);
            }
        }

        if (pifs.is_header_found && ret == PIFS_SUCCESS)
        {
#if PIFS_ENABLE_DIRECTORIES
            /* Current working directory is the root directory */
            for (i = 0; i < PIFS_TASK_COUNT_MAX; i++)
            {
                pifs.current_entry_list_address[i] = pifs.header.root_entry_list_address;
            }
#endif
            ret = pifs_get_free_pages(&i, &pifs.free_data_page_num);
            pifs_initialized = TRUE;
#if PIFS_DEBUG_LEVEL >= 6
            print_buffer(&pifs.header, sizeof(pifs.header), 0);
#endif
#if PIFS_DEBUG_LEVEL >= 5
            pifs_print_header_info();
            pifs_print_free_space_info();
#endif
        }
    }

    PIFS_PUT_MUTEX();

    return ret;
}

/**
 * @brief pifs_delete Shut down file system.
 *
 * @return PIFS_SUCCESS if caches successfully flushed and flash I/F deleted.
 */
pifs_status_t pifs_delete(void)
{
    pifs_status_t ret = PIFS_ERROR_GENERAL;

    if (pifs_initialized)
    {
        /* Flush cache */
        ret = pifs_flush();

        ret = pifs_flash_delete();

#if PIFS_ENABLE_OS
        PIFS_OS_DELETE_MUTEX(pifs_mutex);
#endif

        pifs_initialized = FALSE;
    }

    return ret;
}

/**
 * @brief pifs_mark_page_check Used during file system check. Mark page
 * as used.
 *
 * @param[out] a_free_page_buf  Bitmap of free pages.
 * @param[in] a_block_address   Block address to mark.
 * @param[in] a_page_address    Page address to mark.
 * @param[in] a_page_count      Number of pages to mark.
 * @return PIFS_SUCCESS if all pages marked successfully.
 */
pifs_status_t pifs_mark_page_check(uint8_t * a_free_page_buf,
                                   pifs_block_address_t a_block_address,
                                   pifs_page_address_t a_page_address,
                                   pifs_page_count_t a_page_count)
{
    pifs_status_t   ret = PIFS_SUCCESS;
    pifs_bit_pos_t  bit_pos;
    pifs_size_t     byte_pos;

    PIFS_INFO_MSG("Marking %s, page count: %i\r\n",
                  pifs_ba_pa2str(a_block_address, a_page_address),
                  a_page_count);
    while (a_page_count-- && ret == PIFS_SUCCESS)
    {
        /* Calculate address of block in the temporary buffer */
        bit_pos = ((a_block_address - PIFS_FLASH_BLOCK_RESERVED_NUM) * PIFS_LOGICAL_PAGE_PER_BLOCK + a_page_address);
        byte_pos = bit_pos / PIFS_BYTE_BITS;
        bit_pos %= PIFS_BYTE_BITS;

        //PIFS_PRINT_MSG("Pos %i, bit %i\r\n", byte_pos, bit_pos);
        /* Mask bit */
        a_free_page_buf[byte_pos] &= ~(1u << bit_pos);

        if (a_page_count)
        {
            ret = pifs_inc_ba_pa(&a_block_address, &a_page_address);
        }
    }

    return ret;
}

/**
 * @brief pifs_is_page_free_check Used during file system check.
 * Check if page is used.
 *
 * @param[in] a_free_page_buf Pointer to free page buffer.
 * @param[in] a_block_address Block address of page(s).
 * @param[in] a_page_address  Page address of page(s).
 * @return TRUE: page is free. FALSE: page is used.
 */
bool_t pifs_is_page_free_check(uint8_t * a_free_page_buf,
                               pifs_block_address_t a_block_address,
                               pifs_page_address_t a_page_address)
{
    pifs_bit_pos_t  bit_pos;
    pifs_size_t     byte_pos;

    bit_pos = ((a_block_address - PIFS_FLASH_BLOCK_RESERVED_NUM) * PIFS_LOGICAL_PAGE_PER_BLOCK + a_page_address);
    byte_pos = bit_pos / PIFS_BYTE_BITS;
    bit_pos %= PIFS_BYTE_BITS;

    //PIFS_PRINT_MSG("Pos %i, bit %i\r\n", byte_pos, bit_pos);
    return (a_free_page_buf[byte_pos] & (1u << bit_pos)) ? TRUE : FALSE;
}

/**
 * @brief pifs_check_file_page Check page if it is allocated.
 * Callback function which is called for file's every data and map pages.
 * If delta block and page are equal to original block address, no delta
 * page is used.
 *
 * @param[in] a_file                 Pointer to file.
 * @param[in] a_block_address        Original block address.
 * @param[in] a_page_address         Original page address.
 * @param[in] a_delta_block_address  Delta block address.
 * @param[in] a_delta_page_address   Delta page address.
 * @param[in] a_map_page             TRUE: a_block_address/a_page_address points
 *                                   to a map page.
 *                                   FALSE: a_block_address/a_page_address points
 *                                   to a data page.
 * @param[in] a_func_data            User data.
 *
 * @return PIFS_SUCCESS if page marked successfully.
 */
pifs_status_t pifs_check_file_page(pifs_file_t * a_file,
                                   pifs_block_address_t a_block_address,
                                   pifs_page_address_t a_page_address,
                                   pifs_block_address_t a_delta_block_address,
                                   pifs_page_address_t a_delta_page_address,
                                   bool_t a_map_page,
                                   void * a_func_data)
{
    pifs_status_t ret = PIFS_SUCCESS;
    uint8_t * free_page_buf = (uint8_t*) a_func_data;
    bool_t is_file_deleted = FALSE;
    bool_t is_free;
    bool_t is_to_be_released;


    PIFS_DEBUG_MSG("Check page %s\r\n",
                   pifs_ba_pa2str(a_block_address, a_page_address));
    is_file_deleted = pifs_is_entry_deleted(&a_file->entry);
    if (a_map_page)
    {
        /* Page shall not be free or to be released */
        is_free = pifs_is_page_free(a_block_address, a_page_address);
        if (is_free)
        {
            pifs.error_cntr++;
            PIFS_ERROR_MSG("File '%s' map page at %s is marked free!\r\n",
                           a_file->entry.name,
                           pifs_ba_pa2str(a_block_address, a_page_address));
            ret = PIFS_ERROR_INTEGRITY;
        }
        else
        {
            ret = pifs_mark_page_check(free_page_buf, a_block_address, a_page_address, 1);
        }
        is_to_be_released = pifs_is_page_to_be_released(a_block_address, a_page_address);
        if ((is_to_be_released && !is_file_deleted)
                || (!is_to_be_released && is_file_deleted))
        {
            pifs.error_cntr++;
            PIFS_ERROR_MSG("File '%s' map page at %s is %smarked to be released!\r\n",
                           a_file->entry.name,
                           pifs_ba_pa2str(a_block_address, a_page_address),
                           is_file_deleted ? "NOT " : "");
            ret = PIFS_ERROR_INTEGRITY;
        }
    }
    else if (a_block_address != a_delta_block_address || a_page_address != a_delta_page_address)
    {
        /* Delta page is used */
        PIFS_DEBUG_MSG("Check delta page %s\r\n",
                       pifs_ba_pa2str(a_delta_block_address, a_delta_page_address));
        /* When delta page is used, original page shall be marked as to be released, */
        /* but it shall not be free! */
        is_free = pifs_is_page_free(a_block_address, a_page_address);
        if (is_free)
        {
            pifs.error_cntr++;
            PIFS_ERROR_MSG("File '%s' original page at %s is marked free!\r\n",
                           a_file->entry.name,
                           pifs_ba_pa2str(a_block_address, a_page_address));
            ret = PIFS_ERROR_INTEGRITY;
        }
        else
        {
            ret = pifs_mark_page_check(free_page_buf, a_block_address, a_page_address, 1);
        }
        is_to_be_released = pifs_is_page_to_be_released(a_block_address, a_page_address);
        if (!is_to_be_released)
        {
            pifs.error_cntr++;
            PIFS_ERROR_MSG("File '%s' original page at %s is not marked to be released!\r\n",
                           a_file->entry.name,
                           pifs_ba_pa2str(a_block_address, a_page_address));
            ret = PIFS_ERROR_INTEGRITY;
        }
        /* Delta page shall not be free or to be released */
        is_free = pifs_is_page_free(a_delta_block_address, a_delta_page_address);
        if (is_free)
        {
            pifs.error_cntr++;
            PIFS_ERROR_MSG("File '%s' delta page at %s is marked free!\r\n",
                           a_file->entry.name,
                           pifs_ba_pa2str(a_delta_block_address, a_delta_page_address));
            ret = PIFS_ERROR_INTEGRITY;
        }
        else
        {
            ret = pifs_mark_page_check(free_page_buf, a_delta_block_address, a_delta_page_address, 1);
        }
        is_to_be_released = pifs_is_page_to_be_released(a_delta_block_address, a_delta_page_address);
        if ((is_to_be_released && !is_file_deleted)
                || (!is_to_be_released && is_file_deleted))
        {
            pifs.error_cntr++;
            PIFS_ERROR_MSG("File '%s' delta page at %s is %smarked to be released!\r\n",
                           a_file->entry.name,
                           pifs_ba_pa2str(a_delta_block_address, a_delta_page_address),
                           is_file_deleted ? "NOT " : "");
            ret = PIFS_ERROR_INTEGRITY;
        }
    }
    else
    {
        /* Page shall not be free or to be released */
        is_free = pifs_is_page_free(a_block_address, a_page_address);
        if (is_free)
        {
            pifs.error_cntr++;
            PIFS_ERROR_MSG("File '%s' page at %s is marked free!\r\n",
                           a_file->entry.name,
                           pifs_ba_pa2str(a_block_address, a_page_address));
            ret = PIFS_ERROR_INTEGRITY;
        }
        else
        {
            ret = pifs_mark_page_check(free_page_buf, a_block_address, a_page_address, 1);
        }
        is_to_be_released = pifs_is_page_to_be_released(a_block_address, a_page_address);
        if ((is_to_be_released && !is_file_deleted)
                || (!is_to_be_released && is_file_deleted))
        {
            pifs.error_cntr++;
            PIFS_ERROR_MSG("File '%s' page at %s is %smarked to be released!\r\n",
                           a_file->entry.name,
                           pifs_ba_pa2str(a_block_address, a_page_address),
                           is_file_deleted ? "NOT " : "");
            ret = PIFS_ERROR_INTEGRITY;
        }
    }

    return ret;
}

/**
 * @brief pifs_dir_walker_check Callback function used during file system check.
 *
 * @param[in] a_dirent    Pointer to directory entry.
 * @param[in] a_func_data User data.
 *
 * @return PIFS_SUCCESS when file opened successfully.
 */
pifs_status_t pifs_dir_walker_check(pifs_dirent_t * a_dirent, void * a_func_data)
{
    pifs_status_t ret = PIFS_ERROR_NO_MORE_RESOURCE;
    pifs_file_t * file = NULL;

    /* Check if file name is not cleared (only pifs_rename() is doing this!) */
    if (a_dirent->d_name[0] != PIFS_FLASH_PROGRAMMED_BYTE_VALUE)
    {
        PIFS_PRINT_MSG("Checking file '%s'...\r\n", a_dirent->d_name);
        file = pifs_fopen(a_dirent->d_name, "r");
        if (file)
        {
            ret = pifs_walk_file_pages(file, pifs_check_file_page, a_func_data);
            ret = pifs_fclose(file);
        }
        else
        {
            pifs.error_cntr++;
            PIFS_ERROR_MSG("Cannot open file '%s'!\r\n", a_dirent->d_name);
        }
    }
    return ret;
}

/**
 * @brief pifs_check_free_page_buf Used during file system check.
 * Checks allocated space of files and free space bitmap is consistent.
 *
 * @param[in] a_free_page_buf Pointer to free page buffer.
 * @return
 */
pifs_status_t pifs_check_free_page_buf(uint8_t * a_free_page_buf)
{
    pifs_status_t   ret = PIFS_SUCCESS;
    pifs_address_t  address;
    pifs_size_t     page_cntr;
    bool_t          is_free; /* Page is free in free page buffer (RAM) */
    bool_t          is_free_fsbm; /* Page is free in free space bitmap (flash memory) */
    bool_t          is_tbr_fsbm; /* Page is to be released in free space bitmap (flash memory) */

    address.block_address = PIFS_FLASH_BLOCK_RESERVED_NUM;
    address.page_address = 0;

    page_cntr = PIFS_LOGICAL_PAGE_NUM_FS;
    while (page_cntr--)
    {
        is_free = pifs_is_page_free_check(a_free_page_buf, address.block_address,
                                          address.page_address);
        is_free_fsbm = pifs_is_page_free(address.block_address, address.page_address);
        is_tbr_fsbm = pifs_is_page_to_be_released(address.block_address, address.page_address);
        if (is_free && !is_free_fsbm && !is_tbr_fsbm)
        {
            PIFS_DEBUG_MSG("Found lost page %s\r\n",
                           pifs_address2str(&address));
            ret = pifs_mark_page(address.block_address, address.page_address, 1, FALSE, TRUE);
            if (ret == PIFS_SUCCESS)
            {
                PIFS_NOTICE_MSG("Lost page %s marked to be released\r\n",
                           pifs_address2str(&address));
            }
            else
            {
                PIFS_ERROR_MSG("Lost page %s cannot be marked to be released: %i\r\n",
                           pifs_address2str(&address), ret);
            }
#if 0
            /* Read to page cache */
            pifs_read(address.block_address, address.page_address, 0,
                      NULL, 0);
            print_buffer(pifs.cache_page_buf, PIFS_LOGICAL_PAGE_SIZE_BYTE,
                         address.block_address * PIFS_FLASH_BLOCK_SIZE_BYTE
                         + address.page_address * PIFS_LOGICAL_PAGE_SIZE_BYTE);
#endif
            ret = PIFS_ERROR_GENERAL;
        }
        if (page_cntr)
        {
            (void)pifs_inc_address(&address);
        }
    }

    return ret;
}

/**
 * @brief pifs_check Check file system's integrity.
 *
 * @return PIFS_SUCCESS if file system is OK.
 */
pifs_status_t pifs_check(void)
{
    pifs_char_t * path = PIFS_ROOT_STR;
    pifs_status_t ret = PIFS_ERROR_NO_MORE_RESOURCE;
    uint8_t     * free_page_buf;

#if PIFS_FSCHECK_USE_STATIC_MEMORY
    free_page_buf = pifs.free_pages_buf;
#else
    free_page_buf = malloc(PIFS_FREE_PAGE_BUF_SIZE);
#endif

    if (free_page_buf)
    {
        ret = PIFS_SUCCESS;
        memset(free_page_buf, PIFS_FLASH_ERASED_BYTE_VALUE, PIFS_FREE_PAGE_BUF_SIZE);
        PIFS_PRINT_MSG("Checking files in directory '%s'...\r\n", path);
        pifs.error_cntr = 0;
        ret = pifs_walk_dir(path, TRUE, FALSE, pifs_dir_walker_check, free_page_buf);
        if (pifs.error_cntr)
        {
            PIFS_ERROR_MSG("%i file errors found!\r\n", pifs.error_cntr);
        }
        else
        {
            PIFS_PRINT_MSG("No file errors found.\r\n");
        }
        if (ret == PIFS_SUCCESS)
        {
            /* Mark file system header as used */
            ret = pifs_mark_page_check(free_page_buf,
                                       pifs.header.management_block_address,
                                       0,
                                       PIFS_HEADER_SIZE_PAGE);
        }
        if (ret == PIFS_SUCCESS)
        {
            PIFS_DEBUG_MSG("Marking entry list %s, %i pages\r\n",
                           pifs_address2str(&a_header->root_entry_list_address),
                           PIFS_ENTRY_LIST_SIZE_PAGE);
            /* Mark entry list as used */
            ret = pifs_mark_page_check(free_page_buf,
                                       pifs.header.root_entry_list_address.block_address,
                                       pifs.header.root_entry_list_address.page_address,
                                       PIFS_ENTRY_LIST_SIZE_PAGE);
        }
        if (ret == PIFS_SUCCESS)
        {
            /* Mark free space bitmap as used */
            ret = pifs_mark_page_check(free_page_buf,
                                       pifs.header.free_space_bitmap_address.block_address,
                                       pifs.header.free_space_bitmap_address.page_address,
                                       PIFS_FREE_SPACE_BITMAP_SIZE_PAGE);
        }
        if (ret == PIFS_SUCCESS)
        {
            /* Mark delta page map as used */
            ret = pifs_mark_page_check(free_page_buf,
                                       pifs.header.delta_map_address.block_address,
                                       pifs.header.delta_map_address.page_address,
                                       PIFS_DELTA_MAP_PAGE_NUM);
        }
        if (ret == PIFS_SUCCESS)
        {
            /* Mark wear level list as used */
            ret = pifs_mark_page_check(free_page_buf,
                                       pifs.header.wear_level_list_address.block_address,
                                       pifs.header.wear_level_list_address.page_address,
                                       PIFS_WEAR_LEVEL_LIST_SIZE_PAGE);
        }
        PIFS_PRINT_MSG("Checking free space...\r\n");
        /* TODO check free space */
        PIFS_PRINT_MSG("Free page buffer:\r\n");
        print_buffer(free_page_buf, PIFS_FREE_PAGE_BUF_SIZE, 0);
        ret = pifs_check_free_page_buf(free_page_buf);
#if !PIFS_FSCHECK_USE_STATIC_MEMORY
        free(free_page_buf);
#endif
    }

    return ret;
}
