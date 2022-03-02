#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <dirent.h>
#include <switch.h>

/* Preprocessor definitions. */

#define CACHE_STORAGE_MOUNT_NAME "save"
#define DUMP_PATH                "sdmc:/cache_dumps/"
#define DUMP_BLOCKSIZE           0x800000

/* Type definitions. */

typedef struct {
    u8 save_data_space_id;
    u16 save_data_index;
} CacheStorageInfo;

/* Global variables. */

static const u64 g_appTitleID = 0x0100C090153B4000ULL; // Crunchyroll Title ID -- may be changed before building

static FsSaveDataFilter g_saveDataFilter = {
    .filter_by_application_id = true,
    .filter_by_save_data_type = true,
    .filter_by_user_id = false,
    .filter_by_system_save_data_id = false,
    .filter_by_index = false,
    .save_data_rank = FsSaveDataRank_Primary,
    .padding = { 0, 0 },
    .attr = {
        .application_id = g_appTitleID,
        .uid = {{ 0, 0 }},
        .system_save_data_id = 0,
        .save_data_type = FsSaveDataType_Cache,
        .save_data_rank = FsSaveDataRank_Primary,
        .save_data_index = 0,
        .pad_x24 = 0,
        .unk_x28 = 0,
        .unk_x30 = 0,
        .unk_x38 = 0
    }
};

static PadState g_padState = {0};

static u8 *g_fileBuf = NULL;

/* Function prototypes. */

__attribute__((format(printf, 1, 2))) static void consolePrint(const char *text, ...);

static bool utilsGetApplicationCacheStorageInfo(CacheStorageInfo **out_cache_info, u64 *out_count);
static bool utilsMountApplicationCacheStorage(const CacheStorageInfo *cache_info);
NX_INLINE void utilsUnmountApplicationCacheStorage(void);

static void utilsDumpDirectory(const char *path, u16 idx);
static void utilsDumpFile(const char *path, u16 idx);

void utilsCreateDirectoryTree(const char *path, bool create_last_element);

static void utilsScanPads(void);
static u64 utilsGetButtonsDown(void);
static void utilsWaitForButtonPress(u64 flag);

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    
    int ret = 0;
    
    CacheStorageInfo *cache_info = NULL;
    u64 cache_info_count = 0;
    
    /* Configure input. */
    /* Up to 8 different, full controller inputs. */
    /* Individual Joy-Cons not supported. */
    padConfigureInput(8, HidNpadStyleSet_NpadFullCtrl);
    padInitializeWithMask(&g_padState, 0x1000000FFUL);
    
    /* Initialize console output. */
    consoleInit(NULL);
    
    /* Print message. */
    consolePrint(APP_TITLE ". Built on " BUILD_TIMESTAMP ".\n\n");
    
    /* Check if we're running under HOS 6.0.0+. */
    if (!hosversionAtLeast(6, 0, 0))
    {
        consolePrint("Horizon OS 6.0.0 or later required! Exiting...");
        ret = -1;
        goto end;
    }
    
    /* Allocate memory for our file dump buffer. */
    g_fileBuf = malloc(DUMP_BLOCKSIZE);
    if (!g_fileBuf)
    {
        consolePrint("Failed to allocate memory for file dump buffer!");
        ret = -2;
        goto end;
    }
    
    /* Get application cache storage info. */
    if (!utilsGetApplicationCacheStorageInfo(&cache_info, &cache_info_count))
    {
        ret = -3;
        goto end;
    }
    
    /* Mount available cache storages. */
    for(u64 i = 0; i < cache_info_count; i++)
    {
        CacheStorageInfo *cur_cache_info = &(cache_info[i]);
        
        /* Mount current cache storage. */
        consolePrint("Mounting cache storage #%04u... ", cur_cache_info->save_data_index);
        if (!utilsMountApplicationCacheStorage(cur_cache_info)) continue;
        consolePrint("OK!\n");
        
        /* Directory listing. */
        consolePrint("Directory listing for cache storage #%04u:\n", cur_cache_info->save_data_index);
        utilsDumpDirectory(CACHE_STORAGE_MOUNT_NAME ":/", cur_cache_info->save_data_index);
        consolePrint("\n");
        
        /* Unmount current cache storage. */
        utilsUnmountApplicationCacheStorage();
    }
    
    consolePrint("Process finished. Press any button to exit.");
    utilsWaitForButtonPress(0);
    
end:
    /* Free cache storage info (if needed). */
    if (cache_info) free(cache_info);
    
    /* Free file dump buffer (if needed). */
    if (g_fileBuf) free(g_fileBuf);
    
    /* Wait some time (3 seconds). */
    svcSleepThread(3000000000ULL);
    
    /* Deinitialize console output. */
    consoleExit(NULL);
    
    return ret;
}

