/*
 * libkmod - interface to kernel module operations
 *
 * Copyright (C) 2011  ProFUSION embedded systems
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <ctype.h>
#include <inttypes.h>
#include <limits.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <string.h>

#include "libkmod.h"
#include "libkmod-private.h"

/**
 * kmod_module:
 *
 * Opaque object representing a module.
 */
struct kmod_module {
	struct kmod_ctx *ctx;
	char *name;
	char *path;
	struct kmod_list *dep;
	char *options;
	char *install_commands;
	char *remove_commands;
	char *alias; /* only set if this module was created from an alias */
	int n_dep;
	int refcount;
	struct {
		bool dep : 1;
		bool options : 1;
		bool install_commands : 1;
		bool remove_commands : 1;
	} init;
};

inline char *modname_normalize(const char *modname, char buf[NAME_MAX],
								size_t *len)
{
	size_t s;

	for (s = 0; s < NAME_MAX - 1; s++) {
		const char c = modname[s];
		if (c == '-')
			buf[s] = '_';
		else if (c == '\0' || c == '.')
			break;
		else
			buf[s] = c;
	}

	buf[s] = '\0';

	if (len)
		*len = s;

	return buf;
}

static char *path_to_modname(const char *path, char buf[NAME_MAX], size_t *len)
{
	char *modname;

	modname = basename(path);
	if (modname == NULL || modname[0] == '\0')
		return NULL;

	return modname_normalize(modname, buf, len);
}

static inline const char *path_join(const char *path, size_t prefixlen,
							char buf[PATH_MAX])
{
	size_t pathlen;

	if (path[0] == '/')
		return path;

	pathlen = strlen(path);
	if (prefixlen + pathlen + 1 >= PATH_MAX)
		return NULL;

	memcpy(buf + prefixlen, path, pathlen + 1);
	return buf;
}

int kmod_module_parse_depline(struct kmod_module *mod, char *line)
{
	struct kmod_ctx *ctx = mod->ctx;
	struct kmod_list *list = NULL;
	const char *dirname;
	char buf[PATH_MAX];
	char *p, *saveptr;
	int err = 0, n = 0;
	size_t dirnamelen;

	if (mod->init.dep)
		return mod->n_dep;
	assert(mod->dep == NULL);
	mod->init.dep = true;

	p = strchr(line, ':');
	if (p == NULL)
		return 0;

	*p = '\0';
	dirname = kmod_get_dirname(mod->ctx);
	dirnamelen = strlen(dirname);
	if (dirnamelen + 2 >= PATH_MAX)
		return 0;

	memcpy(buf, dirname, dirnamelen);
	buf[dirnamelen] = '/';
	dirnamelen++;
	buf[dirnamelen] = '\0';

	if (mod->path == NULL) {
		const char *str = path_join(line, dirnamelen, buf);
		if (str == NULL)
			return 0;
		mod->path = strdup(str);
		if (mod->path == NULL)
			return 0;
	}

	p++;
	for (p = strtok_r(p, " \t", &saveptr); p != NULL;
					p = strtok_r(NULL, " \t", &saveptr)) {
		struct kmod_module *depmod;
		const char *path;

		path = path_join(p, dirnamelen, buf);
		if (path == NULL) {
			ERR(ctx, "could not join path '%s' and '%s'.\n",
			    dirname, p);
			goto fail;
		}

		err = kmod_module_new_from_path(ctx, path, &depmod);
		if (err < 0) {
			ERR(ctx, "ctx=%p path=%s error=%s\n",
						ctx, path, strerror(-err));
			goto fail;
		}

		DBG(ctx, "add dep: %s\n", path);

		list = kmod_list_append(list, depmod);
		n++;
	}

	DBG(ctx, "%d dependencies for %s\n", n, mod->name);

	mod->dep = list;
	mod->n_dep = n;
	return n;

fail:
	kmod_module_unref_list(list);
	mod->init.dep = false;
	return err;
}

/**
 * kmod_module_new_from_name:
 * @ctx: kmod library context
 * @name: name of the module
 * @mod: where to save the created struct kmod_module
 *
 * Create a new struct kmod_module using the module name. @name can not be an
 * alias, file name or anything else; it must be a module name. There's no
 * check if the module does exists in the system.
 *
 * This function is also used internally by many others that return a new
 * struct kmod_module or a new list of modules.
 *
 * The initial refcount is 1, and needs to be decremented to release the
 * resources of the kmod_module. Since libkmod keeps track of all
 * kmod_modules created, they are all released upon @ctx destruction too. Do
 * not unref @ctx before all the desired operations with the returned
 * kmod_module are done.
 *
 * Returns: 0 on success or < 0 otherwise. It fails if name is not a valid
 * module name or if memory allocation failed.
 */
