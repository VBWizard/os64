

/*
 *  linux/include/linux/ext2_fs.h
 *
 * Copyright (C) 1992, 1993, 1994, 1995
 * Remy Card (card@masi.ibp.fr)
 * Laboratoire MASI - Institut Blaise Pascal
 * Universite Pierre et Marie Curie (Paris VI)
 *
 *  from
 *
 *  linux/include/linux/minix_fs.h
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

#ifndef _LINUX_EXT2_FS_H
#define _LINUX_EXT2_FS_H

#include <stdint.h>

/*
 * The second extended filesystem constants/structures
 */

/*
 * Define EXT2FS_DEBUG to produce debug messages
 */
#undef EXT2FS_DEBUG

/*
 * Define EXT2_PREALLOCATE to preallocate data blocks for expanding files
 */
#define EXT2_PREALLOCATE

/*
 * The second extended file system version
 */
#define EXT2FS_DATE		"95/08/09"
#define EXT2FS_VERSION		"0.5b"

/*
 * Debug code
 */
#ifdef EXT2FS_DEBUG
#	define ext2_debug(f, a...)	{ \
					printk ("EXT2-fs DEBUG (%s, %d): %s:", \
						__FILE__, __LINE__, __FUNCTION__); \
				  	printk (f, ## a); \
					}
#else
#	define ext2_debug(f, a...)	/**/
#endif

/*
 * Special inodes numbers
 */
#define	EXT2_BAD_INO		 1	/* Bad blocks inode */
#define EXT2_ROOT_INO		 2	/* Root inode */
#define EXT2_ACL_IDX_INO	 3	/* ACL inode */
#define EXT2_ACL_DATA_INO	 4	/* ACL inode */
#define EXT2_BOOT_LOADER_INO	 5	/* Boot loader inode */
#define EXT2_UNDEL_DIR_INO	 6	/* Undelete directory inode */

/* First non-reserved inode for old ext2 filesystems */
#define EXT2_GOOD_OLD_FIRST_INO	11

/*
 * The second extended file system magic number
 */
#define EXT2_SUPER_MAGIC	0xEF53

/*
 * Maximal count of links to a file
 */
#define EXT2_LINK_MAX		32000

/*
 * Macro-instructions used to manage several block sizes
 */
#define EXT2_MIN_BLOCK_SIZE		1024
#define	EXT2_MAX_BLOCK_SIZE		4096
#define EXT2_MIN_BLOCK_LOG_SIZE		  10
#ifdef __KERNEL__
# define EXT2_BLOCK_SIZE(s)		((s)->s_blocksize)
#else
# define EXT2_BLOCK_SIZE(s)		(EXT2_MIN_BLOCK_SIZE << (s)->s_log_block_size)
#endif
#define EXT2_ACLE_PER_BLOCK(s)		(EXT2_BLOCK_SIZE(s) / sizeof (struct ext2_acl_entry))
#define	EXT2_ADDR_PER_BLOCK(s)		(EXT2_BLOCK_SIZE(s) / sizeof (uint32_t))
#ifdef __KERNEL__
# define EXT2_BLOCK_SIZE_BITS(s)	((s)->s_blocksize_bits)
#else
# define EXT2_BLOCK_SIZE_BITS(s)	((s)->s_log_block_size + 10)
#endif
#ifdef __KERNEL__
#define	EXT2_ADDR_PER_BLOCK_BITS(s)	((s)->u.ext2_sb.s_addr_per_block_bits)
#define EXT2_INODE_SIZE(s)		((s)->u.ext2_sb.s_inode_size)
#define EXT2_FIRST_INO(s)		((s)->u.ext2_sb.s_first_ino)
#else
#define EXT2_INODE_SIZE(s)	(((s)->s_rev_level == EXT2_GOOD_OLD_REV) ? \
				 EXT2_GOOD_OLD_INODE_SIZE : \
				 (s)->s_inode_size)
#define EXT2_FIRST_INO(s)	(((s)->s_rev_level == EXT2_GOOD_OLD_REV) ? \
				 EXT2_GOOD_OLD_FIRST_INO : \
				 (s)->s_first_ino)
#endif

/*
 * Macro-instructions used to manage fragments
 */
#define EXT2_MIN_FRAG_SIZE		1024
#define	EXT2_MAX_FRAG_SIZE		4096
#define EXT2_MIN_FRAG_LOG_SIZE		  10
#ifdef __KERNEL__
# define EXT2_FRAG_SIZE(s)		((s)->u.ext2_sb.s_frag_size)
# define EXT2_FRAGS_PER_BLOCK(s)	((s)->u.ext2_sb.s_frags_per_block)
#else
# define EXT2_FRAG_SIZE(s)		(EXT2_MIN_FRAG_SIZE << (s)->s_log_frag_size)
# define EXT2_FRAGS_PER_BLOCK(s)	(EXT2_BLOCK_SIZE(s) / EXT2_FRAG_SIZE(s))
#endif

