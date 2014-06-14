#ifndef _BURP_XATTR_H
#define _BURP_XATTR_H

#ifdef HAVE_XATTR
#if defined(HAVE_LINUX_OS) \
 || defined(HAVE_FREEBSD_OS) \
 || defined(HAVE_OPENBSD_OS) \
 || defined(HAVE_NETBSD_OS)
extern int has_xattr(const char *path, char cmd);
extern int get_xattr(struct asfd *asfd, const char *path, struct stat *statp,
	char **xattrtext, size_t *xlen, struct conf *conf);
extern int set_xattr(struct asfd *asfd, const char *path, struct stat *statp,
	const char *xattrtext, size_t xlen, char cmd, struct conf *conf);
#endif
#endif

#endif