KMOD_EXPORT int kmod_module_new_from_name(struct kmod_ctx *ctx,
						const char *name,
						struct kmod_module **mod)
{
	struct kmod_module *m;
	size_t namelen;
	char name_norm[NAME_MAX];

	if (ctx == NULL || name == NULL)
		return -ENOENT;

	alias_normalize(name, name_norm, &namelen);

	m = kmod_pool_get_module(ctx, name_norm);
	if (m != NULL) {
		*mod = kmod_module_ref(m);
		return 0;
	}

	m = malloc(sizeof(*m) + namelen + 1);
	if (m == NULL) {
		free(m);
		return -ENOMEM;
	}

	memset(m, 0, sizeof(*m));

	m->ctx = kmod_ref(ctx);
	m->name = (char *)m + sizeof(*m);
	memcpy(m->name, name_norm, namelen + 1);
	m->refcount = 1;

	kmod_pool_add_module(ctx, m);

	*mod = m;

	return 0;
}

int kmod_module_new_from_alias(struct kmod_ctx *ctx, const char *alias,
				const char *name, struct kmod_module **mod)
{
	int err;
	struct kmod_module *m;

	err = kmod_module_new_from_name(ctx, alias, mod);
	if (err < 0)
		return err;

	m = *mod;

	/* if module did not came from pool */
	if (m->alias == NULL) {
		m->alias = m->name;
		m->name = strdup(name);
		if (m->name == NULL)
			goto fail_oom;
	}

	return 0;

fail_oom:
	ERR(ctx, "out of memory\n");
	kmod_module_unref(m);
	*mod = NULL;
	return err;
}

/**
 * kmod_module_new_from_path:
 * @ctx: kmod library context
 * @path: path where to find the given module
 * @mod: where to save the created struct kmod_module
 *
 * Create a new struct kmod_module using the module path. @path must be an
 * existent file with in the filesystem and must be accessible to libkmod.
 *
 * The initial refcount is 1, and needs to be decremented to release the
 * resources of the kmod_module. Since libkmod keeps track of all
 * kmod_modules created, they are all released upon @ctx destruction too. Do
 * not unref @ctx before all the desired operations with the returned
 * kmod_module are done.
 *
 * If @path is relative, it's treated as relative to the current working
 * directory. Otherwise, give an absolute path.
 *
 * Returns: 0 on success or < 0 otherwise. It fails if file does not exist, if
 * it's not a valid file for a kmod_module or if memory allocation failed.
 */
KMOD_EXPORT int kmod_module_new_from_path(struct kmod_ctx *ctx,
						const char *path,
						struct kmod_module **mod)
{
	struct kmod_module *m;
	int err;
	struct stat st;
	char name[NAME_MAX];
	char *abspath;
	size_t namelen;

	if (ctx == NULL || path == NULL)
		return -ENOENT;

	abspath = path_make_absolute_cwd(path);
	if (abspath == NULL)
		return -ENOMEM;

	err = stat(abspath, &st);
	if (err < 0) {
		free(abspath);
		return -errno;
	}

	if (path_to_modname(path, name, &namelen) == NULL) {
		free(abspath);
		return -ENOENT;
	}

	m = kmod_pool_get_module(ctx, name);
	if (m != NULL) {
		if (m->path == NULL)
			m->path = abspath;
		else if (streq(m->path, abspath))
			free(abspath);
		else {
			ERR(ctx, "kmod_module '%s' already exists with different path\n",
									name);
			free(abspath);
			return -EEXIST;
		}

		*mod = kmod_module_ref(m);
		return 0;
	}

	m = malloc(sizeof(*m) + namelen + 1);
	if (m == NULL)
		return -errno;

	memset(m, 0, sizeof(*m));

	m->ctx = kmod_ref(ctx);
	m->name = (char *)m + sizeof(*m);
	memcpy(m->name, name, namelen);
	m->path = abspath;
	m->refcount = 1;

	kmod_pool_add_module(ctx, m);

	*mod = m;

	return 0;
}

/**
 * kmod_module_unref:
 * @mod: kmod module
 *
 * Drop a reference of the kmod module. If the refcount reaches zero, its
 * resources are released.
 *
 * Returns: NULL if @mod is NULL or if the module was released. Otherwise it
 * returns the passed @mod with its refcount decremented.
 */
KMOD_EXPORT struct kmod_module *kmod_module_unref(struct kmod_module *mod)
{
	if (mod == NULL)
		return NULL;

	if (--mod->refcount > 0)
		return mod;

	DBG(mod->ctx, "kmod_module %p released\n", mod);

	kmod_module_unref_list(mod->dep);
	kmod_unref(mod->ctx);
	free(mod->options);
	free(mod->install_commands);
	free(mod->remove_commands);
	free(mod->path);
	if (mod->alias != NULL)
		free(mod->name);
	free(mod);
	return NULL;
}

