/*
 * Optional vectorized filesystem support for sender-side file-list scans.
 *
 * The feature is compiled with --enable-vfsi and enabled at runtime with
 * VFSI_IMPL=nfs (or dummy) plus VFSI_LIBRARY=/path/to/libvfsi_c.so.
 */

#include "rsync.h"
#include "ifuncs.h"
#include "rsync-vfsi.h"
#include <vfsi.h>

#if VFSI_ABI_VERSION != 3
#error "rsync VFSI support requires VFSI C ABI v3"
#endif

#include <dlfcn.h>

extern int am_daemon;
extern int am_root;
extern int copy_dirlinks;
extern int copy_links;
extern int copy_unsafe_links;
extern int insecure_links;
extern int one_file_system;
extern int preserve_hard_links;
extern int module_id;
extern char curr_dir[MAXPATHLEN];

enum {
	VFSI_NF4REG = 1,
	VFSI_NF4DIR = 2,
};

struct vfsi_bindings {
	void *handle;
	uint32_t (*abi_version)(void);
	int (*dummy_open_mount)(const char *, const char *, struct vfsi_fs **);
	int (*nfs_open_mount_export)(const char *, const char *, const char *,
				     struct vfsi_fs **);
	int (*listdirv)(struct vfsi_fs *, const char *const *, size_t, size_t,
			bool, vfsi_listdirv_cb, void *);
	void (*free)(struct vfsi_fs *);
};

struct vfsi_map_slot {
	uint64_t hash;
	const char *key;
	void *value;
};

struct vfsi_map {
	struct vfsi_map_slot *slots;
	size_t size;
	size_t used;
};

struct vfsi_dir {
	char *path;
	char **names;
	size_t nr;
	size_t alloc;
};

struct vfsi_entry {
	char *path;
	struct vfsi_attrs attrs;
};

static struct {
	struct vfsi_bindings bindings;
	int loader_state;
	struct vfsi_fs *fs;
	char *mountpoint;
	struct vfsi_dir **dirs;
	size_t dirs_nr;
	size_t dirs_alloc;
	struct vfsi_entry **entries;
	size_t entries_nr;
	size_t entries_alloc;
	struct vfsi_map dirs_by_path;
	struct vfsi_map entries_by_path;
	int active;
	int failed;
} vfsi_state;

static uint64_t vfsi_hash(const char *s)
{
	uint64_t hash = UINT64_C(1469598103934665603);

	while (*s) {
		hash ^= (unsigned char)*s++;
		hash *= UINT64_C(1099511628211);
	}
	return hash ? hash : 1;
}

static void vfsi_map_insert_slot(struct vfsi_map *map, uint64_t hash,
				 const char *key, void *value)
{
	size_t pos = (size_t)hash & (map->size - 1);

	while (map->slots[pos].hash)
		pos = (pos + 1) & (map->size - 1);
	map->slots[pos].hash = hash;
	map->slots[pos].key = key;
	map->slots[pos].value = value;
	map->used++;
}

static void vfsi_map_grow(struct vfsi_map *map)
{
	struct vfsi_map_slot *old = map->slots;
	size_t old_size = map->size;
	size_t i;

	map->size = old_size ? old_size * 2 : 64;
	map->slots = new_array0(struct vfsi_map_slot, map->size);
	map->used = 0;
	for (i = 0; i < old_size; i++) {
		if (old[i].hash)
			vfsi_map_insert_slot(map, old[i].hash, old[i].key,
					     old[i].value);
	}
	free(old);
}

static void vfsi_map_put(struct vfsi_map *map, const char *key, void *value)
{
	uint64_t hash = vfsi_hash(key);
	size_t pos;

	if (!map->size || (map->used + 1) * 4 >= map->size * 3)
		vfsi_map_grow(map);
	pos = (size_t)hash & (map->size - 1);
	while (map->slots[pos].hash) {
		if (map->slots[pos].hash == hash && !strcmp(map->slots[pos].key, key)) {
			map->slots[pos].value = value;
			return;
		}
		pos = (pos + 1) & (map->size - 1);
	}
	vfsi_map_insert_slot(map, hash, key, value);
}