/*
 * ACL structures
 */
struct ext2_acl_header	/* Header of Access Control Lists */
{
	uint32_t	aclh_size;
	uint32_t	aclh_file_count;
	uint32_t	aclh_acle_count;
	uint32_t	aclh_first_acle;
};

struct ext2_acl_entry	/* Access Control List Entry */
{
	uint32_t	acle_size;
	uint16_t	acle_perms;	/* Access permissions */
	uint16_t	acle_type;	/* Type of entry */
	uint16_t	acle_tag;	/* User or group identity */
	uint16_t	acle_pad1;
	uint32_t	acle_next;	/* Pointer on next entry for the */
					/* same inode or on next free entry */
};

/*
 * Structure of a blocks group descriptor
 */
struct ext2_group_desc
{
	uint32_t	bg_block_bitmap;		/* Blocks bitmap block */
	uint32_t	bg_inode_bitmap;		/* Inodes bitmap block */
	uint32_t	bg_inode_table;		/* Inodes table block */
	uint16_t	bg_free_blocks_count;	/* Free blocks count */
	uint16_t	bg_free_inodes_count;	/* Free inodes count */
	uint16_t	bg_used_dirs_count;	/* Directories count */
	uint16_t	bg_pad;
	uint32_t	bg_reserved[3];
};

/*
 * Macro-instructions used to manage group descriptors
 */
#ifdef __KERNEL__
# define EXT2_BLOCKS_PER_GROUP(s)	((s)->u.ext2_sb.s_blocks_per_group)
# define EXT2_DESC_PER_BLOCK(s)		((s)->u.ext2_sb.s_desc_per_block)
# define EXT2_INODES_PER_GROUP(s)	((s)->u.ext2_sb.s_inodes_per_group)
# define EXT2_DESC_PER_BLOCK_BITS(s)	((s)->u.ext2_sb.s_desc_per_block_bits)
#else
# define EXT2_BLOCKS_PER_GROUP(s)	((s)->s_blocks_per_group)
# define EXT2_DESC_PER_BLOCK(s)		(EXT2_BLOCK_SIZE(s) / sizeof (struct ext2_group_desc))
# define EXT2_INODES_PER_GROUP(s)	((s)->s_inodes_per_group)
#endif

/*
 * Constants relative to the data blocks
 */
#define	EXT2_NDIR_BLOCKS		12
#define	EXT2_IND_BLOCK			EXT2_NDIR_BLOCKS
#define	EXT2_DIND_BLOCK			(EXT2_IND_BLOCK + 1)
#define	EXT2_TIND_BLOCK			(EXT2_DIND_BLOCK + 1)
#define	EXT2_N_BLOCKS			(EXT2_TIND_BLOCK + 1)

/*
 * Inode flags
 */
#define	EXT2_SECRM_FL			0x00000001 /* Secure deletion */
#define	EXT2_UNRM_FL			0x00000002 /* Undelete */
#define	EXT2_COMPR_FL			0x00000004 /* Compress file */
#define EXT2_SYNC_FL			0x00000008 /* Synchronous updates */
#define EXT2_IMMUTABLE_FL		0x00000010 /* Immutable file */
#define EXT2_APPEND_FL			0x00000020 /* writes to file may only append */
#define EXT2_NODUMP_FL			0x00000040 /* do not dump file */
#define EXT2_RESERVED_FL		0x80000000 /* reserved for ext2 lib */
	
/*
 * ioctl commands
 */
#define	EXT2_IOC_GETFLAGS		_IOR('f', 1, long)
#define	EXT2_IOC_SETFLAGS		_IOW('f', 2, long)
#define	EXT2_IOC_GETVERSION		_IOR('v', 1, long)
#define	EXT2_IOC_SETVERSION		_IOW('v', 2, long)


/*
 * Structure of an inode on the disk
 */