__attribute__((format(printf, 1, 2))) static void consolePrint(const char *text, ...)
{
    va_list v;
    va_start(v, text);
    vfprintf(stdout, text, v);
    va_end(v);
    consoleUpdate(NULL);
}

static bool utilsGetApplicationCacheStorageInfo(CacheStorageInfo **out_cache_info, u64 *out_count)
{
    if (!out_cache_info || !out_count)
    {
        consolePrint("\nInvalid parameters!");
        return false;
    }
    
    Result rc = 0;
    
    FsSaveDataInfoReader sv_reader = {0};
    FsSaveDataInfo sv_info = {0};
    u64 total_entries = 0;
    
    CacheStorageInfo *cache_info = NULL, *tmp_cache_info = NULL;
    u64 count = 0;
    
    bool success = false;
    
    for(u8 i = 0; i < 2; i++)
    {
        FsSaveDataSpaceId save_data_space_id = (i == 0 ? FsSaveDataSpaceId_User : FsSaveDataSpaceId_SdUser);
        
        /* Retrieve save data info reader. */
        rc = fsOpenSaveDataInfoReaderWithFilter(&sv_reader, save_data_space_id, &g_saveDataFilter);
        if (R_FAILED(rc))
        {
            consolePrint("\nfsOpenSaveDataInfoReaderWithFilter failed! (0x%08X, %u).\n", rc, save_data_space_id);
            continue;
        }
        
        while(true)
        {
            /* Get current save data info entry. */
            rc = fsSaveDataInfoReaderRead(&sv_reader, &sv_info, 1, (s64*)&total_entries);
            if (R_FAILED(rc) || !total_entries) break;
            
            /* Reallocate cache info buffer. */
            tmp_cache_info = realloc(cache_info, (count + 1) * sizeof(CacheStorageInfo));
            if (!tmp_cache_info)
            {
                consolePrint("\nFailed to allocate memory for cache storage entry #%lu!\n", count + 1);
                break;
            }
            
            cache_info = tmp_cache_info;
            tmp_cache_info = NULL;
            
            /* Update data. */
            cache_info[count].save_data_space_id = sv_info.save_data_space_id;
            cache_info[count].save_data_index = sv_info.save_data_index;
            count++;
        }
        
        /* Close save data info reader. */
        fsSaveDataInfoReaderClose(&sv_reader);
    }
    
    success = (cache_info != NULL && count);
    if (success)
    {
        *out_cache_info = cache_info;
        *out_count = count;
    }
    
    return success;
}

static bool utilsMountApplicationCacheStorage(const CacheStorageInfo *cache_info)
{
    if (!cache_info)
    {
        consolePrint("\nInvalid parameters!\n");
        return false;
    }
    
    int res = 0;
    Result rc = 0;
    FsFileSystem fs = {0};
    FsSaveDataAttribute attr = {0};
    
    /* Set attributes. */
    attr.application_id = g_appTitleID;
    attr.save_data_type = FsSaveDataType_Cache;
    attr.save_data_index = cache_info->save_data_index;
    
    /* Open cache storage filesystem. */
    rc = fsOpenSaveDataFileSystem(&fs, cache_info->save_data_space_id, &attr);
    if (R_FAILED(rc))
    {
        consolePrint("\nfsOpenSaveDataFileSystem failed! (0x%08X, %u, %u).\n", rc, cache_info->save_data_space_id, cache_info->save_data_index);
        return false;
    }
    
    /* Mount filesystem through devoptab. */
    res = fsdevMountDevice(CACHE_STORAGE_MOUNT_NAME, fs);
    if (res == -1)
    {
        consolePrint("\nfsdevMountDevice failed! (%u, %u).\n", cache_info->save_data_space_id, cache_info->save_data_index);
        fsFsClose(&fs);
        return false;
    }
    
    return true;
}

NX_INLINE void utilsUnmountApplicationCacheStorage(void)
{
    fsdevUnmountDevice(CACHE_STORAGE_MOUNT_NAME);
}

