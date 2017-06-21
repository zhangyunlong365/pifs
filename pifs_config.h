/**
 * @file        pifs_config.h
 * @brief       Pi file system's configuration
 * @author      Copyright (C) Peter Ivanov, 2017
 *
 * Created:     2017-06-11 09:10:19
 * Last modify: 2017-06-16 12:26:51 ivanovp {Time-stamp}
 * Licence:     GPL
 */
#ifndef _INCLUDE_PIFS_CONFIG_H_
#define _INCLUDE_PIFS_CONFIG_H_

#define PIFS_OPEN_FILE_NUM_MAX      4       /**< Maximum number of opened file */
#define PIFS_FILENAME_LEN_MAX       32      /**< Maximum length of file name */
#define PIFS_ENTRY_NUM_MAX          254     /**< Maximum number of files and directories */
#define PIFS_CHECKSUM_SIZE          4       /**< Number of checksum bytes. Valid values are 1, 2 and 4. */
#define PIFS_PAGE_COUNT_SIZE        2       /**< Size of page count in map entry. Valid values are 1, 2 and 4. */
#define PIFS_MAP_PAGE_NUM           1       /**< Default number of map pages. Minimum: 1 */
#define PIFS_OPTIMIZE_FOR_RAM       1

#define PIFS_PACKED_ATTRIBUTE       __attribute__((packed))

#endif /* _INCLUDE_PIFS_CONFIG_H_ */