/**
 * kmod_module_ref:
 * @mod: kmod module
 *
 * Take a reference of the kmod module, incrementing its refcount.
 *
 * Returns: the passed @module with its refcount incremented.
 */
KMOD_EXPORT struct kmod_module *kmod_module_ref(struct kmod_module *mod)
{
	if (mod == NULL)
		return NULL;

	mod->refcount++;

	return mod;
}

#define CHECK_ERR_AND_FINISH(_err, _label_err, _list, label_finish)	\
	do {								\
		if ((_err) < 0)						\
			goto _label_err;				\
		if (*(_list) != NULL)					\
			goto finish;					\
	} while (0)

/**
 * kmod_module_new_from_lookup:
 * @ctx: kmod library context
 * @given_alias: alias to look for
 * @list: an empty list where to save the list of modules matching
 * @given_alias
 *
 * Create a new list of kmod modules using an alias or module name and lookup
 * libkmod's configuration files and indexes in order to find the module.
 * Once it's found in one of the places, it stops searching and create the
 * list of modules that is saved in @list.
 *
 * The search order is: 1. aliases in configuration file; 2. module names in
 * modules.dep index; 3. symbol aliases in modules.symbols index; 4. aliases
 * in modules.alias index.
 *
 * The initial refcount is 1, and needs to be decremented to release the
 * resources of the kmod_module. The returned @list must be released by
 * calling kmod_module_unref_list(). Since libkmod keeps track of all
 * kmod_modules created, they are all released upon @ctx destruction too. Do
 * not unref @ctx before all the desired operations with the returned list are
 * completed.
 *
 * Returns: 0 on success or < 0 otherwise. It fails if any of the lookup
 * methods failed, which is basically due to memory allocation fail. If module
 * is not found, it still returns 0, but @list is an empty list.
 */
KMOD_EXPORT int kmod_module_new_from_lookup(struct kmod_ctx *ctx,
						const char *given_alias,
						struct kmod_list **list)
{
	int err;
	char alias[NAME_MAX];

	if (ctx == NULL || given_alias == NULL)
		return -ENOENT;

	if (list == NULL || *list != NULL) {
		ERR(ctx, "An empty list is needed to create lookup\n");
		return -ENOSYS;
	}

	if (alias_normalize(given_alias, alias, NULL) < 0)
		return -EINVAL;

	/* Aliases from config file override all the others */
	err = kmod_lookup_alias_from_config(ctx, alias, list);
	CHECK_ERR_AND_FINISH(err, fail, list, finish);

	err = kmod_lookup_alias_from_moddep_file(ctx, alias, list);
	CHECK_ERR_AND_FINISH(err, fail, list, finish);

	err = kmod_lookup_alias_from_symbols_file(ctx, alias, list);
	CHECK_ERR_AND_FINISH(err, fail, list, finish);

// TODO: add lookup for install commands here.

	err = kmod_lookup_alias_from_aliases_file(ctx, alias, list);
	CHECK_ERR_AND_FINISH(err, fail, list, finish);

finish:

	return err;
fail:
	kmod_module_unref_list(*list);
	*list = NULL;
	return err;
}
#undef CHECK_ERR_AND_FINISH

/**
 * kmod_module_unref:
 * @list: list of kmod modules
 *
 * Drop a reference of each kmod module in @list and releases the resources
 * taken by the list itself.
 *
 * Returns: NULL if @mod is NULL or if the module was released. Otherwise it
 * returns the passed @mod with its refcount decremented.
 */
KMOD_EXPORT int kmod_module_unref_list(struct kmod_list *list)
{
	for (; list != NULL; list = kmod_list_remove(list))
		kmod_module_unref(list->data);

	return 0;
}

/**
 * kmod_module_unref:
 * @mod: kmod module
 *
 * Search the modules.dep index to find the dependencies of the given @mod.
 * The result is cached in @mod, so subsequent calls to this function will
 * return the already searched list of modules.
 *
 * Returns: NULL on failure or if there are any dependencies. Otherwise it
 * returns a list of kmod modules that can be released by calling
 * kmod_module_unref_list().
 */
