#include "include.h"
#include "../cmd.h"

static int read_fp_msg(FILE *fp, gzFile zp, char **buf, size_t len)
{
	char *b=NULL;
	ssize_t r=0;

	/* Now we know how long the data is, so read it. */
	if(!(*buf=(char *)malloc_w(len+1, __func__)))
		return -1;

	b=*buf;
	while(len>0)
	{
		if((zp && (r=gzread(zp, b, len))<=0)
		  || (fp && (r=fread(b, 1, len, fp))<=0))
		{
			if(*buf) free(*buf);
			*buf=NULL;
			if(r==0)
			{
				if(zp && gzeof(zp)) return 1;
				if(fp && feof(fp)) return 1;
			}
			return -1;
		}
		b+=r;
		len-=r;
	}
	*b='\0';
	return 0;
}

static int read_fp(FILE *fp, gzFile zp, struct iobuf *rbuf)
{
	int asr;
	unsigned int r;
	char *tmp=NULL;

	// First, get the command and length
	if((asr=read_fp_msg(fp, zp, &tmp, 5)))
	{
		if(tmp) free(tmp);
		return asr;
	}

	if((sscanf(tmp, "%c%04X", (char *)&rbuf->cmd, &r))!=2)
	{
		logp("sscanf of '%s' failed\n", tmp);
		if(tmp) free(tmp);
		return -1;
	}
	rbuf->len=r;
	if(tmp) free(tmp);

	if(!(asr=read_fp_msg(fp,
		zp, &rbuf->buf, rbuf->len+1))) // +1 for '\n'
			rbuf->buf[rbuf->len]='\0'; // remove new line.

	return asr;
}

static int unexpected(struct iobuf *rbuf, const char *func)
{
	iobuf_log_unexpected(rbuf, func);
	iobuf_free_content(rbuf);
	return -1;
}

static int read_stat(struct asfd *asfd, struct iobuf *rbuf, FILE *fp,
	gzFile zp, struct sbuf *sb, struct cntr *cntr)
{
	while(1)
	{
		iobuf_free_content(rbuf);
		if(fp || zp)
		{
			int asr;
			if((asr=read_fp(fp, zp, rbuf)))
			{
				//logp("read_fp returned: %d\n", asr);
				return asr;
			}
			if(rbuf->buf[rbuf->len]=='\n')
				rbuf->buf[rbuf->len]='\0';
		}
		else
		{
			if(asfd->read(asfd))
			{
				break;
			}
			if(rbuf->cmd==CMD_WARNING)
			{
				logp("WARNING: %s\n", rbuf->buf);
				cntr_add(cntr, rbuf->cmd, 0);
				continue;
			}
		}
		if(rbuf->cmd==CMD_DATAPTH)
		{
			iobuf_move(&(sb->protocol1->datapth), rbuf);
		}
		else if(rbuf->cmd==CMD_ATTRIBS)
		{
			iobuf_move(&sb->attr, rbuf);
			attribs_decode(sb);

			return 0;
		}
		else if((rbuf->cmd==CMD_GEN && !strcmp(rbuf->buf, "backupend"))
		  || (rbuf->cmd==CMD_GEN && !strcmp(rbuf->buf, "restoreend"))
		  || (rbuf->cmd==CMD_GEN && !strcmp(rbuf->buf, "phase1end"))
		  || (rbuf->cmd==CMD_GEN && !strcmp(rbuf->buf, "backupphase2"))
		  || (rbuf->cmd==CMD_GEN && !strcmp(rbuf->buf, "estimateend")))
		{
			iobuf_free_content(rbuf);
			return 1;
		}
		else
			return unexpected(rbuf, __func__);
	}
	iobuf_free_content(rbuf);
	return -1;
}

static int do_sbufl_fill_from_net(struct sbuf *sb, struct asfd *asfd,
	struct cntr *cntr)
{
	int ars;
	static struct iobuf *rbuf=NULL;
	rbuf=asfd->rbuf;
	iobuf_free_content(rbuf);
	if((ars=read_stat(asfd, rbuf, NULL, NULL, sb, cntr))
	  || (ars=asfd->read(asfd))) return ars;
	iobuf_move(&sb->path, rbuf);
	if(sbuf_is_link(sb))
	{
		if((ars=asfd->read(asfd))) return ars;
		iobuf_move(&sb->link, rbuf);
		if(!cmd_is_link(rbuf->cmd))
			return unexpected(rbuf, __func__);
	}
	return 0;
}

