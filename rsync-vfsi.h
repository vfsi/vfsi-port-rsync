#ifndef RSYNC_VFSI_H
#define RSYNC_VFSI_H

int rsync_vfsi_listdir(const char *path, struct file_struct *file,
                      const char *const **names, size_t *count);
int rsync_vfsi_stat(const char *path, STRUCT_STAT *st);
void rsync_vfsi_remember_dir(struct file_struct *file, const char *path);
void rsync_vfsi_prepare_dirs(struct file_struct *const *files, size_t count);
void rsync_vfsi_prepare_siblings(struct file_struct *const *files, int next);
void rsync_vfsi_listdir_done(void);
void rsync_vfsi_reset(void);

#endif /* RSYNC_VFSI_H */