KMOD_EXPORT struct kmod_list *kmod_module_get_dependencies(const struct kmod_module *mod)
{
	struct kmod_list *l, *l_new, *list_new = NULL;

	if (mod == NULL)
		return NULL;

	if (!mod->init.dep) {
		/* lazy init */
		char *line = kmod_search_moddep(mod->ctx, mod->name);

		if (line == NULL)
			return NULL;

		kmod_module_parse_depline((struct kmod_module *)mod, line);
		free(line);

		if (!mod->init.dep)
			return NULL;
	}

	kmod_list_foreach(l, mod->dep) {
		l_new = kmod_list_append(list_new, kmod_module_ref(l->data));
		if (l_new == NULL) {
			kmod_module_unref(l->data);
			goto fail;
		}

		list_new = l_new;
	}

	return list_new;

fail:
	ERR(mod->ctx, "out of memory\n");
	kmod_module_unref_list(list_new);
	return NULL;
}

/**
 * kmod_module_get_module:
 * @entry: an entry in a list of kmod modules.
 *
 * Get the kmod module of this @entry in the list, increasing its refcount.
 * After it's used, unref it. Since the refcount is incremented upon return,
 * you still have to call kmod_module_unref_list() to release the list of kmod
 * modules.
 *
 * Returns: NULL on failure or the kmod_module contained in this list entry
 * with its refcount incremented.
 */
KMOD_EXPORT struct kmod_module *kmod_module_get_module(const struct kmod_list *entry)
{
	if (entry == NULL)
		return NULL;

	return kmod_module_ref(entry->data);
}

/**
 * kmod_module_get_name:
 * @mod: kmod module
 *
 * Get the name of this kmod module. Name is always available, independently
 * if it was created by kmod_module_new_from_name() or another function and
 * it's always normalized (dashes are replaced with underscores).
 *
 * Returns: the name of this kmod module.
 */
KMOD_EXPORT const char *kmod_module_get_name(const struct kmod_module *mod)
{
	return mod->name;
}

/**
 * kmod_module_get_path:
 * @mod: kmod module
 *
 * Get the path of this kmod module. If this kmod module was not created by
 * path, it can search the modules.dep index in order to find out the module
 * under context's dirname (see kmod_get_dirname()).
 *
 * Returns: the path of this kmod module or NULL if such information is not
 * available.
 */
KMOD_EXPORT const char *kmod_module_get_path(const struct kmod_module *mod)
{
	char *line;

	DBG(mod->ctx, "name='%s' path='%s'\n", mod->name, mod->path);

	if (mod->path != NULL)
		return mod->path;
	if (mod->init.dep)
		return NULL;

	/* lazy init */
	line = kmod_search_moddep(mod->ctx, mod->name);
	if (line == NULL)
		return NULL;

	kmod_module_parse_depline((struct kmod_module *) mod, line);
	free(line);

	return mod->path;
}


extern long delete_module(const char *name, unsigned int flags);

/**
 * kmod_module_remove_module:
 * @mod: kmod module
 * @flags: flags to pass to Linux kernel when removing the module
 *
 * Remove a module from Linux kernel.
 *
 * Returns: 0 on success or < 0 on failure.
 */
KMOD_EXPORT int kmod_module_remove_module(struct kmod_module *mod,
							unsigned int flags)
{
	int err;

	if (mod == NULL)
		return -ENOENT;

	/* Filter out other flags */
	flags &= (KMOD_REMOVE_FORCE | KMOD_REMOVE_NOWAIT);

	err = delete_module(mod->name, flags);
	if (err != 0) {
		ERR(mod->ctx, "Could not remove '%s': %s\n", mod->name,
							strerror(-err));
		return err;
	}

	return 0;
}

extern long init_module(void *mem, unsigned long len, const char *args);

/**
 * kmod_module_insert_module:
 * @mod: kmod module
 * @flags: flags are not passed to Linux Kernel, but instead it dictates the
 * behavior of this function. They are not implemented yet.
 * @options: module's options to pass to Linux Kernel.
 *
 * Insert a module in Linux kernel. It opens the file pointed by @mod,
 * mmap'ing it and passing to kernel.
 *
 * Returns: 0 on success or < 0 on failure.
 */
KMOD_EXPORT int kmod_module_insert_module(struct kmod_module *mod,
							unsigned int flags,
							const char *options)
{
	int err;
	void *mmaped_file;
	struct stat st;
	int fd;
	const char *args = options ? options : "";

	if (mod == NULL)
		return -ENOENT;

	if (mod->path == NULL) {
		ERR(mod->ctx, "Not supported to load a module by name yet\n");
		return -ENOSYS;
	}

	if (flags != 0)
		INFO(mod->ctx, "Flags are not implemented yet\n");

	if ((fd = open(mod->path, O_RDONLY)) < 0) {
		err = -errno;
		return err;
	}

	fstat(fd, &st);

	if ((mmaped_file = mmap(0, st.st_size, PROT_READ,
					MAP_PRIVATE, fd, 0)) == MAP_FAILED) {
		close(fd);
		return -errno;
	}

	err = init_module(mmaped_file, st.st_size, args);
	if (err < 0)
		ERR(mod->ctx, "Failed to insert module '%s'\n", mod->path);

	munmap(mmaped_file, st.st_size);
	close(fd);

	return err;
}

