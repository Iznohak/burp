#include "burp.h"
#include "handy.h"
#include "prog.h"
#include "msg.h"
#include "asyncio.h"
#include "counter.h"
#include "find.h"
#include "berrno.h"
#include "forkchild.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

#if defined(WIN64)
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

void close_fd(int *fd)
{
	if(*fd<0) return;
	//logp("closing %d\n", *fd);
	close(*fd);
	*fd=-1;
}

void close_fp(FILE **fp)
{
	if(!*fp) return;
	fclose(*fp);
	*fp=NULL;
}

void gzclose_fp(gzFile *fp)
{
	if(!*fp) return;
	gzflush(*fp, Z_FINISH);
	gzclose(*fp);
	*fp=NULL;
}

int is_dir(const char *path)
{
        struct stat buf;

        if(lstat(path, &buf)) return 0;

        return S_ISDIR(buf.st_mode);
}

/* This may be given binary data, of which we need to already know the length */
char *prepend_len(const char *prep, size_t plen, const char *fname, size_t flen, const char *sep, size_t slen, size_t *newlen)
{
	size_t l=0;
	char *rpath=NULL;

	l+=plen;
	l+=flen;
	l+=slen;
	l+=1;

	if(!(rpath=(char *)malloc(l)))
	{
		logp("could not malloc for prepend to %s\n", fname);
		return NULL;
	}
	if(plen) memcpy(rpath,           prep,  plen);
	if(slen) memcpy(rpath+plen,      sep,   slen);
	if(flen) memcpy(rpath+plen+slen, fname, flen);
	rpath[plen+slen+flen]='\0';

	if(newlen) (*newlen)+=slen+flen;
	return rpath;
}

char *prepend(const char *prep, const char *fname, size_t len, const char *sep)
{
	return prepend_len(prep, prep?strlen(prep):0,
		fname, len,
		sep, (sep && *fname)?strlen(sep):0, NULL);
}

char *prepend_s(const char *prep, const char *fname, size_t len)
{
	if(!prep || !*prep)
	{
		char *ret=NULL;
		if(!(ret=strdup(fname)))
			logp("out of memory in prepend_s\n");
		return ret;
	}
	// Try to avoid getting a double slash in the path.
	if(fname && fname[0]=='/')
	{
		fname++;
		len--;
	}
	return prepend(prep, fname, len, "/");
}

int mkpath(char **rpath, const char *limit)
{
	char *cp=NULL;
	struct stat buf;
	//printf("mkpath: %s\n", *rpath);
	if((cp=strrchr(*rpath, '/')))
	{
#ifdef HAVE_WIN32
		int windows_stupidity=0;
		*cp='\0';
		if(strlen(*rpath)==2 && (*rpath)[1]==':')
		{
			(*rpath)[1]='\0';
			windows_stupidity++;
		}
#else
		*cp='\0';
#endif
		if(!**rpath)
		{
			// We are down to the root, which is OK.
		}
		else if(lstat(*rpath, &buf))
		{
			// does not exist - recurse further down, then come
			// back and try to mkdir it.
			if(mkpath(rpath, limit)) return -1;

			// Require that the user has set up the required paths
			// on the server correctly. I have seen problems with
			// part of the path being a temporary symlink that
			// gets replaced by burp with a proper directory.
			// Allow it to create the actual directory specified,
			// though.

			// That is, if limit is:
			// /var/spool/burp
			// and /var/spool exists, the directory will be
			// created.
			// If only /var exists, the directory will not be
			// created.

			// Caller can give limit=NULL to create the whole
			// path with no limit, as in a restore.
			if(limit && pathcmp(*rpath, limit)<0)
			{
				logp("will not mkdir %s\n", *rpath);
#ifdef HAVE_WIN32
				if(windows_stupidity) (*rpath)[1]=':';
#endif
				*cp='/';
				return -1;
			}
			if(mkdir(*rpath, 0777))
			{
				logp("could not mkdir %s: %s\n", *rpath, strerror(errno));
#ifdef HAVE_WIN32
				if(windows_stupidity) (*rpath)[1]=':';
#endif
				*cp='/';
				return -1;
			}
		}
		else if(S_ISDIR(buf.st_mode))
		{
			// Is a directory - can put the slash back and return.
		}
		else if(S_ISLNK(buf.st_mode))
		{
			// to help with the 'current' symlink
		}
		else
		{
			// something funny going on
			logp("warning: wanted '%s' to be a directory\n",
				*rpath);
		}
#ifdef HAVE_WIN32
		if(windows_stupidity) (*rpath)[1]=':';
#endif
		*cp='/';
	}
	return 0;
}

