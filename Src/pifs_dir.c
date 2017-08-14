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
#include "pifs_fsbm.h"
#include "pifs_helper.h"
#include "pifs_delta.h"
#include "pifs_entry.h"
#include "pifs_map.h"
#include "pifs_merge.h"
#include "pifs_dir.h"
#include "buffer.h" /* DEBUG */

#define PIFS_DEBUG_LEVEL 2
#include "pifs_debug.h"

#if PIFS_ENABLE_DIRECTORIES
/**
 * @brief pifs_delete_chars Delete character in a string.
 *
 * @param[out] a_string String to be cutted.
 * @param[in] a_idx     Start index of to be deleted characters.
 * @param[in] a_count   Number of characters to delete.
 */
void pifs_delete_chars(pifs_char_t * a_string, pifs_size_t a_idx, pifs_size_t a_count)
{
    pifs_size_t i;
    pifs_size_t len;

    PIFS_DEBUG_MSG("before %s, idx: %i, count: %i\r\n", a_string, a_idx, a_count);
    len = strlen(a_string) - a_count + 1;
    for (i = a_idx; i < len; i++)
    {
        a_string[i] = a_string[i + a_count];
    }
    PIFS_DEBUG_MSG("after %s\r\n", a_string);
}

/**
 * @brief pifs_normalize_path Change "/aaa/bbb/ccc/../.." to "/aaa".
 * Used when directory changed.
 *
 * @param[ou] a_path Path to be normalized.
 */
void pifs_normalize_path(pifs_char_t * const a_path)
{
    typedef enum
    {
        NORM_start = 0,
        NORM_separator,
        NORM_dot,
        NORM_dot2,
        NORM_other,
    } norm_state_t;
    norm_state_t norm_state;
    size_t       i;
    size_t       separator_pos[2];
    size_t       path_len;
    bool_t       is_deleted;
    bool_t       end;

    do
    {
        PIFS_DEBUG_MSG("start %s\r\n", a_path);
        separator_pos[1] = 0; /* Befor last separator's position */
        separator_pos[0] = 0; /* Last separator's position */
        is_deleted = FALSE;
        end = FALSE;
        path_len = strlen(a_path);
        norm_state = NORM_start;
        for (i = 0; i < path_len && !end; i++)
        {
            if (a_path[i] != PIFS_DOT_CHAR && a_path[i] != PIFS_PATH_SEPARATOR_CHAR)
            {
                norm_state = NORM_other;
            }
            if (a_path[i] == PIFS_DOT_CHAR)
            {
                PIFS_DEBUG_MSG(".state: %i\r\n", norm_state);
                if (norm_state == NORM_separator)
                {
                    norm_state = NORM_dot;
                }
                else if (norm_state == NORM_dot)
                {
                    norm_state = NORM_dot2;
                }
            }
            if (a_path[i] == PIFS_PATH_SEPARATOR_CHAR
                     || i == (path_len - 1))
            {
                PIFS_DEBUG_MSG("/state: %i\r\n", norm_state);
                PIFS_DEBUG_MSG("separator_pos[1]: %i\r\n", separator_pos[1]);
                PIFS_DEBUG_MSG("separator_pos[0]: %i\r\n", separator_pos[0]);
                if (i == (path_len - 1))
                {
                    i++;
                }
                if (norm_state == NORM_dot)
                {
                    pifs_delete_chars(a_path, i - 2, 2);
                    is_deleted = TRUE;
                    end = TRUE;
                }
                else if (norm_state == NORM_dot2)
                {
                    pifs_delete_chars(a_path, separator_pos[1], i - separator_pos[1]);
                    is_deleted = TRUE;
                    end = TRUE;
                }
                norm_state = NORM_separator;
                separator_pos[1] = separator_pos[0];
                separator_pos[0] = i;
            }
        }
        PIFS_DEBUG_MSG("end %s\r\n", a_path);
    } while (is_deleted);
}

/**
 * @brief pifs_resolve_path Walk through directories and find last directory's
 * entry.
 * Example: a_path = "/aaa/bbb/ccc/ddd/name.txt". After running a_filename
 * will be "name.txt", a_resolved_entry_list_address will be address of "ddd".
 *
 * @param[in] a_path                         Path to be walked.
 * @param[in] a_current_entry_list_address   Current entry list's address.
 * @param[out] a_filename                    Last element of path, which can be file name
 *                                           or directory as well.
 * @param[out] a_resolved_entry_list_address Last directory's entry list.
 * @return PIFS_SUCCESS if entry list's address is resolved.
 */
