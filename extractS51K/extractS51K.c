/*
   This program extracts files from S51K filesystem (big-endian).
   Written by Alexander V. Lukyanov using parts of linux kernel (Dec 1999).
   This code is covered by GNU GPL.
*/

#include <stdio.h>
#include <assert.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <utime.h>

typedef int s32;
typedef unsigned u32;
typedef unsigned short u16;
typedef short s16;
typedef u16 sysv_ino_t;

#ifdef __GNUC__
#define __packed2__  __attribute__ ((packed, aligned(2)))
#else
#error I want gcc!
#endif

/* Among the inodes ... */
/* 0 is non-existent */
#define SYSV_BADBL_INO  1       /* inode of bad blocks file */
#define SYSV_ROOT_INO   2       /* inode of root directory */
        
#define SYSV_NICINOD    100     /* number of inode cache entries */
#define SYSV_NICFREE    50      /* number of free block list chunk entries */

/* SystemV2 super-block data on disk */
struct sysv2_super_block {
        u16     s_isize;                /* index of first data zone */
        u32     s_fsize __packed2__;    /* total number of zones of this fs */
        /* the start of the free block list: */
        u16     s_nfree;                /* number of free blocks in s_free, <= SYSV_NICFREE */
        u32     s_free[SYSV_NICFREE];   /* first free block list chunk */
        /* the cache of free inodes: */
        u16     s_ninode;               /* number of free inodes in s_inode, <= SYSV_NICINOD */
        sysv_ino_t     s_inode[SYSV_NICINOD]; /* some free inodes */
        /* locks, not used by Linux: */
        char    s_flock;                /* lock during free block list manipulation */
        char    s_ilock;                /* lock during inode cache manipulation */
        char    s_fmod;                 /* super-block modified flag */
        char    s_ronly;                /* flag whether fs is mounted read-only */
        u32     s_time __packed2__;     /* time of last super block update */
        s16     s_dinfo[4];             /* device information ?? */
        u32     s_tfree __packed2__;    /* total number of free zones */
        u16     s_tinode;               /* total number of free inodes */
        char    s_fname[6];             /* file system volume name */
        char    s_fpack[6];             /* file system pack name */
        s32     s_fill[14];
        s32     s_state;                /* file system state: 0xcb096f43 means clean */
        s32     s_magic;                /* version of file system */
        s32     s_type;                 /* type of file system: 1 for 512 byte blocks
                                                                2 for 1024 byte blocks */
};

/* SystemV/Coherent inode data on disk */

struct sysv_inode {
        u16 i_mode;
        u16 i_nlink;
        u16 i_uid;
        u16 i_gid;
        u32 i_size;
        union { /* directories, regular files, ... */
                unsigned char i_addb[3*(10+1+1+1)+1]; /* zone numbers: max. 10 data blocks,
                                              * then 1 indirection block,
                                              * then 1 double indirection block,
                                              * then 1 triple indirection block.
                                              * Then maybe a "file generation number" ??
                                              */
                /* devices */
                u16 i_rdev;
                /* named pipes on Coherent */
                struct {
                        char p_addp[30];
                        s16 p_pnc;
                        s16 p_prx;
                        s16 p_pwx;
                } i_p;
        } i_a;
        u32 i_atime;    /* time of last access */  
        u32 i_mtime;    /* time of last modification */
        u32 i_ctime;    /* time of creation */
};

/* SystemV/Coherent directory entry on disk */

#define SYSV_NAMELEN    14      /* max size of name in struct sysv_dir_entry */

struct sysv_dir_entry {
        sysv_ino_t inode;
        char name[SYSV_NAMELEN]; /* up to 14 characters, the rest are zeroes */
};


#define		BSIZE	1024
#define		INOPB	(BSIZE/sizeof(struct sysv_inode))

char *used_inodes[65536];

void create_inode(struct sysv_inode *i,const char *path);
void process_directory(struct sysv_inode *i,const char *path);
struct sysv_inode *get_inode(int n);
u16 fs2host16(u16);
u32 fs2host32(u32);

char *map;
int offset;