int build_path(const char *datadir, const char *fname, size_t flen, char **rpath, const char *limit)
{
	//logp("build path: '%s/%s'\n", datadir, fname);
	if(!(*rpath=prepend_s(datadir, fname, flen))) return -1;
	if(mkpath(rpath, limit))
	{
		if(*rpath) { free(*rpath); *rpath=NULL; }
		return -1;
	}
	return 0;
}

// return -1 for error, 0 for OK, 1 if the client wants to interrupt the
// transfer.
int do_quick_read(const char *datapth, struct cntr *cntr)
{
	int r=0;
	char cmd;
	size_t len=0;
	char *buf=NULL;
	if(async_read_quick(&cmd, &buf, &len)) return -1;

	if(buf)
	{
		if(cmd==CMD_WARNING)
		{
			logp("WARNING: %s\n", buf);
			do_filecounter(cntr, cmd, 0);
		}
		else if(cmd==CMD_INTERRUPT)
		{
			// Client wants to interrupt - double check that
			// it is still talking about the file that we are
			// sending.
			if(datapth && !strcmp(buf, datapth))
				r=1;
		}
		else
		{
			logp("unexpected cmd in quick read: %c:%s\n", cmd, buf);
			r=-1;
		}
		free(buf);
	}
	return r;
}

char *get_checksum_str(unsigned char *checksum)
{
	//int i=0;
	//char tmp[3]="";
	static char str[64]="";
/*
	str[0]='\0';
	// Windows does not seem to like me writing it all at the same time.
	// Fuck knows why.
	for(i=0; i<MD5_DIGEST_LENGTH; i++)
	{
		snprintf(tmp, sizeof(tmp), "%02x", checksum[i]);
		strcat(str, tmp);
	}
*/
	snprintf(str, sizeof(str),
	  "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
		checksum[0], checksum[1],
		checksum[2], checksum[3],
		checksum[4], checksum[5],
		checksum[6], checksum[7],
		checksum[8], checksum[9],
		checksum[10], checksum[11],
		checksum[12], checksum[13],
		checksum[14], checksum[15]);
	return str;
}

int write_endfile(unsigned long long bytes, unsigned char *checksum)
{
	int ret=0;
	char endmsg[128]="";

	snprintf(endmsg, sizeof(endmsg), "%llu:%s",
		bytes, get_checksum_str(checksum));

	ret=async_write_str(CMD_END_FILE, endmsg);
	return ret;
}

static int do_encryption(EVP_CIPHER_CTX *ctx, unsigned char *inbuf, size_t inlen, unsigned char *outbuf, size_t *outlen, MD5_CTX *md5)
{
	if(!inlen) return 0;
	if(!EVP_CipherUpdate(ctx, outbuf, (int *)outlen, inbuf, (int)inlen))
	{
		logp("Encryption failure.\n");
		return -1;
	}
	if(*outlen>0)
	{
		int ret;
		if(!(ret=async_write(CMD_APPEND, (const char *)outbuf, *outlen)))
		{
			if(!MD5_Update(md5, outbuf, *outlen))
			{
				logp("MD5_Update() failed\n");
				return -1;
			}
		}
		return ret;
	}
	return 0;
}