pifs_status_t pifs_resolve_path(const pifs_char_t * a_path,
                                pifs_address_t a_current_entry_list_address,
                                pifs_char_t * const a_filename,
                                pifs_address_t * const a_resolved_entry_list_address)
{
    pifs_status_t       ret = PIFS_SUCCESS;
    const pifs_char_t * curr_path_pos = a_path;
    pifs_char_t       * curr_separator_pos = NULL;
    pifs_address_t      entry_list_address = a_current_entry_list_address;
    pifs_char_t         name[PIFS_FILENAME_LEN_MAX]; /* TODO try to avoid stack use */
    pifs_entry_t      * entry = &pifs.entry;
    pifs_size_t         len;

    PIFS_DEBUG_MSG("path: [%s]\r\n", a_path);
    /* Check if it is an absolute path */
    if (a_path[0] == PIFS_PATH_SEPARATOR_CHAR)
    {
        /* Absolute path, start from root entry list */
        curr_path_pos++;
        entry_list_address = pifs.header.root_entry_list_address;
    }
    while ((curr_separator_pos = strchr(curr_path_pos, PIFS_PATH_SEPARATOR_CHAR))
           && ret == PIFS_SUCCESS)
    {
        len = curr_separator_pos - curr_path_pos;
        memcpy(name, curr_path_pos, len);
        name[len] = PIFS_EOS;
        PIFS_DEBUG_MSG("name: [%s]\r\n", name);
        ret = pifs_find_entry(PIFS_FIND_ENTRY, name, entry,
                              entry_list_address.block_address,
                              entry_list_address.page_address);
        if (ret == PIFS_SUCCESS)
        {
            if (PIFS_IS_DIR(entry->attrib))
            {
                entry_list_address = entry->first_map_address;
            }
            else
            {
                PIFS_ERROR_MSG("'%s' is not directory!\r\n", entry->name);
                ret = PIFS_ERROR_IS_NOT_DIRECTORY;
            }
        }
        curr_path_pos = curr_separator_pos + 1;
    }
    if (curr_path_pos == a_path)
    {
        strncpy(a_filename, a_path, PIFS_FILENAME_LEN_MAX);
    }
    else
    {
        strncpy(a_filename, curr_path_pos, PIFS_FILENAME_LEN_MAX);
    }
    if (ret == PIFS_SUCCESS)
    {
        *a_resolved_entry_list_address = entry_list_address;
    }
    PIFS_INFO_MSG("a_filename: [%s] entry list address: %s\r\n",
                   a_filename, pifs_address2str(a_resolved_entry_list_address));

    return ret;
}

bool_t pifs_is_directory_empty(const pifs_char_t * a_path)
{
    bool_t          empty = TRUE;
    pifs_DIR      * dir;
    pifs_dirent_t * dirent;

    dir = pifs_opendir(a_path);
    if (dir != NULL)
    {
        while ((dirent = pifs_readdir(dir)))
        {
            if (!PIFS_IS_DOT_DIR(dirent->d_name))
            {
                empty = FALSE;
            }
        }
        if (pifs_closedir (dir) != 0)
        {
            PIFS_ERROR_MSG("Cannot close directory!\r\n");
        }
    }

    PIFS_DEBUG_MSG("%s empty: %s\r\n", a_path, pifs_yes_no(empty));
    return empty;
}
#endif

/**
 * @brief pifs_opendir Open directory for listing.
 *
 * @param[in] a_name Pointer to directory name. TODO currently path is omitted!
 * @return Pointer to file system's directory.
 */
