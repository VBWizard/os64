#include "driver/ahci.h"
#include "driver/system/ata.h"
#include "CONFIG.h"
#include "time.h"
#include "paging.h"
#include "kmalloc.h"
#include "serial_logging.h"
#include "BasicRenderer.h"
#include "memory/memcpy.h"
#include "memory/memset.h"
#include "driver/system/pci.h"
#include "panic.h"

pci_device_t kPCISATADevice;
extern uint8_t kPCIDeviceCount;
extern uint8_t kPCIFunctionCount;
extern pci_device_t* kPCIDeviceHeaders;
extern pci_device_t* kPCIDeviceFunctions;
ataDeviceInfo_t* kATADeviceInfo;
int ahciCapsCount;
ahcicaps_t* ahciCaps;
int kATADeviceInfoCount;
uint8_t ahciReadBuff[512];
int ahciHostCount = 0;

hba_port_t* kAHCICurrentDisk;
HBA_MEM* ahciABAR;
uintptr_t* ahciDiskBuffer;
HBA_MEM *ABARs;



void ata_start_cmd(volatile hba_port_t *port) {
    // Wait until CR (bit15) is cleared
    while (port->cmd.CR);

    // Set FRE (bit4) and ST (bit0)
    port->cmd.FRE = 1;
    port->cmd.ST = 1;
}

void ata_stop_cmd(volatile hba_port_t *port) {
    // Clear ST (bit0)
    port->cmd.ST = 0;
    // Wait until FR (bit14), CR (bit15) are cleared
    while (1) {
        if (!(port->cmd.FR) || !(port->cmd.CR))
            break;
        waitTicks(10);
    }

    // Clear FRE (bit4)
    port->cmd.FRE = 0;
}

int ata_find_cmdslot(const hba_port_t *port) {
    printd(DEBUG_AHCI, "AHCI: find_cmdslot - finding a slot to use to execute a command\n");
    // An empty command slot has its respective bit cleared to �0� in both the PxCI and PxSACT registers.
    // If not set in SACT and CI, the slot is free // Checked
    
    uint32_t slots = (port->sact | port->ci);
    int num_of_slots = ahciABAR->cap.NCS;
    printd(DEBUG_AHCI,"ahciABAR = %08X, Total slots available: %d\n",ahciABAR, ahciABAR->cap.NCS);
    for (int idx = 0; idx < num_of_slots; idx++) {

        if ((slots & 1) == 0) {
            printd(DEBUG_AHCI, "AHCI: [command slot is : %d]\n", idx);
            return idx;

        }
        printd(DEBUG_AHCI, "Slot %u is busy (0x%08x)\n", idx,slots);
        slots >>= 1;
    }
    printd(DEBUG_AHCI, "AHCI: Cannot find free command list entry, count=%u, slots=0x%08x\n", num_of_slots, slots);
    return -1;
}

void ahci_port_rebase(volatile hba_port_t *port, int portno, uintptr_t remapBase) {
    //each command list is 1k (32k possible per port))
    printd(DEBUG_AHCI, "AHCI: Rebasing port %u (0x%08x) clb/fb from 0x%08x/0x%08x\n\t", portno, port, port->clb, port->fb);
    ata_stop_cmd(port); // Stop command engine

    // Command list offset: 1K*portno
    // Command list entry size = 32
    // Command list entry maxim count = 32
    // Command list maxim size = 32*32 = 1K per port
    port->clbu = (remapBase + (portno<<10)) >> 32;
    port->clb = (remapBase + (portno<<10));
	uint64_t thePort = (uint64_t)port->clbu << 32 | port->clb;
	memset((void*)thePort, 0, 1024);
    
    //each FIS is 256 bytes
    // FIS offset: 32K+256*portno
    // FIS entry size = 256 bytes per port
    port->fbu = port->clbu;
    port->fb = (port->clb + 0x1000);
	thePort = (uint64_t)port->fbu << 32 | port->fb;
	memset((void*)thePort, 0, 256);


    // Command table offset: 40K + 8K*portno
    // Command table size = 256*32 = 8K per port
    HBA_CMD_HEADER *cmdheader = (HBA_CMD_HEADER*)(uint64_t)(port->clb);
    for (int i = 0; i < 32; i++) {
        cmdheader[i].prdtl = 8; // 8 prdt entries per command table
        // 256 bytes per command table, 64+16+48+16*8
        // Command table offset: 40K + 8K*portno + cmdheader_index*256
        cmdheader[i].ctba = remapBase + (40 << 10) + (portno << 13) + (i << 8);
        cmdheader[i].ctbau = 0;
        memset((void*) cmdheader[i].ctba_64, 0, 256);
    }
    printd(DEBUG_AHCI, " to 0x%08x/0x%08x\n", port->clb, port->fb);
    ata_start_cmd(port); // Start command engine
}

