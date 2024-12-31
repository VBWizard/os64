#include "ahci.h"
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
#include "block_device.h"
#include "vfs.h"
#include "panic.h"
#include "strings.h"

pci_device_t kPCISATADevice;
extern uint8_t kPCIDeviceCount;
extern uint8_t kPCIFunctionCount;
extern pci_device_t* kPCIDeviceHeaders;
extern pci_device_t* kPCIDeviceFunctions;
block_device_info_t* kBlockDeviceInfo;
int kBlockDeviceInfoCount;
int ahciCapsCount;
ahcicaps_t* ahciCaps;
uint8_t ahciReadBuff[512];
int ahciHostCount = 0;
hba_port_t* kAHCICurrentDisk;
HBA_MEM* ahciABAR;
uintptr_t* ahciDiskBuffer;
HBA_MEM *kABARs;
char* kAHCIBuffer = NULL;
uint64_t kAHCIPortRemapBase = AHCI_PORT_BASE_REMAP_ADDRESS; //probably only need 0xA000


void ahci_start_cmd(volatile hba_port_t *port) {
    // Wait until CR (bit15) is cleared
    while (port->cmd.CR);

    // Set FRE (bit4) and ST (bit0)
    port->cmd.FRE = 1;
    port->cmd.ST = 1;
}

void ahci_stop_cmd(volatile hba_port_t *port) {
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
    printd(DEBUG_AHCI | DEBUG_DETAILED, "AHCI: find_cmdslot - finding a slot to use to execute a command\n");
    // An empty command slot has its respective bit cleared to 0 in both the PxCI and PxSACT registers.
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
    printd(DEBUG_AHCI, "AHCI: Stopping and rebasing port %u (0x%08x) clb/fb from 0x%08x/0x%08x to 0x%08x/0x%08x\n", 
				portno, port, port->clb, port->fb, (remapBase + (portno<<10)), (remapBase + (portno<<10))+0x1000);
    ahci_stop_cmd(port); // Stop command engine

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

	printd(DEBUG_AHCI, "AHCI: Done rebasing, setting up a cmdheader at the clb (0x%016x)\n",port->clb);

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
	printd(DEBUG_AHCI,"AHCI:\tctba zero = 0x%08x\n",cmdheader[0].ctba);
	printd(DEBUG_AHCI, "AHCI: Restarting port\n");
    ahci_start_cmd(port); // Start command engine
}

