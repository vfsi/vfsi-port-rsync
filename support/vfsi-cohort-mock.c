/* Deterministic sender-frontier SDK double. No network or published SDK required. */
#define _GNU_SOURCE 1
#include <vfsi.h>
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct vfsi_fs { unsigned calls; };
struct vfsi_dir_cursor { int fd; char *path; struct vfsi_fs *owner; };
struct row { size_t index; char *name; struct vfsi_attrs attrs; struct vfsi_dir_cursor *child; };
static unsigned live;
static void event(const char *kind, const char *path, size_t count, size_t anchored)
{
    const char *log = getenv("VFSI_TEST_LOG");
    FILE *out = log ? fopen(log,"a") : NULL;
    if (out) { fprintf(out,"%s\t%zu\t%zu\t%s\n",kind,count,anchored,path ? path : ""); fclose(out); }
}
static struct vfsi_dir_cursor *cursor(struct vfsi_fs *fs, int fd, const char *path)
{
    struct vfsi_dir_cursor *c;
    if (fd < 0) return NULL;
    c = calloc(1,sizeof(*c)); assert(c);
    c->fd=fd; c->path=strdup(path); c->owner=fs; assert(c->path); live++;
    return c;
}
static void release(struct vfsi_dir_cursor *c)
{
    if (c) { close(c->fd); free(c->path); free(c); assert(live); live--; }
}
uint32_t vfsi_abi_version(void) { return VFSI_ABI_VERSION; }
int vfsi_dummy_open_mount(const char *root, const char *mountpoint, struct vfsi_fs **out)
{
    (void)root; (void)mountpoint;
    *out=calloc(1,sizeof(**out)); return *out ? 0 : ENOMEM;
}
void vfsi_free(struct vfsi_fs *fs)
{
    event("FREE",NULL,live,0); assert(live == 0); free(fs);
}
static void attributes(struct vfsi_attrs *a, const struct stat *s)
{
#if defined(__APPLE__)
    const struct timespec *atime = &s->st_atimespec;
    const struct timespec *mtime = &s->st_mtimespec;
    const struct timespec *ctime = &s->st_ctimespec;
#else
    const struct timespec *atime = &s->st_atim;
    const struct timespec *mtime = &s->st_mtim;
    const struct timespec *ctime = &s->st_ctim;
#endif
    memset(a,0,sizeof(*a)); a->abi_version=VFSI_ABI_VERSION; a->struct_size=sizeof(*a);
    a->ftype=S_ISDIR(s->st_mode) ? 2 : S_ISREG(s->st_mode) ? 1 : S_ISLNK(s->st_mode) ? 5 : 0;
    a->mode=s->st_mode & 07777; a->size=s->st_size; a->nlink=s->st_nlink;
    a->fileid=s->st_ino; a->blocks=s->st_blocks; a->uid=s->st_uid; a->gid=s->st_gid;
    a->atime_sec=atime->tv_sec; a->atime_nsec=atime->tv_nsec;
    a->mtime_sec=mtime->tv_sec; a->mtime_nsec=mtime->tv_nsec;
    a->ctime_sec=ctime->tv_sec; a->ctime_nsec=ctime->tv_nsec;
}
static int listing(struct vfsi_fs *fs, const char *const *paths,
    struct vfsi_dir_cursor **input, size_t count, const struct vfsi_listing_options *opts,
    vfsi_indexed_listdir_cursor_cb cb, void *data, struct vfsi_listing_result *results, int children)
{
    struct vfsi_dir_cursor *owned[32]={0};
    struct row *rows=NULL;
    size_t i,nr=0,alloc=0,bytes=0,anchored=0;
    int error=0,stop=0;
    const char *fault=getenv("VFSI_TEST_FAULT");
    assert(count <= 32);
    memset(results,0,count*sizeof(*results));
    for (i=0;i<count;i++) { owned[i]=input[i]; input[i]=NULL; anchored+=owned[i] != NULL; }
    event(children ? "BATCH" : "LEGACY",NULL,count,anchored);
    fs->calls++;
    for (i=0;i<count && !error;i++) {
        DIR *dir;
        struct dirent *entry;
        int fd;
        if (!owned[i]) owned[i]=cursor(fs,open(paths[i],O_RDONLY|O_DIRECTORY|O_NOFOLLOW),paths[i]);
        if (!owned[i]) { error=errno; break; }
        assert(owned[i]->owner == fs);
        event("DIR",paths[i],0,0);
        fd=dup(owned[i]->fd);
        dir=fd >= 0 ? fdopendir(fd) : NULL;
        if (!dir) { if (fd>=0) close(fd); error=errno; break; }
        while ((entry=readdir(dir)) != NULL) {
            struct stat st;
            struct row *row;
            char *path;
            size_t len;
            if (!strcmp(entry->d_name,".") || !strcmp(entry->d_name,"..")) continue;
            if (fstatat(dirfd(dir),entry->d_name,&st,AT_SYMLINK_NOFOLLOW)) { error=errno; break; }
            assert(asprintf(&path,"%s/%s",paths[i],entry->d_name)>=0);
            len=strlen(path);
            if (nr >= opts->max_entries || len > opts->max_path_bytes-bytes) { free(path); error=EFBIG; break; }
            bytes+=len;
            if (nr==alloc) { alloc=alloc ? alloc*2 : 32; rows=realloc(rows,alloc*sizeof(*rows)); assert(rows); }
            row=&rows[nr++]; memset(row,0,sizeof(*row)); row->index=i; row->name=strdup(entry->d_name); assert(row->name);
            attributes(&row->attrs,&st);
            if (children && S_ISDIR(st.st_mode)) {
                row->child=cursor(fs,openat(dirfd(dir),entry->d_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW),path);
                assert(row->child);
            }
            free(path);
        }
        closedir(dir);
        release(owned[i]); owned[i]=NULL;
    }
    for (i=0;i<count;i++) release(owned[i]);
    if (!error) {
        for (i=0;i<count;i++) results[i].completion=1;
        for (i=0;i<nr;i++) {
            struct vfsi_dir_cursor *child=rows[i].child;
            rows[i].child=NULL; /* callback owns it, including when it stops */
            if (fs->calls==2 && fault && !strcmp(fault,"invalid")) rows[i].attrs.abi_version=0;
            results[rows[i].index].delivered++;
            if (!cb(rows[i].index,paths[rows[i].index],rows[i].name,&rows[i].attrs,child,data)) stop=1;
            if (fs->calls==2 && fault && !strcmp(fault,"partial")) { error=EIO; break; }
            if (stop) break;
        }
        if (stop) for (i=0;i<count;i++) results[i].completion=2;
    }
    for (i=0;i<nr;i++) { release(rows[i].child); free(rows[i].name); }
    free(rows);
    return error;
}
#ifndef VFSI_TEST_OLD_LIBRARY
void vfsi_dir_cursor_free(struct vfsi_dir_cursor *c) { release(c); }
int vfsi_listdirs_from_cursors(struct vfsi_fs *fs, const char *const *dirs,
    struct vfsi_dir_cursor **cursors, size_t count, const struct vfsi_listing_options *options,
    vfsi_indexed_listdir_cursor_cb cb, void *data, struct vfsi_listing_result *results)
{
    return listing(fs,dirs,cursors,count,options,cb,data,results,1);
}
#endif
#ifndef VFSI_TEST_NO_LEGACY
struct legacy { vfsi_indexed_listdir_cb cb; void *data; };
static bool legacy_row(size_t index, const char *dir, const char *name,
    const struct vfsi_attrs *attrs, struct vfsi_dir_cursor *child, void *data)
{
    struct legacy *legacy=data; release(child);
    return legacy->cb(index,dir,name,attrs,legacy->data);
}
int vfsi_listdirs(struct vfsi_fs *fs, const char *const *dirs, size_t count,
    const struct vfsi_listing_options *options, vfsi_indexed_listdir_cb cb,
    void *data, struct vfsi_listing_result *results)
{
    struct vfsi_dir_cursor *input[32]={0};
    struct legacy legacy={cb,data};
    return listing(fs,dirs,input,count,options,legacy_row,&legacy,results,0);
}

#endif /* VFSI_TEST_NO_LEGACY */

#ifdef VFSI_TEST_ATTRIBUTES
/* Standalone projection test; the Python driver compares native stat values. */
int main(int argc, char **argv)
{
    struct stat st;
    struct vfsi_attrs attrs;
    if (argc != 2 || stat(argv[1], &st))
        return 1;
    attributes(&attrs, &st);
    printf("%lld %u %lld %u %lld %u\n",
        (long long)attrs.atime_sec, attrs.atime_nsec,
        (long long)attrs.mtime_sec, attrs.mtime_nsec,
        (long long)attrs.ctime_sec, attrs.ctime_nsec);
    return 0;
}
#endif