int ahci_check_type(const hba_port_t *port, uint32_t* sig) {
    uint32_t ssts = port->ssts;
    uint8_t ipm = (ssts >> 8) & 0x0F;
    uint8_t det = ssts & 0x0F;

    if (det != HBA_PORT_DET_PRESENT) // Check drive status
        return AHCI_DEV_NULL;
    if (ipm != HBA_PORT_IPM_ACTIVE)
        return AHCI_DEV_NULL;

    *sig = port->sig;
    switch (port->sig) {
        case SATA_SIG_ATAPI:
            return AHCI_DEV_SATAPI;
        case SATA_SIG_SEMB:
            return AHCI_DEV_SEMB;
        case SATA_SIG_PM:
            return AHCI_DEV_PM;
        case SATA_SIG_ATAPI & 0xFFFF0000:
            return AHCI_DEV_SATAPI;
        default:
            return AHCI_DEV_SATA;
    }
}

void ahci_port_activate_device(HBA_MEM* hba_mem, hba_port_t* hba_port)
{
    printd(DEBUG_AHCI,"activate port %d @ 0x%08x:\n", hba_port - hba_mem->ports,hba_port);
    /* first check the presence flag */
    if ( (hba_port->ssts & 0x7) == HBA_PORT_DET_NOT_PRESENT) 
	{ //check DET status
                    printd(DEBUG_AHCI,"activate: DET_NOT_PRESENT\n");
                    /* nothing attached? */
                    if (hba_port->cmd.CPD) { /* we rely on CPD */
                                    if (!hba_port->cmd.CPS) {
                                                    printd(DEBUG_AHCI,"confirmed by CPD\n");
                                                    return;
                                    }
                                    /* there's something */
                                    if (!hba_port->cmd.POD) {
                                                    hba_port->cmd.POD = 1; /* power it */
                                                    wait(20);
                                    }
                                    if ((hba_port->ssts & 0x7) != HBA_PORT_DET_NOT_PRESENT)
                                                    goto next_step;
                    }
                    /* spin-up? */
                    if (!hba_port->cmd.SUD) { /* always !1 if cap.sss == 0 */
                                    printd(DEBUG_AHCI,"not spun-up yet?\n");
                                    if (hba_port->serr.AsUlong & (1 << 26)) /* eXchange bit */
                                                    hba_port->serr.AsUlong |= (1 << 26); /* RWC */
                                    if ((hba_port->ssts & 0x7) != 0) { /* set to 0 prior sud */
                                                    hba_port->sctl.DET = 0;
                                                    wait(20);
                                    }
                                    hba_port->cmd.SUD = 1;
                                    waitTicks(5); /* wait 50 mus */
                                    if (hba_port->serr.AsUlong & ((1 << 26) | (1 << 18))) { /* received sth. */
                                                    /* COMRESET, COMWAKE */
                                                    goto next_step;
                                    }
                                    /* send the reset */
                                    hba_port->sctl.DET = 1;
                                    waitTicks(100);
                                    hba_port->sctl.DET = 0;
                                    wait(20);
                                    if ((hba_port->ssts & 0x7) != HBA_PORT_DET_NOT_PRESENT)
                                                    goto next_step;
                                    hba_port->cmd.SUD = 0; /* we're done enter listening mode */
                                    printd(DEBUG_AHCI,"not present\n");
                                    goto exit;
                    }
                    /* just try ICC */
                    if (hba_port->sctl.IPM != 1) {
                                    printd(DEBUG_AHCI,"IPM != ACTIVE\n");
                                    int ct = 50;
                                    while (hba_port->cmd.ICC && ct--)
                                                    waitTicks(1);
                                    hba_port->cmd.ICC = 1;
                                    wait(10);
                                    if ((hba_port->ssts & 0x7) != HBA_PORT_DET_NOT_PRESENT)
                                                    goto next_step;
                                    printd(DEBUG_AHCI,"unable to set to active\n");
                                    goto exit;
                    }
    }

    /* det != 1*/
next_step:
    if ((hba_port->ssts & 0x7) == HBA_PORT_DET_PRESENT) {
                    /* almost done */
                    if (hba_port->sctl.IPM != HBA_PORT_IPM_ACTIVE) {
                                    int ct, wc;
                                    printd(DEBUG_AHCI,"Present but not active.\n");
activ:
                                    ct = 50, wc = 0;
rewait:
                                    while (hba_port->cmd.ICC && ct--) /* 500ms */
                                                    waitTicks(1);
                                    if (hba_port->sctl.IPM != HBA_PORT_IPM_ACTIVE && !wc) {
                                                    hba_port->cmd.ICC = 1;
                                                    waitTicks(1);
                                                    wc++; ct = 50;
                                                    goto rewait;
                                    }
                                    if (wc) {
                                                    /* reset */
                                                    hba_port->sctl.DET = 1;
                                                    waitTicks(100);
                                                    goto next_step;
                                    }
                    }
                    printd(DEBUG_AHCI,"Device at port %d is active and present.\n",
                                    hba_port - hba_mem->ports);
                    printd(DEBUG_AHCI,"details: %x %x %x %u %u\n", hba_port->serr.AsUlong, hba_port->tfd.AsUchar,
                                    hba_port->tfd.ERR, (hba_port->ssts & 0x7), hba_port->sctl.IPM);
                    return;
    } else if ((hba_port->ssts & 0x7) == 4/*?*/) {
                    /* just deactivated */
                    printd(DEBUG_AHCI,"PHY offline mode\n");
                    goto activ;
    } else if ((hba_port->ssts & 0x7) == 5/*DET_PRESENT_NO_PHY*/) {
                    printd(DEBUG_AHCI,"PRESENT_NO_PHY mode\n");
                    goto activ; /* try the same */
    }
exit:
                    /* we're really done */
                    printd(DEBUG_AHCI,"Not present at port %d\n", hba_port - hba_mem->ports);
                    return;
}

