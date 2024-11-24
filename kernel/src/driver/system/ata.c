#include "driver/system/ata.h"
#include "io.h"
#include "time.h"
#include "CONFIG.h"
#include "serial_logging.h"

bool useCDROMIdentify=false;

void ataGetModelFromIdentify(ataDeviceInfo_t* devInfo)
{
    uint16_t* ident=devInfo->ATAIdentifyData+27;
    for (int cnt=0;cnt<40;cnt+=2)
    {
        devInfo->ATADeviceModel[cnt]=(*ident >> 8) & 0xFF;
        devInfo->ATADeviceModel[cnt+1]=(*ident++) & 0xFF;
    }
    devInfo->ATADeviceModel[79]='\0';
}


int ataIdentify(ataDeviceInfo_t* devInfo)
{
    //For ATA devices, read the identity data.  For SATA, we've already read it into ATAIdentityData
    if (devInfo->queryATAData)
    {
        if (useCDROMIdentify)
            outb(devInfo->ioPort+ATA_PORT_COMMAND, ATA_IDENTIFY_CDROM_COMMAND);
        else
            outb(devInfo->ioPort+ATA_PORT_COMMAND, ATA_IDENTIFY_COMMAND);
        wait(ATA_STANDARD_WAIT_MS);
        for (int readCount=0;readCount<=255;readCount++)
        {
            devInfo->ATAIdentifyData[readCount]=inw(devInfo->ioPort+ATA_PORT_DATA);
        }
    }
    ataGetModelFromIdentify(devInfo);
    devInfo->totalSectorCount = devInfo->ATAIdentifyData[60] | (devInfo->ATAIdentifyData[61]<<16);
    devInfo->sectorSize = (devInfo->ATAIdentifyData[106] & 1<<12)==1<<12
            ?devInfo->ATAIdentifyData[117] | (devInfo->ATAIdentifyData[18]<<16)
            :512;
    devInfo->dmaSupported=devInfo->ATAIdentifyData[49]>>8 & 0x1;
    devInfo->lbaSupported=devInfo->ATAIdentifyData[49]>>9 & 0x1;
    devInfo->lba48Supported=devInfo->ATAIdentifyData[83]>>10 & 0x1;
    
#ifndef DEBUG_NONE
    if ((DEBUG_OPTIONS & DEBUG_HARDDRIVE) == DEBUG_HARDDRIVE)
        printd(DEBUG_HARDDRIVE,"drive %d, model=%s\n",devInfo->driveNo, devInfo->ATADeviceModel);
#endif
    return 1;
}