static void *vfsi_map_get(const struct vfsi_map *map, const char *key)
{
	uint64_t hash;
	size_t pos;

	if (!map->size)
		return NULL;
	hash = vfsi_hash(key);
	pos = (size_t)hash & (map->size - 1);
	while (map->slots[pos].hash) {
		if (map->slots[pos].hash == hash && !strcmp(map->slots[pos].key, key))
			return map->slots[pos].value;
		pos = (pos + 1) & (map->size - 1);
	}
	return NULL;
}

static int vfsi_absolute_path(const char *path, char *buf, size_t size)
{
	size_t len;

	if (*path == '/')
		len = strlcpy(buf, path, size);
	else
		len = pathjoin(buf, size, curr_dir, path);
	if (len >= size)
		return 0;
	clean_fname(buf, CFN_COLLAPSE_DOT_DOT_DIRS | CFN_DROP_TRAILING_DOT_DIR);
	return 1;
}

static int vfsi_path_is_under(const char *path, const char *root)
{
	size_t len = strlen(root);

	return !strncmp(path, root, len)
	    && (len == 1 || path[len] == '\0' || path[len] == '/');
}

static int vfsi_eligible(void)
{
	const char *impl = getenv("VFSI_IMPL");
	const char *library = getenv("VFSI_LIBRARY");

	if (!impl || !*impl || !strcmp(impl, "off") || !library || !*library)
		return 0;
	if (strcmp(impl, "nfs") && strcmp(impl, "dummy"))
		return 0;
	/* These modes need metadata or path-resolution semantics that this
	 * integration does not consume safely. */
	if (am_daemon || module_id >= 0 || am_root < 0 || copy_links
	 || copy_unsafe_links || copy_dirlinks || insecure_links
	 || one_file_system || preserve_hard_links)
		return 0;
	return 1;
}

static int vfsi_load_symbol(void *handle, const char *name, void **out)
{
	void *symbol = dlsym(handle, name);

	if (!symbol) {
		rprintf(FWARNING, "vfsi: cannot load %s: %s\n", name, dlerror());
		return 0;
	}
	*out = symbol;
	return 1;
}

static int vfsi_load(void)
{
	struct vfsi_bindings candidate = { 0 };
	const char *library = getenv("VFSI_LIBRARY");
	void *symbol;

	if (vfsi_state.loader_state)
		return vfsi_state.loader_state > 0;
	candidate.handle = dlopen(library, RTLD_NOW | RTLD_LOCAL);
	if (!candidate.handle) {
		rprintf(FWARNING, "vfsi: cannot load %s: %s\n", library, dlerror());
		goto fail;
	}
#define LOAD(name, field) do { \
	if (!vfsi_load_symbol(candidate.handle, name, &symbol)) \
		goto fail; \
	memcpy(&(field), &symbol, sizeof(symbol)); \
} while (0)
	LOAD("vfsi_abi_version", candidate.abi_version);
	if (candidate.abi_version() != VFSI_ABI_VERSION) {
		rprintf(FWARNING, "vfsi: incompatible ABI (need %d)\n",
			VFSI_ABI_VERSION);
		goto fail;
	}
	LOAD("vfsi_dummy_open_mount", candidate.dummy_open_mount);
	LOAD("vfsi_nfs_open_mount_export", candidate.nfs_open_mount_export);
	LOAD("vfsi_listdirv", candidate.listdirv);
	LOAD("vfsi_free", candidate.free);
#undef LOAD
	vfsi_state.bindings = candidate;
	vfsi_state.loader_state = 1;
	return 1;

fail:
	if (candidate.handle)
		dlclose(candidate.handle);
	vfsi_state.loader_state = -1;
	return 0;
}

struct vfsi_mount {
	char *mountpoint;
	char *host;
	char *export_root;
};

static void vfsi_mount_clear(struct vfsi_mount *mount)
{
	free(mount->mountpoint);
	free(mount->host);
	free(mount->export_root);
	memset(mount, 0, sizeof(*mount));
}