pifs_DIR * pifs_opendir(const pifs_char_t * a_name)
{
    pifs_status_t   ret = PIFS_SUCCESS;
    pifs_size_t     i;
    pifs_dir_t    * dir = NULL;
#if PIFS_ENABLE_DIRECTORIES
    pifs_address_t  entry_list_address = pifs.current_entry_list_address;
    pifs_char_t     filename[PIFS_FILENAME_LEN_MAX];
    pifs_entry_t  * entry = &pifs.entry;
#endif

#if PIFS_ENABLE_DIRECTORIES
    /* Resolve a_filename's relative/absolute file path and update
     * entry_list_address regarding that */
    if (a_name[0] == PIFS_PATH_SEPARATOR_CHAR
            && a_name[1] == PIFS_EOS)
    {
        /* Root directory: "/" or "\" */
        entry_list_address = pifs.header.root_entry_list_address;
    }
    else if (a_name[0] == PIFS_DOT_CHAR
            && a_name[1] == PIFS_EOS)
    {
        /* Current directory: "." */
        entry_list_address = pifs.current_entry_list_address;
    }
    else
    {
        ret = pifs_resolve_path(a_name, pifs.current_entry_list_address,
                                filename, &entry_list_address);
        if (ret == PIFS_SUCCESS)
        {
            ret = pifs_find_entry(PIFS_FIND_ENTRY, filename, entry,
                                  entry_list_address.block_address,
                                  entry_list_address.page_address);
        }
        if (ret == PIFS_SUCCESS)
        {
            entry_list_address = entry->first_map_address;
        }
    }
#else
    (void) a_name;
#endif

    if (ret == PIFS_SUCCESS)
    {
        for (i = 0; i < PIFS_OPEN_DIR_NUM_MAX; i++)
        {
            dir = &pifs.dir[i];
            if (!dir->is_used)
            {
                dir->is_used = TRUE;
                dir->find_deleted = !a_name;
                dir->entry_page_index = 0;
#if PIFS_ENABLE_DIRECTORIES
                dir->entry_list_address = entry_list_address;
#else
                dir->entry_list_address = pifs.header.root_entry_list_address;
#endif
                PIFS_WARNING_MSG("Opening directory at %s\r\n",
                                 pifs_address2str(&dir->entry_list_address));
                dir->entry_list_index = 0;
            }
        }
        if (dir == NULL)
        {
            PIFS_SET_ERRNO(PIFS_ERROR_NO_MORE_RESOURCE);
        }
    }
    else
    {
        PIFS_SET_ERRNO(ret);
    }

    return (pifs_DIR*) dir;
}

/**
 * @brief pifs_inc_entry Increase directory entry's pointer.
 *
 * @param[in] a_dir Pointer to directory structure.
 * @return
 */
static pifs_status_t pifs_inc_entry(pifs_dir_t * a_dir)
{
    pifs_status_t ret = PIFS_SUCCESS;

    a_dir->entry_list_index++;
    if (a_dir->entry_list_index >= PIFS_ENTRY_PER_PAGE)
    {
        a_dir->entry_list_index = 0;
        (void)pifs_inc_address(&a_dir->entry_list_address);
        a_dir->entry_page_index++;
        if (a_dir->entry_page_index >= PIFS_ENTRY_LIST_SIZE_PAGE)
        {
            ret = PIFS_ERROR_NO_MORE_ENTRY;
        }
    }

    return ret;
}

/**
 * @brief pifs_readdir Read one directory entry from opened directory.
 *
 * @param[in] a_dirp Pointer to the opened directory.
 * @return Entry if found or NULL.
 */