static void utilsDumpDirectory(const char *path, u16 idx)
{
    if (!path || !*path)
    {
        consolePrint("Invalid parameters!\n\n");
        return;
    }
    
    DIR *dir = NULL;
    struct dirent *entry = NULL;
    static int dir_level = 0;
    char tmp_path[FS_MAX_PATH] = {0};
    
    /* Open directory. */
    dir = opendir(path);
    if (!dir)
    {
        consolePrint("Failed to open directory \"%s\"! (%d).\n", path, errno);
        return;
    }
    
    /* Read directory contents. */
    while((entry = readdir(dir)) != NULL)
    {
        /* Skip current and parent directory entries. */
        if (entry->d_type == DT_DIR && (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))) continue;
        
        /* Print tabs. */
        consolePrint("%*s", (dir_level + 1) * 3, "");
        
        /* Generate new path string. */
        snprintf(tmp_path, sizeof(tmp_path), "%s/%s", path, entry->d_name);
        
        switch(entry->d_type)
        {
            case DT_REG: /* File. */
                consolePrint("- [F] %s\n", entry->d_name);
                
                /* Dump file. */
                utilsDumpFile(tmp_path, idx);
                
                break;
            case DT_DIR: /* Directory. */
                consolePrint("- [D] %s:\n", entry->d_name);
                
                /* Increase directory level. */
                dir_level++;
                
                /* Dump directory. */
                utilsDumpDirectory(tmp_path, idx);
                
                /* Decrease directory level. */
                dir_level--;
                
                break;
            default:
                consolePrint("- [?] %s\n", entry->d_name);
                break;
        }
    }
}

static void utilsDumpFile(const char *path, u16 idx)
{
    FILE *src_fd = NULL, *dst_fd = NULL;
    size_t src_size = 0, blksize = DUMP_BLOCKSIZE;
    char sdmc_path[FS_MAX_PATH] = {0}, *pch = NULL;
    bool success = false;
    
    if (!g_fileBuf || !path || !*path || (pch = strchr(path, ':')) == NULL)
    {
        consolePrint("Invalid parameters!\n\n");
        return;
    }
    
    /* Generate output path. */
    snprintf(sdmc_path, sizeof(sdmc_path), DUMP_PATH "%016lX/%04u%s", g_appTitleID, idx, pch + 1);
    
    /* Create directory tree. */
    utilsCreateDirectoryTree(sdmc_path, false);
    
    /* Open files. */
    src_fd = fopen(path, "rb");
    if (!src_fd)
    {
        consolePrint("Failed to open \"%s\" in read mode!\n", path);
        goto end;
    }
    
    dst_fd = fopen(sdmc_path, "wb");
    if (!dst_fd)
    {
        consolePrint("Failed to open \"%s\" in write mode!", sdmc_path);
        goto end;
    }
    
    /* Get source file size. */
    fseek(src_fd, 0, SEEK_END);
    src_size = ftell(src_fd);
    rewind(src_fd);
    
    /* Dump file data. */
    for(size_t blk = 0; blk < src_size; blk += blksize)
    {
        if ((blk + blksize) > src_size) blksize = (src_size - blk);
        
        size_t bytes = fread(g_fileBuf, 1, blksize, src_fd);
        if (bytes != blksize)
        {
            consolePrint("Failed to read 0x%lX byte(s) chunk at offset 0x%lX from \"%s\"!\n", blksize, blk, path);
            goto end;
        }
        
        bytes = fwrite(g_fileBuf, 1, blksize, dst_fd);
        if (bytes != blksize)
        {
            consolePrint("Failed to write 0x%lX byte(s) chunk to offset 0x%lX in \"%s\"!\n", blksize, blk, sdmc_path);
            goto end;
        }
    }
    
    success = true;
    
end:
    if (dst_fd)
    {
        fclose(dst_fd);
        if (!success) remove(sdmc_path);
    }
    
    if (src_fd) fclose(src_fd);
}

void utilsCreateDirectoryTree(const char *path, bool create_last_element)
{
    char *ptr = NULL, *tmp = NULL;
    size_t path_len = 0;
    
    if (!path || !(path_len = strlen(path))) return;
    
    tmp = calloc(path_len + 1, sizeof(char));
    if (!tmp) return;
    
    ptr = strchr(path, '/');
    while(ptr)
    {
        sprintf(tmp, "%.*s", (int)(ptr - path), path);
        mkdir(tmp, 0777);
        ptr = strchr(++ptr, '/');
    }
    
    if (create_last_element) mkdir(path, 0777);
    
    free(tmp);
}

static void utilsScanPads(void)
{
    padUpdate(&g_padState);
}

static u64 utilsGetButtonsDown(void)
{
    return padGetButtonsDown(&g_padState);
}

static void utilsWaitForButtonPress(u64 flag)
{
    /* Don't consider stick movement as button inputs. */
    if (!flag) flag = ~(HidNpadButton_StickLLeft | HidNpadButton_StickLRight | HidNpadButton_StickLUp | HidNpadButton_StickLDown | HidNpadButton_StickRLeft | HidNpadButton_StickRRight | \
                        HidNpadButton_StickRUp | HidNpadButton_StickRDown);
    
    while(appletMainLoop())
    {
        utilsScanPads();
        if (utilsGetButtonsDown() & flag) break;
    }
}