#ifdef __linux__
static void vfsi_unescape_mount_field(char *field)
{
	char *src = field;
	char *dst = field;

	while (*src) {
		if (src[0] == '\\' && src[1] >= '0' && src[1] <= '7'
		 && src[2] >= '0' && src[2] <= '7'
		 && src[3] >= '0' && src[3] <= '7') {
			*dst++ = ((src[1] - '0') << 6) | ((src[2] - '0') << 3)
			       | (src[3] - '0');
			src += 4;
		} else
			*dst++ = *src++;
	}
	*dst = '\0';
}
#endif

static int vfsi_find_mount(const char *path, struct vfsi_mount *result)
{
#ifdef __linux__
	FILE *fp;
	char line[8192];
	size_t best_len = 0;

	fp = fopen("/proc/self/mounts", "r");
	if (!fp)
		return 0;
	while (fgets(line, sizeof(line), fp)) {
		char source[4096], mountpoint[4096], fstype[64];
		char *export_sep;
		size_t len;

		if (sscanf(line, "%4095s %4095s %63s", source, mountpoint,
			   fstype) != 3)
			continue;
		if (strcmp(fstype, "nfs") && strcmp(fstype, "nfs4"))
			continue;
		vfsi_unescape_mount_field(source);
		vfsi_unescape_mount_field(mountpoint);
		len = strlen(mountpoint);
		if (len <= best_len || !vfsi_path_is_under(path, mountpoint))
			continue;
		export_sep = strstr(source, ":/");
		if (!export_sep)
			continue;
		vfsi_mount_clear(result);
		result->mountpoint = strdup(mountpoint);
		result->host = new_array(char, (size_t)(export_sep - source) + 1);
		memcpy(result->host, source, (size_t)(export_sep - source));
		result->host[export_sep - source] = '\0';
		result->export_root = strdup(export_sep + 1);
		best_len = len;
	}
	fclose(fp);
	return result->mountpoint != NULL;
#else
	(void)path;
	(void)result;
	return 0;
#endif
}

static int vfsi_open_for(const char *path)
{
	struct vfsi_bindings *b = &vfsi_state.bindings;
	const char *impl = getenv("VFSI_IMPL");
	const char *host_override = getenv("VFSI_HOST");
	const char *export_override = getenv("VFSI_EXPORT");
	const char *mount_override = getenv("VFSI_MOUNT");
	struct vfsi_mount mount = { 0 };
	int rc;

	if (vfsi_state.fs)
		return vfsi_path_is_under(path, vfsi_state.mountpoint);
	if (!vfsi_load())
		return 0;
	if (!strcmp(impl, "dummy")) {
		const char *root = getenv("VFSI_ROOT");

		if (!root || !mount_override)
			return 0;
		vfsi_state.mountpoint = strdup(mount_override);
		rc = b->dummy_open_mount(root, mount_override, &vfsi_state.fs);
	} else {
		if (!vfsi_find_mount(path, &mount)) {
			if (!mount_override || !export_override)
				return 0;
			mount.mountpoint = strdup(mount_override);
			mount.export_root = strdup(export_override);
			mount.host = strdup(host_override ? host_override : "127.0.0.1");
		}
		if (host_override) {
			free(mount.host);
			mount.host = strdup(host_override);
		}
		if (export_override) {
			free(mount.export_root);
			mount.export_root = strdup(export_override);
		}
		if (mount_override) {
			free(mount.mountpoint);
			mount.mountpoint = strdup(mount_override);
		}
		vfsi_state.mountpoint = strdup(mount.mountpoint);
		rc = b->nfs_open_mount_export(mount.host, mount.export_root,
					      mount.mountpoint, &vfsi_state.fs);
		vfsi_mount_clear(&mount);
	}
	if (rc) {
		rprintf(FWARNING, "vfsi: filesystem open failed: %d\n", rc);
		free(vfsi_state.mountpoint);
		vfsi_state.mountpoint = NULL;
		vfsi_state.fs = NULL;
		return 0;
	}
	return 1;
}