struct pifs_dirent * pifs_readdir(pifs_DIR * a_dirp)
{
    pifs_status_t   ret = PIFS_SUCCESS;
    pifs_dir_t    * dir = (pifs_dir_t*) a_dirp;
    pifs_entry_t  * entry = &dir->entry;
    pifs_dirent_t * dirent = NULL;
    bool_t          entry_found = FALSE;

#if PIFS_USE_DELTA_FOR_ENTRIES
    ret = pifs_read_delta(dir->entry_list_address.block_address,
                          dir->entry_list_address.page_address,
                          dir->entry_list_index * PIFS_ENTRY_SIZE_BYTE, &entry,
                          PIFS_ENTRY_SIZE_BYTE);
#else
    do
    {
        ret = pifs_read(dir->entry_list_address.block_address,
                        dir->entry_list_address.page_address,
                        dir->entry_list_index * PIFS_ENTRY_SIZE_BYTE, entry,
                        PIFS_ENTRY_SIZE_BYTE);
        if (ret == PIFS_SUCCESS)
        {
            if (dir->find_deleted || !pifs_is_entry_deleted(entry))
            {
                entry_found = TRUE;
            }
            else
            {
                ret = pifs_inc_entry(dir);
            }
        }
    } while (ret == PIFS_SUCCESS && !entry_found);
#endif
    if (ret == PIFS_SUCCESS
            && !pifs_is_buffer_erased(entry, PIFS_ENTRY_SIZE_BYTE)
            && entry_found)
    {
        /* Copy entry */
        dir->directory_entry.d_ino = entry->first_map_address.block_address * PIFS_FLASH_BLOCK_SIZE_BYTE
                + entry->first_map_address.page_address * PIFS_LOGICAL_PAGE_SIZE_BYTE;
        strncpy(dir->directory_entry.d_name, entry->name, sizeof(dir->directory_entry.d_name));
        dir->directory_entry.d_filesize = entry->file_size;
#if PIFS_ENABLE_ATTRIBUTES
        dir->directory_entry.d_attrib = entry->attrib;
#endif
        dir->directory_entry.d_first_map_block_address = entry->first_map_address.block_address;
        dir->directory_entry.d_first_map_page_address = entry->first_map_address.page_address;
#if PIFS_ENABLE_USER_DATA
        memcpy(&dir->directory_entry.d_user_data, &entry->user_data, sizeof(dir->directory_entry.d_user_data));
#endif
        dirent = &dir->directory_entry;
    }
    if (ret == PIFS_SUCCESS)
    {
        pifs_inc_entry(dir);
    }
    PIFS_SET_ERRNO(ret);

    return dirent;
}

/**
 * @brief pifs_closedir Close opened directory.
 *
 * @param[in] a_dirp Pointer to directory to close.
 * @return 0 if successfully closed, -1 if directory was not opened.
 */
int pifs_closedir(pifs_DIR * const a_dirp)
{
    int           ret = -1;
    pifs_dir_t  * dir = (pifs_dir_t*) a_dirp;

    if (dir->is_used)
    {
        dir->is_used = FALSE;
        ret = 0;
    }

    return ret;
}

/**
 * @brief pifs_walk_dir Walk through directory.
 * TODO implement a non-recursive method: it should not use
 * pifs_opendir() and store positions
 * size_t current_entry_pos[PIFS_MAX_DIR_DEPTH];
 *
 * @param[in] a_path            Path to walk.
 * @param[in] a_recursive       Enter directories and walk them too.
 * @param[in] a_stop_at_error   TRUE: Stop at first error.
 * @param[in] a_dir_walker_func Pointer to callback function.
 * @return PIFS_SUCCESS if successfully walked.
 */
pifs_status_t pifs_walk_dir(const pifs_char_t * const a_path, bool_t a_recursive, bool_t a_stop_at_error,
                            pifs_dir_walker_func_t a_dir_walker_func, void * a_func_data)
{
    pifs_status_t   ret = PIFS_ERROR_FILE_NOT_FOUND;
    pifs_status_t   ret2;
    pifs_status_t   ret_error = PIFS_SUCCESS;
    pifs_DIR      * dir;
    pifs_dirent_t * dirent;
#if PIFS_ENABLE_DIRECTORIES
    pifs_char_t     path[PIFS_PATH_LEN_MAX];
#endif

#if !PIFS_ENABLE_DIRECTORIES
    (void)a_recursive;
#endif

    dir = pifs_opendir(a_path);
    if (dir != NULL)
    {
        ret = PIFS_SUCCESS;
        while ((dirent = pifs_readdir(dir)) && ret == PIFS_SUCCESS)
        {
            ret2 = (*a_dir_walker_func)(dirent, a_func_data);
            if (a_stop_at_error)
            {
                ret = ret2;
            }
            else if (ret2 != PIFS_SUCCESS)
            {
                ret_error = ret2;
            }
#if PIFS_ENABLE_DIRECTORIES
            if (ret == PIFS_SUCCESS)
            {
                if (a_recursive && PIFS_IS_DIR(dirent->d_attrib))
                {
                    strncpy(path, a_path, PIFS_PATH_LEN_MAX);
                    strncat(path, PIFS_PATH_SEPARATOR_STR, PIFS_PATH_LEN_MAX);
                    strncat(path, dirent->d_name, PIFS_PATH_LEN_MAX);
                    ret = pifs_walk_dir(path, TRUE, a_stop_at_error, a_dir_walker_func, a_func_data);
                }
            }
#endif
        }
        if (pifs_closedir (dir) != 0)
        {
            PIFS_ERROR_MSG("Cannot close directory!\r\n");
            ret = PIFS_ERROR_GENERAL;
        }
    }

    if (!a_stop_at_error && ret == PIFS_SUCCESS)
    {
        ret = ret_error;
    }

    return ret;
}