EVP_CIPHER_CTX *enc_setup(int encrypt, const char *encryption_password)
{
	EVP_CIPHER_CTX *ctx=NULL;
	const char *enc_iv="[lkd.$G�"; // never change this.

	if(!(ctx=(EVP_CIPHER_CTX *)malloc(sizeof(EVP_CIPHER_CTX))))
	{
		logp("out of memory\n");
		return NULL;
	}
        memset(ctx, 0, sizeof(EVP_CIPHER_CTX));
	// Don't set key or IV because we will modify the parameters.
	EVP_CIPHER_CTX_init(ctx);
	if(!(EVP_CipherInit_ex(ctx, EVP_bf_cbc(), NULL, NULL, NULL, encrypt)))
	{
		logp("EVP_CipherInit_ex failed\n");
		free(ctx);
		return NULL;
	}
	EVP_CIPHER_CTX_set_key_length(ctx, strlen(encryption_password));
	// We finished modifying parameters so now we can set key and IV

	if(!EVP_CipherInit_ex(ctx, NULL, NULL,
		(unsigned char *)encryption_password,
		(unsigned char *)enc_iv, encrypt))
	{
		logp("Second EVP_CipherInit_ex failed\n");
		free(ctx);
		return NULL;
	}
	return ctx;
}

int send_whole_file_gz(const char *fname, const char *datapth, int quick_read, unsigned long long *bytes, const char *encpassword, struct cntr *cntr, int compression, const char *extrameta, size_t elen)
{
	int ret=0;
	int zret=0;
	MD5_CTX md5;
#ifdef HAVE_WIN32
	BFILE bfd;
#else
	FILE *fp=NULL;
#endif
	size_t metalen=0;
	const char *metadata=NULL;

	unsigned have;
	z_stream strm;
	int flush=Z_NO_FLUSH;
	unsigned char in[ZCHUNK];
	unsigned char out[ZCHUNK];

	size_t eoutlen;
	unsigned char eoutbuf[ZCHUNK+EVP_MAX_BLOCK_LENGTH];

	EVP_CIPHER_CTX *enc_ctx=NULL;

	if(encpassword && !(enc_ctx=enc_setup(1, encpassword)))
		return -1;

	if(!MD5_Init(&md5))
	{
		logp("MD5_Init() failed\n");
		return -1;
	}

//logp("send_whole_file_gz: %s%s\n", fname, extrameta?" (meta)":"");

	if((metadata=extrameta))
	{
		metalen=elen;
	}
	else
	{
#ifdef HAVE_WIN32
		binit(&bfd);
		if(bopen(&bfd, fname, O_RDONLY | O_BINARY | O_NOATIME, 0)<0)
		{
			berrno be;
			logp("Could not open %s: %s\n",
				fname, be.bstrerror(errno));
			return -1;
		}
#else
		if(!(fp=fopen(fname, "rb")))
		{
			logp("Could not open %s: %s\n",
				fname, strerror(errno));
			return -1;
		}
#endif
	}

	/* allocate deflate state */
	strm.zalloc = Z_NULL;
	strm.zfree = Z_NULL;
	strm.opaque = Z_NULL;
	if((zret=deflateInit2(&strm, compression, Z_DEFLATED, (15+16),
		8, Z_DEFAULT_STRATEGY))!=Z_OK)

	{
		if(!metadata)
		{
#ifdef HAVE_WIN32
			bclose(&bfd);
#else
			fclose(fp);
#endif
		}
		return -1;
	}

	do
	{
		if(metadata)
		{
			if(metalen>ZCHUNK)
				strm.avail_in=ZCHUNK;
			else
				strm.avail_in=metalen;
			memcpy(in, metadata, strm.avail_in);
			metadata+=strm.avail_in;
			metalen-=strm.avail_in;
		}
		else
		{
#ifdef HAVE_WIN32
			strm.avail_in=(uint32_t)bread(&bfd, in, ZCHUNK);
#else
			strm.avail_in=fread(in, 1, ZCHUNK, fp);
#endif
		}
		if(strm.avail_in<0)
		{
			logp("Error in read: %d\n", strm.avail_in);
			ret=-1;
			break;
		}
		*bytes+=strm.avail_in;

		// The checksum needs to be later if encryption is being used.
		if(!enc_ctx)
		{
			if(!MD5_Update(&md5, in, strm.avail_in))
			{
				logp("MD5_Update() failed\n");
				ret=-1;
				break;
			}
		}

		if(strm.avail_in) flush=Z_NO_FLUSH;
		else flush=Z_FINISH;

		strm.next_in=in;

		/* run deflate() on input until output buffer not full, finish
			compression if all of source has been read in */
		do
		{
			strm.avail_out = ZCHUNK;
			strm.next_out = out;
			zret = deflate(&strm, flush); /* no bad return value */
			if(zret==Z_STREAM_ERROR) /* state not clobbered */
			{
				logp("z_stream_error\n");
				ret=-1;
				break;
			}
			have = ZCHUNK-strm.avail_out;

			if(enc_ctx)
			{
				if(do_encryption(enc_ctx, out, have, eoutbuf, &eoutlen, &md5))
				{
					ret=-1;
					break;
				}
			}
			else if(async_write(CMD_APPEND, (const char *)out, have))
			{
				ret=-1;
				break;
			}
			if(quick_read && datapth)
			{
				int qr;
				if((qr=do_quick_read(datapth, cntr))<0)
				{
					ret=-1;
					break;
				}
				if(qr) // client wants to interrupt
				{
					goto cleanup;
				}
			}
		} while (!strm.avail_out);

		if(ret) break;

		if(strm.avail_in) /* all input will be used */
		{
			ret=-1;
			logp("strm.avail_in=%d\n", strm.avail_in);
			break;
		}
	} while(flush!=Z_FINISH);

	if(!ret)
	{
		if(zret!=Z_STREAM_END)
		{
			logp("ret OK, but zstream not finished: %d\n", zret);
			ret=-1;
		}
		else if(enc_ctx)
		{
			if(!EVP_CipherFinal_ex(enc_ctx,
				eoutbuf, (int *)&eoutlen))
			{
				logp("Encryption failure at the end\n");
				ret=-1;
			}
			else if(eoutlen>0)
			{
			  if(async_write(CMD_APPEND, (const char *)eoutbuf, eoutlen))
				ret=-1;
			  else if(!MD5_Update(&md5, eoutbuf, eoutlen))
			  {
				logp("MD5_Update() failed\n");
				ret=-1;
			  }
			}
		}
	}

cleanup:
	deflateEnd(&strm);

	if(!metadata)
	{
#ifdef HAVE_WIN32
		bclose(&bfd);
#else
		fclose(fp);
#endif
	}

	if(enc_ctx)
	{
		EVP_CIPHER_CTX_cleanup(enc_ctx);
		free(enc_ctx);
	}

	if(!ret)
	{
		unsigned char checksum[MD5_DIGEST_LENGTH+1];
		if(!MD5_Final(checksum, &md5))
		{
			logp("MD5_Final() failed\n");
			return -1;
		}

		return write_endfile(*bytes, checksum);
	}
//logp("end of send\n");
	return ret;
}