static struct vfsi_dir *vfsi_add_dir(const char *path)
{
	struct vfsi_dir *dir = vfsi_map_get(&vfsi_state.dirs_by_path, path);

	if (dir)
		return dir;
	dir = new0(struct vfsi_dir);
	dir->path = strdup(path);
	if (vfsi_state.dirs_nr == vfsi_state.dirs_alloc) {
		vfsi_state.dirs_alloc = vfsi_state.dirs_alloc
			? vfsi_state.dirs_alloc * 2 : 64;
		vfsi_state.dirs = realloc_array(vfsi_state.dirs,
			struct vfsi_dir *, vfsi_state.dirs_alloc);
	}
	vfsi_state.dirs[vfsi_state.dirs_nr++] = dir;
	vfsi_map_put(&vfsi_state.dirs_by_path, dir->path, dir);
	return dir;
}

static void vfsi_add_entry(const char *dir_path, const char *name,
			   const struct vfsi_attrs *attrs)
{
	struct vfsi_dir *dir = vfsi_add_dir(dir_path);
	struct vfsi_entry *entry;
	size_t dlen = strlen(dir_path);
	size_t nlen = strlen(name);

	if (dir->nr == dir->alloc) {
		dir->alloc = dir->alloc ? dir->alloc * 2 : 32;
		dir->names = realloc_array(dir->names, char *, dir->alloc);
	}
	dir->names[dir->nr++] = strdup(name);

	entry = new(struct vfsi_entry);
	entry->path = new_array(char, dlen + (dlen == 1 ? 0 : 1) + nlen + 1);
	memcpy(entry->path, dir_path, dlen);
	if (dlen != 1)
		entry->path[dlen++] = '/';
	memcpy(entry->path + dlen, name, nlen + 1);
	entry->attrs = *attrs;
	if (vfsi_state.entries_nr == vfsi_state.entries_alloc) {
		vfsi_state.entries_alloc = vfsi_state.entries_alloc
			? vfsi_state.entries_alloc * 2 : 256;
		vfsi_state.entries = realloc_array(vfsi_state.entries,
			struct vfsi_entry *, vfsi_state.entries_alloc);
	}
	vfsi_state.entries[vfsi_state.entries_nr++] = entry;
	vfsi_map_put(&vfsi_state.entries_by_path, entry->path, entry);
	if (attrs->ftype == VFSI_NF4DIR)
		vfsi_add_dir(entry->path);
}

struct vfsi_collect_state {
	int valid;
};

static bool vfsi_collect(const char *dir, const char *name,
			 const struct vfsi_attrs *attrs, void *data)
{
	struct vfsi_collect_state *state = data;

	if (!name || !*name || !strcmp(name, ".") || !strcmp(name, ".."))
		return true;
	if (!dir || !attrs || attrs->abi_version != VFSI_ABI_VERSION
	 || attrs->struct_size < sizeof(*attrs)) {
		state->valid = 0;
		return false;
	}
	vfsi_add_entry(dir, name, attrs);
	return true;
}

static int vfsi_cache_tree(const char *root)
{
	const char *dirs[1] = { root };
	struct vfsi_collect_state state = { 1 };
	int rc;

	if (!vfsi_open_for(root)) {
		vfsi_state.failed = 1;
		return 0;
	}
	vfsi_add_dir(root);
	rc = vfsi_state.bindings.listdirv(vfsi_state.fs, dirs, 1, 0, true,
					   vfsi_collect, &state);
	if (rc || !state.valid) {
		rprintf(FWARNING, "vfsi: recursive listing failed for %s: %d; using POSIX scan\n",
			root, rc);
		/* Never mix a partial vector snapshot with a POSIX traversal. */
		rsync_vfsi_reset();
		vfsi_state.failed = 1;
		return 0;
	}
	vfsi_state.active = 1;
	if (getenv("VFSI_VERBOSE"))
		rprintf(FINFO, "vfsi: cached %lu entries in %lu directories\n",
			(unsigned long)vfsi_state.entries_nr,
			(unsigned long)vfsi_state.dirs_nr);
	return 1;
}