void ahci_enable_port(HBA_MEM* hba_mem, int port_number)
{
        hba_port_t* port = &hba_mem->ports[port_number];
        int reset_ct = 0;
 
        /* skip non-implemented ports */
        if (!(hba_mem->pi & (1 << port_number)))
                return;
 
Pos1:
        /* clear old data */
        port->serr.AsUlong = 0xffffffff; /* 10.1.2 -> 6. */
        port->pxis.AsUlong = 0xffffffff; /* clear all pending interrupts */
 
        /* first allow for the reception of FISes */
        port->cmd.FRE = 1;
        wait(20); /* wait for BSY to show up */
        while (1) {
                uint8_t sts = port->tfd.AsUchar;
                if (sts & 1) 
                {
                        /* something went wrong! */
                        if (sts == 0x7f) /* no device */
                                break;
                        else if (sts == 0x41 && port->tfd.ERR == 0x1 && port->sig == SATA_SIG_ATAPI){
                                        break; /* no medium */
                        }
                        printf("port%d indicated task file error %x"
                                " while starting up.\n", port_number, port->tfd.ERR);
                        printd(DEBUG_AHCI,"AHCI: scr1: %x %x\n", port->serr.AsUlong, port->tfd.ERR);
                        printd(DEBUG_AHCI,"AHCI: tfd: %x %x\n", port->tfd.AsUchar, port->tfd.ERR);
                        port->serr.AsUlong = 0xffffffff;
                        if (!reset_ct++) {
                                ahci_port_activate_device(hba_mem, port);
                                goto Pos1;
                        } else if (reset_ct == 1) {
                                goto Pos1;
                        } else
                                goto defer;
                }
                if (!(sts & (0x80 | 0x8)))
                        break;
                wait(50);
        }
 
        /* set ST only if BSY,DRQ and DET=3h or IPM=2,6,8 */
        if ((port->ssts & 0x7) != 3)
                ahci_port_activate_device(hba_mem, port);
        if (port->tfd.AsUchar & (0x80 | 0x8))  //BSY | DRQ
                goto defer; /* listen */
        if (!(((port->ssts & 0x7) == 3) || (port->sctl.IPM == 2) ||
                (port->sctl.IPM == 6) || (port->sctl.IPM == 8)))
                goto defer;
        /* we're allowed so set it */
        port->cmd.ST = 1;
        /* Change in PhyRdy, CPS, TFS.err, PCS, DPS(I=1), UFS, HBFS, HBDS, IFS */
        port->ie.AsUlong = (1 << 22) | (1 << 6) | (1 << 31) | (1 << 30)
                | (1 << 5) | (1 << 4) | (1 << 29) | (1 << 28) | (1 << 27);
 
        printd(DEBUG_AHCI,"AHCI: port %d is now processing commands\n", port_number);
        return;
 
defer: /* we're interested in status changes only */
        printd(DEBUG_AHCI,"AHCI: port %d set to listening mode\n", port_number);
        port->ie.AsUlong = (1 << 22) | (1 << 31) | (1 << 6); /* PhyRdy change, CPS, CCS */
}

