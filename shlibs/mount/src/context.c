/*
 * Copyright (C) 2010 Karel Zak <kzak@redhat.com>
 *
 * This file may be redistributed under the terms of the
 * GNU Lesser General Public License.
 */

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#include "c.h"
#include "mountP.h"

/**
 * mnt_new_context:
 *
 * Returns: newly allocated mount context
 */
mnt_context *mnt_new_context()
{
	mnt_context *cxt;
	uid_t ruid, euid;

	cxt = calloc(1, sizeof(*cxt));
	if (!cxt)
		return NULL;

	ruid = getuid();
	euid = geteuid();

	/* if we're really root and aren't running setuid */
	cxt->restricted = (uid_t) 0 == ruid && ruid == euid ? 0 : 1;

	DBG(CXT, mnt_debug_h(cxt, "allocate %s",
				cxt->restricted ? "[RESTRICTED]" : ""));

	return cxt;
}

/**
 * mnt_free_context:
 * @cxt: mount context
 *
 * Deallocates context struct.
 */
void mnt_free_context(mnt_context *cxt)
{
	if (!cxt)
		return;

	DBG(CXT, mnt_debug_h(cxt, "free"));

	free(cxt->fstype_pattern);
	free(cxt->optstr_pattern);
	free(cxt->spec);
	free(cxt->helper);
	free(cxt->orig_user);

	if (!(cxt->flags & MNT_FL_EXTERN_FS))
		mnt_free_fs(cxt->fs);
	if (!(cxt->flags & MNT_FL_EXTERN_FSTAB))
		mnt_free_tab(cxt->fstab);
	if (!(cxt->flags & MNT_FL_EXTERN_CACHE))
		mnt_free_cache(cxt->cache);

	mnt_free_update(cxt->update);
	mnt_free_tab(cxt->mtab);

	free(cxt);
}

/**
 * mnt_reset_context:
 * @cxt: mount context
 *
 * Resets all information in the context that are directly related to
 * the latest mount (spec, source, target, mount options, ....)
 *
 * The match patters, cached fstab, cached canonicalized paths and tags and
 * [e]uid are not reseted. You have to use
 *
 *	mnt_context_set_fstab(cxt, NULL);
 *	mnt_context_set_cache(cxt, NULL);
 *	mnt_context_set_fstype_pattern(cxt, NULL);
 *	mnt_context_set_optstr_pattern(cxt, NULL);
 *
 * to reset these stuff.
 *
 * Returns: 0 on success, negative number in case of error.
 */
int mnt_reset_context(mnt_context *cxt)
{
	int fl;

	if (!cxt)
		return -EINVAL;

	fl = cxt->flags;

	if (cxt->update)
		mnt_update_set_fs(cxt->update, NULL);

	if (!(cxt->flags & MNT_FL_EXTERN_FS))
		mnt_free_fs(cxt->fs);

	mnt_free_tab(cxt->mtab);

	cxt->fs = NULL;
	cxt->mtab = NULL;

	free(cxt->spec);
	free(cxt->helper);
	free(cxt->orig_user);

	cxt->spec = NULL;
	cxt->helper = NULL;

	cxt->mountflags = 0;
	cxt->user_mountflags = 0;
	cxt->mountdata = NULL;
	cxt->flags = MNT_FL_DEFAULT;
	cxt->syscall_errno = 0;
	cxt->helper_status = 0;

	/* restore non-resetable flags */
	cxt->flags |= (fl & MNT_FL_EXTERN_FSTAB);
	cxt->flags |= (fl & MNT_FL_EXTERN_CACHE);

	return 0;
}

static int set_flag(mnt_context *cxt, int flag, int enable)
{
	if (!cxt)
		return -EINVAL;
	if (enable)
		cxt->flags |= flag;
	else
		cxt->flags &= ~flag;
	return 0;
}

/**
 * mnt_context_is_restricted:
 * @cxt: mount context
 *
 * Returns: 0 for unrestricted mount (user is root), or 1 for non-root mounts
 */
int mnt_context_is_restricted(mnt_context *cxt)
{
	assert(cxt);
	return cxt->restricted;
}

