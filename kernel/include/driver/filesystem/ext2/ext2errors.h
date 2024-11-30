/* 
 * File:   ext2Errors.h
 * Author: yogi
 *
 * Created on October 13, 2017, 6:21 PM
 */

#ifndef EXT2ERRORS_H
#define	EXT2ERRORS_H

#ifdef	__cplusplus
extern "C" {
#endif

#define ERROR_NO_FREE_SESSIONS -1
#define ERROR_NO_FREE_FILE_HANDLE -2
#define ERROR_INVALID_FILE_HANDLE -3
#define ERROR_BAD_EXT2_MAGIC -4
#define ERROR_INVALID_DEVICE -5 //Invalid device path passed to initInstance
#define ERROR_INODE_NOT_FOUND -6
#ifdef	__cplusplus
}
#endif

#endif	/* EXT2ERRORS_H */