void ahci_probe_ports(HBA_MEM *ahci_abar) {
    // Search disk in impelemented ports
    uint32_t port_implemented = ahci_abar->pi;
    int i = 0;
    uint64_t port_remap_base = AHCI_PORT_BASE_REMAP_ADDRESS; //probably only need 0xA000
    if (port_implemented > 0)
        printd(DEBUG_AHCI, "AHCI: Probing ports via remapped ABAR 0x%016x, value 0x%02X\n", ahci_abar, ahci_abar->pi);
    while (i < 32) 
    {
        if (port_implemented & 1) 
        {
            ahci_enable_port(ahci_abar,i);
            uint32_t sig = 0;
            //Get the SATA device signature
            int dt = ahci_check_type(&ahci_abar->ports[i], &sig);
            printd(DEBUG_AHCI, "AHCI: Checking port %u (0x%08x), sig=%08X\n", i, &ahci_abar->ports[i], sig);
            //Found a SATA disk
			//TODO: Change ACHI logic to support 64-bit so you can just randomly assign memory to the port
			//port_remap_base = (uint64_t)kmalloc_aligned(0x10000);
			port_remap_base = (uint64_t)kmalloc_dma32(port_remap_base,0x10000);
            if (dt == AHCI_DEV_SATA) {
                printd(DEBUG_AHCI, "AHCI: SATA drive found at port %d (0x%08x)\n", i, &ahci_abar->ports[i]);
                printd(DEBUG_AHCI, "AHCI:\tCLB=0x%08x, fb=0x%08x\n", ahci_abar->ports[i].clb, ahci_abar->ports[i].fb);
                ahci_port_rebase(&ahci_abar->ports[i], i, port_remap_base);
                	//det reset, disable slumber and Partial state
			//reset port, send COMRESET signal
                ahciIdentify(&ahci_abar->ports[i], AHCI_DEV_SATA);
            } else if (dt == AHCI_DEV_SATAPI) {
                printd(DEBUG_AHCI, "AHCI:SATAPI drive found at port %d (0x%08x)\n", i, &ahci_abar->ports[i]);
                printd(DEBUG_AHCI, "AHCI:\tCLB=0x%08x, fb=0x%08x\n", ahci_abar->ports[i].clb, ahci_abar->ports[i].fb);
                ahci_port_rebase(&ahci_abar->ports[i], i, port_remap_base);
                //Run an ATA_IDENTIFY
                ahciIdentify(&ahci_abar->ports[i], AHCI_DEV_SATAPI);
            } else if (dt == AHCI_DEV_SEMB) {
                printd(DEBUG_AHCI, "AHCI: SEMB drive found at port %d (0x%08x)\n", i, &ahci_abar->ports[i]);
            } else if (dt == AHCI_DEV_PM) {
                printd(DEBUG_AHCI, "AHCI: PM drive found at port %d (0x%08x)\n", i, &ahci_abar->ports[i]);
            }
        }

        port_implemented >>= 1;
        i++;
    }
}