/**
 * mnt_context_set_optsmode
 * @cxt: mount context
 * @mode: MNT_OPTSMODE_{AUTO,FORCE,IGNORE,MTABFORCE}
 *
 * Defines a mode how libmount uses fstab mount options:
 *
 *  auto       - use options from fstab if source or target are not
 *               defined (this is mount(8) default).
 *
 *             - For remount operation it reads options from mtab if
 *               the target is not found in fstab.
 *
 *  ignore     - never use mount options from fstab
 *
 *  force      - always use mount options from fstab
 *
 *  mtab-force - for remount operation always use options from mtab (mountinfo)
 *
 * Returns: 0 on success, negative number in case of error.
 */
int mnt_context_set_optsmode(mnt_context *cxt, int mode)
{
	if (!cxt)
		return -EINVAL;
	cxt->optsmode = mode;
	return 0;
}

/**
 * mnt_context_disable_canonicalize:
 * @cxt: mount context
 * @disable: TRUE or FALSE
 *
 * Enable/disable paths canonicalization and tags evaluation. The libmount context
 * canonicalies paths when search in fstab and when prepare source and target paths
 * for mount(2) syscall.
 *
 * This fuction has effect to the private fstab instance only (see
 * mnt_context_set_fstab()). If you want to use an external fstab then you need
 * manage your private mnt_cache (see mnt_tab_set_cache(fstab, NULL).
 *
 * Returns: 0 on success, negative number in case of error.
 */
int mnt_context_disable_canonicalize(mnt_context *cxt, int disable)
{
	return set_flag(cxt, MNT_FL_NOCANONICALIZE, disable);
}

/**
 * mnt_context_enable_lazy:
 * @cxt: mount context
 * @enable: TRUE or FALSE
 *
 * Enable/disable lazy umount (see umount(8) man page, option -l).
 *
 * Returns: 0 on success, negative number in case of error.
 */
int mnt_context_enable_lazy(mnt_context *cxt, int enable)
{
	return set_flag(cxt, MNT_FL_LAZY, enable);
}

/**
 * mnt_context_disable_helpers:
 * @cxt: mount context
 * @disable: TRUE or FALSE
 *
 * Enable/disable /sbin/[u]mount.* helpers (see mount(8) man page, option -i).
 *
 * Returns: 0 on success, negative number in case of error.
 */
int mnt_context_disable_helpers(mnt_context *cxt, int disable)
{
	return set_flag(cxt, MNT_FL_NOHELPERS, disable);
}

/**
 * mnt_context_enable_sloppy:
 * @cxt: mount context
 * @enable: TRUE or FALSE
 *
 * Set/unset sloppy mounting (see mount(8) man page, option -s).
 *
 * Returns: 0 on success, negative number in case of error.
 */
int mnt_context_enable_sloppy(mnt_context *cxt, int enable)
{
	return set_flag(cxt, MNT_FL_SLOPPY, enable);
}

/**
 * mnt_context_enable_fake:
 * @cxt: mount context
 * @enable: TRUE or FALSE
 *
 * Enable/disable fake mounting (see mount(8) man page, option -f).
 *
 * Returns: 0 on success, negative number in case of error.
 */
int mnt_context_enable_fake(mnt_context *cxt, int enable)
{
	return set_flag(cxt, MNT_FL_FAKE, enable);
}

/**
 * mnt_context_disable_mtab:
 * @cxt: mount context
 * @disable: TRUE or FALSE
 *
 * Disable/enable mtab update (see mount(8) man page, option -n).
 *
 * Returns: 0 on success, negative number in case of error.
 */
int mnt_context_disable_mtab(mnt_context *cxt, int disable)
{
	return set_flag(cxt, MNT_FL_NOMTAB, disable);
}

/**
 * mnt_context_disable_lock:
 * @cxt: mount context
 * @disable: TRUE or FALSE
 *
 * Disable/enable mtab lock.
 *
 * Returns: 0 on success, negative number in case of error.
 */
int mnt_context_disable_lock(mnt_context *cxt, int disable)
{
	return set_flag(cxt, MNT_FL_NOLOCK, disable);
}