/**
 * kmod_module_get_options:
 * @mod: kmod module
 *
 * Get options of this kmod module. Options come from the configuration file
 * and are cached in @mod. The first call to this function will search for
 * this module in configuration and subsequent calls return the cached string.
 *
 * Returns: a string with all the options separated by spaces. This string is
 * owned by @mod, do not free it.
 */
KMOD_EXPORT const char *kmod_module_get_options(const struct kmod_module *mod)
{
	if (mod == NULL)
		return NULL;

	if (!mod->init.options) {
		/* lazy init */
		struct kmod_module *m = (struct kmod_module *)mod;
		const struct kmod_list *l, *ctx_options;
		char *opts = NULL;
		size_t optslen = 0;

		ctx_options = kmod_get_options(mod->ctx);

		kmod_list_foreach(l, ctx_options) {
			const char *modname = kmod_option_get_modname(l);
			const char *str;
			size_t len;
			void *tmp;

			DBG(mod->ctx, "modname=%s mod->name=%s mod->alias=%s\n", modname, mod->name, mod->alias);
			if (!(streq(modname, mod->name) || (mod->alias != NULL &&
						streq(modname, mod->alias))))
				continue;

			DBG(mod->ctx, "passed = modname=%s mod->name=%s mod->alias=%s\n", modname, mod->name, mod->alias);
			str = kmod_option_get_options(l);
			len = strlen(str);
			if (len < 1)
				continue;

			tmp = realloc(opts, optslen + len + 2);
			if (tmp == NULL) {
				free(opts);
				goto failed;
			}

			opts = tmp;

			if (optslen > 0) {
				opts[optslen] = ' ';
				optslen++;
			}

			memcpy(opts + optslen, str, len);
			optslen += len;
			opts[optslen] = '\0';
		}

		m->init.options = true;
		m->options = opts;
	}

	return mod->options;

failed:
	ERR(mod->ctx, "out of memory\n");
	return NULL;
}

/**
 * kmod_module_get_install_commands:
 * @mod: kmod module
 *
 * Get install commands for this kmod module. Install commands come from the
 * configuration file and are cached in @mod. The first call to this function
 * will search for this module in configuration and subsequent calls return
 * the cached string. The install commands are returned as they were in the
 * configuration, concatenated by ';'. No other processing is made in this
 * string.
 *
 * Returns: a string with all install commands separated by semicolons. This
 * string is owned by @mod, do not free it.
 */
KMOD_EXPORT const char *kmod_module_get_install_commands(const struct kmod_module *mod)
{
	if (mod == NULL)
		return NULL;

	if (!mod->init.install_commands) {
		/* lazy init */
		struct kmod_module *m = (struct kmod_module *)mod;
		const struct kmod_list *l, *ctx_install_commands;
		char *cmds = NULL;
		size_t cmdslen = 0;

		ctx_install_commands = kmod_get_install_commands(mod->ctx);

		kmod_list_foreach(l, ctx_install_commands) {
			const char *modname = kmod_command_get_modname(l);
			const char *str;
			size_t len;
			void *tmp;

			if (strcmp(modname, mod->name) != 0)
				continue;

			str = kmod_command_get_command(l);
			len = strlen(str);
			if (len < 1)
				continue;

			tmp = realloc(cmds, cmdslen + len + 2);
			if (tmp == NULL) {
				free(cmds);
				goto failed;
			}

			cmds = tmp;

			if (cmdslen > 0) {
				cmds[cmdslen] = ';';
				cmdslen++;
			}

			memcpy(cmds + cmdslen, str, len);
			cmdslen += len;
			cmds[cmdslen] = '\0';
		}

		m->init.install_commands = true;
		m->install_commands = cmds;
	}

	return mod->install_commands;

failed:
	ERR(mod->ctx, "out of memory\n");
	return NULL;
}

/**
 * kmod_module_get_remove_commands:
 * @mod: kmod module
 *
 * Get remove commands for this kmod module. Remove commands come from the
 * configuration file and are cached in @mod. The first call to this function
 * will search for this module in configuration and subsequent calls return
 * the cached string. The remove commands are returned as they were in the
 * configuration, concatenated by ';'. No other processing is made in this
 * string.
 *
 * Returns: a string with all remove commands separated by semicolons. This
 * string is owned by @mod, do not free it.
 */