uint32_t ahci_check_type(const hba_port_t *port, uint32_t* sig) {
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
    if (port_implemented > 0)
        printd(DEBUG_AHCI, "AHCI: Probing ports via ABAR 0x%016lx, value 0x%04X\n", ahci_abar, ahci_abar->pi);
	else
		printd(DEBUG_AHCI, "AHCI: Port not implemented, skipping probing\n");
    
	uint32_t port0 = paging_walk_paging_table_keep_flags((uintptr_t*)kKernelPML4v, (uintptr_t)&ahci_abar->ports[0], true);
	uint32_t port1 = paging_walk_paging_table_keep_flags((uintptr_t*)kKernelPML4v, (uintptr_t)&ahci_abar->ports[1], true);
	
	printd(DEBUG_AHCI, "AHCI: Port0  address is 0x%016lx (physical=0x%016lx), Port1 address is 0x%016lx (physical=0x%016lx)\n", &ahci_abar->ports[0], port0, &ahci_abar->ports[1], port1);

	while (i < 32) 
    {
        if (port_implemented & 1) 
        {
            ahci_enable_port(ahci_abar,i);
            uint32_t sig = 0;
            //Get the SATA device signature
            uint32_t dt = ahci_check_type(&ahci_abar->ports[i], &sig);
			printd(DEBUG_AHCI | DEBUG_DETAILED, "AHCI port signature is 0x%08x\n",dt);
            //Found a SATA disk
			uintptr_t base = (uintptr_t)((uint64_t)ahci_abar->ports[i].clbu << 32 | ahci_abar->ports[i].clb);
			if (base != 0)
			{
				paging_map_pages((pt_entry_t*)kKernelPML4v, base, base, 0x40000 / PAGE_SIZE, PAGE_PRESENT | PAGE_WRITE | PAGE_PCD);
				if (dt == AHCI_DEV_SATA) {
					printd(DEBUG_AHCI, "AHCI: SATA drive found at port %d (0x%08x)\n", i, &ahci_abar->ports[i]);
					printd(DEBUG_AHCI | DEBUG_DETAILED, "AHCI:\tCLB=0x%08x, fb=0x%08x\n", ahci_abar->ports[i].clb, ahci_abar->ports[i].fb);
					ahci_port_rebase(&ahci_abar->ports[i], i, base);
					ahciIdentify(&ahci_abar->ports[i], AHCI_DEV_SATA);
				} else if (dt == AHCI_DEV_SATAPI) {
					printd(DEBUG_AHCI, "AHCI:SATAPI drive found at port %d (0x%08x)\n", i, &ahci_abar->ports[i]);
					printd(DEBUG_AHCI | DEBUG_DETAILED, "AHCI:\tCLB=0x%08x, fb=0x%08x\n", ahci_abar->ports[i].clb, ahci_abar->ports[i].fb);
					ahci_port_rebase(&ahci_abar->ports[i], i, base);
					//Run an ATA_IDENTIFY
					ahciIdentify(&ahci_abar->ports[i], AHCI_DEV_SATAPI);
				} else if (dt == AHCI_DEV_SEMB) {
					printd(DEBUG_AHCI, "AHCI: SEMB drive found at port %d (0x%08x)\n", i, &ahci_abar->ports[i]);
				} else if (dt == AHCI_DEV_PM) {
					printd(DEBUG_AHCI, "AHCI: PM drive found at port %d (0x%08x)\n", i, &ahci_abar->ports[i]);
				}
			}
			else
			{
				printd(DEBUG_EXCEPTIONS, "AHCI: SATA drive found at port %u but base is 0x%016lx, which is invalid.  ahci_abar = 0x%016lx, clb=0x%08x, clbu=0x%08x\n", 
					i, base, ahci_abar, ahci_abar->ports[i].clb, ahci_abar->ports[i].clbu);

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
	unsigned char temp = 0;

uint32_t timeout = 1000000; // Example: Timeout after 1 second
uint32_t tfd_value;
uint8_t status;

tfd_value = *(volatile uint32_t *)((uintptr_t)port + 0x20);
status = tfd_value & 0xFF;

	 printd(DEBUG_AHCI, "AHCI: Waiting for BSY & DRQ (tfd=0x%08x)\n", port->tfd);
while ((status & (ATA_SR_BSY | ATA_SR_DRQ)) && timeout--) {
    wait(POLL_INTERVAL);
    tfd_value = *(volatile uint32_t *)((uintptr_t)port + 0x20);
    status = tfd_value & 0xFF;
}

	if (timeout == 0) {
		panic("AHCI: Timeout waiting for BSY & DRQ to clear (tfd=0x%08x)\n", port->tfd.AsUchar);
	}

	printd(DEBUG_AHCI, "AHCI: After waiting for BSY & DRQ (tfd @ 0x%08x=0x%08x), was 0x%08x\n", &port->tfd.AsUchar, port->tfd.AsUchar,temp);
	printd(DEBUG_AHCI, "Before command issue:\n");
	printd(DEBUG_AHCI, "PxCMD: 0x%08x\n", port->cmd.AsUlong);
	printd(DEBUG_AHCI, "PxCI: 0x%08x\n", port->ci);
	printd(DEBUG_AHCI, "PxIS: 0x%08x\n", port->pxis.AsUlong);
	printd(DEBUG_AHCI, "PxSERR: 0x%08x\n", port->serr.AsUlong);


    // Clear error bits in PxIS and PxSERR
    port->pxis.AsUlong = 0xFFFFFFFF;
    port->serr.AsUlong = 0xFFFFFFFF;

    // Issue command
    port->ci |= (1 << cmdslot);
	printd(DEBUG_AHCI, "AHCI: issued the command, ci=0x%08x\n",port->ci);

    // Wait for command completion
    while ((port->ci & (1 << cmdslot)) && (elapsed < COMMAND_TIMEOUT)) {
        if (port->pxis.TFES) {
            printd(DEBUG_AHCI, "AhciIssueCmd: Task file error during command execution\n");
            Status = -2; // Task file error
            break;
        }
//		printd(DEBUG_AHCI, "AHCI Command completion delay ... waiting\n");
        wait(POLL_INTERVAL);
        elapsed += POLL_INTERVAL;
    }

	printd(DEBUG_AHCI, "After command completion:\n");
	printd(DEBUG_AHCI, "PxCI: 0x%08x\n", port->ci);
	printd(DEBUG_AHCI, "PxIS: 0x%08x\n", port->pxis.AsUlong);
	printd(DEBUG_AHCI, "PxSERR: 0x%08x\n", port->serr.AsUlong);
	printd(DEBUG_AHCI, "tfd: 0x%02x\n", port->tfd.AsUchar);

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

void ahciIdentify(hba_port_t* port, int deviceType) {
    printd(DEBUG_AHCI, "AHCI: ahciIdentify, port@0x%08x(%u), clb@0x%08x\n", port, kBlockDeviceInfoCount, &port->clb);
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
    //printd(DEBUG_AHCI, "AHCI: cmdtable=0x%08x,ctba=0x%016x\n", cmdtbl, cmdheader->ctba_64);
	//printd(DEBUG_AHCI, "Initializing prdt_entry at 0x%016x, which contains 0x%016x\n",ahciDiskBuffer, *ahciDiskBuffer);
	cmdtbl->prdt_entry[0].dbau = (uintptr_t)ahciDiskBuffer >> 32;
	cmdtbl->prdt_entry[0].dba = (uintptr_t)ahciDiskBuffer;
	//cmdtbl->prdt_entry[0].dba_64 = ahciDiskBuffer;
uint64_t full_address = ((uint64_t)cmdtbl->prdt_entry[0].dbau << 32) | cmdtbl->prdt_entry[0].dba;
uint32_t *address = (uint32_t *)full_address;
printd(DEBUG_AHCI, "Value at dba (0x%08x):", address);
printd(DEBUG_AHCI, "0x%08x\n", *address);
    cmdtbl->prdt_entry[0].dbc = 0x1ff;
    cmdtbl->prdt_entry[0].i = 1;
	printd(DEBUG_AHCI, "AHCI: Initializing FIS to 0x%08x, configuring for the Identify command\n",(&cmdtbl->cfis));
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
	printd(DEBUG_AHCI, "AHCI: Issuing the command\n");

    int lCmdVal = AhciIssueCmd(port, slot);
    if (lCmdVal) {
        printf("AHCI: ***Error identifying device (%u)***\n",lCmdVal);
        return;
    }
    kBlockDeviceInfo[kBlockDeviceInfoCount].DeviceAvailable = true;
    kBlockDeviceInfo[kBlockDeviceInfoCount].bus = BUS_SATA;
    kBlockDeviceInfo[kBlockDeviceInfoCount].driveNo = kBlockDeviceInfoCount;
    kBlockDeviceInfo[kBlockDeviceInfoCount].ioPort = (uintptr_t) port;
	kBlockDeviceInfo[kBlockDeviceInfoCount].block_extra_info = (void*)&kBlockDeviceInfo[kBlockDeviceInfoCount];
    kBlockDeviceInfo[kBlockDeviceInfoCount].irqNum = 0;
    kBlockDeviceInfo[kBlockDeviceInfoCount].driveHeadPortDesignation = 0x0;
    kBlockDeviceInfo[kBlockDeviceInfoCount].queryATAData = false;
    if (deviceType == AHCI_DEV_SATAPI)
        kBlockDeviceInfo[kBlockDeviceInfoCount].ATADeviceType=ATA_DEVICE_TYPE_SATA_CD;
    else
        kBlockDeviceInfo[kBlockDeviceInfoCount].ATADeviceType=ATA_DEVICE_TYPE_SATA_HD;
    //kBlockDeviceInfo[kBlockDeviceInfoCount].ABAR=ahciABAR;
    memcpy(kBlockDeviceInfo[kBlockDeviceInfoCount].ATAIdentifyData, (void*) ahciDiskBuffer, 512);
	ataIdentify(&kBlockDeviceInfo[kBlockDeviceInfoCount]);
	kBlockDeviceInfo[kBlockDeviceInfoCount].block_device = kmalloc(sizeof(block_device_t));
	kBlockDeviceInfo[kBlockDeviceInfoCount].block_device->name = kmalloc(0x100);
	strtrim(kBlockDeviceInfo[kBlockDeviceInfoCount].ATADeviceModel);
	strncpy(kBlockDeviceInfo[kBlockDeviceInfoCount].block_device->name, kBlockDeviceInfo[kBlockDeviceInfoCount].ATADeviceModel, 0x100);
	kBlockDeviceInfo[kBlockDeviceInfoCount].block_device->device = &kBlockDeviceInfo[kBlockDeviceInfoCount];
	kBlockDeviceInfo[kBlockDeviceInfoCount].block_device->ops = kmalloc(sizeof(block_operations_t));
	kBlockDeviceInfo[kBlockDeviceInfoCount].block_device->ops->read = (void*)&ahci_lba_read;
	//add_block_device(port, &kBlockDeviceInfo[kBlockDeviceInfoCount]);
	kBlockDeviceInfoCount++;
    printd(DEBUG_AHCI, "AHCI: SATA device found, name=%s\n", kBlockDeviceInfo[kBlockDeviceInfoCount - 1].ATADeviceModel);
}

void init_AHCI_device(int device_index, bool function)
{
    char buffer[150];

	if (!function)
	{
		memcpy(&kPCISATADevice, &kPCIDeviceHeaders[device_index], sizeof (pci_device_t));
		printd(DEBUG_AHCI, "AHCI: Found AHCI controller #%u (D) at %02X:%02X:%02X, Class=%02X, Subclass=%02X) '%s'\n", 
			device_index, kPCIDeviceHeaders[device_index].busNo, kPCIDeviceHeaders[device_index].deviceNo, kPCIDeviceHeaders[device_index].funcNo,
			kPCIDeviceHeaders[device_index].class, kPCIDeviceHeaders[device_index].subClass, getDeviceNameP(&kPCISATADevice, buffer));
	}
	else
	{
		memcpy(&kPCISATADevice, &kPCIDeviceFunctions[device_index], sizeof (pci_device_t));
		printd(DEBUG_AHCI, "AHCI: Found AHCI controller #%u (D) at %02X:%02X:%02X, Class=%02X, Subclass=%02X) '%s'\n", 
			device_index, kPCIDeviceFunctions[device_index].busNo, kPCIDeviceFunctions[device_index].deviceNo, kPCIDeviceFunctions[device_index].funcNo,
			kPCIDeviceFunctions[device_index].class, kPCIDeviceFunctions[device_index].subClass, getDeviceNameP(&kPCISATADevice, buffer));
	}
	ahciABAR = (HBA_MEM*)(uintptr_t)kPCISATADevice.baseAdd[5]; //AHCI_ABAR_REMAPPED_ADDRESS + (0x10 * ahciHostCount);
	printd(DEBUG_AHCI, "Identity Mapping ABAR at: 0x%016x\n", ahciABAR);
	paging_map_pages((pt_entry_t*)kKernelPML4v, (uintptr_t)ahciABAR, (uintptr_t)ahciABAR , (ABARS_PAGE_COUNT * sizeof(HBA_MEM)) / PAGE_SIZE + 1,PAGE_PRESENT | PAGE_WRITE | PAGE_PCD);
	//paging_map_page((pt_entry_t*)kKernelPML4v, kPCISATADevice.baseAdd[5],kPCISATADevice.baseAdd[5],PAGE_PRESENT | PAGE_WRITE | PAGE_PCD);
	RELOAD_CR3
	memcpy((void*)&kABARs[ahciHostCount++], (void*) ahciABAR, sizeof (HBA_MEM));
	memcpy(&ahciCaps[ahciCapsCount++], (void*) ahciABAR, sizeof (ahcicaps_t));
	if (!(ahciABAR->ghc.AE)) {
		printd(DEBUG_AHCI,"switching to AHCI mode\n");
		ahciABAR->ghc.AE=1;
	}
	ahciABAR->ghc.IE=1;
	printd(DEBUG_AHCI, "AHCI: AHCI mode enabled for device\n");
	ahci_probe_ports(ahciABAR);
}

bool init_AHCI()
{
	printd(DEBUG_AHCI, "AHCI: Initializing AHCI ...\n");
	bool ahciDeviceFound = false;
    kBlockDeviceInfoCount = 0;
	init_block();
	kBlockDeviceInfo = kmalloc(20 * sizeof(block_device_info_t));
	kBlockDeviceInfo->block_device = kmalloc(sizeof(block_device_t));
	ahciDiskBuffer = kmalloc_aligned(0x10000*20);
	//The AHCI disk buffer has to be accessed using its physical address.  So get rid of the HHMD Offset and map without it
	ahciDiskBuffer = (uintptr_t*)(((uint64_t)ahciDiskBuffer) - kHHDMOffset);
	paging_map_pages((pt_entry_t*)kKernelPML4v, (uintptr_t)ahciDiskBuffer, (uintptr_t)ahciDiskBuffer, 0x10000 / PAGE_SIZE, PAGE_PRESENT | PAGE_WRITE | PAGE_PCD);
	kABARs = kmalloc(ABARS_PAGE_COUNT * sizeof(HBA_MEM));
	ahciCaps = kmalloc(ABARS_PAGE_COUNT * sizeof(ahcicaps_t));
	if (!kPCIDeviceCount) {
        printd(DEBUG_AHCI, "AHCI: PCI not initialized, cannot initialize AHCI.");
        return false;
    }

    for (int cnt = 0; cnt < kPCIDeviceCount; cnt++)
        if (kPCIDeviceHeaders[cnt].class == 1 && kPCIDeviceHeaders[cnt].subClass == 6) 
        {
			ahciDeviceFound = true;
			init_AHCI_device(cnt,false);
        }
    for (int cnt = 0; cnt < kPCIFunctionCount; cnt++)
        if (kPCIDeviceFunctions[cnt].class == 1 && kPCIDeviceFunctions[cnt].subClass == 6) 
        {
			ahciDeviceFound = true;
			init_AHCI_device(cnt,true);
  /*            memcpy(&kPCISATADevice, &kPCIDeviceFunctions[cnt], sizeof (pci_device_t));
            ahciDeviceFound = true;
            printd(DEBUG_AHCI, "AHCI: Found AHCI controller (F) (%02X/%02X/%02X) '%s'\n", cnt, kPCIDeviceFunctions[cnt].class, kPCIDeviceFunctions[cnt].subClass, getDeviceNameP(&kPCISATADevice, buffer));
            printd(DEBUG_AHCI, "ABAR is at: 0x%08x\n", kPCISATADevice.baseAdd[5]);
            ahciABAR = (HBA_MEM*) AHCI_ABAR_REMAPPED_ADDRESS + (0x10 * ahciHostCount);
			paging_map_pages((pt_entry_t*)kKernelPML4v, (uintptr_t) ahciABAR, kPCISATADevice.baseAdd[5] , (ABARS_PAGE_COUNT * sizeof(HBA_MEM)) / PAGE_SIZE + 1,PAGE_PRESENT | PAGE_WRITE | PAGE_PCD);
			paging_map_page((pt_entry_t*)kKernelPML4v, kPCISATADevice.baseAdd[5],kPCISATADevice.baseAdd[5],PAGE_PRESENT | PAGE_WRITE | PAGE_PCD);
            RELOAD_CR3
			memcpy((void*)&kABARs[ahciHostCount++], (void*) ahciABAR, sizeof (HBA_MEM));
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
*/
        }
    if (!ahciDeviceFound) {
        printd(DEBUG_AHCI, "AHCI: No AHCI devices found.\n");
        return false;
    }


    return true;
}

void ahciSetCurrentDisk(hba_port_t* port) {
    kAHCICurrentDisk = port;
}

int find_cmdslot(volatile hba_port_t *port) {
    printd(DEBUG_AHCI, "AHCI: find_cmdslot - finding a slot to use to execute a command\n");
    // An empty command slot has its respective bit cleared to �0� in both the PxCI and PxSACT registers.
    // If not set in SACT and CI, the slot is free // Checked
    
    uint32_t slots = (/*port->sact | */port->ci);
    int num_of_slots = ahciABAR->cap.NCS;
    printd(DEBUG_AHCI,"ahciABAR = %08X, Total slots available: %d\n",ahciABAR, ahciABAR->cap.NCS);
    int i;
    for (i = 0; i < num_of_slots; i++) {

        if ((slots & 1) == 0) {
            printd(DEBUG_AHCI, "AHCI: [command slot is : %d]\n", i);
            return i;

        }
        printd(DEBUG_AHCI, "Slot %u is busy (0x%08x)\n", i,slots);
        slots >>= 1;
    }
    printd(DEBUG_AHCI, "AHCI: Cannot find free command list entry, count=%u, slots=0x%08x\n", num_of_slots, slots);
    return -1;
}

int ahci_lba_read(block_device_info_t* device, uint64_t sector, void* buffer, uint64_t sector_count) {

	// Each PRD can handle 16 sectors = 8 KiB
	uint32_t sectors_per_prd = 16;
	uint32_t prd_count = (sector_count + sectors_per_prd - 1) / sectors_per_prd;

	size_t offset_bytes = 0;
	size_t remaining_sectors = sector_count;

	if (kAHCIBuffer == NULL)
		kAHCIBuffer = kmalloc_dma(AHCI_READ_BUFFER_SIZE);

	if (sector_count == 0)
		panic("ahci_lba-read: Attempt to read a sector_count of 0\n");

	if (sector_count * 512 > AHCI_READ_BUFFER_SIZE)
		panic("ahci_lba_read: Requested read %u larger than %u\n", sector_count * 512, AHCI_READ_BUFFER_SIZE);

	hba_port_t* port = (void*)device->ioPort;

    memset(kAHCIBuffer,0,sector_count*512);
    
    printd(DEBUG_AHCI, "AHCI: read on port=0x%08x,sector=0x%08x,buffer=0x%08x,sector_count=%u\n", port,sector,buffer,sector_count);

    port->pxis.AsUlong = (uint32_t) - 1; // Clear pending interrupt bits
    //int spin = 0; // Spin lock timeout counter

    HBA_CMD_HEADER* cmdhdr = (HBA_CMD_HEADER*) (uint64_t)port->clb;
    int slot = find_cmdslot(port);
    if (slot == -1)
        return -1;
    HBA_CMD_HEADER* cmdheader = cmdhdr + slot;
    printd(DEBUG_AHCI, "AHCI: cmdheader=0x%08x\n", cmdheader);
    cmdheader->prdtl = (uint16_t) ((sector_count - 1) >> 4) + 1; // PRDT entries count

    HBA_CMD_TBL *cmdtbl = (HBA_CMD_TBL*) (uint64_t)(cmdheader->ctba);
    memset(cmdtbl, 0, sizeof (HBA_CMD_TBL) +
            (cmdheader->prdtl) * sizeof (HBA_PRDT_ENTRY));
    printd(DEBUG_AHCI, "AHCI: read - cmdtable=0x%08x,ctba=0x%08x\n", cmdtbl, cmdheader->ctba);

	cmdheader->prdtl = prd_count;

	for (uint32_t i = 0; i < prd_count; i++) {
		uint32_t chunk_sectors = remaining_sectors > sectors_per_prd
			? sectors_per_prd
			: remaining_sectors;
		size_t chunk_size = chunk_sectors * 512;

		// Set base address to kAHCIBuffer + offset
		cmdtbl->prdt_entry[i].dba_64 =
			(uint64_t)( (uint8_t*)kAHCIBuffer + offset_bytes );
		// dbc is byte count minus one
		cmdtbl->prdt_entry[i].dbc = (uint32_t)(chunk_size - 1);

		// Typically interrupt on the last entry
		cmdtbl->prdt_entry[i].i = 0;

		offset_bytes      += chunk_size;
		remaining_sectors -= chunk_sectors;
	}

    // Setup command
    FIS_REG_H2D *cmdfis = (FIS_REG_H2D*) (&cmdtbl->cfis);

    cmdfis->fis_type = FIS_TYPE_REG_H2D;
    cmdfis->c = 1; // Command
    cmdfis->command = ATA_CMD_READ_DMA_EX;
    cmdfis->device = 1 << 6; // LBA mode
    cmdfis->lba0 = (uint8_t) sector;
    cmdfis->lba1 = (uint8_t) (sector >> 8);
    cmdfis->lba2 = (uint8_t) (sector >> 16);
    cmdfis->lba3 = (uint8_t) (sector >> 24);
    cmdfis->lba4 = (uint8_t) (sector >> 32);
    cmdfis->lba5 = (uint8_t) (sector >> 40);

    cmdfis->countl = LOBYTE((uint16_t) sector_count);
    cmdfis->counth = HIBYTE((uint16_t) sector_count);

    int lCMdVal = AhciIssueCmd(port, slot);
    if (lCMdVal) {
        printd(DEBUG_AHCI, "AHCI: ***Error reading from disk***\n");
        return -2;
    }

    // Check again
    if (port->pxis.TFES) {
        printd(DEBUG_AHCI, "AHCI: Read disk error\n");
        return -3;
    }
    memcpy(buffer, kAHCIBuffer, sector_count * 512);
	return 0;
}

/*
void ahciSetCurrentPart(partEntry_t part) 
{
    kAHCICurrentPart = part;
}

int ahciBlockingRead28(uint32_t sector, uint8_t *buffer, uint32_t sector_count) {
    int prdCntr = 0;

    //CLR 06/07/2016 - Must add partition start sector
    sector+=kAHCICurrentPart.partStartSector;
    memset(buffer,0,sector_count*512);
    
    printd(DEBUG_AHCI, "AHCI: read on port=0x%08x,sector=0x%08x,buffer=0x%08x,sector_count=%u\n", kAHCICurrentDisk,sector,buffer,sector_count);

    kAHCICurrentDisk->pxis.AsUlong = (uint32_t) - 1; // Clear pending interrupt bits
    //int spin = 0; // Spin lock timeout counter

    HBA_CMD_HEADER* cmdhdr = (HBA_CMD_HEADER*) kAHCICurrentDisk->clb;
    int slot = find_cmdslot(kAHCICurrentDisk);
    if (slot == -1)
        return false;
    HBA_CMD_HEADER* cmdheader = cmdhdr + slot;
    printd(DEBUG_AHCI, "AHCI: cmdheader=0x%08x\n", cmdheader);
    cmdheader->prdtl = (uint16_t) ((sector_count - 1) >> 4) + 1; // PRDT entries count

    HBA_CMD_TBL *cmdtbl = (HBA_CMD_TBL*) (cmdheader->ctba);
    memset(cmdtbl, 0, sizeof (HBA_CMD_TBL) +
            (cmdheader->prdtl - 1) * sizeof (HBA_PRDT_ENTRY));
    printd(DEBUG_AHCI, "AHCI: read - cmdtable=0x%08x,ctba=0x%08x\n", cmdtbl, cmdheader->ctba);

    // 8K bytes (16 sectors) per PRDT
    for (int i = 0; i < cmdheader->prdtl - 1; i++) {
        cmdtbl->prdt_entry[prdCntr].dba = (uint32_t) buffer;
        cmdtbl->prdt_entry[prdCntr].dbc = 8 * 1024; // 8K bytes
        cmdtbl->prdt_entry[prdCntr].i = 1;
        buffer += 4 * 1024; // 4K words
        sector_count -= 16; // 16 sectors
        prdCntr++;
    }
    // Last entry
    cmdtbl->prdt_entry[prdCntr].dba = (uint32_t) buffer;
    cmdtbl->prdt_entry[prdCntr].dbc = sector_count << 9; // 512 bytes per sector
    cmdtbl->prdt_entry[prdCntr].i = 1;

    // Setup command
    FIS_REG_H2D *cmdfis = (FIS_REG_H2D*) (&cmdtbl->cfis);

    cmdfis->fis_type = FIS_TYPE_REG_H2D;
    cmdfis->c = 1; // Command
    cmdfis->command = ATA_CMD_READ_DMA_EX;
    cmdfis->lba0 = (uint8_t) sector;
    cmdfis->lba1 = (uint8_t) (sector >> 8);
    cmdfis->lba2 = (uint8_t) (sector >> 16);
    cmdfis->device = 1 << 6; // LBA mode

    cmdfis->lba3 = (uint8_t) (sector >> 24);
    cmdfis->lba4 = (uint8_t) 0;
    cmdfis->lba5 = (uint8_t) 0;

    cmdfis->countl = LOBYTE((uint16_t) sector_count);
    cmdfis->counth = HIBYTE((uint16_t) sector_count);

    int lCMdVal = AhciIssueCmd(kAHCICurrentDisk, slot);
    if (!lCMdVal) {
        printd(DEBUG_AHCI, "AHCI: ***Error reading from disk***\n");
        return -1;
    }

    // Check again
    if (kAHCICurrentDisk->pxis.TFES) {
        printd(DEBUG_AHCI, "AHCI: Read disk error\n");
        return false;
    }
    return true;
}

int ahciRead(volatile hba_port_t* port, int sector, uint8_t* buffer, int sector_count) {
    ahciSetCurrentDisk(port);
    if (ahciBlockingRead28(sector, buffer, sector_count))
        return sector_count;
    else
        return 0;
}
*/