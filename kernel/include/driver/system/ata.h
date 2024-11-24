/* 
 * File:   atahd.h
 * Author: yogi
 *
 * Created on May 3, 2016, 6:19 PM
 * Generic ATA library for use in booting an OS
 */

/*1F0 (Read and Write): Data Register
1F1 (Read): Error Register
1F1 (Write): Features Register
1F2 (Read and Write): Sector Count Register
1F3 (Read and Write): LBA Low Register
1F4 (Read and Write): LBA Mid Register
1F5 (Read and Write): LBA High Register
1F6 (Read and Write): Drive/Head Register
1F7 (Read): Status Register
1F7 (Write): Command Register
3F6 (Read): Alternate Status Register
3F6 (Write): Device Control Register*/

/*The status register is an 8-bit register which contains the following bits, listed in order from left to right:
BSY (busy)
DRDY (device ready)
DF (Device Fault)
DSC (seek complete)
DRQ (Data Transfer Requested)
CORR (data corrected)
IDX (index mark)
ERR (error)*/

/*The error register is also an 8-bit register, and contains the following bits, again listed in order from left to right:
BBK (Bad Block)
UNC (Uncorrectable data error)
MC (Media Changed)
IDNF (ID mark Not Found)
MCR (Media Change Requested)
ABRT (command aborted)
TK0NF (Track 0 Not Found)
AMNF (Address Mark Not Found)*/

#ifndef ATAHD_H
#define	ATAHD_H

#include <stdbool.h>
#include "time.h"
#include "ahci.h"

extern int kTicksPerMS;
#define SHOW_STATUS \
printk("status=%02X", inb(ATA_PORT_STATUS));

#define ATA_STANDARD_WAIT_MS 5
/*PORT definitions*/
#define ATA_PORT_DATA 0x0
#define ATA_PORT_ERROR 0x1
#define ATA_PORT_SECTORCOUNT 0X2
#define ATA_PORT_SECTOR_NUMBER 0x3
#define ATA_PORT_CYLINDER_LOW 0x4
#define ATA_PORT_CYLINDER_HIGH 0x5
#define ATA_DRIVE_SELECT 0x6
#define ATA_PORT_COMMAND  0x7

#define ATA_PORT_CONTROL 0x3f6
#define ATA_PORT_STATUS  ATA_PORT_COMMAND
#define ATA_MASTER_DISK 0xA0
#define ATA_SLAVE_DISK 0xB0
#define ATA_IDENTIFY_COMMAND 0xEC
#define ATA_IDENTIFY_CDROM_COMMAND 0xA1
#define ATA_DRQ_ERROR 0x1
#define ATA_STATUS_DRQ  0x08
#define ATA_STATUS_SEEK 0x10
#define ATA_STATUS_READY 0x40
#define ATA_STATUS_BUSY 0x80
#define ATA_STATUS_WRITE_ERROR 1 << 5
#define ATA_STATUS_ERROR 1
// device control reg (CB_DC) bits
#define ATA_CB_DC_HD15   0x08  // bit should always be set to one
#define ATA_CB_DC_SRST   0x04  // soft reset
#define ATA_CB_DC_NIEN   0x02  // disable interrupts
/* The default and seemingly universal sector size for CD-ROMs. */
#define ATAPI_SECTOR_SIZE 2048
/* The default ISA IRQ numbers of the ATA controllers. */
#define ATA_IRQ_PRIMARY     0x0E
#define ATA_IRQ_SECONDARY   0x0F
/* The necessary I/O ports, indexed by "bus". */
#define ATA_DATA(x)         (x)
#define ATA_FEATURES(x)     (x+1)
#define ATA_SECTOR_COUNT(x) (x+2)
#define ATA_ADDRESS1(x)     (x+3)
#define ATA_ADDRESS2(x)     (x+4)
#define ATA_ADDRESS3(x)     (x+5)
//#define ATA_DRIVE_SELECT(x) (x+6)
#define ATA_COMMAND(x)      (x+7)
#define ATA_DCR(x)          (x+0x206)   /* device control register */
 
/* valid values for "bus" */
#define ATA_BUS_PRIMARY     0x1F0
#define ATA_BUS_SECONDARY   0x170
/* valid values for "drive" */
#define ATA_DRIVE_MASTER    0xA0
#define ATA_DRIVE_SLAVE     0xB0
#define ATA_MAX_DRIVES_PER_CONTROLLER 2
#define ATA_COMMAND_READ_SECTOR 0x20
#define ATA_COMMAND_WRITE_SECTOR 0x30


enum whichDrive
{
    master = 0,
    slave = 1
};

enum eATADeviceType
{
    ATA_DEVICE_TYPE_HD,
    ATA_DEVICE_TYPE_CD,
    ATA_DEVICE_TYPE_SATA_HD,
    ATA_DEVICE_TYPE_SATA_CD
};

enum whichBus
{
    ATAPrimary,
    ATASecondary,
    SATA
};

typedef struct 
{
    uint16_t ATAIdentifyData[256];
    char ATADeviceModel[80];
    bool queryATAData;
    uint8_t ATADeviceAvailable;
    int ATADeviceType;
    uint32_t totalSectorCount;
    uint32_t sectorSize;
    bool lbaSupported;
    bool lba48Supported;
    bool dmaSupported;
    enum whichBus bus; 
    enum whichDrive driveNo;
    uintptr_t ioPort;
    uint8_t irqNum;
    uint8_t driveHeadPortDesignation;
    HBA_MEM* ABAR;
} __attribute__((packed)) ataDeviceInfo_t;

int ataIdentify(ataDeviceInfo_t* devInfo);

#endif	/* ATAHD_H */

