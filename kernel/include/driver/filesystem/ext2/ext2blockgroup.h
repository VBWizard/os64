/* 
 * File:   extblockgroup.h
 * Author: yogi
 *
 * Created on October 16, 2017, 2:59 AM
 */

#ifndef EXTBLOCKGROUP_H
#define	EXTBLOCKGROUP_H

#ifdef	__cplusplus
extern "C" {
#endif

    void getBlockGroupDescriptor(int sess, int blockGroupNum, struct ext2_group_desc* gd);

#ifdef	__cplusplus
}
#endif

#endif	/* EXTBLOCKGROUP_H */

