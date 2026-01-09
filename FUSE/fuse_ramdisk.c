#define FUSE_USE_VERSION 30
#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <stdlib.h>
#include <stdarg.h>

typedef enum { LOG_ERROR = 0, LOG_INFO = 1, LOG_DEBUG = 2 } log_level_t;
static log_level_t current_log_level = LOG_INFO;

static void log_msg(log_level_t level, const char *fmt, ...) {
    if (level > current_log_level) return;
    
    char buffer[1024];
    const char *lvlstr = (level == LOG_ERROR) ? "ERROR" : (level == LOG_INFO) ? "INFO" : "DEBUG";
    

    int offset = snprintf(buffer, sizeof(buffer), "fuse_ramdisk: %s: ", lvlstr);
    
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buffer + offset, sizeof(buffer) - offset, fmt, ap);
    va_end(ap);

    fprintf(stderr, "%s\n", buffer);

    // <3> = KERN_ERR, <6> = KERN_INFO, <7> = KERN_DEBUG
    const char *k_prio = (level == LOG_ERROR) ? "<3>" : (level == LOG_INFO) ? "<6>" : "<7>";
    
    int kfd = open("/dev/kmsg", O_WRONLY);
    if (kfd >= 0) {
        char kbuffer[1050];
        snprintf(kbuffer, sizeof(kbuffer), "%s%s\n", k_prio, buffer);
        write(kfd, kbuffer, strlen(kbuffer));
        close(kfd);
    }
}

#define MAX_FILES 100
#define MAX_FILE_SIZE 4096

struct MyFile {
    char name[256];
    mode_t mode;    // Permissions and S_IFREG/S_IFDIR
    size_t size;
    int parent_index;
    char data_buffer[MAX_FILE_SIZE];
};

static struct MyFile file_table[MAX_FILES] = {0};

static void init_root() {
    file_table[0].mode = S_IFDIR | 0755;
    file_table[0].name[0] = '/'; 
    file_table[0].parent_index = 0; // Root is its own parent
    file_table[0].size = 4096;
    log_msg(LOG_INFO, "init_root: created root directory");
}

static int find_free_slot() {
    for (int i = 1; i < MAX_FILES; i++) {
        if (file_table[i].name[0] == '\0') {
            return i;
        }
    }
    return -1; 
}

static int resolve_path_to_index(const char *path) {
    log_msg(LOG_DEBUG, "resolve_path_to_index: '%s'", path);

    if (strcmp(path, "/") == 0) {
        log_msg(LOG_DEBUG, "resolve_path_to_index: root -> 0");
        return 0;
    }

    char path_copy[512];
    strncpy(path_copy, path, sizeof(path_copy) - 1);

    size_t len = strlen(path_copy);
    while (len > 1 && path_copy[len - 1] == '/') {
        path_copy[len - 1] = '\0';
        len--;
    }
    path_copy[sizeof(path_copy) - 1] = '\0';

    int current_parent_index = 0;
    int found_index = -1;
    
    char *p = path_copy;
    if (*p == '/') p++;
    
    // code written with help of AI
    while (*p != '\0') {
        // Find the end of the token (next slash or end of string)
        char *slash = strchr(p, '/');
        size_t token_len;
        
        if (slash != NULL) {
            token_len = slash - p;
            *slash = '\0';
        } else {
            token_len = strlen(p);
        }
        
        if (token_len == 0) {
            // Empty token (e.g. double slash) - continue
            if (slash) p = slash + 1;
            else break;
            continue;
        }

        // Search for the token in the table
        int found = 0;
        for (int i = 1; i < MAX_FILES; i++) {
            if (file_table[i].name[0] != '\0' &&
                file_table[i].parent_index == current_parent_index &&
                strcmp(file_table[i].name, p) == 0) 
            {
                current_parent_index = i;
                found_index = i;
                found = 1;
                break;
            }
        }
        
        if (!found) {
            char token_safe[256];
            strncpy(token_safe, p, sizeof(token_safe) - 1);
            token_safe[sizeof(token_safe) - 1] = '\0';
            log_msg(LOG_DEBUG, "resolve_path_to_index: '%s' not found (token '%s')", path, token_safe);
            return -ENOENT;
        }
        
        if (slash) {
            p = slash + 1;
        } else {
            break;
        }
    }
    //end

    log_msg(LOG_DEBUG, "resolve_path_to_index: '%s' -> index %d", path, found_index);
    return found_index;
}