/**
 * mnt_context_enable_force:
 * @cxt: mount context
 * @enable: TRUE or FALSE
 *
 * Enable/disable force umounting (see umount(8) man page, option -f).
 *
 * Returns: 0 on success, negative number in case of error.
 */
int mnt_context_enable_force(mnt_context *cxt, int enable)
{
	return set_flag(cxt, MNT_FL_FORCE, enable);
}

/**
 * mnt_context_enable_verbose:
 * @cxt: mount context
 * @enable: TRUE or FALSE
 *
 * Enable/disable verbose output (see also mnt_context_mount_strerror()).
 *
 * Returns: 0 on success, negative number in case of error.
 */
int mnt_context_enable_verbose(mnt_context *cxt, int enable)
{
	return set_flag(cxt, MNT_FL_VERBOSE, enable);
}

/**
 * mnt_context_enable_loopdel:
 * @cxt: mount context
 * @enable: TRUE or FALSE
 *
 * Enable/disable loop delete (destroy) after umount (see umount(8), option -d)
 *
 * Returns: 0 on success, negative number in case of error.
 */
int mnt_context_enable_loopdel(mnt_context *cxt, int enable)
{
	return set_flag(cxt, MNT_FL_LOOPDEL, enable);
}

/**
 * mnt_context_set_spec:
 * @cxt: mount context
 * @spec: unresolved source (device, label, uuid, ...) or target (mountpoint)
 *
 * Returns: 0 on success, negative number in case of error.
 */
int mnt_context_set_spec(mnt_context *cxt, const char *spec)
{
	char *p;

	if (!cxt)
		return -EINVAL;
	p = strdup(spec);
	if (!p)
		return -ENOMEM;
	free(cxt->spec);
	cxt->spec = p;
	return 0;
}

/**
 * mnt_context_set_fs:
 * @cxt: mount context
 * @fs: filesystem description
 *
 * The mount context uses private @fs by default. This function allows to
 * overwrite the private @fs with an external instance. Note that the external
 * @fs instance is not deallocated by mnt_free_context() or mnt_reset_context().
 *
 * The @fs will be modified by mnt_context_set_{source,target,optstr,fstype}
 * functions, Ft the @fs is NULL then all current FS specific setting (source,
 * target, etc., exclude spec) is reseted.
 *
 * Returns: 0 on success, negative number in case of error.
 */
int mnt_context_set_fs(mnt_context *cxt, mnt_fs *fs)
{
	if (!cxt)
		return -EINVAL;
	if (!(cxt->flags & MNT_FL_EXTERN_FS))
		mnt_free_fs(cxt->fs);

	set_flag(cxt, MNT_FL_EXTERN_FS, fs != NULL);
	cxt->fs = fs;
	return 0;
}

mnt_fs *mnt_context_get_fs(mnt_context *cxt)
{
	if (!cxt)
		return NULL;
	if (!cxt->fs) {
		cxt->fs = mnt_new_fs();
		cxt->flags &= ~MNT_FL_EXTERN_FS;
	}
	return cxt->fs;
}

/**
 * mnt_context_set_source:
 * @cxt: mount context
 * @source: mount source (device, directory, UUID, LABEL, ...)
 *
 * Returns: 0 on success, negative number in case of error.
 */
int mnt_context_set_source(mnt_context *cxt, const char *source)
{
	return mnt_fs_set_source(mnt_context_get_fs(cxt), source);
}

/**
 * mnt_context_set_target:
 * @cxt: mount context
 * @target: mountpoint
 *
 * Returns: 0 on success, negative number in case of error.
 */
int mnt_context_set_target(mnt_context *cxt, const char *target)
{
	return mnt_fs_set_target(mnt_context_get_fs(cxt), target);
}

/**
 * mnt_context_set_fstype:
 * @cxt: mount context
 * @fstype: filesystem type
 *
 * Note that the @fstype has to be the real FS type. For comma-separated list of
 * filesystems or for "no<fs>" notation use mnt_context_set_fstype_pattern().
 *
 * Returns: 0 on success, negative number in case of error.
 */