typedef struct {

	uint16_t	i_mode;		/* File mode */
	uint16_t	i_uid;		/* Owner Uid */
	uint32_t	i_size;		/* Size in bytes */
	uint32_t	i_atime;	/* Access time */
	uint32_t	i_ctime;	/* Creation time */
	uint32_t	i_mtime;	/* Modification time */
	uint32_t	i_dtime;	/* Deletion Time */
	uint16_t	i_gid;		/* Group Id */
	uint16_t	i_links_count;	/* Links count */
	uint32_t	i_blocks;	/* Blocks count */
	uint32_t	i_flags;	/* File flags */
	union {
		struct {
			uint32_t  l_i_reserved1;
		} linux1;
		struct {
			uint32_t  h_i_translator;
		} hurd1;
		struct {
			uint32_t  m_i_reserved1;
		} masix1;
	} osd1;				/* OS dependent 1 */

	uint32_t	i_block[EXT2_N_BLOCKS];/* Pointers to blocks */
	uint32_t	i_version;	/* File version (for NFS) */
	uint32_t	i_file_acl;	/* File ACL */
	uint32_t	i_dir_acl;	/* Directory ACL */
	uint32_t	i_faddr;	/* Fragment address */
	union {
		struct {
			uint8_t	l_i_frag;	/* Fragment number */
			uint8_t	l_i_fsize;	/* Fragment size */
			uint16_t	i_pad1;
			uint32_t	l_i_reserved2[2];
		} linux2;
		struct {
			uint8_t	h_i_frag;	/* Fragment number */
			uint8_t	h_i_fsize;	/* Fragment size */
			uint16_t	h_i_mode_high;
			uint16_t	h_i_uid_high;
			uint16_t	h_i_gid_high;
			uint32_t	h_i_author;
		} hurd2;
		struct {
			uint8_t	m_i_frag;	/* Fragment number */
			uint8_t	m_i_fsize;	/* Fragment size */
			uint16_t	m_pad1;
			uint32_t	m_i_reserved2[2];
		} masix2;
	} osd2;				/* OS dependent 2 */
} ext2_inode_t;

#if defined(__KERNEL__) || defined(__linux__)
#define i_reserved1	osd1.linux1.l_i_reserved1
#define i_frag		osd2.linux2.l_i_frag
#define i_fsize		osd2.linux2.l_i_fsize
#define i_reserved2	osd2.linux2.l_i_reserved2
#endif

#ifdef	__hurd__
#define i_translator	osd1.hurd1.h_i_translator
#define i_frag		osd2.hurd2.h_i_frag;
#define i_fsize		osd2.hurd2.h_i_fsize;
#define i_uid_high	osd2.hurd2.h_i_uid_high
#define i_gid_high	osd2.hurd2.h_i_gid_high
#define i_author	osd2.hurd2.h_i_author
#endif

#ifdef	__masix__
#define i_reserved1	osd1.masix1.m_i_reserved1
#define i_frag		osd2.masix2.m_i_frag
#define i_fsize		osd2.masix2.m_i_fsize
#define i_reserved2	osd2.masix2.m_i_reserved2
#endif

/*
 * File system states
 */
#define	EXT2_VALID_FS			0x0001	/* Unmounted cleanly */
#define	EXT2_ERROR_FS			0x0002	/* Errors detected */

/*
 * Mount flags
 */
#define EXT2_MOUNT_CHECK_NORMAL		0x0001	/* Do some more checks */
#define EXT2_MOUNT_CHECK_STRICT		0x0002	/* Do again more checks */
#define EXT2_MOUNT_CHECK		(EXT2_MOUNT_CHECK_NORMAL | \
					 EXT2_MOUNT_CHECK_STRICT)
#define EXT2_MOUNT_GRPID		0x0004	/* Create files with directory's group */
#define EXT2_MOUNT_DEBUG		0x0008	/* Some debugging messages */
#define EXT2_MOUNT_ERRORS_CONT		0x0010	/* Continue on errors */
#define EXT2_MOUNT_ERRORS_RO		0x0020	/* Remount fs ro on errors */
#define EXT2_MOUNT_ERRORS_PANIC		0x0040	/* Panic on errors */
#define EXT2_MOUNT_MINIX_DF		0x0080	/* Mimics the Minix statfs */

#define clear_opt(o, opt)		o &= ~EXT2_MOUNT_##opt
#define set_opt(o, opt)			o |= EXT2_MOUNT_##opt
#define test_opt(sb, opt)		((sb)->u.ext2_sb.s_mount_opt & \
					 EXT2_MOUNT_##opt)
/*
 * Maximal mount counts between two filesystem checks
 */
#define EXT2_DFL_MAX_MNT_COUNT		20	/* Allow 20 mounts */
#define EXT2_DFL_CHECKINTERVAL		0	/* Don't use interval check */

/*
 * Behaviour when detecting errors
 */