static int resolve_parent_and_name(const char *path, int *parent_index_out, char *child_name_out) {
    log_msg(LOG_DEBUG, "resolve_parent_and_name: parsing '%s'", path);
    
    char *path_copy = strdup(path);
    if (path_copy == NULL) {
        log_msg(LOG_ERROR, "resolve_parent_and_name: strdup failed");
        return -ENOMEM;
    }

    char *last_slash = strrchr(path_copy, '/');
    if (last_slash == NULL) {
        free(path_copy);
        log_msg(LOG_ERROR, "resolve_parent_and_name: no slash in path");
        return -EINVAL;
    }

    strncpy(child_name_out, last_slash + 1, 255);
    child_name_out[255] = '\0';

    if (last_slash == path_copy) {
        path_copy[1] = '\0';
    } else {
        *last_slash = '\0';
    }

    *parent_index_out = resolve_path_to_index(path_copy);
    log_msg(LOG_DEBUG, "resolve_parent_and_name: parent_index=%d child='%s'", *parent_index_out, child_name_out);
    
    free(path_copy);
    return *parent_index_out;
}


static int my_fs_getattr(const char *path, struct stat *stbuf) {
    memset(stbuf, 0, sizeof(struct stat));
    
    int index = resolve_path_to_index(path);
    if (index < 0) return index; 

    stbuf->st_mode = file_table[index].mode;
    stbuf->st_nlink = (S_ISDIR(stbuf->st_mode)) ? 2 : 1; 
    stbuf->st_size = file_table[index].size;
    stbuf->st_uid = getuid();
    stbuf->st_gid = getgid();
    
    return 0;
}

static int my_fs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                         off_t offset, struct fuse_file_info *fi) {
    
    int parent_index = resolve_path_to_index(path);
    if (parent_index < 0) return parent_index;

    if (!S_ISDIR(file_table[parent_index].mode)) {
        return -ENOTDIR;
    }

    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);

    for (int i = 1; i < MAX_FILES; i++) {
        if (file_table[i].name[0] != '\0' && file_table[i].parent_index == parent_index) {
            filler(buf, file_table[i].name, NULL, 0);
        }
    }
    return 0;
}

static int my_fs_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
    log_msg(LOG_INFO, "create: '%s' mode=0%o", path, mode);
    
    int exists = resolve_path_to_index(path);
    if (exists >= 0) {
        log_msg(LOG_ERROR, "create: '%s' already exists", path);
        return -EEXIST;
    }
    if (exists < 0 && exists != -ENOENT) return exists;

    char child_name[256];
    int parent_index;
    
    int pres = resolve_parent_and_name(path, &parent_index, child_name);
    if (pres < 0) {
        log_msg(LOG_ERROR, "create: parent lookup failed for '%s'", path);
        return pres;
    }

    if (!S_ISDIR(file_table[parent_index].mode)) {
        log_msg(LOG_ERROR, "create: parent not a directory");
        return -ENOTDIR;
    }

    int index = find_free_slot();
    if (index == -1) {
        log_msg(LOG_ERROR, "create: no free slots");
        return -ENOSPC;
    }

    strncpy(file_table[index].name, child_name, 255);
    file_table[index].mode = S_IFREG | mode;
    file_table[index].size = 0;
    file_table[index].parent_index = parent_index;
    file_table[index].data_buffer[0] = '\0'; 

    log_msg(LOG_INFO, "create: created '%s' at index %d", child_name, index);
    return 0;
}

static int my_fs_mkdir(const char *path, mode_t mode) {
    log_msg(LOG_INFO, "mkdir: '%s' mode=0%o", path, mode);
    
    int exists = resolve_path_to_index(path);
    if (exists >= 0) {
        log_msg(LOG_ERROR, "mkdir: '%s' already exists", path);
        return -EEXIST;
    }
    if (exists < 0 && exists != -ENOENT) return exists;

    char child_name[256];
    int parent_index;
    
    int pres = resolve_parent_and_name(path, &parent_index, child_name);
    if (pres < 0) {
        log_msg(LOG_ERROR, "mkdir: parent lookup failed for '%s'", path);
        return pres;
    }

    if (!S_ISDIR(file_table[parent_index].mode)) {
        log_msg(LOG_ERROR, "mkdir: parent not a directory");
        return -ENOTDIR;
    }

    int index = find_free_slot();
    if (index == -1) {
        log_msg(LOG_ERROR, "mkdir: no free slots");
        return -ENOSPC;
    } 
    
    strncpy(file_table[index].name, child_name, 255);
    file_table[index].mode = S_IFDIR | mode;
    file_table[index].size = 4096;
    file_table[index].parent_index = parent_index;

    log_msg(LOG_INFO, "mkdir: created dir '%s' at index %d", child_name, index);
    return 0;
}

static int my_fs_read(const char *path, char *buf, size_t size, off_t offset,
                      struct fuse_file_info *fi) {
    
    int index = resolve_path_to_index(path);
    if (index < 0) return index;

    if (S_ISDIR(file_table[index].mode)) return -EISDIR;

    size_t len = file_table[index].size;
    if (offset < len) {
        if (offset + size > len) size = len - offset;
        memcpy(buf, file_table[index].data_buffer + offset, size);
    } else {
        size = 0;
    }
    return size;
}