int mnt_context_set_fstype(mnt_context *cxt, const char *fstype)
{
	return mnt_fs_set_fstype(mnt_context_get_fs(cxt), fstype);
}

/**
 * mnt_context_set_optstr:
 * @cxt: mount context
 * @optstr: comma delimited mount options
 *
 * Returns: 0 on success, negative number in case of error.
 */
int mnt_context_set_optstr(mnt_context *cxt, const char *optstr)
{
	return mnt_fs_set_optstr(mnt_context_get_fs(cxt), optstr);
}

/**
 * mnt_context_append_optstr:
 * @cxt: mount context
 * @optstr: comma delimited mount options
 *
 * Returns: 0 on success, negative number in case of error.
 */
int mnt_context_append_optstr(mnt_context *cxt, const char *optstr)
{
	return mnt_fs_append_optstr(mnt_context_get_fs(cxt), optstr);
}

/**
 * mnt_context_set_fstype_pattern:
 * @cxt: mount context
 * @pattern: FS name pattern (or NULL to reset the current setting)
 *
 * See mount(8), option -t.
 *
 * Returns: 0 on success, negative number in case of error.
 */
int mnt_context_set_fstype_pattern(mnt_context *cxt, const char *pattern)
{
	char *p = NULL;

	if (!cxt)
		return -EINVAL;
	if (pattern) {
		p = strdup(pattern);
		if (!p)
			return -ENOMEM;
	}
	free(cxt->fstype_pattern);
	cxt->fstype_pattern = p;
	return 0;
}

/**
 * mnt_context_set_optstr_pattern:
 * @cxt: mount context
 * @pattern: options pattern (or NULL to reset the current setting)
 *
 * See mount(8), option -O.
 *
 * Returns: 0 on success, negative number in case of error.
 */
int mnt_context_set_optstr_pattern(mnt_context *cxt, const char *pattern)
{
	char *p = NULL;

	if (!cxt)
		return -EINVAL;
	if (pattern) {
		p = strdup(pattern);
		if (!p)
			return -ENOMEM;
	}
	free(cxt->optstr_pattern);
	cxt->optstr_pattern = p;
	return 0;
}

/**
 * mnt_context_set_fstab:
 * @cxt: mount context
 * @tb: fstab
 *
 * The mount context reads /etc/fstab to the the private mnt_tab by default.
 * This function allows to overwrite the private fstab with an external
 * instance. Note that the external instance is not deallocated by mnt_free_context().
 *
 * The fstab is used read-only and is not modified, it should be possible to
 * share the fstab between more mount contexts (TODO: tests it.)
 *
 * If the @tb argument is NULL then the current private fstab instance is
 * reseted.
 *
 * Returns: 0 on success, negative number in case of error.
 */
int mnt_context_set_fstab(mnt_context *cxt, mnt_tab *tb)
{
	if (!cxt)
		return -EINVAL;
	if (!(cxt->flags & MNT_FL_EXTERN_FSTAB))
		mnt_free_tab(cxt->fstab);

	set_flag(cxt, MNT_FL_EXTERN_FSTAB, tb != NULL);
	cxt->fstab = tb;
	return 0;
}

/**
 * mnt_context_set_cache:
 * @cxt: mount context
 * @cache: cache instance or nULL
 *
 * The mount context maintains a private mnt_cache by default.  This function
 * allows to overwrite the private cache with an external instance. Note that
 * the external instance is not deallocated by mnt_free_context().
 *
 * If the @cache argument is NULL then the current private cache instance is
 * reseted.
 *
 * Returns: 0 on success, negative number in case of error.
 */
int mnt_context_set_cache(mnt_context *cxt, mnt_cache *cache)
{
	if (!cxt)
		return -EINVAL;
	if (!(cxt->flags & MNT_FL_EXTERN_CACHE))
		mnt_free_cache(cxt->cache);

	set_flag(cxt, MNT_FL_EXTERN_CACHE, cache != NULL);
	cxt->cache = cache;
	return 0;
}

/**
 * mnt_context_get_cache
 * @cxt: mount context
 *
 * See also mnt_context_set_cache().
 *
 * Returns: pointer to cache or NULL if canonicalization is disabled.
 */