int main(int argc,char **argv)
{
	char *file;
	int hd;
	struct stat st;
	struct sysv_inode *root_ino;
	struct sysv2_super_block *sb;

	if(argc<2)
	{
		printf("Usage: %s file [offset]\n",argv[0]);
		return 1;
	}
	
	file=argv[1];
	hd=open(file,O_RDONLY);
	if(hd==-1)
	{
		perror(file);
		return 1;
	}

	if(argc>=3)
		offset=strtol(argv[2],0,0);

	printf("Using offset 0x%08x\n",offset);
	
	fstat(hd,&st);
	map=mmap(0,st.st_size,PROT_READ,MAP_SHARED,hd,0);
	if(map==MAP_FAILED)
	{
		perror("mmap");
		return 1;
	}
	
	sb=(struct sysv2_super_block*)(map+offset+512);
	if(fs2host32(sb->s_magic)!=0xFD187E20
	|| fs2host32(sb->s_type) !=2)
	{
		fprintf(stderr,"No S51K filesystem found (magic=0x%08X)\n",
			fs2host32(sb->s_magic));
		return 1;
	}

	root_ino=get_inode(SYSV_ROOT_INO);
	used_inodes[SYSV_ROOT_INO]=strdup("root");
	create_inode(root_ino,"root");

	return 0;
}

struct sysv_inode *get_inode(int n)
{
	return (struct sysv_inode*)
		(map+offset+BSIZE*2+sizeof(struct sysv_inode)*(n-1));
}

int read3bytes(unsigned char *a,int n)
{
	return ((((a[n*3]<<8)+a[n*3+1])<<8)+a[n*3+2]);
}

int read4bytes(unsigned char *a,int n)
{
	return ((((((a[n*4]<<8)+a[n*4+1])<<8)+a[n*4+2])<<8)+a[n*4+3]);
}

char *abs_block(int n)
{
	return map+offset+n*BSIZE;
}

char *inode_block(struct sysv_inode *i, int n)
{
	if(n<10)
		return abs_block(read3bytes(i->i_a.i_addb,n));

	if(n<10+BSIZE/4)
		return abs_block(read4bytes(
			abs_block(read3bytes(i->i_a.i_addb,10)),n-10));
			
	if(n<10+BSIZE/4+BSIZE/4*BSIZE/4)
		return abs_block(read4bytes(
			abs_block(read4bytes(
			 abs_block(read3bytes(i->i_a.i_addb,11)),
					(n-10-BSIZE/4)/(BSIZE/4))),
					(n-10-BSIZE/4)%(BSIZE/4)));
					
	/* triple indirect blocks... */
	abort();
}

void create_inode(struct sysv_inode *i,const char *path)
{
	struct utimbuf ut;
	int mode=fs2host16(i->i_mode);
	long size=fs2host32(i->i_size);
	puts(path);
	fflush(stdout);
	if(S_ISDIR(mode))
	{
		mkdir(path,mode);
		process_directory(i,path);
	}
	else if(((mode&S_IFMT)==0) || ((mode&S_IFMT)==S_IFREG))
	{
		int nblk=(size+BSIZE-1)/BSIZE;
		int n;
		int last=size%BSIZE;
		
		int fd=open(path,O_WRONLY|O_CREAT,mode);
		if(fd==-1)
		{
			perror(path);
			return;
		}
		
		for(n=0; n<nblk; n++)
		{
			char *e=inode_block(i,n);
			int limit=BSIZE;
			if(n==nblk-1)
				limit=last;
			write(fd,e,limit);
		}
		close(fd);
	}
	chown(path,fs2host16(i->i_uid),fs2host16(i->i_gid));
	ut.actime =fs2host32(i->i_atime);
	ut.modtime=fs2host32(i->i_mtime);
	utime(path,&ut);
}

void process_directory(struct sysv_inode *i,const char *path)
{
	long size=fs2host32(i->i_size);
	int nblk=(size+BSIZE-1)/BSIZE;
	int n,j;
	char *path1=alloca(strlen(path)+1+SYSV_NAMELEN+1);
	char *add;
	char *slash;
	int last=size%BSIZE;
	
	for(n=0; n<nblk; n++)
	{
		struct sysv_dir_entry *e=
				(struct sysv_dir_entry*)inode_block(i,n);
		int limit=BSIZE/sizeof(*e);
		if(n==nblk-1)
			limit=last/sizeof(*e);
		for(j=0; j<limit; j++,e++)
		{
			int inode=fs2host16(e->inode);
			if(inode!=0 && e->name[0])
			{
				strcpy(path1,path);
				strcat(path1,"/");
				add=path1+strlen(path1);
				strncpy(add,e->name,SYSV_NAMELEN);
				add[SYSV_NAMELEN]=0;
				while((slash=strchr(add,'/')))
					*slash='_';
				if(used_inodes[inode])
					link(used_inodes[inode],path1);
				else {
				  used_inodes[inode]=strdup(path1);
				  create_inode(get_inode(inode),path1);
				}
			}
		}
	}
}

u16 fs2host16(u16 a)
{
	unsigned char *u=(unsigned char*)&a;
	return (u[0]*256+u[1]);
}
u32 fs2host32(u32 a)
{
	unsigned char *u=(unsigned char*)&a;
	return (((u[0]*256+u[1])*256+u[2])*256+u[3]);
}