#define EXT2_ERRORS_CONTINUE		1	/* Continue execution */
#define EXT2_ERRORS_RO			2	/* Remount fs read-only */
#define EXT2_ERRORS_PANIC		3	/* Panic */
#define EXT2_ERRORS_DEFAULT		EXT2_ERRORS_CONTINUE

typedef struct ext2_super_block ext2_super_block_t;

/*
 * Structure of the super block
 */
struct ext2_super_block {
	uint32_t	s_inodes_count;		/* Inodes count */
	uint32_t	s_blocks_count;		/* Blocks count */
	uint32_t	s_r_blocks_count;	/* Reserved blocks count */
	uint32_t	s_free_blocks_count;	/* Free blocks count */
	uint32_t	s_free_inodes_count;	/* Free inodes count */
	uint32_t	s_first_data_block;	/* First Data Block */
	uint32_t	s_log_block_size;	/* Block size */
	int32_t	s_log_frag_size;	/* Fragment size */
	uint32_t	s_blocks_per_group;	/* # Blocks per group */
	uint32_t	s_frags_per_group;	/* # Fragments per group */
	uint32_t	s_inodes_per_group;	/* # Inodes per group */
	uint32_t	s_mtime;		/* Mount time */
	uint32_t	s_wtime;		/* Write time */
	uint16_t	s_mnt_count;		/* Mount count */
	int16_t	s_max_mnt_count;	/* Maximal mount count */
	uint16_t	s_magic;		/* Magic signature */
	uint16_t	s_state;		/* File system state */
	uint16_t	s_errors;		/* Behaviour when detecting errors */
	uint16_t	s_minor_rev_level; 	/* minor revision level */
	uint32_t	s_lastcheck;		/* time of last check */
	uint32_t	s_checkinterval;	/* max. time between checks */
	uint32_t	s_creator_os;		/* OS */
	uint32_t	s_rev_level;		/* Revision level */
	uint16_t	s_def_resuid;		/* Default uid for reserved blocks */
	uint16_t	s_def_resgid;		/* Default gid for reserved blocks */
	/*
	 * These fields are for EXT2_DYNAMIC_REV superblocks only.
	 *
	 * Note: the difference between the compatible feature set and
	 * the incompatible feature set is that if there is a bit set
	 * in the incompatible feature set that the kernel doesn't
	 * know about, it should refuse to mount the filesystem.
	 * 
	 * e2fsck's requirements are more strict; if it doesn't know
	 * about a feature in either the compatible or incompatible
	 * feature set, it must abort and not try to meddle with
	 * things it doesn't understand...
	 */
	uint32_t	s_first_ino; 		/* First non-reserved inode */
	uint16_t   s_inode_size; 		/* size of inode structure */
	uint16_t	s_block_group_nr; 	/* block group # of this superblock */
	uint32_t	s_feature_compat; 	/* compatible feature set */
	uint32_t	s_feature_incompat; 	/* incompatible feature set */
	uint32_t	s_feature_ro_compat; 	/* readonly-compatible feature set */
	uint32_t	s_reserved[230];	/* Padding to the end of the block */
};

/*
 * Codes for operating systems
 */
#define EXT2_OS_LINUX		0
#define EXT2_OS_HURD		1
#define EXT2_OS_MASIX		2
#define EXT2_OS_FREEBSD		3
#define EXT2_OS_LITES		4

/*
 * Revision levels
 */
#define EXT2_GOOD_OLD_REV	0	/* The good old (original) format */
#define EXT2_DYNAMIC_REV	1 	/* V2 format w/ dynamic inode sizes */

#define EXT2_CURRENT_REV	EXT2_GOOD_OLD_REV
#define EXT2_MAX_SUPP_REV	EXT2_DYNAMIC_REV

#define EXT2_GOOD_OLD_INODE_SIZE 128

/*
 * Default values for user and/or group using reserved blocks
 */
#define	EXT2_DEF_RESUID		0
#define	EXT2_DEF_RESGID		0

/*
 * Structure of a directory entry
 */
#define EXT2_NAME_LEN 255

struct ext2_dir_entry {
	uint32_t	inode;			/* Inode number */
	uint16_t	rec_len;		/* Directory entry length */
	uint8_t	name_len;		/* Name length */
        uint8_t    file_type;
	char	name[EXT2_NAME_LEN];	/* File name */
};

/*
 * EXT2_DIR_PAD defines the directory entries boundaries
 *
 * NOTE: It must be a multiple of 4
 */
#define EXT2_DIR_PAD		 	4
#define EXT2_DIR_ROUND 			(EXT2_DIR_PAD - 1)
#define EXT2_DIR_REC_LEN(name_len)	(((name_len) + 8 + EXT2_DIR_ROUND) & \
					 ~EXT2_DIR_ROUND)