KMOD_EXPORT const char *kmod_module_get_remove_commands(const struct kmod_module *mod)
{
	if (mod == NULL)
		return NULL;

	if (!mod->init.remove_commands) {
		/* lazy init */
		struct kmod_module *m = (struct kmod_module *)mod;
		const struct kmod_list *l, *ctx_remove_commands;
		char *cmds = NULL;
		size_t cmdslen = 0;

		ctx_remove_commands = kmod_get_remove_commands(mod->ctx);

		kmod_list_foreach(l, ctx_remove_commands) {
			const char *modname = kmod_command_get_modname(l);
			const char *str;
			size_t len;
			void *tmp;

			if (strcmp(modname, mod->name) != 0)
				continue;

			str = kmod_command_get_command(l);
			len = strlen(str);
			if (len < 1)
				continue;

			tmp = realloc(cmds, cmdslen + len + 2);
			if (tmp == NULL) {
				free(cmds);
				goto failed;
			}

			cmds = tmp;

			if (cmdslen > 0) {
				cmds[cmdslen] = ';';
				cmdslen++;
			}

			memcpy(cmds + cmdslen, str, len);
			cmdslen += len;
			cmds[cmdslen] = '\0';
		}

		m->init.remove_commands = true;
		m->remove_commands = cmds;
	}

	return mod->remove_commands;

failed:
	ERR(mod->ctx, "out of memory\n");
	return NULL;
}

/**
 * SECTION:libkmod-loaded
 * @short_description: currently loaded modules
 *
 * Information about currently loaded modules, as reported by Linux kernel.
 * These information are not cached by libkmod and are always read from /sys
 * and /proc/modules.
 */

/**
 * kmod_module_new_from_loaded:
 * @ctx: kmod library context
 * @list: where to save the list of loaded modules
 *
 * Create a new list of kmod modules with all modules currently loaded in
 * kernel. It uses /proc/modules to get the names of loaded modules and to
 * create kmod modules by calling kmod_module_new_from_name() in each of them.
 * They are put are put in @list in no particular order.
 *
 * The initial refcount is 1, and needs to be decremented to release the
 * resources of the kmod_module. The returned @list must be released by
 * calling kmod_module_unref_list(). Since libkmod keeps track of all
 * kmod_modules created, they are all released upon @ctx destruction too. Do
 * not unref @ctx before all the desired operations with the returned list are
 * completed.
 *
 * Returns: 0 on success or < 0 on error.
 */
KMOD_EXPORT int kmod_module_new_from_loaded(struct kmod_ctx *ctx,
						struct kmod_list **list)
{
	struct kmod_list *l = NULL;
	FILE *fp;
	char line[4096];

	if (ctx == NULL || list == NULL)
		return -ENOENT;

	fp = fopen("/proc/modules", "r");
	if (fp == NULL) {
		int err = -errno;
		ERR(ctx, "could not open /proc/modules: %s\n", strerror(errno));
		return err;
	}

	while (fgets(line, sizeof(line), fp)) {
		struct kmod_module *m;
		struct kmod_list *node;
		int err;
		char *saveptr, *name = strtok_r(line, " \t", &saveptr);

		err = kmod_module_new_from_name(ctx, name, &m);
		if (err < 0) {
			ERR(ctx, "could not get module from name '%s': %s\n",
				name, strerror(-err));
			continue;
		}

		node = kmod_list_append(l, m);
		if (node)
			l = node;
		else {
			ERR(ctx, "out of memory\n");
			kmod_module_unref(m);
		}
	}

	fclose(fp);
	*list = l;

	return 0;
}

/**
 * kmod_module_initstate_str:
 * @state: the state as returned by kmod_module_get_initstate()
 *
 * Translate a initstate to a string.
 *
 * Returns: the string associated to the @state. This string is statically
 * allocated, do not free it.
 */
KMOD_EXPORT const char *kmod_module_initstate_str(enum kmod_module_initstate state)
{
    switch (state) {
    case KMOD_MODULE_BUILTIN:
	return "builtin";
    case KMOD_MODULE_LIVE:
	return "live";
    case KMOD_MODULE_COMING:
	return "coming";
    case KMOD_MODULE_GOING:
	return "going";
    default:
	return NULL;
    }
}

/**
 * kmod_module_get_initstate:
 * @mod: kmod module
 *
 * Get the initstate of this @mod, as returned by Linux Kernel, by reading
 * /sys filesystem.
 *
 * Returns: < 0 on error or enum kmod_initstate if module is found in kernel.
 */