static int do_sbufl_fill_from_file(struct sbuf *sb, FILE *fp, gzFile zp,
	int phase1, struct cntr *cntr)
{
	int ars;
	struct iobuf rbuf;
	//free_sbufl(sb);
	iobuf_init(&rbuf);
	if((ars=read_stat(NULL /* no async */,
		&rbuf, fp, zp, sb, cntr))) return ars;
	if((ars=read_fp(fp, zp, &rbuf))) return ars;
	iobuf_move(&sb->path, &rbuf);
	if(sbuf_is_link(sb))
	{
		if((ars=read_fp(fp, zp, &rbuf))) return ars;
		iobuf_move(&sb->link, &rbuf);
		if(!cmd_is_link(rbuf.cmd))
			return unexpected(&rbuf, __func__);
	}
	else if(!phase1 && sbuf_is_filedata(sb))
	{
		if((ars=read_fp(fp, zp, &rbuf))) return ars;
		iobuf_move(&(sb->protocol1->endfile), &rbuf);
		if(!cmd_is_endfile(rbuf.cmd))
			return unexpected(&rbuf, __func__);
	}
	return 0;
}

int sbufl_fill(struct sbuf *sb, struct asfd *asfd,
	FILE *fp, gzFile zp, struct cntr *cntr)
{
	if(fp || zp) return do_sbufl_fill_from_file(sb, fp, zp, 0, cntr);
	return do_sbufl_fill_from_net(sb, asfd, cntr);
}

int sbufl_fill_phase1(struct sbuf *sb, FILE *fp, gzFile zp, struct cntr *cntr)
{
	return do_sbufl_fill_from_file(sb, fp, zp, 1, cntr);
}

static int sbufl_to_fp(struct sbuf *sb, FILE *mp, int write_endfile)
{
	if(!sb->path.buf) return 0;
	if(sb->protocol1 && sb->protocol1->datapth.buf
	  && iobuf_send_msg_fp(&(sb->protocol1->datapth), mp))
		return -1;
	if(iobuf_send_msg_fp(&sb->attr, mp)
	  || iobuf_send_msg_fp(&sb->path, mp))
		return -1;
	if(sb->link.buf
	  && iobuf_send_msg_fp(&sb->link, mp))
		return -1;
	if(write_endfile
	  && sbuf_is_filedata(sb)
	  && sb->protocol1
	  && iobuf_send_msg_fp(&sb->protocol1->endfile, mp))
		return -1;
	return 0;
}

static int sbufl_to_zp(struct sbuf *sb, gzFile zp, int write_endfile)
{
	if(!sb->path.buf) return 0;
	if(sb->protocol1 && sb->protocol1->datapth.buf
	  && iobuf_send_msg_zp(&(sb->protocol1->datapth), zp))
		return -1;
	if(iobuf_send_msg_zp(&sb->attr, zp)
	  || iobuf_send_msg_zp(&sb->path, zp))
		return -1;
	if(sb->link.buf
	  && iobuf_send_msg_zp(&sb->link, zp))
		return -1;
	if(write_endfile
	  && sbuf_is_filedata(sb)
	  && sb->protocol1
	  && iobuf_send_msg_zp(&sb->protocol1->endfile, zp))
		return -1;
	return 0;
}

int sbufl_to_manifest(struct sbuf *sb, FILE *mp, gzFile zp)
{
	if(mp) return sbufl_to_fp(sb, mp, 1);
	if(zp) return sbufl_to_zp(sb, zp, 1);
	logp("No valid file pointer given to sbufl_to_manifest()\n");
	return -1;
}

int sbufl_to_manifest_phase1(struct sbuf *sb, FILE *mp, gzFile zp)
{
	if(mp) return sbufl_to_fp(sb, mp, 0);
	if(zp) return sbufl_to_zp(sb, zp, 0);
	logp("No valid file pointer given to sbufl_to_manifest_phase1()\n");
	return -1;
}