int send_whole_file(const char *fname, const char *datapth, int quick_read, unsigned long long *bytes, struct cntr *cntr, const char *extrameta, size_t elen)
{
	int ret=0;
	size_t s=0;
	MD5_CTX md5;
	char buf[4096]="";

	if(!MD5_Init(&md5))
	{
		logp("MD5_Init() failed\n");
		return -1;
	}

	if(extrameta)
	{
		size_t metalen=0;
		const char *metadata=NULL;

		metadata=extrameta;
		metalen=elen;

		// Send metadata in chunks, rather than all at once.
		while(metalen>0)
		{
			if(metalen>ZCHUNK) s=ZCHUNK;
			else s=metalen;

			if(!MD5_Update(&md5, metadata, s))
			{
				logp("MD5_Update() failed\n");
				ret=-1;
			}
			if(async_write(CMD_APPEND, metadata, s))
			{
				ret=-1;
			}

			metadata+=s;
			metalen-=s;

			*bytes+=s;
		}
	}
	else
	{
#ifdef HAVE_WIN32
		BFILE bfd;
		binit(&bfd);
		if(bopen(&bfd, fname, O_RDONLY | O_BINARY | O_NOATIME, 0)<0)
		{
			berrno be;
			logp("Could not open %s: %s\n", fname, be.bstrerror(errno));
			return -1;
		}
		while((s=(uint32_t)bread(&bfd, buf, 4096))>0)
		{
			*bytes+=s;
			if(!MD5_Update(&md5, buf, s))
			{
				logp("MD5_Update() failed\n");
				ret=-1;
				break;
			}
			if(async_write(CMD_APPEND, buf, s))
			{
				ret=-1;
				break;
			}
			if(quick_read)
			{
				int qr;
				if((qr=do_quick_read(datapth, cntr))<0)
				{
					ret=-1;
					break;
				}
				if(qr)
				{
					// client wants to interrupt
					break;
				}
			}
		}
		bclose(&bfd);
#else
		FILE *fp=NULL;
	//printf("send_whole_file: %s\n", fname);
		if(!(fp=fopen(fname, "rb")))
		{
			logp("Could not open %s: %s\n", fname, strerror(errno));
			return -1;
		}
		while((s=fread(buf, 1, 4096, fp))>0)
		{
			*bytes+=s;
			if(!MD5_Update(&md5, buf, s))
			{
				logp("MD5_Update() failed\n");
				ret=-1;
				break;
			}
			if(async_write(CMD_APPEND, buf, s))
			{
				ret=-1;
				break;
			}
			if(quick_read)
			{
				int qr;
				if((qr=do_quick_read(datapth, cntr))<0)
				{
					ret=-1;
					break;
				}
				if(qr)
				{
					// client wants to interrupt
					break;
				}
			}
		}
		fclose(fp);
#endif
	}
	if(!ret)
	{
		unsigned char checksum[MD5_DIGEST_LENGTH+1];
		if(!MD5_Final(checksum, &md5))
		{
			logp("MD5_Final() failed\n");
			return -1;
		}
		return write_endfile(*bytes, checksum);
	}
	return ret;
}