mnt_cache *mnt_context_get_cache(mnt_context *cxt)
{
	if (!cxt || (cxt->flags & MNT_FL_NOCANONICALIZE))
		return NULL;

	if (!cxt->cache) {
		cxt->cache = mnt_new_cache();
		if (!cxt->cache)
			return NULL;
		cxt->flags &= ~MNT_FL_EXTERN_CACHE;
	}
	return cxt->cache;
}

/**
 * mnt_context_get_lock:
 * @cxt: mount context
 *
 * The lock is available after mnt_context_prepare_mount() or
 * mnt_context_prepare_umount().
 *
 * The application that uses libmount context does not have to care about
 * mtab locking, but with a small exceptions: the application has to be able to
 * remove the lock file when interrupted by signal. It means that properly written
 * mount(8)-like application has to call mnt_unlock_file() from a signal handler.
 *
 * See also mnt_unlock_file(), mnt_context_disable_lock() and
 * mnt_context_disable_mtab().
 *
 * It's not error if this function returns NULL (it usually means that the
 * context is not prepared yet, or mtab update is unnecessary).
 *
 * Returns: pointer to lock struct.
 */
mnt_lock *mnt_context_get_lock(mnt_context *cxt)
{
	if (!cxt || !cxt->update || (cxt->flags & (MNT_FL_NOMTAB | MNT_FL_NOLOCK)))
		return NULL;
	return mnt_update_get_lock(cxt->update);
}

/**
 * mnt_context_set_mountflags:
 * @cxt: mount context
 * @flags: mount(2) flags (MS_* flags)
 *
 * Note that mount context allows to define mount options by mount flags. It
 * means you can for example use
 *
 *	mnt_context_set_mountflags(cxt, MS_NOEXEC | MS_NOSUID);
 *
 * rather than
 *
 *	mnt_context_set_optstr(cxt, "noexec,nosuid");
 *
 * these both calls have the same effect.
 *
 * Returns: 0 on success, negative number in case of error.
 */
int mnt_context_set_mountflags(mnt_context *cxt, unsigned long flags)
{
	if (!cxt)
		return -EINVAL;
	cxt->mountflags = flags;
	return 0;
}

/**
 * mnt_context_get_mountflags:
 * @cxt: mount context
 * @flags: returns mount flags
 *
 * Converts mount options string to MS_* flags and bitewise-OR the result with
 * already defined flags (see mnt_context_set_mountflags()).
 *
 * Returns: 0 on success, negative number in case of error.
 */
int mnt_context_get_mountflags(mnt_context *cxt, unsigned long *flags)
{
	int rc = 0;
	if (!cxt || !flags)
		return -EINVAL;

	*flags = 0;
	if (!(cxt->flags & MNT_FL_MOUNTFLAGS_MERGED) && cxt->fs) {
		const char *o = mnt_fs_get_optstr(cxt->fs);
		if (o)
			rc = mnt_optstr_get_mountflags(o, flags);
	}
	if (!rc)
		*flags |= cxt->mountflags;
	return rc;
}

/**
 * mnt_context_set_userspace_mountflags:
 * @cxt: mount context
 * @flags: mount(2) flags (MNT_MS_* flags, e.g. MNT_MS_LOOP)
 *
 * See also notest for mnt_context_set_mountflags().
 *
 * Returns: 0 on success, negative number in case of error.
 */
int mnt_context_set_userspace_mountflags(mnt_context *cxt, unsigned long flags)
{
	if (!cxt)
		return -EINVAL;
	cxt->user_mountflags = flags;
	return 0;
}

/**
 * mnt_context_get_userspace_mountflags:
 * @cxt: mount context
 * @flags: returns mount flags
 *
 * Converts mount options string to MNT_MS_* flags and bitewise-OR the result with
 * already defined flags (see mnt_context_set_userspace_mountflags()).
 *
 * Returns: 0 on success, negative number in case of error.
 */