#if PIFS_ENABLE_DIRECTORIES
/**
 * @brief pifs_mkdir Create directory.
 *
 * @param[in] a_filename Path to create.
 * @return PIFS_SUCCESS if directory successfully created.
 */
int pifs_mkdir(const pifs_char_t * const a_filename)
{
    pifs_status_t        ret = PIFS_SUCCESS;
    pifs_entry_t       * entry = &pifs.entry;
    pifs_block_address_t ba;
    pifs_page_address_t  pa;
    pifs_page_count_t    page_count_found;
    pifs_address_t       entry_list_address = pifs.current_entry_list_address;
    pifs_char_t          filename[PIFS_FILENAME_LEN_MAX];

    /* Resolve a_filename's relative/absolute file path and update
     * entry_list_address regarding that */
    ret = pifs_resolve_path(a_filename, pifs.current_entry_list_address,
                            filename, &entry_list_address);

    if (ret == PIFS_SUCCESS)
    {
        ret = pifs_find_entry(PIFS_FIND_ENTRY, filename, entry,
                              entry_list_address.block_address,
                              entry_list_address.page_address);
    }
    if (ret == PIFS_SUCCESS)
    {
        ret = PIFS_ERROR_FILE_ALREADY_EXIST;
    }
    else if (ret == PIFS_ERROR_FILE_NOT_FOUND)
    {
        /* Order of steps to create a directory: */
        /* #1 Find free pages for entry list */
        /* #2 Create entry of a_file, which contains the entry list's address */
        /* #3 Mark entry list page */
        /* #4 Create "." and ".." entries */
        ret = pifs_find_free_page_wl(PIFS_ENTRY_LIST_SIZE_PAGE, PIFS_ENTRY_LIST_SIZE_PAGE,
                                     PIFS_BLOCK_TYPE_PRIMARY_MANAGEMENT,
                                     &ba, &pa, &page_count_found);
        if (ret == PIFS_SUCCESS)
        {
            PIFS_DEBUG_MSG("Entry list: %u free page found %s\r\n", page_count_found, pifs_ba_pa2str(ba, pa));
            memset(entry, PIFS_FLASH_ERASED_BYTE_VALUE, PIFS_ENTRY_SIZE_BYTE);
            strncpy((char*)entry->name, filename, PIFS_FILENAME_LEN_MAX);
            PIFS_SET_ATTRIB(entry->attrib, PIFS_ATTRIB_ARCHIVE | PIFS_ATTRIB_DIR);
            entry->first_map_address.block_address = ba;
            entry->first_map_address.page_address = pa;
            ret = pifs_append_entry(entry,
                                    entry_list_address.block_address,
                                    entry_list_address.page_address);
            if (ret == PIFS_SUCCESS)
            {
                PIFS_DEBUG_MSG("Entry created\r\n");
                ret = pifs_mark_page(ba, pa, PIFS_ENTRY_LIST_SIZE_PAGE, TRUE);
                if (ret == PIFS_SUCCESS)
                {
                    /* Add current directory's entry "." */
                    memset(entry, PIFS_FLASH_ERASED_BYTE_VALUE, PIFS_ENTRY_SIZE_BYTE);
                    strncpy((char*)entry->name, PIFS_DOT_STR, PIFS_FILENAME_LEN_MAX);
                    PIFS_SET_ATTRIB(entry->attrib, PIFS_ATTRIB_ARCHIVE | PIFS_ATTRIB_DIR);
                    entry->first_map_address.block_address = ba;
                    entry->first_map_address.page_address = pa;
                    ret = pifs_append_entry(entry, ba, pa);
                }
                if (ret == PIFS_SUCCESS)
                {
                    /* Add upper directory's entry ".." */
                    strncpy((char*)entry->name, PIFS_DOUBLE_DOT_STR, PIFS_FILENAME_LEN_MAX);
                    PIFS_SET_ATTRIB(entry->attrib, PIFS_ATTRIB_ARCHIVE | PIFS_ATTRIB_DIR);
                    entry->first_map_address.block_address = entry_list_address.block_address;
                    entry->first_map_address.page_address = entry_list_address.page_address;
                    ret = pifs_append_entry(entry, ba, pa);
                }
            }
            else
            {
                PIFS_DEBUG_MSG("Cannot create entry!\r\n");
                PIFS_SET_ERRNO(PIFS_ERROR_NO_MORE_ENTRY);
            }
        }
    }

    return ret;
}