/*
 * Feature set definitions --- none are defined as of now
 */
#define EXT2_FEATURE_COMPAT_SUPP	0
#define EXT2_FEATURE_INCOMPAT_SUPP	0
#define EXT2_FEATURE_RO_COMPAT_SUPP	0

#ifdef __KERNEL__
/*
 * Function prototypes
 */

/*
 * Ok, these declarations are also in <linux/kernel.h> but none of the
 * ext2 source programs needs to include it so they are duplicated here.
 */
# define NORET_TYPE    /**/
# define ATTRIB_NORET  __attribute__((noreturn))
# define NORET_AND     noreturn,

/* acl.c */
extern int ext2_permission (struct inode *, int);

/* balloc.c */
extern int ext2_new_block (const struct inode *, unsigned long,
			   uint32_t *, uint32_t *, int *);
extern void ext2_free_blocks (const struct inode *, unsigned long,
			      unsigned long);
extern unsigned long ext2_count_free_blocks (struct super_block *);
extern void ext2_check_blocks_bitmap (struct super_block *);

/* bitmap.c */
extern unsigned long ext2_count_free (struct buffer_head *, unsigned);

/* dir.c */
extern int ext2_check_dir_entry (const char *, struct inode *,
				 struct ext2_dir_entry *, struct buffer_head *,
				 unsigned long);

/* file.c */
extern int ext2_read (struct inode *, struct file *, char *, int);
extern int ext2_write (struct inode *, struct file *, char *, int);

/* fsync.c */
extern int ext2_sync_file (struct inode *, struct file *);

/* ialloc.c */
extern struct inode * ext2_new_inode (const struct inode *, int, int *);
extern void ext2_free_inode (struct inode *);
extern unsigned long ext2_count_free_inodes (struct super_block *);
extern void ext2_check_inodes_bitmap (struct super_block *);

/* inode.c */
extern int ext2_bmap (struct inode *, int);

extern struct buffer_head * ext2_getblk (struct inode *, long, int, int *);
extern struct buffer_head * ext2_bread (struct inode *, int, int, int *);

extern int ext2_getcluster (struct inode * inode, long block);
extern void ext2_read_inode (struct inode *);
extern void ext2_write_inode (struct inode *);
extern void ext2_put_inode (struct inode *);
extern int ext2_sync_inode (struct inode *);
extern void ext2_discard_prealloc (struct inode *);

/* ioctl.c */
extern int ext2_ioctl (struct inode *, struct file *, unsigned int,
		       unsigned long);

/* namei.c */
extern void ext2_release (struct inode *, struct file *);
extern int ext2_lookup (struct inode *,const char *, int, struct inode **);
extern int ext2_create (struct inode *,const char *, int, int,
			struct inode **);
extern int ext2_mkdir (struct inode *, const char *, int, int);
extern int ext2_rmdir (struct inode *, const char *, int);
extern int ext2_unlink (struct inode *, const char *, int);
extern int ext2_symlink (struct inode *, const char *, int, const char *);
extern int ext2_link (struct inode *, struct inode *, const char *, int);
extern int ext2_mknod (struct inode *, const char *, int, int, int);
extern int ext2_rename (struct inode *, const char *, int,
			struct inode *, const char *, int, int);

/* super.c */
extern void ext2_error (struct super_block *, const char *, const char *, ...)
	__attribute__ ((format (printf, 3, 4)));
extern NORET_TYPE void ext2_panic (struct super_block *, const char *,
				   const char *, ...)
	__attribute__ ((NORET_AND format (printf, 3, 4)));
extern void ext2_warning (struct super_block *, const char *, const char *, ...)
	__attribute__ ((format (printf, 3, 4)));
extern void ext2_put_super (struct super_block *);
extern void ext2_write_super (struct super_block *);
extern int ext2_remount (struct super_block *, int *, char *);
extern struct super_block * ext2_read_super (struct super_block *,void *,int);
extern int init_ext2_fs(void);
extern void ext2_statfs (struct super_block *, struct statfs *, int);

/* truncate.c */
extern void ext2_truncate (struct inode *);

/*
 * Inodes and files operations
 */

/* dir.c */
extern struct inode_operations ext2_dir_inode_operations;

/* file.c */
extern struct inode_operations ext2_file_inode_operations;

/* symlink.c */
extern struct inode_operations ext2_symlink_inode_operations;

#endif	/* __KERNEL__ */

#endif	/* _LINUX_EXT2_FS_H */
