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

/* Global variables. */

static PadState g_padState = {0};
static const u64 g_crTitleID = 0x0100C090153B4000ULL;
static NsApplicationControlData g_nsAppControlData = {0};
static u8 *g_fileBuf = NULL;

/* Function prototypes. */

__attribute__((format(printf, 1, 2))) static void consolePrint(const char *text, ...);

static bool utilsRetrieveCrunchyrollControlData(void);
static bool utilsMountCrunchyrollCacheStorage(u16 idx);
NX_INLINE void utilsUnmountCrunchyrollCacheStorage(void);

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
    Result rc = 0;
    
    NacpStruct *nacp = NULL;
    
    /* Configure input. */
    /* Up to 8 different, full controller inputs. */
    /* Individual Joy-Cons not supported. */
    padConfigureInput(8, HidNpadStyleSet_NpadFullCtrl);
    padInitializeWithMask(&g_padState, 0x1000000FFUL);
    
    /* Initialize console output. */
    consoleInit(NULL);
    
    /* Print message. */
    consolePrint(APP_TITLE ". Built on " BUILD_TIMESTAMP ".\n\n");
    
    /* Initialize ns interface. */
    consolePrint("Initializing ns... ");
    
    rc = nsInitialize();
    if (R_FAILED(rc))
    {
        consolePrint("\nnsInitialize failed! (0x%08X).\n", rc);
        ret = -1;
        goto end1;
    }
    
    consolePrint("OK!\n\n");
    
    /* Retrieve Crunchyroll's control data. */
    consolePrint("Retrieving Crunchyroll's control data... ");
    if (!utilsRetrieveCrunchyrollControlData()) goto end2;
    consolePrint("OK!\n\n");
    
    /* Print cache storage properties. */
    nacp = &(g_nsAppControlData.nacp);
    consolePrint("Cache storage properties:\n\t- Size: 0x%lX.\n\t- Journal size: 0x%lX.\n\t- Data + Journal Max Size: 0x%lX.\n\t- Max index: %u.\n\n", \
                 nacp->cache_storage_size, \
                 nacp->cache_storage_journal_size, \
                 nacp->cache_storage_data_and_journal_size_max, \
                 nacp->cache_storage_index_max);
    
    /* Allocate memory for our file dump buffer. */
    g_fileBuf = malloc(DUMP_BLOCKSIZE);
    if (!g_fileBuf)
    {
        consolePrint("\nFailed to allocate memory for file dump buffer!\n");
        ret = -2;
        goto end2;
    }
    
    /* Try to open any cache storage(s). */
    for(u16 i = 0; i <= nacp->cache_storage_index_max; i++)
    {
        /* Mount current cache storage. */
        consolePrint("Mounting cache storage #%u... ", i);
        if (!utilsMountCrunchyrollCacheStorage(i)) continue;
        consolePrint("OK!\n");
        
        /* Directory listing. */
        consolePrint("Directory listing for cache storage #%u:\n", i);
        utilsDumpDirectory(CACHE_STORAGE_MOUNT_NAME ":/", i);
        consolePrint("\n");
        
        /* Unmount current cache storage. */
        utilsUnmountCrunchyrollCacheStorage();
    }
    
    /* Free file dump buffer. */
    free(g_fileBuf);
    
    consolePrint("Process finished. Press any button to exit.");
    utilsWaitForButtonPress(0);
    
end2:
    /* Close ns interface. */
    nsExit();
    
end1:
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

static bool utilsRetrieveCrunchyrollControlData(void)
{
    Result rc = 0;
    u64 write_size = 0;
    
    /* Retrieve ns application control data. */
    rc = nsGetApplicationControlData(NsApplicationControlSource_Storage, g_crTitleID, &g_nsAppControlData, sizeof(NsApplicationControlData), &write_size);
    if (R_FAILED(rc))
    {
        consolePrint("\nnsGetApplicationControlData failed for title ID \"%016lX\"! (0x%08X).\n", g_crTitleID, rc);
        return false;
    }
    
    if (write_size < sizeof(NacpStruct))
    {
        consolePrint("\nRetrieved application control data buffer is too small! (0x%lX).\n", write_size);
        return false;
    }
    
    return true;
}

static bool utilsMountCrunchyrollCacheStorage(u16 idx)
{
    Result rc = 0;
    
    /* Open cache storage. */
    rc = fsdevMountCacheStorage(CACHE_STORAGE_MOUNT_NAME, g_crTitleID, idx);
    if (R_FAILED(rc))
    {
        consolePrint("\nfsdevMountCacheStorage failed! (0x%08X).\n\n", rc);
        return false;
    }
    
    return true;
}

NX_INLINE void utilsUnmountCrunchyrollCacheStorage(void)
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
    snprintf(sdmc_path, sizeof(sdmc_path), DUMP_PATH "%016lX/%04u%s", g_crTitleID, idx, pch + 1);
    
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
