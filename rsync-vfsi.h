#ifndef RSYNC_VFSI_H
#define RSYNC_VFSI_H

int rsync_vfsi_listdir(const char *path, const char *const **names, size_t *count);
int rsync_vfsi_stat(const char *path, STRUCT_STAT *st);
void rsync_vfsi_reset(void);

#endif /* RSYNC_VFSI_H */