int mnt_context_get_userspace_mountflags(mnt_context *cxt, unsigned long *flags)
{
	int rc = 0;
	if (!cxt || !flags)
		return -EINVAL;

	*flags = 0;
	if (!(cxt->flags & MNT_FL_MOUNTFLAGS_MERGED) && cxt->fs) {
		const char *o = mnt_fs_get_optstr(cxt->fs);
		if (o)
			rc = mnt_optstr_get_userspace_mountflags(o, flags);
	}
	if (!rc)
		*flags |= cxt->user_mountflags;
	return rc;
}

static int is_loop(mnt_context *cxt)
{
	unsigned long fl = 0;

	if (cxt->user_mountflags & MNT_MS_LOOP)
		return 1;
	if (!mnt_context_get_mountflags(cxt, &fl) && (fl & MNT_MS_LOOP))
		return 1;

	/* TODO:
	 *	- support MNT_MS_{OFFSET,SIZELIMIT,ENCRYPTION}
	 */
	return 0;
}

/**
 * mnt_context_set_mountdata:
 * @cxt: mount context
 * @data: mount(2) data
 *
 * The mount context generates mountdata from mount options by default. This
 * function allows to overwrite this behavior, and @data will be used instead
 * of mount options.
 *
 * The libmount does not deallocated the data by mnt_free_context(). Note that
 * NULL is also valid mount data.
 *
 * Returns: 0 on success, negative number in case of error.
 */
int mnt_context_set_mountdata(mnt_context *cxt, void *data)
{
	if (!cxt)
		return -EINVAL;
	cxt->mountdata = data;
	cxt->flags |= MNT_FL_MOUNTDATA;
	return 0;
}

/*
 * Translates LABEL/UUID/path to mountable path
 */
int mnt_context_prepare_srcpath(mnt_context *cxt)
{
	const char *path = NULL, *type;
	mnt_cache *cache;
	const char *t, *v, *src;
	int rc = 0;

	if (!cxt || !cxt->fs)
		return -EINVAL;

	src = mnt_fs_get_source(cxt->fs);

	/* ignore filesystems without a real source */
	if (!src || (cxt->fs->flags & (MNT_FS_PSEUDO | MNT_FS_NET)))
		return 0;

	DBG(CXT, mnt_debug_h(cxt, "preparing srcpath '%s'", src));

	cache = mnt_context_get_cache(cxt);
	type = mnt_fs_get_fstype(cxt->fs);

	if (!mnt_fs_get_tag(cxt->fs, &t, &v)) {
		/*
		 * Source is TAG (evaluate)
		 */
		if (cache)
			path = mnt_resolve_tag(t, v, cache);

		rc = path ? mnt_fs_set_source(cxt->fs, path) : -EINVAL;

	} else if (!type || (strncmp(type, "9p", 2) &&
			     strncmp(type, "nfs", 3) &&
			     strncmp(type, "cifs", 4) &&
			     strncmp(type, "smbfs", 5))) {
		/*
		 * Source is PATH (canonicalize)
		 */
		if (cache) {
			path = mnt_resolve_path(src, cache);
			if (strcmp(path, src))
				rc = mnt_fs_set_source(cxt->fs, path);
		}
	}

	if (rc) {
		DBG(CXT, mnt_debug_h(cxt, "failed to prepare srcpath"));
		return rc;
	}

	if (!path)
		path = src;

	/*
	 * Initialize loop device
	 */
	if (is_loop(cxt) &&
	    !(cxt->mountflags & (MS_BIND | MS_MOVE |
			         MS_PROPAGATION | MS_REMOUNT))) {
		; /* TODO */
	}

	DBG(CXT, mnt_debug_h(cxt, "final srcpath '%s'", path));
	return 0;
}