/**
 * @brief pifs_rmdir Remove directory.
 *
 * @param[in] a_filename Path and directory to remove.
 * @return PIFS_SUCCESS if directory removed.
 */
int pifs_rmdir(const pifs_char_t * const a_filename)
{
    pifs_status_t        ret = PIFS_SUCCESS;
    pifs_entry_t       * entry = &pifs.entry;
    pifs_address_t       entry_list_address = pifs.current_entry_list_address;
    pifs_char_t          filename[PIFS_FILENAME_LEN_MAX];

    if (pifs_is_directory_empty(a_filename))
    {
        /* Resolve a_filename's relative/absolute file path and update */
        /* entry_list_address regarding that */
        ret = pifs_resolve_path(a_filename, pifs.current_entry_list_address,
                                filename, &entry_list_address);

        if (ret == PIFS_SUCCESS)
        {
            ret = pifs_find_entry(PIFS_DELETE_ENTRY, filename, entry,
                                  entry_list_address.block_address,
                                  entry_list_address.page_address);
        }
    }
    else
    {
        ret = PIFS_ERROR_DIRECTORY_NOT_EMPTY;
    }

    return ret;
}

int pifs_chdir(const pifs_char_t * const a_filename)
{
    pifs_status_t     ret = PIFS_SUCCESS;
    pifs_entry_t    * entry = &pifs.entry;
    pifs_address_t    entry_list_address = pifs.current_entry_list_address;
    pifs_char_t       filename[PIFS_FILENAME_LEN_MAX];
    pifs_char_t       separator[2] = { PIFS_PATH_SEPARATOR_CHAR, 0 };

    if (a_filename[0] == PIFS_PATH_SEPARATOR_CHAR
            && a_filename[1] == PIFS_EOS)
    {
        /* Root directory: "/" or "\" */
        pifs.current_entry_list_address = pifs.header.root_entry_list_address;
        pifs.cwd[0] = PIFS_PATH_SEPARATOR_CHAR;
        pifs.cwd[1] = PIFS_EOS;
    }
    else
    {
        /* Resolve a_filename's relative/absolute file path and update */
        /* entry_list_address regarding that */
        ret = pifs_resolve_path(a_filename, pifs.current_entry_list_address,
                                filename, &entry_list_address);

        if (ret == PIFS_SUCCESS)
        {
            ret = pifs_find_entry(PIFS_FIND_ENTRY, filename, entry,
                                  entry_list_address.block_address,
                                  entry_list_address.page_address);
        }
        if (ret == PIFS_SUCCESS)
        {
            if (PIFS_IS_DIR(entry->attrib))
            {
                /* Update current working directory (pifs.cwd) */
                pifs.current_entry_list_address = entry->first_map_address;
                if (pifs.cwd[strlen(pifs.cwd) - 1] != PIFS_PATH_SEPARATOR_CHAR)
                {
                    strncat(pifs.cwd, separator, PIFS_PATH_LEN_MAX);
                }
                strncat(pifs.cwd, a_filename, PIFS_PATH_LEN_MAX);
                pifs_normalize_path((pifs_char_t*)&pifs.cwd);
                if (pifs.cwd[0] == PIFS_EOS)
                {
                    pifs.cwd[0] = PIFS_PATH_SEPARATOR_CHAR;
                    pifs.cwd[1] = PIFS_EOS;
                }
            }
            else
            {
                ret = PIFS_ERROR_IS_NOT_DIRECTORY;
            }
        }
    }

    return ret;
}

/**
 * @brief pifs_getcwd Get current working directory.
 *
 * @param[out] a_buffer Path to fill.
 * @param[in] a_size    Size of a_buffer.
 * @return Pointer to a_buffer.
 */
pifs_char_t * pifs_getcwd(pifs_char_t * a_buffer, size_t a_size)
{
    strncpy(a_buffer, pifs.cwd, a_size);

    return a_buffer;
}
#endif