int set_non_blocking(int fd)
{
    int flags;
    if((flags = fcntl(fd, F_GETFL, 0))<0) flags = 0;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
     
int set_blocking(int fd)
{
    int flags;
    if((flags = fcntl(fd, F_GETFL, 0))<0) flags = 0;
    return fcntl(fd, F_SETFL, flags | ~O_NONBLOCK);
}

int do_rename(const char *oldpath, const char *newpath)
{
	if(rename(oldpath, newpath))
	{
		logp("could not rename '%s' to '%s': %s\n",
			oldpath, newpath, strerror(errno)); 
		return -1; 
	}
	return 0;
}

char *get_tmp_filename(const char *basis)
{
	return prepend(basis, ".tmp", strlen(".tmp"), 0 /* no slash */);
}

void add_fd_to_sets(int fd, fd_set *read_set, fd_set *write_set, fd_set *err_set, int *max_fd)
{
	if(read_set) FD_SET((unsigned int) fd, read_set);
	if(write_set) FD_SET((unsigned int) fd, write_set);
	if(err_set) FD_SET((unsigned int) fd, err_set);

	if(fd > *max_fd) *max_fd = fd;
}

#if defined(HAVE_WIN32) && !defined(WIN64)
// This first version is for Windows 32 bit, which does not let me use the
// addrinfo stuff in the second version, and therefore does not support
// IPv6 properly.
int init_client_socket(const char *host, const char *port)
{
	int rfd=-1;
	struct sockaddr_in sin;
	struct hostent *hp=NULL;
	int p=atoi(port);

	if(!(hp=gethostbyname(host)))
	{
		logp("unknown host: %s\n", host);
		return -1;
	}

	memset(&sin, 0, sizeof(sin));
	sin.sin_family=AF_INET;
	memcpy((char *)&sin.sin_addr, hp->h_addr, hp->h_length);
	sin.sin_port=htons(p);

	if((rfd=socket(PF_INET, SOCK_STREAM, 0))<0)
	{
		berrno be;
		logp("socket error: %s\n", be.bstrerror());
		return -1;
	}
	if(connect(rfd, (struct sockaddr *)&sin, sizeof(sin))<0)
	{
		logp("could not connect to %s:%s\n", host, port);
		close_fd(&rfd);
		return -1;
	}

#ifdef HAVE_WIN32
	setmode(rfd, O_BINARY);
#endif
	return rfd;
}
#else
// This second version is supposed to do IPv6 properly and does not work
// for 32 bit Windows (but does for 64 bit Windows and unix-style machines)
int init_client_socket(const char *host, const char *port)
{
	int rfd=-1;
	int gai_ret;
	struct addrinfo hints;
	struct addrinfo *result;
	struct addrinfo *rp;

	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = 0;
	hints.ai_protocol = 0;

	if((gai_ret=getaddrinfo(host, port, &hints, &result)))
	{
		logp("getaddrinfo: %s\n", gai_strerror(rfd));
		return -1;
	}

	for(rp=result; rp; rp=rp->ai_next)
	{
		rfd=socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if(rfd<0) continue;
		if(connect(rfd, rp->ai_addr, rp->ai_addrlen) != -1) break;
		close(rfd);
	}
	freeaddrinfo(result);
	if(!rp)
	{
		/* host==NULL and AI_PASSIVE not set -> loopback */
		logp("could not connect to %s:%s\n",
			host?host:"loopback", port);
		close_fd(&rfd);
		return -1;
	}

#ifdef HAVE_WIN32
	setmode(rfd, O_BINARY);
#endif
	return rfd;
}

#endif

void reuseaddr(int fd)
{
	int tmpfd;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
		(sockopt_val_t)&tmpfd, sizeof(tmpfd));
}