int mnt_context_guess_fstype(mnt_context *cxt)
{
	char *type;
	const char *dev;
	int rc = -EINVAL;

	if (!cxt || !cxt->fs)
		return -EINVAL;

	if (cxt->mountflags & (MS_BIND | MS_MOVE | MS_PROPAGATION))
		goto none;

	type = (char *) mnt_fs_get_fstype(cxt->fs);
	if (type && !strcmp(type, "auto")) {
		mnt_fs_set_fstype(cxt->fs, NULL);
		type = NULL;
	}

	if (type)
		goto done;
	if (cxt->flags & MS_REMOUNT)
		goto none;
	dev = mnt_fs_get_srcpath(cxt->fs);
	if (!dev)
		goto err;

	if (access(dev, F_OK) == 0) {
		mnt_cache *cache = mnt_context_get_cache(cxt);

		type = mnt_get_fstype(dev, &cxt->ambi, cache);
		if (type) {
			rc = mnt_fs_set_fstype(cxt->fs, type);
			if (!cache)
				free(type);	/* type is not cached */
		}
	} else {
		if (strchr(dev, ':') != NULL)
			rc = mnt_fs_set_fstype(cxt->fs, "nfs");
		else if (!strncmp(dev, "//", 2))
			rc = mnt_fs_set_fstype(cxt->fs, "cifs");
	}
	if (rc)
		goto err;
done:
	DBG(CXT, mnt_debug_h(cxt, "detected FS type: %s",
				mnt_fs_get_fstype(cxt->fs)));
	return 0;
none:
	return mnt_fs_set_fstype(cxt->fs, "none");
err:
	DBG(CXT, mnt_debug_h(cxt, "failed to detect FS type"));
	return rc;
}

/*
 * The default is to use fstype from cxt->fs, this could be overwritten by
 * @type. The @act is MNT_ACT_{MOUNT,UMOUNT}.
 *
 * Returns: 0 on success or negative number in case of error. Note that success
 * does not mean that there is any usable helper, you have to check cxt->helper.
 */
int mnt_context_prepare_helper(mnt_context *cxt, const char *name,
				const char *type)
{
	char search_path[] = FS_SEARCH_PATH;		/* from config.h */
	char *p = NULL, *path;

	assert(cxt);
	assert(cxt->fs);

	if (!type)
		type = mnt_fs_get_fstype(cxt->fs);

	if ((cxt->flags & MNT_FL_NOHELPERS) || !type ||
	    !strcmp(type, "none") || (cxt->fs->flags & MNT_FS_SWAP))
		return 0;

	path = strtok_r(search_path, ":", &p);
	while (path) {
		char helper[PATH_MAX];
		struct stat st;
		int rc;

		rc = snprintf(helper, sizeof(helper), "%s/%s.%s",
						path, name, type);
		path = strtok_r(NULL, ":", &p);

		if (rc >= sizeof(helper) || rc < 0)
			continue;

		rc = stat(helper, &st);
		if (rc == -1 && errno == ENOENT && strchr(type, '.')) {
			/* If type ends with ".subtype" try without it */
			*strrchr(helper, '.') = '\0';
			rc = stat(helper, &st);
		}

		DBG(CXT, mnt_debug_h(cxt, "%s ... %s", helper,
					rc ? "not found" : "found"));
		if (rc)
			continue;

		if (cxt->helper)
			free(cxt->helper);
		cxt->helper = strdup(helper);
		if (!cxt->helper)
			return -ENOMEM;
		return 0;
	}

	return 0;
}

/*
 * Prepare /etc/mtab or /var/run/mount/mountinfo update
 */
int mnt_context_prepare_update(mnt_context *cxt, int act)
{
	int rc;

	if ((cxt->flags & MNT_FL_NOMTAB) || cxt->helper)
		return 0;

	if (!cxt->update) {
		cxt->update = mnt_new_update(act, cxt->mountflags, cxt->fs);
		if (!cxt->update)
			return -ENOMEM;
	} else {
		rc = mnt_update_set_action(cxt->update, act);
		if (!rc)
			rc = mnt_update_set_mountflags(cxt->update, cxt->mountflags);
		if (!rc)
			rc = mnt_update_set_fs(cxt->update, cxt->fs);
		if (rc)
			return rc;
	}

	if (cxt->flags & MNT_FL_NOLOCK)
		mnt_update_disable_lock(cxt->update, TRUE);

	rc = mnt_prepare_update(cxt->update);

	if (rc == 1)
		/* mtab update is unnecessary for this system */
		rc = 0;

	return rc;
}