static int my_fs_write(const char *path, const char *buf, size_t size,
                       off_t offset, struct fuse_file_info *fi) {
    log_msg(LOG_DEBUG, "write: '%s' size=%zu offset=%ld", path, size, offset);
    
    int index = resolve_path_to_index(path);
    if (index < 0) {
        log_msg(LOG_ERROR, "write: '%s' not found", path);
        return index;
    }

    if (S_ISDIR(file_table[index].mode)) {
        log_msg(LOG_ERROR, "write: '%s' is a directory", path);
        return -EISDIR;
    }

    if (offset + size > MAX_FILE_SIZE) {
        log_msg(LOG_ERROR, "write: exceeds max file size");
        return -EFBIG;
    } 

    memcpy(file_table[index].data_buffer + offset, buf, size);
    
    if (offset + size > file_table[index].size) {
        file_table[index].size = offset + size;
    }

    log_msg(LOG_INFO, "write: '%s' wrote %zu bytes, new size=%zu", path, size, file_table[index].size);
    return size;
}

static int my_fs_truncate(const char *path, off_t size) {
    
    int index = resolve_path_to_index(path);
    if (index < 0) return index;
    if (S_ISDIR(file_table[index].mode)) return -EISDIR; 

    if (size > MAX_FILE_SIZE) return -EFBIG;

    file_table[index].size = size;
    if (size == 0) file_table[index].data_buffer[0] = '\0';
    
    return 0;
}

static int my_fs_unlink(const char *path) {
    log_msg(LOG_INFO, "unlink: '%s'", path);
    
    int index = resolve_path_to_index(path);
    if (index < 0) {
        log_msg(LOG_ERROR, "unlink: '%s' not found", path);
        return index;
    }
    
    if (S_ISDIR(file_table[index].mode)) {
        log_msg(LOG_ERROR, "unlink: '%s' is a directory", path);
        return -EISDIR;
    }

    file_table[index].name[0] = '\0';
    file_table[index].size = 0;

    log_msg(LOG_INFO, "unlink: removed '%s' (index %d)", path, index);
    return 0;
}

static int my_fs_rmdir(const char *path) {
    log_msg(LOG_INFO, "rmdir: '%s'", path);
    
    int index = resolve_path_to_index(path);
    if (index < 0) {
        log_msg(LOG_ERROR, "rmdir: '%s' not found", path);
        return index;
    }
    
    if (!S_ISDIR(file_table[index].mode)) {
        log_msg(LOG_ERROR, "rmdir: '%s' not a directory", path);
        return -ENOTDIR;
    }
    
    for (int i = 1; i < MAX_FILES; i++) {
        if (file_table[i].name[0] != '\0' && file_table[i].parent_index == index) {
            log_msg(LOG_ERROR, "rmdir: '%s' not empty (has child '%s')", path, file_table[i].name);
            return -ENOTEMPTY;
        }
    }

    file_table[index].name[0] = '\0';
    file_table[index].size = 0;

    log_msg(LOG_INFO, "rmdir: removed '%s' (index %d)", path, index);
    return 0;
}

static int my_fs_utimens(const char *path, const struct timespec tv[2]) {
    int index = resolve_path_to_index(path);
    if (index < 0) return index;
    return 0;
}

static int my_fs_statfs(const char *path, struct statvfs *stbuf) {
    memset(stbuf, 0, sizeof(struct statvfs));

    stbuf->f_bsize = 4096;
    stbuf->f_frsize = 4096;
    stbuf->f_blocks = MAX_FILES;
    stbuf->f_bfree = 0;
    stbuf->f_bavail = 0;
    
    int free_count = 0;
    for (int i = 0; i < MAX_FILES; i++) {
        if (file_table[i].name[0] == '\0') free_count++;
    }
    
    stbuf->f_bfree = free_count;
    stbuf->f_bavail = free_count;
    stbuf->f_namemax = 255;

    log_msg(LOG_DEBUG, "statfs: reporting %d free slots out of %d", free_count, MAX_FILES);
    return 0;
}

static struct fuse_operations my_fs_ops = {
    .getattr   = my_fs_getattr,
    .readdir   = my_fs_readdir,
    .create    = my_fs_create,   
    .mkdir     = my_fs_mkdir,
    .rmdir     = my_fs_rmdir,
    .read      = my_fs_read,
    .write     = my_fs_write,
    .truncate  = my_fs_truncate,
    .unlink    = my_fs_unlink,   
    .utimens   = my_fs_utimens,
    .statfs    = my_fs_statfs,
};


int main(int argc, char *argv[]) {
    init_root();

    char **clean_argv = malloc(sizeof(char *) * argc);
    if (!clean_argv) return 1;
    int clean_argc = 0;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--debug") == 0) {
            current_log_level = LOG_DEBUG;
            log_msg(LOG_INFO, "Debug logging enabled");
        } else {
            clean_argv[clean_argc++] = argv[i];
        }
    }
    
    int ret = fuse_main(clean_argc, clean_argv, &my_fs_ops, NULL);
    free(clean_argv);
    return ret;
}