KMOD_EXPORT int kmod_module_get_initstate(const struct kmod_module *mod)
{
	char path[PATH_MAX], buf[32];
	int fd, err, pathlen;

	pathlen = snprintf(path, sizeof(path),
				"/sys/module/%s/initstate", mod->name);
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		err = -errno;

		if (pathlen > (int)sizeof("/initstate") - 1) {
			struct stat st;
			path[pathlen - (sizeof("/initstate") - 1)] = '\0';
			if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
				return KMOD_MODULE_BUILTIN;
		}

		DBG(mod->ctx, "could not open '%s': %s\n",
			path, strerror(-err));
		return err;
	}

	err = read_str_safe(fd, buf, sizeof(buf));
	close(fd);
	if (err < 0) {
		ERR(mod->ctx, "could not read from '%s': %s\n",
			path, strerror(-err));
		return err;
	}

	if (streq(buf, "live\n"))
		return KMOD_MODULE_LIVE;
	else if (streq(buf, "coming\n"))
		return KMOD_MODULE_COMING;
	else if (streq(buf, "going\n"))
		return KMOD_MODULE_GOING;

	ERR(mod->ctx, "unknown %s: '%s'\n", path, buf);
	return -EINVAL;
}

/**
 * kmod_module_get_size:
 * @mod: kmod module
 *
 * Get the size of this kmod module as returned by Linux kernel. It reads the
 * file /proc/modules to search for this module and get its size.
 *
 * Returns: the size of this kmod module.
 */
KMOD_EXPORT long kmod_module_get_size(const struct kmod_module *mod)
{
	// FIXME TODO: this should be available from /sys/module/foo
	FILE *fp;
	char line[4096];
	int lineno = 0;
	long size = -ENOENT;

	if (mod == NULL)
		return -ENOENT;

	fp = fopen("/proc/modules", "r");
	if (fp == NULL) {
		int err = -errno;
		ERR(mod->ctx,
		    "could not open /proc/modules: %s\n", strerror(errno));
		return err;
	}

	while (fgets(line, sizeof(line), fp)) {
		char *saveptr, *endptr, *tok = strtok_r(line, " \t", &saveptr);
		long value;

		lineno++;
		if (tok == NULL || !streq(tok, mod->name))
			continue;

		tok = strtok_r(NULL, " \t", &saveptr);
		if (tok == NULL) {
			ERR(mod->ctx,
			"invalid line format at /proc/modules:%d\n", lineno);
			break;
		}

		value = strtol(tok, &endptr, 10);
		if (endptr == tok || *endptr != '\0') {
			ERR(mod->ctx,
			"invalid line format at /proc/modules:%d\n", lineno);
			break;
		}

		size = value;
		break;
	}
	fclose(fp);
	return size;
}

/**
 * kmod_module_get_refcnt:
 * @mod: kmod module
 *
 * Get the ref count of this @mod, as returned by Linux Kernel, by reading
 * /sys filesystem.
 *
 * Returns: 0 on success or < 0 on failure.
 */
KMOD_EXPORT int kmod_module_get_refcnt(const struct kmod_module *mod)
{
	char path[PATH_MAX];
	long refcnt;
	int fd, err;

	snprintf(path, sizeof(path), "/sys/module/%s/refcnt", mod->name);
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		err = -errno;
		ERR(mod->ctx, "could not open '%s': %s\n",
			path, strerror(errno));
		return err;
	}

	err = read_str_long(fd, &refcnt, 10);
	close(fd);
	if (err < 0) {
		ERR(mod->ctx, "could not read integer from '%s': '%s'\n",
			path, strerror(-err));
		return err;
	}

	return (int)refcnt;
}

/**
 * kmod_module_get_holders:
 * @mod: kmod module
 *
 * Get a list of kmod modules that are holding this @mod, as returned by Linux
 * Kernel. After use, free the @list by calling kmod_module_unref_list().
 *
 * Returns: a new list of kmod modules on success or NULL on failure.
 */
KMOD_EXPORT struct kmod_list *kmod_module_get_holders(const struct kmod_module *mod)
{
	char dname[PATH_MAX];
	struct kmod_list *list = NULL;
	DIR *d;

	if (mod == NULL)
		return NULL;
	snprintf(dname, sizeof(dname), "/sys/module/%s/holders", mod->name);

	d = opendir(dname);
	if (d == NULL) {
		ERR(mod->ctx, "could not open '%s': %s\n",
						dname, strerror(errno));
		return NULL;
	}

	for (;;) {
		struct dirent de, *entp;
		struct kmod_module *holder;
		struct kmod_list *l;
		int err;

		err = readdir_r(d, &de, &entp);
		if (err != 0) {
			ERR(mod->ctx, "could not iterate for module '%s': %s\n",
						mod->name, strerror(-err));
			goto fail;
		}

		if (entp == NULL)
			break;

		if (de.d_name[0] == '.') {
			if (de.d_name[1] == '\0' ||
			    (de.d_name[1] == '.' && de.d_name[2] == '\0'))
				continue;
		}

		err = kmod_module_new_from_name(mod->ctx, de.d_name, &holder);
		if (err < 0) {
			ERR(mod->ctx, "could not create module for '%s': %s\n",
				de.d_name, strerror(-err));
			goto fail;
		}

		l = kmod_list_append(list, holder);
		if (l != NULL) {
			list = l;
		} else {
			ERR(mod->ctx, "out of memory\n");
			kmod_module_unref(holder);
			goto fail;
		}
	}

	closedir(d);
	return list;

fail:
	closedir(d);
	kmod_module_unref_list(list);
	return NULL;
}