int AhciIssueCmd(volatile hba_port_t *port, int cmdslot) 
{
    unsigned int elapsed = 0;
    int Status = 0; // 0 for success, negative for errors

    // Wait until BSY and DRQ are cleared
    while (port->tfd.BSY || port->tfd.DRQ) {
        wait(POLL_INTERVAL);
    }

    // Clear error bits in PxIS and PxSERR
    port->pxis.AsUlong = 0xFFFFFFFF;
    port->serr.AsUlong = 0xFFFFFFFF;

    // Issue command
    port->ci |= (1 << cmdslot);

    // Wait for command completion
    while ((port->ci & (1 << cmdslot)) && (elapsed < COMMAND_TIMEOUT)) {
        if (port->pxis.TFES) {
            printd(DEBUG_AHCI, "AhciIssueCmd: Task file error during command execution\n");
            Status = -2; // Task file error
            break;
        }
        wait(POLL_INTERVAL);
        elapsed += POLL_INTERVAL;
    }

    if (elapsed >= COMMAND_TIMEOUT) {
        printd(DEBUG_AHCI, "AhciIssueCmd: Command timeout\n");
        Status = -1; // Timeout error
    }

    // Check for errors after command completion
    if (port->tfd.ERR || port->pxis.TFES) {
        printd(DEBUG_AHCI, "AhciIssueCmd: Error after command completion\n");
        Status = -2; // Device error
        // Clear error bits
        port->pxis.AsUlong = 0xFFFFFFFF;
        port->serr.AsUlong = 0xFFFFFFFF;
    }

    return Status;
}

