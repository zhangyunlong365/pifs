/**
 * @file        flash_config.h
 * @brief       Flash configuration
 * @author      Copyright (C) Peter Ivanov, 2017
 *
 * Created:     2017-06-11 09:10:19
 * Last modify: 2017-06-15 14:06:42 ivanovp {Time-stamp}
 * Licence:     GPL
 */
#ifndef _INCLUDE_FLASH_CONFIG_H_
#define _INCLUDE_FLASH_CONFIG_H_

#define FLASH_TYPE_M25P40   0
#define FLASH_TYPE_M25P80   1
#define FLASH_TYPE_N25Q128A 2

#define FLASH_TYPE          FLASH_TYPE_M25P80

#if FLASH_TYPE == FLASH_TYPE_M25P40
/* Geometry of M25P40 */
#define PIFS_FLASH_BLOCK_NUM_ALL            8       /**< Number of blocks in flash memory */
#define PIFS_FLASH_BLOCK_RESERVED_NUM       0       /**< Index of first block to use by the file system */
#define PIFS_FLASH_PAGE_PER_BLOCK           256     /**< Number of pages in a block */
#define PIFS_FLASH_PAGE_SIZE_BYTE           256     /**< Size of a page in bytes */
#define PIFS_FLASH_PAGE_SIZE_SPARE          0       /**< Number of spare bytes in a page */

/* Configuration of filesystem */
#define PIFS_MANAGEMENT_PAGE_PER_BLOCK_MAX  8       /**< Maximum number of management pages per block */
#elif FLASH_TYPE == FLASH_TYPE_M25P80
/* Geometry of M25P40 */
#define PIFS_FLASH_BLOCK_NUM_ALL            16      /**< Number of blocks in flash memory */
#define PIFS_FLASH_BLOCK_RESERVED_NUM       0       /**< Index of first block to use by the file system */
#define PIFS_FLASH_PAGE_PER_BLOCK           256     /**< Number of pages in a block */
#define PIFS_FLASH_PAGE_SIZE_BYTE           256     /**< Size of a page in bytes */
#define PIFS_FLASH_PAGE_SIZE_SPARE          0       /**< Number of spare bytes in a page */

/* Configuration of filesystem */
#define PIFS_MANAGEMENT_PAGE_PER_BLOCK_MAX  8       /**< Maximum number of management pages per block */
#elif FLASH_TYPE == FLASH_TYPE_N25Q128A
/* Geometry of N25Q128A */
#define PIFS_FLASH_BLOCK_NUM_ALL            4096    /**< Number of blocks in flash memory */
#define PIFS_FLASH_BLOCK_RESERVED_NUM       4       /**< Index of first block to use by the file system */
#define PIFS_FLASH_PAGE_PER_BLOCK           16      /**< Number of pages in a block */
#define PIFS_FLASH_PAGE_SIZE_BYTE           256     /**< Size of a page in bytes */
#define PIFS_FLASH_PAGE_SIZE_SPARE          0       /**< Number of spare bytes in a page */

/* Configuration of filesystem */
#define PIFS_MANAGEMENT_PAGE_PER_BLOCK_MAX  4       /**< Maximum number of management pages per block */
#endif

#define PIFS_FLASH_ERASED_VALUE       0xFFu
#define PIFS_FLASH_PROGRAMMED_VALUE   0x00u

#if PIFS_MANAGEMENT_PAGE_PER_BLOCK_MAX >= PIFS_FLASH_PAGE_PER_BLOCK / 2
#error PIFS_MANAGEMENT_PAGE_PER_BLOCK_MAX is too big.
#endif

#endif /* _INCLUDE_FLASH_CONFIG_H_ */