int rsync_vfsi_listdir(const char *path, const char *const **names, size_t *count)
{
	char absolute[MAXPATHLEN];
	struct vfsi_dir *dir;

	if (vfsi_state.failed || !vfsi_eligible()
	 || !vfsi_absolute_path(path, absolute, sizeof absolute))
		return 0;
	dir = vfsi_map_get(&vfsi_state.dirs_by_path, absolute);
	if (!dir) {
		/* A command can have multiple disjoint source roots. Empty
		 * directories are pre-created by the first recursive listing, so a
		 * miss here means this is a genuinely new root. */
		if (!vfsi_cache_tree(absolute))
			return 0;
		dir = vfsi_map_get(&vfsi_state.dirs_by_path, absolute);
	}
	if (!dir)
		return 0;
	*names = (const char *const *)dir->names;
	*count = dir->nr;
	return 1;
}

static int vfsi_attrs_to_stat(const struct vfsi_attrs *attrs, STRUCT_STAT *st)
{
	if (attrs->ftype != VFSI_NF4REG && attrs->ftype != VFSI_NF4DIR)
		return 0;
	memset(st, 0, sizeof(*st));
	st->st_ino = attrs->fileid;
	st->st_mode = (attrs->mode & 07777)
		    | (attrs->ftype == VFSI_NF4DIR ? S_IFDIR : S_IFREG);
	st->st_nlink = attrs->nlink;
	st->st_uid = attrs->uid;
	st->st_gid = attrs->gid;
	st->st_size = attrs->size;
	st->st_blocks = attrs->blocks;
	st->st_atime = attrs->atime_sec;
	st->st_mtime = attrs->mtime_sec;
	st->st_ctime = attrs->ctime_sec;
#ifdef ST_ATIME_NSEC
	st->ST_ATIME_NSEC = attrs->atime_nsec;
#endif
#ifdef ST_MTIME_NSEC
	st->ST_MTIME_NSEC = attrs->mtime_nsec;
#endif
	return 1;
}

int rsync_vfsi_stat(const char *path, STRUCT_STAT *st)
{
	char absolute[MAXPATHLEN];
	struct vfsi_entry *entry;

	if (!vfsi_state.active
	 || !vfsi_absolute_path(path, absolute, sizeof absolute))
		return 0;
	entry = vfsi_map_get(&vfsi_state.entries_by_path, absolute);
	return entry ? vfsi_attrs_to_stat(&entry->attrs, st) : 0;
}

void rsync_vfsi_reset(void)
{
	size_t i, j;

	for (i = 0; i < vfsi_state.dirs_nr; i++) {
		struct vfsi_dir *dir = vfsi_state.dirs[i];

		for (j = 0; j < dir->nr; j++)
			free(dir->names[j]);
		free(dir->names);
		free(dir->path);
		free(dir);
	}
	for (i = 0; i < vfsi_state.entries_nr; i++) {
		free(vfsi_state.entries[i]->path);
		free(vfsi_state.entries[i]);
	}
	free(vfsi_state.dirs);
	free(vfsi_state.entries);
	free(vfsi_state.dirs_by_path.slots);
	free(vfsi_state.entries_by_path.slots);
	if (vfsi_state.fs && vfsi_state.bindings.free)
		vfsi_state.bindings.free(vfsi_state.fs);
	free(vfsi_state.mountpoint);
	vfsi_state.fs = NULL;
	vfsi_state.mountpoint = NULL;
	vfsi_state.dirs = NULL;
	vfsi_state.entries = NULL;
	vfsi_state.dirs_nr = vfsi_state.dirs_alloc = 0;
	vfsi_state.entries_nr = vfsi_state.entries_alloc = 0;
	memset(&vfsi_state.dirs_by_path, 0, sizeof(vfsi_state.dirs_by_path));
	memset(&vfsi_state.entries_by_path, 0, sizeof(vfsi_state.entries_by_path));
	vfsi_state.active = 0;
	vfsi_state.failed = 0;
}
