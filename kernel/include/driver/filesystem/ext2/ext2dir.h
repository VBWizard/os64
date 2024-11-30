/* 
 * File:   ext2dir.h
 * Author: yogi
 *
 * Created on October 16, 2017, 3:16 AM
 */

#ifndef EXT2DIR_H
#define	EXT2DIR_H

#ifdef	__cplusplus
extern "C" {
#endif

#include "ext2.h"

    int getDirEntry(sFile *file, ext2_inode_t parent, char* path, int* fileType);
    int getDir(sFile *file,  ext2_inode_t parent, char* dirBuffer, int bufferSize);


#ifdef	__cplusplus
}
#endif

#endif	/* EXT2DIR_H */

