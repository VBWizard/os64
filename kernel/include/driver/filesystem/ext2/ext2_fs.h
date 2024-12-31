

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


#define ERROR_BAD_EXT2_MAGIC 0x1

#define EXT2_NAME_LEN 255
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
typedef struct
{
	uint32_t	bg_block_bitmap;		/* Blocks bitmap block */
	uint32_t	bg_inode_bitmap;		/* Inodes bitmap block */
	uint32_t	bg_inode_table;		/* Inodes table block */
	uint16_t	bg_free_blocks_count;	/* Free blocks count */
	uint16_t	bg_free_inodes_count;	/* Free inodes count */
	uint16_t	bg_used_dirs_count;	/* Directories count */
	uint16_t	bg_pad;
	uint32_t	bg_reserved[3];
} ext2_group_desc_t;

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
#include <stdint.h>

#pragma pack(push, 1) // Using pragma pack to ensure no padding is added.
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    EXT2_FILE_TYPE_UNKNOWN = 0x0,    // Unknown file type
    EXT2_FILE_TYPE_REGULAR = 0x1,    // Regular file
    EXT2_FILE_TYPE_DIRECTORY = 0x2, // Directory
    EXT2_FILE_TYPE_CHAR_DEVICE = 0x3, // Character device
    EXT2_FILE_TYPE_LINUX_DIRECTORY = 0x4, // Block device
    EXT2_FILE_TYPE_FIFO = 0x5,       // FIFO (pipe)
    EXT2_FILE_TYPE_SOCKET = 0x6,     // Socket
    EXT2_FILE_TYPE_SYMLINK = 0x7     // Symbolic link
} E_EXT2_INODE_TYPE_T;

typedef struct {
    union {
        uint16_t i_mode; // Raw i_mode value
        struct {
            uint16_t other_execute : 1;     // Other execute permission
            uint16_t other_write   : 1;     // Other write permission
            uint16_t other_read    : 1;     // Other read permission
            uint16_t group_execute : 1;     // Group execute permission
            uint16_t group_write   : 1;     // Group write permission
            uint16_t group_read    : 1;     // Group read permission
            uint16_t owner_execute : 1;     // Owner execute permission
            uint16_t owner_write   : 1;     // Owner write permission
            uint16_t owner_read    : 1;     // Owner read permission
            uint16_t setuid        : 1;     // Set UID
            uint16_t setgid        : 1;     // Set GID
            uint16_t sticky        : 1;     // Sticky bit
            E_EXT2_INODE_TYPE_T file_type     : 4;     // File type (bits 15–12)
        };
    };
    uint16_t i_uid;        // Lower 16 bits of Owner UID
    uint32_t i_size;       // File size in bytes (lower 32 bits)
    uint32_t i_atime;      // Last access time
    uint32_t i_ctime;      // Creation time
    uint32_t i_mtime;      // Last modification time
    uint32_t i_dtime;      // Deletion time
    uint16_t i_gid;        // Lower 16 bits of Group ID
    uint16_t i_links_count; // Number of hard links
    uint32_t i_blocks;     // Number of 512-byte blocks reserved for the file
    uint32_t i_flags;      // File flags
    uint32_t i_osd1;       // OS-specific value (unused in most cases)

    uint32_t i_block[15];  // Pointers to blocks (12 direct, 1 singly indirect, 1 doubly indirect, 1 triply indirect)
    uint32_t i_generation; // File version (used by NFS)
    uint32_t i_file_acl;   // File ACL (extended attributes for ext2)
    uint32_t i_dir_acl;    // Directory ACL (only for directories, otherwise 0)
    uint32_t i_faddr;      // Fragment address (unused in most implementations)
    uint8_t  i_osd2[12];   // OS-specific value (usually zero in ext2)
} ext2_inode_t;


#pragma pack(pop) // Resetting the pack value.

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

typedef struct {
    uint32_t    inode;        /* Inode number */
    uint16_t    rec_len;      /* Directory entry length */
    uint8_t     name_len;     /* Name length */
    uint8_t     file_type;     /* File type */
    char        name[EXT2_NAME_LEN];   /* File name */
} __attribute__((packed)) ext2_dir_entry_2_t;

#endif	/* _LINUX_EXT2_FS_H */