void write_status(const char *client, char phase, const char *path, struct cntr *p1cntr, struct cntr *cntr)
{
	static time_t lasttime=0;
	if(status_wfd>=0 && client)
	{
		int l;
		char *w=NULL;
		time_t now=0;
		time_t diff=0;
		static char wbuf[1024]="";

		// Only update every 2 seconds.
		now=time(NULL);
		diff=now-lasttime;
		if(diff<2)
		{
			// Might as well do this in case they fiddled their
			// clock back in time.
			if(diff<0) lasttime=now;
			return;
		}
		lasttime=now;

		snprintf(wbuf, sizeof(wbuf),
			"%s\t%c\t%c\t"
			"%llu/%llu/%llu/%llu\t"
			"%llu/%llu/%llu/%llu\t"
			"%llu/%llu/%llu/%llu\t"
			"%llu/%llu/%llu/%llu\t"
			"%llu/%llu/%llu/%llu\t"
			"%llu/%llu/%llu/%llu\t"
			"%llu/%llu/%llu/%llu\t"
			"%llu/%llu/%llu/%llu\t"
			"%llu/%llu/%llu/%llu\t"
			"%llu/%llu/%llu/%llu\t"
			"%llu\t%llu\t%llu\t%llu\t%llu\t%s\n",
				client, STATUS_RUNNING, phase,

				cntr->total,
				cntr->total_changed,
				cntr->total_same,
				p1cntr->total,

				cntr->file,
				cntr->file_changed,
				cntr->file_same,
				p1cntr->file,

				cntr->enc,
				cntr->enc_changed,
				cntr->enc_same,
				p1cntr->enc,

				cntr->meta,
				cntr->meta_changed,
				cntr->meta_same,
				p1cntr->meta,

				cntr->encmeta,
				cntr->encmeta_changed,
				cntr->encmeta_same,
				p1cntr->encmeta,

				cntr->dir,
				cntr->dir_changed,
				cntr->dir_same,
				p1cntr->dir,

				cntr->slink,
				cntr->slink_changed,
				cntr->slink_same,
				p1cntr->slink,

				cntr->hlink,
				cntr->hlink_changed,
				cntr->hlink_same,
				p1cntr->hlink,

				cntr->special,
				cntr->special_changed,
				cntr->special_same,
				p1cntr->special,

				cntr->total,
				cntr->total_changed,
				cntr->total_same,
				p1cntr->total,

				cntr->gtotal,
				cntr->warning,
				cntr->byte,
				cntr->recvbyte,
				cntr->sentbyte,
				path?path:"");

		// Make sure there is a new line at the end.
		l=strlen(wbuf);
		if(wbuf[l-1]!='\n') wbuf[l-1]='\n';
		w=wbuf;
		while(*w)
		{
			size_t wl=0;
			if((wl=write(status_wfd, w, strlen(w)))<0)
			{
				logp("error writing status down pipe to server: %s\n", strerror(errno));
				close_fd(&status_wfd);
				break;
			}
			w+=wl;
		}
	}
}

static void log_script_output(const char *str, FILE **fp)
{
	char buf[256]="";
	if(fp && *fp)
	{
		while(fgets(buf, sizeof(buf), *fp))
			logp("%s%s", str, buf);
		if(feof(*fp))
		{
			fclose(*fp);
			*fp=NULL;
		}
	}
}

