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
#include <vfsi_dynamic.h>
#include <vfsi_posix.h>

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
extern int preserve_uid;
extern int preserve_gid;
extern int preserve_atimes;
extern int module_id;
extern char curr_dir[MAXPATHLEN];

enum {
	VFSI_NF4REG = 1,
	VFSI_NF4DIR = 2,
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

/* Child cursors are optional even when building against an older ABI-v3 SDK. */
struct vfsi_dir_cursor;
struct vfsi_dir {
	char *path;
	char **names;
	struct vfsi_entry **entries;
	size_t nr, alloc;
	struct vfsi_dir_cursor *cursor;
	struct file_struct *file;
	struct vfsi_dir *prev, *next;
	int loaded;
};

struct vfsi_entry {
	struct vfsi_attrs attrs;
	struct vfsi_dir_cursor *cursor;
	char path[];
};

static struct {
	vfsi_dynamic_api bindings;
	int loader_state;
	struct vfsi_fs *fs;
	char *mountpoint;
	struct vfsi_dir *dirs, *active;
	struct hashtable *dirs_by_file;
	struct vfsi_map entries_by_path;
	size_t cached_entries, cached_path_bytes, dirs_nr;
	int failed;
} vfsi_state;

#define VFSI_FRONTIER_DIRS 24
#define VFSI_FRONTIER_ENTRIES 200000
#define VFSI_FRONTIER_BYTES (64 * 1024 * 1024)

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

/* Reuse dense tables, but release oversized ones after sparse directories.
 * This keeps reset work proportional to indexed entries, including empty dirs. */
static void vfsi_map_clear(struct vfsi_map *map)
{
	if (map->size > 64 && map->used < map->size / 4) {
		free(map->slots);
		memset(map, 0, sizeof(*map));
	} else if (map->used) {
		memset(map->slots, 0, map->size * sizeof(struct vfsi_map_slot));
		map->used = 0;
	}
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

static int vfsi_load(void)
{
	vfsi_dynamic_api candidate = { 0 };
	const char *library = getenv("VFSI_LIBRARY");

	if (vfsi_state.loader_state)
		return vfsi_state.loader_state > 0;
	candidate.handle = dlopen(library, RTLD_NOW | RTLD_LOCAL);
	if (!candidate.handle) {
		rprintf(FWARNING, "vfsi: cannot load %s: %s\n", library, dlerror());
		goto fail;
	}
	if (vfsi_dynamic_load(&candidate, candidate.handle))
		goto fail;
	if (!candidate.listdirs
#ifdef VFSI_DIRECTORY_CURSORS_VERSION
	 && !(candidate.listdirs_from_cursors && candidate.dir_cursor_free)
#endif
	)
		goto fail;

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

static void collect_mount(const char *host, const char *export_root, const char *point, void *data)
{
    struct vfsi_mount *mount = data;
    vfsi_mount_clear(mount);
    mount->host = strdup(host);
    mount->export_root = strdup(export_root);
    mount->mountpoint = strdup(point);
}

static int vfsi_find_mount(const char *path, struct vfsi_mount *result)
{
    if (!vfsi_state.bindings.nfs_mount_info ||
        vfsi_state.bindings.nfs_mount_info(path, collect_mount, result))
        return 0;
    if (!result->host || !result->export_root || !result->mountpoint) {
        vfsi_mount_clear(result);
        return 0;
    }
    return 1;
}

static int vfsi_open_for(const char *path)
{
	vfsi_dynamic_api *b = &vfsi_state.bindings;
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

		if (!b->dummy_open_mount || !root || !mount_override)
			return 0;
		vfsi_state.mountpoint = strdup(mount_override);
		rc = b->dummy_open_mount(root, mount_override, &vfsi_state.fs);
	} else {
		if (!host_override && !export_override && !mount_override) {
			if (!b->nfs_from_mount) return 0;
			rc = b->nfs_from_mount(path, &vfsi_state.fs);
			if (rc) return 0;
			vfsi_state.mountpoint = strdup(path);
			return 1;
		}
		if (!b->nfs_open_mount_export) return 0;
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


static int vfsi_has_cursors(void)
{
#ifdef VFSI_DIRECTORY_CURSORS_VERSION
	return vfsi_state.bindings.listdirs_from_cursors && vfsi_state.bindings.dir_cursor_free;
#else
	return 0;
#endif
}

static void vfsi_free_cursor(struct vfsi_dir_cursor *cursor)
{
#ifdef VFSI_DIRECTORY_CURSORS_VERSION
	if (cursor)
		vfsi_state.bindings.dir_cursor_free(cursor);
#else
	(void)cursor;
#endif
}

static struct vfsi_dir *vfsi_find_dir(struct file_struct *file)
{
	struct ht_int64_node *slot;
	if (!file || !vfsi_state.dirs_by_file)
		return NULL;
	slot = hashtable_find(vfsi_state.dirs_by_file, (int64)(uintptr_t)file, NULL);
	return slot ? slot->data : NULL;
}

static struct vfsi_dir *vfsi_add_dir(const char *path, struct file_struct *file)
{
	struct vfsi_dir *dir = new0(struct vfsi_dir);
	dir->path = strdup(path);
	dir->file = file;
	dir->next = vfsi_state.dirs;
	if (dir->next)
		dir->next->prev = dir;
	vfsi_state.dirs = dir;
	vfsi_state.dirs_nr++;
	if (file) {
		struct ht_int64_node *slot;
		if (!vfsi_state.dirs_by_file)
			vfsi_state.dirs_by_file = hashtable_create(64, 1);
		slot = hashtable_find(vfsi_state.dirs_by_file, (int64)(uintptr_t)file, dir);
		slot->data = dir;
	}
	return dir;
}

static void vfsi_discard_dir(struct vfsi_dir *dir)
{
	size_t i;
	if (vfsi_state.active == dir) {
		vfsi_state.active = NULL;
		vfsi_map_clear(&vfsi_state.entries_by_path);
	}
	for (i = 0; i < dir->nr; i++) {
		struct vfsi_entry *entry = dir->entries[i];
		vfsi_state.cached_entries--;
		vfsi_state.cached_path_bytes -= strlen(entry->path);
		vfsi_free_cursor(entry->cursor);
		free(entry);
	}
	vfsi_free_cursor(dir->cursor);
	if (dir->file && vfsi_state.dirs_by_file) {
		struct ht_int64_node *slot = hashtable_find(vfsi_state.dirs_by_file,
			(int64)(uintptr_t)dir->file, NULL);
		if (slot && slot->data == dir)
			slot->data = NULL;
	}
	if (dir->prev)
		dir->prev->next = dir->next;
	else
		vfsi_state.dirs = dir->next;
	if (dir->next)
		dir->next->prev = dir->prev;
	free(dir->names);
	free(dir->entries);
	free(dir->path);
	free(dir);
	vfsi_state.dirs_nr--;
	/* rsync's hash table retains keys whose data is cleared. Rebuild when
	 * retired records dominate, so a long walk does not index the whole tree. */
	if (vfsi_state.dirs_by_file && vfsi_state.dirs_by_file->entries > 64
	 && (size_t)vfsi_state.dirs_by_file->entries / 2 > vfsi_state.dirs_nr) {
		struct vfsi_dir *pending;
		hashtable_destroy(vfsi_state.dirs_by_file);
		vfsi_state.dirs_by_file = hashtable_create(64, 1);
		for (pending = vfsi_state.dirs; pending; pending = pending->next) {
			if (pending->file)
				hashtable_find(vfsi_state.dirs_by_file, (int64)(uintptr_t)pending->file, pending);
		}
	}
}

struct vfsi_collect_state {
	struct vfsi_dir **dirs;
	size_t count;
	int valid;
};

static bool vfsi_collect_row(size_t index, const char *path, const char *name,
			 const struct vfsi_attrs *attrs, struct vfsi_dir_cursor *child, void *data)
{
	struct vfsi_collect_state *state = data;
	struct vfsi_dir *dir;
	struct vfsi_entry *entry;
	size_t dlen, nlen, plen;
	if (!name || !*name || !strcmp(name, ".") || !strcmp(name, "..")) {
		vfsi_free_cursor(child);
		return true;
	}
	if (index >= state->count || !path || strcmp(path, state->dirs[index]->path)
	 || strchr(name, '/') || !attrs || attrs->abi_version != VFSI_ABI_VERSION
	 || attrs->struct_size < sizeof(*attrs) || (child && attrs->ftype != VFSI_NF4DIR))
		goto invalid;
	dir = state->dirs[index];
	dlen = strlen(path);
	nlen = strlen(name);
	if (dlen >= MAXPATHLEN || nlen >= MAXPATHLEN - dlen - 1)
		goto invalid;
	plen = dlen + (dlen == 1 ? 0 : 1) + nlen;
	if (vfsi_state.cached_entries >= VFSI_FRONTIER_ENTRIES
	 || plen > VFSI_FRONTIER_BYTES - vfsi_state.cached_path_bytes)
		goto invalid;
	if (dir->nr == dir->alloc) {
		dir->alloc = dir->alloc ? dir->alloc * 2 : 32;
		dir->names = realloc_array(dir->names, char *, dir->alloc);
		dir->entries = realloc_array(dir->entries, struct vfsi_entry *, dir->alloc);
	}
	/* Keep the pathname and cache record in one allocation/lifetime. */
	entry = (struct vfsi_entry *)new_array(char, sizeof(*entry) + plen + 1);
	memcpy(entry->path, path, dlen);
	if (dlen != 1)
		entry->path[dlen++] = '/';
	memcpy(entry->path + dlen, name, nlen + 1);
	entry->attrs = *attrs;
	entry->cursor = child;
	dir->names[dir->nr] = entry->path + dlen;
	dir->entries[dir->nr++] = entry;
	vfsi_state.cached_entries++;
	vfsi_state.cached_path_bytes += plen;
	return true;
invalid:
	vfsi_free_cursor(child);
	state->valid = 0;
	return false;
}

static bool vfsi_collect_indexed(size_t index, const char *dir, const char *name,
			 const struct vfsi_attrs *attrs, void *data)
{
	return vfsi_collect_row(index, dir, name, attrs, NULL, data);
}

#ifdef VFSI_DIRECTORY_CURSORS_VERSION
static bool vfsi_collect_cursor(size_t index, const char *dir, const char *name,
		const struct vfsi_attrs *attrs, struct vfsi_dir_cursor *child, void *data)
{
	return vfsi_collect_row(index, dir, name, attrs, child, data);
}
#endif

/* Publish only complete bounded cohorts. Pending sibling listings survive a
 * descent into another subtree; accepted child cursors outlive entry caches. */
static int vfsi_cache_dirs(struct vfsi_dir **dirs, size_t count)
{
	struct vfsi_collect_state state = { dirs, count, 1 };
	const char *paths[VFSI_FRONTIER_DIRS];
	struct vfsi_listing_result results[VFSI_FRONTIER_DIRS] = {{ 0 }};
	struct vfsi_listing_options options = {
		.max_entries = VFSI_FRONTIER_ENTRIES - vfsi_state.cached_entries,
		.max_path_bytes = VFSI_FRONTIER_BYTES - vfsi_state.cached_path_bytes,
		.attributes = VFSI_ATTR_MODE | VFSI_ATTR_SIZE | VFSI_ATTR_NLINK |
			VFSI_ATTR_FILEID | VFSI_ATTR_BLOCKS | VFSI_ATTR_MTIME
	};
	size_t i, entries = 0, retained = 0;
	int rc;
	if (preserve_uid) options.attributes |= VFSI_ATTR_UID;
	if (preserve_gid) options.attributes |= VFSI_ATTR_GID;
	if (preserve_atimes) options.attributes |= VFSI_ATTR_ATIME;
	for (i = 0; i < count; i++)
		paths[i] = dirs[i]->path;
#ifdef VFSI_DIRECTORY_CURSORS_VERSION
	if (vfsi_has_cursors()) {
		struct vfsi_dir_cursor *cursors[VFSI_FRONTIER_DIRS];
		for (i = 0; i < count; i++) {
			cursors[i] = dirs[i]->cursor;
			dirs[i]->cursor = NULL; /* the call array now owns the seed */
			retained += cursors[i] != NULL;
		}
		rc = vfsi_state.bindings.listdirs_from_cursors(vfsi_state.fs, paths,
			cursors, count, &options, vfsi_collect_cursor, &state, results);
		for (i = 0; i < count; i++)
			dirs[i]->cursor = cursors[i];
	} else
#endif
		rc = vfsi_state.bindings.listdirs(vfsi_state.fs, paths, count,
			&options, vfsi_collect_indexed, &state, results);
	for (i = 0; i < count && !rc; i++) {
		if (results[i].completion != 1 || results[i].delivered != dirs[i]->nr)
			rc = EIO;
		entries += dirs[i]->nr;
	}
	if (rc || !state.valid) {
		rprintf(FWARNING, "vfsi: directory cohort failed: %d; using POSIX scan\n", rc ? rc : EIO);
		rsync_vfsi_reset();
		vfsi_state.failed = 1;
		return 0;
	}
	for (i = 0; i < count; i++)
		dirs[i]->loaded = 1;
	if (getenv("VFSI_TRACE"))
		fprintf(stderr, "vfsi: listdirs completed mode=%s dirs=%zu cursors=%zu entries=%zu\n",
			vfsi_has_cursors() ? "cohort" : "paths", count, retained, entries);
	if (getenv("VFSI_VERBOSE"))
		rprintf(FINFO, "vfsi: cached %lu entries in %lu directories\n",
			(unsigned long)entries, (unsigned long)count);
	return 1;
}

int rsync_vfsi_listdir(const char *path, struct file_struct *file,
		      const char *const **names, size_t *count)
{
	char absolute[MAXPATHLEN];
	struct vfsi_dir *dir;
	size_t i;
	if (vfsi_state.failed || !vfsi_eligible()
	 || !vfsi_absolute_path(path, absolute, sizeof absolute))
		return 0;
	if (vfsi_state.active)
		rsync_vfsi_listdir_done();
	dir = vfsi_find_dir(file);
	if (dir && strcmp(dir->path, absolute)) {
		vfsi_discard_dir(dir);
		dir = NULL;
	}
	if (!dir) {
		if (!vfsi_open_for(absolute)) {
			vfsi_state.failed = 1;
			return 0;
		}
		dir = vfsi_add_dir(absolute, file);
	}
	if (!dir->loaded && !vfsi_cache_dirs(&dir, 1))
		return 0;
	vfsi_state.active = dir;
	for (i = 0; i < dir->nr; i++)
		vfsi_map_put(&vfsi_state.entries_by_path, dir->entries[i]->path, dir->entries[i]);
	*names = (const char *const *)dir->names;
	*count = dir->nr;
	return 1;
}

void rsync_vfsi_listdir_done(void)
{
	if (vfsi_state.active)
		vfsi_discard_dir(vfsi_state.active);
}

/* Called only after send_file_name() has admitted the directory through its
 * normal filters. Excluded seeds remain in the active listing and are freed. */
void rsync_vfsi_remember_dir(struct file_struct *file, const char *path)
{
	char absolute[MAXPATHLEN];
	struct vfsi_entry *entry;
	struct vfsi_dir *dir;
	if (!vfsi_has_cursors() || !file || !S_ISDIR(file->mode) || !vfsi_state.active
	 || vfsi_find_dir(file) || !vfsi_absolute_path(path, absolute, sizeof absolute))
		return;
	entry = vfsi_map_get(&vfsi_state.entries_by_path, absolute);
	if (!entry || !entry->cursor)
		return;
	dir = vfsi_add_dir(absolute, file);
	dir->cursor = entry->cursor;
	entry->cursor = NULL;
}

void rsync_vfsi_prepare_dirs(struct file_struct *const *files, size_t count)
{
	struct vfsi_dir *dirs[VFSI_FRONTIER_DIRS], *first;
	size_t i, nr = 0;
	if (!count || vfsi_state.failed || !vfsi_has_cursors() || !vfsi_state.fs)
		return;
	first = vfsi_find_dir(files[0]);
	if (!first || first->loaded)
		return;
	/* Keep a reserve for the directory rsync is about to consume. Cold sibling
	 * snapshots share one global quota, rather than multiplying by tree depth. */
	if (vfsi_state.cached_entries >= VFSI_FRONTIER_ENTRIES / 2
	 || vfsi_state.cached_path_bytes >= VFSI_FRONTIER_BYTES / 2)
		return;
	for (i = 0; i < count && i < 256 && nr < VFSI_FRONTIER_DIRS; i++) {
		struct vfsi_dir *dir;
		if (!S_ISDIR(files[i]->mode) || files[i]->flags & FLAG_MOUNT_DIR)
			continue;
		dir = vfsi_find_dir(files[i]);
		if (dir && !dir->loaded) {
			size_t j;
			for (j = 0; j < nr && dirs[j] != dir; j++) {}
			if (j == nr)
				dirs[nr++] = dir;
		}
	}
	if (nr)
		vfsi_cache_dirs(dirs, nr);
}

/* Skip look-ahead entirely for inactive/legacy scans and already loaded cohorts.
 * Bound inspected links as well as admitted operands for sparse sender frontiers. */
void rsync_vfsi_prepare_siblings(struct file_struct *const *files, int next)
{
	struct file_struct *cohort[VFSI_FRONTIER_DIRS];
	struct vfsi_dir *first;
	size_t nr = 0, inspected = 0;
	if (next < 0 || vfsi_state.failed || !vfsi_has_cursors() || !vfsi_state.fs)
		return;
	first = vfsi_find_dir(files[next]);
	if (!first || first->loaded)
		return;
	while (next >= 0 && nr < VFSI_FRONTIER_DIRS && inspected++ < 256) {
		struct file_struct *candidate = files[next];
		if (candidate->flags & FLAG_CONTENT_DIR)
			cohort[nr++] = candidate;
		next = DIR_NEXT_SIBLING(F_DIR_NODE_P(candidate));
	}
	rsync_vfsi_prepare_dirs(cohort, nr);
}

static int vfsi_attrs_to_stat(const struct vfsi_attrs *attrs, STRUCT_STAT *st)
{
#if !defined(USE_STAT64_FUNCS)
    /* These modes still use kernel metadata for hard links and mount identity. */
    if (!attrs || (attrs->ftype != VFSI_NF4REG && attrs->ftype != VFSI_NF4DIR))
        return 0;
    return vfsi_attrs_stat_fields(attrs, st) == 0;
#else
	if ((attrs->ftype != VFSI_NF4REG && attrs->ftype != VFSI_NF4DIR) ||
	    attrs->size > INT64_MAX || attrs->blocks > INT64_MAX ||
	    attrs->mtime_nsec >= 1000000000 || attrs->atime_nsec >= 1000000000)
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
	if ((uint64_t)st->st_size != attrs->size || (uint64_t)st->st_blocks != attrs->blocks ||
	    (uint64_t)st->st_ino != attrs->fileid || (int64_t)st->st_mtime != attrs->mtime_sec ||
	    (int64_t)st->st_atime != attrs->atime_sec)
		return 0;
#ifdef ST_ATIME_NSEC
	st->ST_ATIME_NSEC = attrs->atime_nsec;
#endif
#ifdef ST_MTIME_NSEC
	st->ST_MTIME_NSEC = attrs->mtime_nsec;
#endif
	return 1;
#endif
}


int rsync_vfsi_stat(const char *path, STRUCT_STAT *st)
{
	char absolute[MAXPATHLEN];
	struct vfsi_entry *entry;
	if (!vfsi_state.active || !vfsi_absolute_path(path, absolute, sizeof absolute))
		return 0;
	entry = vfsi_map_get(&vfsi_state.entries_by_path, absolute);
	return entry ? vfsi_attrs_to_stat(&entry->attrs, st) : 0;
}

void rsync_vfsi_reset(void)
{
	if (vfsi_state.dirs_by_file)
		hashtable_destroy(vfsi_state.dirs_by_file);
	vfsi_state.dirs_by_file = NULL;
	while (vfsi_state.dirs)
		vfsi_discard_dir(vfsi_state.dirs);
	free(vfsi_state.entries_by_path.slots);
	memset(&vfsi_state.entries_by_path, 0, sizeof(vfsi_state.entries_by_path));
	if (vfsi_state.fs && vfsi_state.bindings.free)
		vfsi_state.bindings.free(vfsi_state.fs);
	free(vfsi_state.mountpoint);
	vfsi_state.fs = NULL;
	vfsi_state.mountpoint = NULL;
	vfsi_state.failed = 0;
}