struct kmod_module_section {
	unsigned long address;
	char name[];
};

static void kmod_module_section_free(struct kmod_module_section *section)
{
	free(section);
}

/**
 * kmod_module_get_sections:
 * @mod: kmod module
 *
 * Get a list of kmod sections of this @mod, as returned by Linux Kernel. The
 * structure contained in this list is internal to libkmod and their fields
 * can be obtained by calling kmod_module_section_get_name() and
 * kmod_module_section_get_address().
 *
 * After use, free the @list by calling kmod_module_section_free_list().
 *
 * Returns: a new list of kmod module sections on success or NULL on failure.
 */
KMOD_EXPORT struct kmod_list *kmod_module_get_sections(const struct kmod_module *mod)
{
	char dname[PATH_MAX];
	struct kmod_list *list = NULL;
	DIR *d;
	int dfd;

	if (mod == NULL)
		return NULL;

	snprintf(dname, sizeof(dname), "/sys/module/%s/sections", mod->name);

	d = opendir(dname);
	if (d == NULL) {
		ERR(mod->ctx, "could not open '%s': %s\n",
			dname, strerror(errno));
		return NULL;
	}

	dfd = dirfd(d);

	for (;;) {
		struct dirent de, *entp;
		struct kmod_module_section *section;
		struct kmod_list *l;
		unsigned long address;
		size_t namesz;
		int fd, err;

		err = readdir_r(d, &de, &entp);
		if (err != 0) {
			ERR(mod->ctx, "could not iterate for module '%s': %s\n",
						mod->name, strerror(-err));
			goto fail;
		}

		if (de.d_name[0] == '.') {
			if (de.d_name[1] == '\0' ||
			    (de.d_name[1] == '.' && de.d_name[2] == '\0'))
				continue;
		}

		fd = openat(dfd, de.d_name, O_RDONLY);
		if (fd < 0) {
			ERR(mod->ctx, "could not open '%s/%s': %m\n",
							dname, de.d_name);
			goto fail;
		}

		err = read_str_ulong(fd, &address, 16);
		close(fd);
		if (err < 0) {
			ERR(mod->ctx, "could not read long from '%s/%s': %m\n",
							dname, de.d_name);
			goto fail;
		}

		namesz = strlen(de.d_name) + 1;
		section = malloc(sizeof(*section) + namesz);

		if (section == NULL) {
			ERR(mod->ctx, "out of memory\n");
			goto fail;
		}

		section->address = address;
		memcpy(section->name, de.d_name, namesz);

		l = kmod_list_append(list, section);
		if (l != NULL) {
			list = l;
		} else {
			ERR(mod->ctx, "out of memory\n");
			free(section);
			goto fail;
		}
	}

	closedir(d);
	return list;

fail:
	closedir(d);
	kmod_module_unref_list(list);
	return NULL;
}

/**
 * kmod_module_section_get_module_name:
 * @entry: a list entry representing a kmod module section
 *
 * Get the name of a kmod module section.
 *
 * After use, free the @list by calling kmod_module_section_free_list().
 *
 * Returns: the name of this kmod module section on success or NULL on
 * failure. The string is owned by the section, do not free it.
 */
KMOD_EXPORT const char *kmod_module_section_get_name(const struct kmod_list *entry)
{
	struct kmod_module_section *section;

	if (entry == NULL)
		return NULL;

	section = entry->data;
	return section->name;
}

/**
 * kmod_module_section_get_address:
 * @entry: a list entry representing a kmod module section
 *
 * Get the address of a kmod module section.
 *
 * After use, free the @list by calling kmod_module_section_free_list().
 *
 * Returns: the address of this kmod module section on success or ULONG_MAX
 * on failure.
 */
KMOD_EXPORT unsigned long kmod_module_section_get_address(const struct kmod_list *entry)
{
	struct kmod_module_section *section;

	if (entry == NULL)
		return (unsigned long)-1;

	section = entry->data;
	return section->address;
}

/**
 * kmod_module_section_free_list:
 * @list: kmod module section list
 *
 * Release the resources taken by @list
 */
KMOD_EXPORT void kmod_module_section_free_list(struct kmod_list *list)
{
	while (list) {
		kmod_module_section_free(list->data);
		list = kmod_list_remove(list);
	}
}