int run_script(const char *script, struct strlist **userargs, int userargc, const char *arg1, const char *arg2, const char *arg3, const char *arg4, const char *arg5, struct cntr *cntr)
{
	int a=0;
	int l=0;
	pid_t p;
	int pid_status=0;
	FILE *serr=NULL;
	FILE *sout=NULL;
	char *cmd[64]={ NULL };

	if(!script) return 0;

	cmd[l++]=(char *)script;
	if(arg1) cmd[l++]=(char *)arg1;
	if(arg2) cmd[l++]=(char *)arg2;
	if(arg3) cmd[l++]=(char *)arg3;
	if(arg4) cmd[l++]=(char *)arg4;
	if(arg5) cmd[l++]=(char *)arg5;
	for(a=0; a<userargc && l<64-1; a++)
	cmd[l++]=userargs[a]->path;
	cmd[l++]=NULL;

	fflush(stdout); fflush(stderr);
	if((p=forkchild(NULL, &sout, &serr, cmd[0], cmd))==-1) return -1;
#ifdef HAVE_WIN32
	// My windows forkchild currently just executes, then returns.
	return 0;
#endif

	do {
		log_script_output("", &sout);
		log_script_output("", &serr);
	} while(!(a=waitpid(p, &pid_status, WNOHANG)));
	log_script_output("", &sout);
	log_script_output("", &serr);

	if(a<0)
	{
		logp("%s waitpid error: %s\n", script, strerror(errno));
		return -1;
	}

	if(WIFEXITED(pid_status))
	{
		int ret=WEXITSTATUS(pid_status);
		logp("%s returned: %d\n", script, ret);
		if(cntr && ret) logw(cntr, "%s returned: %d\n", script, ret);
		return ret;
	}
	else if(WIFSIGNALED(pid_status))
	{
		logp("%s terminated on signal %s\n",
			script, WTERMSIG(pid_status));
		if(cntr) logw(cntr, "%s terminated on signal %s\n",
			script, WTERMSIG(pid_status));
	}
	else
	{
		logp("Strange return when trying to run %s\n", script);
		if(cntr) logw(cntr, "Strange return when trying to run %s\n", script);
	}

	return -1;
}

char *comp_level(struct config *conf)
{
	static char comp[8]="";
	snprintf(comp, sizeof(comp), "wb%d", conf->compression);
	return comp;
}

/* Function based on src/lib/priv.c from bacula. */
int chuser_and_or_chgrp(const char *user, const char *group)
{
#if defined(HAVE_PWD_H) && defined(HAVE_GRP_H)
	struct passwd *passw = NULL;
	struct group *grp = NULL;
	gid_t gid;
	uid_t uid;
	char *username=NULL;

	if(!user && !group) return 0;

	if(user)
	{
		if(!(passw=getpwnam(user)))
		{
			logp("could not find user '%s': %s\n",
				user, strerror(errno));
			return -1;
		}
	}
	else
	{
		if(!(passw=getpwuid(getuid())))
		{
			logp("could not find password entry: %s\n",
				strerror(errno));
			return -1;
		}
		user=passw->pw_name;
	}
	// Any OS uname pointer may get overwritten, so save name, uid, and gid
	if(!(username=strdup(user)))
	{
		logp("out of memory\n");
		return -1;
	}
	uid=passw->pw_uid;
	gid=passw->pw_gid;
	if(group)
	{
		if(!(grp=getgrnam(group)))
		{
			logp("could not find group '%s': %s\n", group,
				strerror(errno));
			free(username);
			return -1;
		}
		gid=grp->gr_gid;
	}
	if(initgroups(username, gid))
	{
		if(grp)
			logp("could not initgroups for group '%s', user '%s': %s\n", group, user, strerror(errno));
		else
			logp("could not initgroups for user '%s': %s\n", user, strerror(errno));
		free(username);
		return -1;
	}
	free(username);
	if(grp)
	{
		if(setgid(gid))
		{
			logp("could not set group '%s': %s\n", group,
				strerror(errno));
			return -1;
		}
	}
	if(setuid(uid))
	{
		logp("could not set specified user '%s': %s\n", username,
			strerror(errno));
		return -1;
	}
#endif
	return 0;
}