void ahciIdentify(volatile hba_port_t* port, int deviceType) {
    printd(DEBUG_AHCI, "AHCI: ahciIdentify, port@0x%08x(%u), 0x%08x\n", port, kATADeviceInfoCount, &port->clb);
    HBA_CMD_HEADER* cmdhdr = (HBA_CMD_HEADER*)(uint64_t) port->clb;
    int slot = ata_find_cmdslot(port);
    if (slot == -1)
        return;
    port->ie.AsUlong = 0xFFFFFFFF;
    HBA_CMD_HEADER* cmdheader = cmdhdr + slot;
    printd(DEBUG_AHCI, "AHCI: cmdheader=0x%08x\n", cmdheader);
    cmdheader->prdtl = 1;
    cmdheader->cfl = 5; 
    cmdheader->w = 0;
    cmdheader->a = 0;
    cmdheader->c = 0;
    cmdheader->p = 0;
    HBA_CMD_TBL *cmdtbl = (HBA_CMD_TBL*) cmdheader->ctba_64;
    memset(cmdtbl, 0, sizeof (HBA_CMD_TBL) +
            (cmdheader->prdtl - 1) * sizeof (HBA_PRDT_ENTRY));
    printd(DEBUG_AHCI, "AHCI: cmdtable=0x%08x,ctba=0x%08x\n", cmdtbl, cmdheader->ctba);
    cmdtbl->prdt_entry[0].dba_64 = ahciDiskBuffer;
    cmdtbl->prdt_entry[0].dbc = 0x1ff;
    cmdtbl->prdt_entry[0].i = 1;

    FIS_REG_H2D *cmdfis = (FIS_REG_H2D*) (&cmdtbl->cfis);
    cmdfis->fis_type = FIS_TYPE_REG_H2D;
    cmdfis->c = 1; // Command
    if (deviceType == AHCI_DEV_SATAPI)
        cmdfis->command = ATA_IDENTIFY_CDROM_COMMAND;
    else
        cmdfis->command = ATA_IDENTIFY_COMMAND;
    cmdfis->lba0 = 0;
    cmdfis->lba1 = 0;
    cmdfis->lba2 = 0;
    cmdfis->lba3 = 0;
    cmdfis->lba4 = 0;
    cmdfis->lba5 = 0;
    cmdfis->lba5 = 0;
    cmdfis->pmport = 0;
    cmdfis->device = 0;
    cmdfis->countl = 1;
    cmdfis->counth = 0;
    int lCmdVal = AhciIssueCmd(port, slot);
    if (lCmdVal) {
        printf("AHCI: ***Error identifying device (%u)***\n",lCmdVal);
        return;
    }
    kATADeviceInfo[kATADeviceInfoCount].ATADeviceAvailable = true;
    kATADeviceInfo[kATADeviceInfoCount].bus = SATA;
    kATADeviceInfo[kATADeviceInfoCount].driveNo = kATADeviceInfoCount;
    kATADeviceInfo[kATADeviceInfoCount].ioPort = (uintptr_t) port;
    kATADeviceInfo[kATADeviceInfoCount].irqNum = 0;
    kATADeviceInfo[kATADeviceInfoCount].driveHeadPortDesignation = 0x0;
    kATADeviceInfo[kATADeviceInfoCount].queryATAData = false;
    if (deviceType == AHCI_DEV_SATAPI)
        kATADeviceInfo[kATADeviceInfoCount].ATADeviceType=ATA_DEVICE_TYPE_SATA_CD;
    else
        kATADeviceInfo[kATADeviceInfoCount].ATADeviceType=ATA_DEVICE_TYPE_SATA_HD;
    kATADeviceInfo[kATADeviceInfoCount].ABAR=ahciABAR;
    memcpy(kATADeviceInfo[kATADeviceInfoCount].ATAIdentifyData, (void*) ahciDiskBuffer, 512);
	ataIdentify(&kATADeviceInfo[kATADeviceInfoCount++]);
    printd(DEBUG_AHCI, "AHCI: SATA device found, name=%s\n", kATADeviceInfo[kATADeviceInfoCount - 1].ATADeviceModel);
}


bool init_AHCI()
{
    kATADeviceInfoCount = 4;
    bool ahciDeviceFound = false;
    char buffer[150];

	ahciDiskBuffer = kmalloc(0x10000*20);
	//The AHCI disk buffer has to be accessed using its physical address.  So get rid of the HHMD Offset and map without it
	ahciDiskBuffer = (uintptr_t*)(((uint64_t)ahciDiskBuffer) - kHHDMOffset);
	paging_map_pages((pt_entry_t*)kKernelPML4v, (uintptr_t)ahciDiskBuffer, (uintptr_t)ahciDiskBuffer, 0x10000 / PAGE_SIZE, PAGE_PRESENT | PAGE_WRITE | PAGE_PCD);
	ABARs = kmalloc(ABARS_PAGE_COUNT * sizeof(HBA_MEM));
	ahciCaps = kmalloc(ABARS_PAGE_COUNT * sizeof(ahcicaps_t));
	    if (!kPCIDeviceCount) {
        printd(DEBUG_AHCI, "AHCI: PCI not initialized, cannot initialize AHCI.");
        return false;
    }

    for (int cnt = 0; cnt < kPCIDeviceCount; cnt++)
        if (kPCIDeviceHeaders[cnt].class == 1 && kPCIDeviceHeaders[cnt].subClass == 6) 
        {
            memcpy(&kPCISATADevice, &kPCIDeviceHeaders[cnt], sizeof (pci_device_t));
            ahciDeviceFound = true;
            printd(DEBUG_AHCI, "AHCI: Found AHCI controller (D) (%02X/%02X/%02X) '%s'\n", cnt, kPCIDeviceHeaders[cnt].class, kPCIDeviceHeaders[cnt].subClass, getDeviceNameP(&kPCISATADevice, buffer));
            printd(DEBUG_AHCI, "ABAR is at: before/remapped - 0x%08x/", kPCISATADevice.baseAdd[5]);
            ahciABAR = (HBA_MEM*) AHCI_ABAR_REMAPPED_ADDRESS + (0x10 * ahciHostCount);
			paging_map_pages((pt_entry_t*)kKernelPML4v, (uintptr_t) ahciABAR, kPCISATADevice.baseAdd[5] , (ABARS_PAGE_COUNT * sizeof(HBA_MEM)) / PAGE_SIZE + 1,PAGE_PRESENT | PAGE_WRITE | PAGE_PCD);
			paging_map_page((pt_entry_t*)kKernelPML4v, kPCISATADevice.baseAdd[5],kPCISATADevice.baseAdd[5],PAGE_PRESENT | PAGE_WRITE | PAGE_PCD);
            RELOAD_CR3
			memcpy((void*)&ABARs[ahciHostCount++], (void*) ahciABAR, sizeof (HBA_MEM));
            printd(DEBUG_AHCI, "0x%08x\n", ahciABAR);
            memcpy(&ahciCaps[ahciCapsCount++], (void*) ahciABAR, sizeof (ahcicaps_t));
            if (!(ahciABAR->ghc.AE)) {
                printd(DEBUG_AHCI,"switching to AHCI mode\n");
                ahciABAR->ghc.AE=1;
            }
            ahciABAR->ghc.IE=1;
            if (ahciABAR->cap2 & 1) {
                panic("Write support for BIOS handoff!!!");
            }
            ahci_probe_ports(ahciABAR);
        }
    for (int cnt = 0; cnt < kPCIFunctionCount; cnt++)
        if (kPCIDeviceFunctions[cnt].class == 1 && kPCIDeviceFunctions[cnt].subClass == 6) 
        {
            memcpy(&kPCISATADevice, &kPCIDeviceFunctions[cnt], sizeof (pci_device_t));
            ahciDeviceFound = true;
            printd(DEBUG_AHCI, "AHCI: Found AHCI controller (F) (%02X/%02X/%02X) '%s'\n", cnt, kPCIDeviceFunctions[cnt].class, kPCIDeviceFunctions[cnt].subClass, getDeviceNameP(&kPCISATADevice, buffer));
            printd(DEBUG_AHCI, "ABAR is at: 0x%08x\n", kPCISATADevice.baseAdd[5]);
            ahciABAR = (HBA_MEM*) AHCI_ABAR_REMAPPED_ADDRESS + (0x10 * ahciHostCount);
			paging_map_pages((pt_entry_t*)kKernelPML4v, (uintptr_t) ahciABAR, kPCISATADevice.baseAdd[5] , (ABARS_PAGE_COUNT * sizeof(HBA_MEM)) / PAGE_SIZE + 1,PAGE_PRESENT | PAGE_WRITE | PAGE_PCD);
			paging_map_page((pt_entry_t*)kKernelPML4v, kPCISATADevice.baseAdd[5],kPCISATADevice.baseAdd[5],PAGE_PRESENT | PAGE_WRITE | PAGE_PCD);
            RELOAD_CR3
			memcpy((void*)&ABARs[ahciHostCount++], (void*) ahciABAR, sizeof (HBA_MEM));
            printd(DEBUG_AHCI, "0x%08x\n", ahciABAR);
            memcpy(&ahciCaps[ahciCapsCount++], (void*) ahciABAR, sizeof (ahcicaps_t));
            if (!(ahciABAR->ghc.AE)) {
                printd(DEBUG_AHCI,"switching to AHCI mode\n");
                ahciABAR->ghc.AE=1;
            }
            ahciABAR->ghc.IE=1;
            if (ahciABAR->cap2 & 1) {
                panic("Write support for BIOS handoff!!!");
            }
            ahci_probe_ports(ahciABAR);
        }
    if (!ahciDeviceFound) {
        printd(DEBUG_AHCI, "AHCI: No AHCI devices found.");
        return false;
    }


    return true;
}