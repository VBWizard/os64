/* 
 * File:   ahci.h
 * Author: yogi
 *
 * Created on May 25, 2016, 10:30 PM
 */

#ifndef AHCI_H
#define	AHCI_H

/*Portions from: https://www.virtualbox.org/svn/vbox/trunk/src/VBox/Devices/PC/BIOS/ahci.c*/
/* AHCI host adapter driver to boot from SATA disks.
 */

/*
 * Copyright (C) 2011-2024 Oracle and/or its affiliates.
 *
 * This file is part of VirtualBox base platform packages, as
 * available from https://www.virtualbox.org.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation, in version 3 of the
 * License.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <https://www.gnu.org/licenses>.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include <stdint.h>
#include <stdbool.h>
#include "vfs.h"

#define ABARS_PAGE_COUNT 10
#define AHCI_ABAR_REMAPPED_ADDRESS 0xFFF00000
#define AHCI_PORT_BASE_REMAP_ADDRESS 0x4400000
#define COMMAND_TIMEOUT 1000 // Total timeout in milliseconds
#define POLL_INTERVAL    100  // Polling interval in milliseconds

#define ATA_CMD_READ_DMA 0xC8
#define ATA_CMD_READ_DMA_EX 0x25
#define ATA_CMD_WRITE_DMA 0xCA
#define ATA_CMD_WRITE_DMA_EX 0x35

#define ATA_DEV_BUSY 0x80
#define ATA_DEV_DRQ 0x08

#define LOBYTE(w) ((uint8_t)(w))
#define HIBYTE(w) ((uint8_t)(((uint16_t)(w) >> 8) & 0xFF))
#define LOWORD(w) ((uint16_)(w))
#define HIWORD(w) ((uint16_t)(((uint32_t)(w) >> 16)&0xFFFF))


#define VDS_SERVICE             0x81

#define VDS_GET_VERSION         0x02    /* Get version */
#define VDS_LOCK_BUFFER         0x03    /* Lock DMA buffer region */
#define VDS_UNLOCK_BUFFER       0x04    /* Unlock DMA buffer region */
#define VDS_SG_LOCK             0x05    /* Scatter/gather lock region */
#define VDS_SG_UNLOCK           0x06    /* Scatter/gather unlock region */
#define VDS_REQUEST_BUFFER      0x07    /* Request DMA buffer */
#define VDS_RELEASE_BUFFER      0x08    /* Release DMA buffer */
#define VDS_BUFFER_COPYIN       0x09    /* Copy into DMA buffer */
#define VDS_BUFFER_COPYOUT      0x0A    /* Copy out of DMA buffer */
#define VDS_DISABLE_DMA_XLAT    0x0B    /* Disable DMA translation */
#define VDS_ENABLE_DMA_XLAT     0x0C    /* Enable DMA translation */

/* VDS error codes */

#define VDS_SUCCESS             0x00    /* No error */
#define VDS_ERR_NOT_CONTIG      0x01    /* Region not contiguous */
#define VDS_ERR_BOUNDRY_CROSS   0x02    /* Rgn crossed phys align boundary */
#define VDS_ERR_CANT_LOCK       0x03    /* Unable to lock pages */
#define VDS_ERR_NO_BUF          0x04    /* No buffer available */
#define VDS_ERR_RGN_TOO_BIG     0x05    /* Region too large for buffer */
#define VDS_ERR_BUF_IN_USE      0x06    /* Buffer currently in use */
#define VDS_ERR_RGN_INVALID     0x07    /* Invalid memory region */
#define VDS_ERR_RGN_NOT_LOCKED  0x08    /* Region was not locked */
#define VDS_ERR_TOO_MANY_PAGES  0x09    /* Num pages greater than table len */
#define VDS_ERR_INVALID_ID      0x0A    /* Invalid buffer ID */
#define VDS_ERR_BNDRY_VIOL      0x0B    /* Buffer boundary violated */
#define VDS_ERR_INVAL_DMACHN    0x0C    /* Invalid DMA channel number */
#define VDS_ERR_COUNT_OVRFLO    0x0D    /* Disable count overflow */
#define VDS_ERR_COUNT_UNDRFLO   0x0E    /* Disable count underflow */
#define VDS_ERR_UNSUPP_FUNC     0x0F    /* Function not supported */
#define VDS_ERR_BAD_FLAG        0x10    /* Reserved flag bits set in DX */

/* VDS option flags */

#define VDSF_AUTOCOPY           0x02    /* Automatic copy to/from buffer */
#define VDSF_NOALLOC            0x04    /* Disable auto buffer allocation */
#define VDSF_NOREMAP            0x08    /* Disable auto remap feature */
#define VDSF_NO64K              0x10    /* Region can't cross 64K boundary */
#define VDSF_NO128K             0x20    /* Region can't cross 128K boundary */
#define VDSF_COPYTBL            0x40    /* Copy page table for S/G remap */
#define VDSF_NPOK               0x80    /* Allow non-present pages for S/G */
/* Number of S/G table entries in EDDS. */
#define NUM_EDDS_SG         16
/*SATA device types*/
#define	SATA_SIG_ATA	0x00000101	// SATA drive
#define	SATA_SIG_ATAPI	0xEB140101	// SATAPI drive
#define	SATA_SIG_SEMB	0xC33C0101	// Enclosure management bridge
#define	SATA_SIG_PM	0x96690101	// Port multiplier
/*FROM: https://raw.githubusercontent.com/rajesh5310/SBUnix/master/sys/ahci.c*/
#define AHCI_DEV_NULL 0
#define AHCI_DEV_SATA 1
#define AHCI_DEV_SATAPI 4
#define AHCI_DEV_SEMB 2
#define AHCI_DEV_PM 3
#define HBA_PORT_DET_PRESENT 3
#define HBA_PORT_DET_NOT_PRESENT 0
#define HBA_PORT_IPM_ACTIVE 1
#define HBA_PxIS_TFES	(1 << 30)	/* TFES - Task File Error Status */
#define HBA_PxIS_HBFS	(1 << 29)	/* HBFS - Host Bus Fatal Error Status */
#define HBA_PxIS_HBDS	(1 << 28)	/* HBDS - Host Bus Data Error Status */
#define HBA_PxIS_IFS	(1 << 27)	/* IFS - Interface Fatal Error Status */
#define HBA_PxIS_FATAL	(HBA_PxIS_TFES | HBA_PxIS_HBFS | \
				HBA_PxIS_HBDS | HBA_PxIS_IFS)
#define HBA_PxIS_PCS	(1 <<  6)	/* PCS - Port Connect Change Status */
#define HBA_PxCMD_ICC_SHIFT	28
#define HBA_PxCMD_ICC_MASK	(0xf << HBA_PxCMD_ICC_SHIFT)
#define HBA_PxCMD_ICC_ACTIVE	(0x1 << HBA_PxCMD_ICC_SHIFT)
#define HBA_PxCMD_CR		(1 << 15) /* CR - Command list Running */
#define HBA_PxCMD_FR		(1 << 14) /* FR - FIS receive Running */
#define HBA_PxCMD_FRE		(1 <<  4) /* FRE - FIS Receive Enable */
#define HBA_PxCMD_SUD		(1 <<  1) /* SUD - Spin-Up Device */
#define HBA_PxCMD_ST		(1 <<  0) /* ST - Start (command processing) */

/*
 * AHCI PRDT structure.
 */
typedef struct
{
    uint32_t    phys_addr;
    uint32_t    something;
    uint32_t    reserved;
    uint32_t    len;
} ahci_prdt;


//Command and Status as defined in AHCI1.0 section 3.3.7
typedef union _AHCI_COMMAND {
 
    struct {
        //LSB
        uint32_t ST :1; //0: Start: When set, the HBA may process the command list. When cleared, the HBA may not process the command list. Whenever this bit is changed from a ?0 to a, the HBA starts processing the command list at entry ?0 . Whenever this bit is changed from a ?1 to a ?0 , the PxCI register is cleared by the HBA upon the HBA putting the controller into an idle state. This bit shall only be set to 1 by software after PxCMD.FRE has been set to 1. Refer to section 10.3.1 for important restrictions on when ST can be set to ?1 .
        uint32_t SUD :1; //1: Spin-Up Device: This bit is read/write for HBAs that support staggered spin-up via CAP.SSS. This bit is read only ?1 for HBAs that do not support staggered spin-up. On an edge detect from ?0 to ?1 , the HBA shall start a COMRESET initializatoin sequence to the device. Clearing this bit to 0 does not cause any OOB signal to be sent on the interface.  When this bit is cleared to 0 and PxSCTL.DET=0h, the HBA will enter listen mode as detailed in section 10.9.1.
        uint32_t POD :1; //2: Power On Device: This bit is read/write for HBAs that support cold presence detection on this port as indicated by PxCMD.CPD set to ?1 . This bit is read only ?1 for HBAs that do not support cold presence detect. When set, the HBA sets the state of a pin on the HBA to ?1 so that it may be used to provide power to a cold-presence detectable port.
        uint32_t CLO :1; //3: Command List Override: Setting this bit to ?1 causes PxTFD.STS.BSY and PxTFD.STS.DRQ to be cleared to ?0 . This allows a software reset to be transmitted to the device regardless of whether the BSY and DRQ bits are still set in the PxTFD.STS register. The HBA sets this bit to ?0 when PxTFD.STS.BSY and PxTFD.STS.DRQ have been cleared to ?0 . A write to this register with a value of ?0 shall have no effect. This bit shall only be set to ?1 immediately prior to setting the PxCMD.ST bit to ?1 from previous value of ?0 . Setting this bit to ?1 at any other time is not supported and will result in indeterminate behavior.
        uint32_t FRE :1; //4: FIS Receive Enable: When set, the HBA may post received FISes into the FIS receive area pointed to by PxFB (and for 64-bit HBAs, PxFBU). When cleared, received FISes are not accepted by the HBA, except for the first D2H register FIS after the initialization sequence, and no FISes are posted to the FIS receive area. System software must not set this bit until PxFB (PxFBU) have been programmed with valid pointer to the FIS receive area, and if software wishes to move the base, this bit must first be cleared, and software must wait for the FR bit in this register to be cleared. Refer to section 10.3.2 for important restrictions on when FRE can be set and cleared.
        uint32_t DW6_Reserved :3; //5-7
 
        uint32_t CCS :5; //8-12: Current Command Slot: This field is valid when P0CMD.ST is set to ?1 and shall be set to the command slot value of the command that is currently being issued by the HBA. When P0CMD.ST transitions from ?1 to ?0 , this field shall be reset to ?0 . After P0CMD.ST transitions from ?0 to ?1 , the highest priority slot to issue from next is command slot 0. After the first command has been issued, the highest priority slot to issue from next is P0CMD.CCS + 1. For example, after the HBA has issued its first command, if CCS = 0h and P0CI is set to 3h, the next command that will be issued is from command slot 1.
        uint32_t MPSS :1; //13: Mechanical Presence Switch State: The MPSS bit reports the state of an interlocked switch attached to this port. If CAP.SIS is set to ?1? and the interlocked switch is closed then this bit is cleared to ?0 . If CAP.SIS is set to ?1 and the interlocked switch is open then this bit is set to ?1 . If CAP.SIS is set to ?0 then this bit is cleared to ?0 . Software should only use this bit if both CAP.SIS and P0CMD.ISP are set to ?1 .
        uint32_t FR :1; //14: FIS Receive Running: When set, the FIS Receive DMA engine for the port is running. See section 10.3.2 for details on when this bit is set and cleared by the HBA.
        uint32_t CR :1; //15: Command List Running: When this bit is set, the command list DMA engine for the port is running. See the AHCI state machine in section 5.2.2 for details on when this bit is set and cleared by the HBA.
 
        uint32_t CPS :1; //16: Cold Presence State: The CPS bit reports whether a device is currently detected on this port. If CPS is set to ?1 , then the HBA detects via cold presence that a device is attached to this port. If CPS is cleared to ?0 , then the HBA detects via cold presence that there is no device attached to this port.
        uint32_t PMA :1; //17: Port Multiplier Attached: This bit is read/write for HBAs that support a Port Multiplier (CAP.SPM = ?1 ). This bit is read-only for HBAs that do not support a port Multiplier (CAP.SPM = ?0 ). When set to ?1 by software, a Port Multiplier is attached to the HBA for this port. When cleared to ?0 by software, a Port Multiplier is not attached to the HBA for this port. Software is responsible for detecting whether a Port Multiplier is present; hardware does not auto-detect the presence of a Port Multiplier.
        uint32_t HPCP :1;//18: Hot Plug Capable Port:  When set to 1, indicates that this ports signal and power connectors are externally accessible via a joint signal and power connector for blindmate device hot plug.  When cleared to 0, indicates that this ports signal and power connectors are not externally accessible via a joint signal and power connector.
        uint32_t MPSP :1;//19: Mechanical Presence Switch Attached to Port : If set to ?1 , the platform supports an interlocked switch attached to this port. If cleared to ?0 , the platform does not support an interlocked switch attached to this port. When this bit is set to ?1 , P0CMD.HPCP should also be set to ?1 .
        uint32_t CPD :1; //20: Cold Presence Detection: If set to ?1 , the platform supports cold presence detection on this port. If cleared to ?0 , the platform does not support cold presence detection on this port. When this bit is set to ?1 , P0CMD.HPCP should also be set to ?1 .
        uint32_t ESP :1; //21: AHCI 1.1 External SATA Port: When set to '1', indicates that this ports signal connector is externally accessible on a signal only connector. When set to '1', CAP.SXS shall be set to '1'. When cleared to 0, indicates that this ports signal connector is not externally accessible on a signal only connector.  ESP is mutually exclusive with the HPCP bit in this register.
        uint32_t FBSCP :1; //22: FIS-based Switching Capable Port (FBSCP): When set to 1, indicates that this port supports Port Multiplier FIS-based switching.  When cleared to 0, indicates that this port does not support FIS-based switching. This bit may only be set to 1 if both CAP.SPM and CAP.FBSS are set to 1.
        uint32_t APSTE :1; // Automatic Partial to Slumber Transitions Enabled (APSTE): When set to 1, the HBA may perform Automatic Partial to Slumber Transitions.  When cleared to 0 the port shall not perform Automatic Partial to Slumber Transitions.  Software shall only set this bit to 1 if CAP2.APST is set to 1; if CAP2.APST is cleared to 0 software shall treat this bit as reserved.
 
        uint32_t ATAPI :1; // Device is ATAPI: When set, the connected device is an ATAPI device. This bit is used by the HBA to control whether or not to generate the desktop LED when commands are active. See section 10.10 for details on the activity LED.
        uint32_t DLAE :1; // Drive LED on ATAPI Enable: When set, the HBA shall drive the LED pin active for commands regardless of the state of P0CMD.ATAPI. When cleared, the HBA shall only drive the LED pin active for commands if P0CMD.ATAPI set to ?0 . See section 10.10 for details on the activity LED.
        uint32_t ALPE :1; // Aggressive Link Power Management Enable: When set to '1', the HBA shall aggressively enter a lower link power state (Partial or Slumber) based upon the setting of the ASP bit. Software shall only set this bit to 1 if CAP.SALP is set to 1; if CAP.SALP is cleared to 0 software shall treat this bit as reserved. See section 8.3.1.3 for details.
        uint32_t ASP :1; // Aggressive Slumber / Partial: When set to '1', and ALPE is set, the HBA shall aggressively enter the Slumber state when it clears the PxCI register and the PxSACT register is cleared or when it clears the PxSACT register and PxCI is cleared. When cleared, and ALPE is set, the HBA shall aggressively enter the Partial state when it clears the PxCI register and the PxSACT register is cleared or when it clears the PxSACT register and PxCI is cleared.  If CAP.SALP is cleared to 0 software shall treat this bit as reserved.  See section 8.3.1.3 for details.
        uint32_t ICC :4; // Interface Communication Control: This field is used to control power management states of the interface. If the Link layer is currently in the L_IDLE state, writes to this field shall cause the HBA to initiate a transition to the interface power management state requested. If the Link layer is not currently in the L_IDLE state, writes to this field shall have no effect.
        //MSB
    };
 
    uint32_t AsUlong;
 
}  AHCI_COMMAND, *PAHCI_COMMAND;

//Interrupt Enable as defined in AHCI1.0 section 3.3.6
typedef union _AHCI_INTERRUPT_ENABLE {
 
    struct {
        //LSB
        uint32_t DHRE :1; // Device to Host Register FIS Interrupt Enable: When set, GHC.IE is set, and P0IS.DHRS is set, the HBA shall generate an interrupt.
        uint32_t PSE :1; // PIO Setup FIS Interrupt Enable: When set, GHC.IE is set, and P0IS.PSS is set, the HBA shall generate an interrupt.
        uint32_t DSE :1; // DMA Setup FIS Interrupt Enable: When set, GHC.IE is set, and P0IS.DSS is set, the HBA shall generate an interrupt.
        uint32_t SDBE :1; // Set Device Bits FIS Interrupt Enable: When set, GHC.IE is set, and P0IS.SDBS is set, the HBA shall generate an interrupt.
        uint32_t UFE :1; // Unknown FIS Interrupt Enable: When set, GHC.IE is set, and P0IS.UFS is set to ?1 , the HBA shall generate an interrupt.
        uint32_t DPE :1; // Descriptor Processed Interrupt Enable: When set, GHC.IE is set, and P0IS.DPS is set, the HBA shall generate an interrupt.
        uint32_t PCE :1; // Port Change Interrupt Enable: When set, GHC.IE is set, and P0IS.PCS is set, the HBA shall generate an interrupt.
        uint32_t DMPE :1; //Device Mechanical Presence Enable (DMPE): When set, and GHC.IE is set to 1, and P0IS.DMPS is set, the HBA shall generate an interrupt.  For systems that do not support a mechanical presence switch, this bit shall be a read-only 0.
        uint32_t DW5_Reserved :14; //
        uint32_t PRCE :1; // PhyRdy Change Interrupt Enable: When set to ?1 , and GHC.IE is set to ?1 , and P0IS.PRCS is set to ?1 , the HBA shall generate an interrupt.
        uint32_t IPME :1; // Incorrect Port Multiplier Enable: When set, and GHC.IE and P0IS.IPMS are set, the HBA shall generate an interupt.
        uint32_t OFE :1; // Overflow Enable: When set, and GHC.IE and P0IS.OFS are set, the HBA shall generate an interupt.
        uint32_t DW5_Reserved2 :1; //
        uint32_t INFE :1; // Interface Non-fatal Error Enable: When set, GHC.IE is set, and P0IS.INFS is set, the HBA shall generate an interrupt.
        uint32_t IFE :1; // Interface Fatal Error Enable: When set, GHC.IE is set, and P0IS.IFS is set, the HBA shall generate an interrupt..
        uint32_t HBDE :1; // Host Bus Data Error Enable: when set, GHC.IE is set, and P0IS.HBDS is set, the HBA shall generate an interrupt..
        uint32_t HBFE :1; // Host Bus Fatal Error Enable: When set, GHC.IE is set, and P0IS.HBFS is set, the HBA shall generate an interrupt.
        uint32_t TFEE :1; // Task File Error Enable: When set, GHC.IE is set, and P0S.TFES is set, the HBA shall generate an interrupt.
        uint32_t CPDE :1; //Cold Presence Detect Enable: When set, GHC.IE is set, and P0S.CPDS is set, the HBA shall generate an interrupt. For systems that do not support cold presence detect, this bit shall be a read-only ?0 .
        //MSB
    };
 
    uint32_t AsUlong;
 
} AHCI_INTERRUPT_ENABLE, *PAHCI_INTERRUPT_ENABLE;
 
//HBA Capabilities as defined in AHCI1.0 section 3.1.1
typedef union _AHCI_HBA_CAPABILITIES  {
 
    struct {
        //LSB
        uint32_t NP :5;  //0-4: Number of Ports: 0 s based value indicating the maximum number of ports supported by the HBA silicon. A maximum of 32 ports can be supported. A value of ?0h?, indicating one port, is the minimum requirement. Note that the number of ports indicated in this field may be more than the number of ports indicated in the GHC.PI register.
        uint32_t SXS :1; //5: AHCI 1.1 Supports External SATA: When set to 1, indicates that the HBA has one or more Serial ATA ports that has a signal only connector that is externally accessible. If this bit is set, software may refer to the PxCMD.ESP bit to determine whether a specific port has its signal connector externally accessible as a signal only connector (i.e. power is not part of that connector).  When the bit is cleared to 0, indicates that the HBA has no Serial ATA ports that have a signal only connector externally accessible.
        uint32_t EMS :1; //6: AHCI 1.1 Enclosure Management Supported: When set to 1, indicates that the HBA supports enclosure management as defined in section 12.  When enclosure management is supported, the HBA has implemented the EM_LOC and EM_CTL global HBA registers.  When cleared to 0, indicates that the HBA does not support enclosure management and the EM_LOC and EM_CTL global HBA registers are not implemented.
        uint32_t CCCS :1; //7: AHCI 1.1 Command Completion Coalescing Supported: When set to 1, indicates that the HBA supports command completion coalescing as defined in section 11.  When command completion coalescing is supported, the HBA has implemented the CCC_CTL and the CCC_PORTS global HBA registers.  When cleared to 0, indicates that the HBA does not support command completion coalescing and the CCC_CTL and CCC_PORTS global HBA registers are not implemented.
        uint32_t NCS :5; //8-12: Number of Command Slots: 0 s based value indicating the number of command slots supported by this HBA. A minimum of 1 and maximum of 32 slots can be supported.
        uint32_t PSC :1; //13: Partial State Capable: Indicates whether the HBA can support transitions to the Partial state. When cleared to ?0 , software must not allow the HBA to initiate transitions to the Partial state via agressive link power management nor the PxCMD.ICC field in each port, and the PxSCTL.IPM field in each port must be programmed to disallow device initiated Partial requests. When set to ?1 , HBA and device initiated Partial requests can be supported.
        uint32_t SSC :1; //14: Slumber State Capable: Indicates whether the HBA can support transitions to the Slumber state. When cleared to ?0 , software must not allow the HBA to initiate transitions to the Slumber state via agressive link power management nor the PxCMD.ICC field in each port, and the PxSCTL.IPM field in each port must be programmed to disallow device initiated Slumber requests. When set to ?1 , HBA and device initiated Slumber requests can be supported.
        uint32_t PMD :1; //15: PIO Multiple DRQ Block: If set to 1 , the HBA supports multiple DRQ block data transfers for the PIO command protocol. If cleared to ?0 the HBA only supports single DRQ block data transfers for the PIO command protocol.
        uint32_t FBSS :1; //16: AHCI 1.1  FIS-based Switching Supported: When set to 1, indicates that the HBA supports Port Multiplier FIS-based switching. When cleared to 0, indicates that the HBA does not support FIS-based switching.  AHCI 1.0 and 1.1 HBAs shall have this bit cleared to 0.
        uint32_t SPM :1; //17: Supports Port Multiplier: Indicates whether the HBA can support a Port Multiplier. When set, a Port Multiplier using command-based switching is supported. When cleared to ?0 , a Port Multiplier is not supported, and a Port Multiplier may not be attached to this HBA.
        uint32_t SAM :1; //18: Supports AHCI mode only: The SATA controller may optionally support AHCI access mechanisms only. A value of '0' indicates that in addition to the native AHCI mechanism (via ABAR), the SATA controller implements a legacy, task-file based register interface such as SFF-8038i. A value of '1' indicates that the SATA controller does not implement a legacy, task-file based register interface.
        uint32_t SNZO :1; //19: Supports Non-Zero DMA Offsets: When set to 1 , indicates that the HBA can support non-zero DMA offsets for DMA Setup FISes. This bit is reserved for future AHCI enhancements. AHCI 1.0 HBAs must have this bit cleared to ?0 .
        uint32_t ISS :4; //20-23: Interface Speed Support: Indicates the maximum speed the HBA can support on its ports. These encodings match the PxSCTL.DET.SPD field, which is programmable by system software. Values are:
        uint32_t SCLO :1; //24: Supports Command List Override: When set to 1 , indicates that the HBA supports the PxCMD.CLO bit and its associated function. When cleared to ?0 , the HBA is not capable of clearing the BSY and DRQ bits in the Status register in order to issue a software reset if these bits are still set from a previous operation.
        uint32_t SAL :1; //25: Supports Activity LED: When set to 1 , indicates that the HBA supports a single output pin which indicates activity. This pin can be connected to an LED on the platform to indicate device activity on any drive. See section 10.10 for more information.
        uint32_t SALP :1; //26: Supports Aggressive Link Power Management: When set to ?1 , indicates that the HBA can support auto-generating link requests to the Partial or Slumber states when there are no commands to process. Refer to section 8.3.1.3.
        uint32_t SSS :1; //27: Supports Staggered Spin-up: When set to 1 , indicates that the HBA supports staggered spin-up on its ports, for use in balancing power spikes. This value is loaded by the BIOS prior to OS initiallization.
        uint32_t SMPS :1; //28: Supports Mechanical Presence Switch: ): When set to 1, the HBA supports mechanical presence switches on its ports for use in hot plug operations.  When cleared to 0, this function is not supported.  This value is loaded by the BIOS prior to OS initialization.
        uint32_t SSNTF :1; //29: AHCI 1.1 Supports SNotification Register: When set to 1, indicates that the HBA supports the PxSNTF (SNotification) register and its associated functionality.  When cleared to 0, the HBA does not support the PxSNTF (SNotification) register and its associated functionality.  Refer to section 10.10.1.
        uint32_t SNCQ :1; //30: Supports Native Command Queuing: Indicates whether the HBA supports Serial ATA native command queuing. If set to ?1 , an HBA shall handle DMA Setup FISes natively, and shall handle the auto-activate optimization through that FIS. If cleared to ?0 , native command queuing is not supported and software should not issue any native command queuing commands.
        uint32_t S64A :1; //31: Supports 64-bit Addressing: Indicates whether the HBA can access 64-bit data structures. If true, the HBA shall make the 32-bit upper bits of the port DMA Descriptor, the PRD Base, and each PRD entry read/write. If cleared, these are read- only and treated as ?0 by the HBA.
        //MSB
    };
 
    uint32_t caps;
 
}  AHCI_HBA_CAPABILITIES, *PAHCI_HBA_CAPABILITIES;

//Global HBA Control as defined in AHCI1.0 section 3.1.2
typedef union _AHCI_Global_HBA_CONTROL {
 
    struct {
        //LSB
        uint32_t HR :1; //HBA Reset: When set by SW, this bit causes an internal reset of the HBA. All state machines that relate to data transfers and queuing shall return to an idle condition, and all ports shall be re-initialized via COMRESET (if staggered spin-up is not supported). If staggered spin-up is supported, then it is the responsibility of software to spin-up each port after the reset has completed. When the HBA has performed the reset action, it shall reset this bit to ?0 . A software write of ?0 shall have no effect. For a description on which bits are reset when this bit is set, see section 10.4.3.
        uint32_t IE :1; //Interrupt Enable: This global bit enables interrupts from the HBA. When cleared (reset default), all interrupt sources from all ports are disabled. When set, interrupts are enabled.
        uint32_t MRSM :1; //MSI Revert to Single Message: When set to 1 by hardware, indicates that the HBA requested more than one MSI vector but has reverted to using the first vector only.  When this bit is cleared to 0, the HBA has not reverted to single MSI mode (i.e. hardware is already in single MSI mode, software has allocated the number of messages requested, or hardware is sharing interrupt vectors if MC.MME < MC.MMC).
        uint32_t Reserved :28; //3-30
        uint32_t AE :1; //AHCI Enable: When set, indicates that communication to the HBA shall be via AHCI mechanisms. This can be used by an HBA that supports both legacy mechanisms (such as SFF-8038i) and AHCI to know when the HBA is running under an AHCI driver. When set, software shall only communicate with the HBA using AHCI. When cleared, software shall only communicate with the HBA using legacy mechanisms. When cleared FISes are not posted to memory and no commands are sent via AHCI mechanisms. Software shall set this bit to ?1 before accessing other AHCI registers. The implementation of this bit is dependent upon the value of the CAP.SAM bit. If CAP.SAM is '0', then GHC.AE shall be read-write and shall have a reset value of '0'. If CAP.SAM is '1', then AE shall be read-only and shall have a reset value of '1'.
        //MSB
    };
 
    uint32_t ctl;
 
} AHCI_Global_HBA_CONTROL, *PAHCI_Global_HBA_CONTROL;
 
//Serial ATA Control as defined in AHCI1.0 section 3.3.11
typedef union _AHCI_SERIAL_ATA_CONTROL {
 
    struct {
          //LSB
        uint32_t DET :4; // Device Detection Initialization: Controls the HBA's device detection and interface initialization. 
                         // 0h No device detection or initialization action requested 
                         // 1h Perform interface communication initialization sequence to establish communication. This is functionally equivalent to a hard reset and results in the interface being reset and communications reinitialized. While this field is 1h, COMRESET is continuously transmitted on the interface. Software should leave the DET field set to 1h for a minimum of 1 millisecond to ensure that a COMRESET is sent on the interface. 4h Disable the Serial ATA interface and put Phy in offline mode. All other values reserved This field may only be modified when P0CMD.ST is '0 . Changing this field while the P0CMD.ST bit is set to '1 results in undefined behavior. When P0CMD.ST is set to '1 , this field should have a value of 0h.Note: It is permissible to implement any of the Serial ATA defined behaviors for transmission of COMRESET when DET=1h.
        uint32_t SPD :4; // Speed Allowed: Indicates the highest allowable speed of the interface. 0h No speed negotiation restrictions 1h Limit speed negotiation to Generation 1 communication rate 2h Limit speed negotiation to a rate not greater than Generation 2 communication rate All other values reserved
        uint32_t IPM :4; // Interface Power Management Transitions Allowed: Indicates which power states the HBA is allowed to transition to. If an interface power management state is disabled, the HBA is not allowed to initiate that state and the HBA must PMNAK P any request from the device to enter that state. 0h No interface restrictions 1h Transitions to the Partial state disabled 2h Transitions to the Slumber state disabled 3h Transitions to both Partial and Slumber states disabled All other values reserved
        uint32_t SPM :4; // Select Power Management: This field is not used by AHCI
        uint32_t PMP :4; // Port Multiplier Port: This field is not used by AHCI.
        uint32_t DW11_Reserved :12;
        //MSB
    };
 
    uint32_t AsUlong;
 
}  AHCI_SERIAL_ATA_CONTROL;

 //Error in Serial ATA Error as defined in AHCI1.0 section 3.3.12
typedef union _AHCI_SERIAL_ATA_ERROR_ERROR {
 
    struct {
        //LSB
        uint32_t I :1; //Recovered Data Integrity Error: A data integrity error occurred that was recovered by the interface through a retry operation or other recovery action.
        uint32_t M :1; //Recovered Communications Error: Communications between the device and host was temporarily lost but was re-established. This can arise from a device temporarily being removed, from a temporary loss of Phy synchronization, or from other causes and may be derived from the PhyNRdy signal between the Phy and Link layers.
        uint32_t Reserved :6; //
        uint32_t T :1; //Transient Data Integrity Error: A data integrity error occurred that was not recovered by the interface.
        uint32_t C :1; //Persistent Communication or Data Integrity Error: A communication error that was not recovered occurred that is expected to be persistent. Persistent communications errors may arise from faulty interconnect with the device, from a device that has been removed or has failed, or a number of other causes.
        uint32_t P :1; //Protocol Error: A violation of the Serial ATA protocol was detected.
        uint32_t E :1; //Internal Error: The host bus adapter experienced an internal error that caused the operation to fail and may have put the host bus adapter into an error state.  The internal error may include a master or target abort when attempting to access system memory, an elasticity buffer overflow, a primitive mis-alignment, a synchronization FIFO overflow, and other internal error conditions.  Typically when an internal error occurs, a non-fatal or fatal status bit in the PxIS register will also be set to give software guidance on the recovery mechanism required.
        uint32_t Reserved2 :4;
        //MSB
    //Diagnostics in Serial ATA Error as defined in AHCI1.0 section 3.3.12
        //LSB
        uint32_t N :1; //0 - PhyRdy Change: Indicates that the PhyRdy signal changed state. This bit is reflected in the P0IS.PRCS bit.
        uint32_t INE :1; //1 - Phy Internal Error: Indicates that the Phy detected some internal error.
        uint32_t W :1; //2 - Comm Wake: Indicates that a Comm Wake signal was detected by the Phy.
        uint32_t B :1; //3 - 10B to 8B Decode Error: Indicates that one or more 10B to 8B decoding errors occurred.
        uint32_t D :1; //4 - Disparity Error: This field is not used by AHCI.
        uint32_t CRC :1; //5 - CRC Error: Indicates that one or more CRC errors occurred with the Link Layer.
        uint32_t H :1; //6 - Handshake Error: Indicates that one or more R_ERR handshake response was received in response to frame transmission. Such errors may be the result of a CRC error detected by the recipient, a disparity or 8b/10b decoding error, or other error condition leading to a negative handshake on a transmitted frame.
        uint32_t S :1; //7 - Link Sequence Error: Indicates that one or more Link state machine error conditions was encountered. The Link Layer state machine defines the conditions under which the link layer detects an erroneous transition.
        uint32_t TRANS :1; //8 - Transport state transition error: Indicates that an error has occurred in the transition from one state to another within the Transport layer since the last time this bit was cleared.
        uint32_t F :1; //9 - Unknown FIS Type: Indicates that one or more FISs were received by the Transport layer with good CRC, but had a type field that was not recognized/known.
        uint32_t X :1; //10 - Exchanged: When set to one this bit indicates that a change in device presence has been detected since the last time this bit was cleared.  The means by which the implementation determines that the device presence has changed is vendor specific. This bit shall always be set to one anytime a COMINIT signal is received.  This bit is reflected in the P0IS.PCS bit.
        uint32_t Reserved3 :5;
        //MSB
    };
 
    uint32_t AsUlong;
} AHCI_SERIAL_ATA_ERROR_ERROR;

//Interrupt Status as defined in AHCI section 3.3.5
typedef union _AHCI_INTERRUPT_STATUS {
 
    struct {
        //LSB
        uint32_t DHRS:1; //Device to Host Register FIS Interrupt: A D2H Register FIS has been received with the 'I' bit set, and has been copied into system memory.
        uint32_t PSS :1; //PIO Setup FIS Interrupt: A PIO Setup FIS has been received with the 'I' bit set, it has been copied into system memory, and the data related to that FIS has been transferred. This bit shall be set even if the data transfer resulted in an error.
        uint32_t DSS :1; //DMA Setup FIS Interrupt: A DMA Setup FIS has been received with the 'I' bit set and has been copied into system memory.
        uint32_t SDBS :1; //Set Device Bits Interrupt: A Set Device Bits FIS has been received with the 'I' bit set and has been copied into system memory.
        uint32_t UFS :1; //Unknown FIS Interrupt: When set to '1 , indicates that an unknown FIS was received and has been copied into system memory. This bit is cleared to ?0 by software clearing the PxSERR.DIAG.F bit to ?0 . Note that this bit does not directly reflect the PxSERR.DIAG.F bit. PxSERR.DIAG.F is set immediately when an unknown FIS is detected, whereas this bit is set when that FIS is posted to memory. Software should wait to act on an unknown FIS until this bit is set to ?1 or the two bits may become out of sync.
        uint32_t DPS :1; //Descriptor Processed: A PRD with the 'I' bit set has transferred all of its data. Refer to section 5.3.2.
        uint32_t PCS :1; //Port Connect Change Status: 1=Change in Current Connect Status. 0=No change in Current Connect Status. This bit reflects the state of PxSERR.DIAG.X. This bit is only cleared when PxSERR.DIAG.X is cleared.
        uint32_t DMPS :1; //Device Mechanical Presence Status (DMPS): When set, indicates that a mechanical presence switch attached to this port has been opened or closed, which may lead to a change in the connection state of the device.  This bit is only valid if both CAP.SMPS and P0CMD.MPSP are set to 1.
        uint32_t DW4_Reserved :14;
        uint32_t PRCS :1; //PhyRdy Change Status: When set to '1 indicates the internal PhyRdy signal changed state. This bit reflects the state of P0SERR.DIAG.N. To clear this bit, software must clear P0SERR.DIAG.N to ?0 .
        uint32_t IPMS :1; //Incorrect Port Multiplier Status: Indicates that the HBA received a FIS from a device whose Port Multiplier field did not match what was expected.  The IPMS bit may be set during enumeration of devices on a Port Multiplier due to the normal Port Multiplier enumeration process.  It is recommended that IPMS only be used after enumeration is complete on the Port Multiplier.
        uint32_t OFS :1; //Overflow Status: Indicates that the HBA received more bytes from a device than was specified in the PRD table for the command.
        uint32_t DW4_Reserved2 :1;
        uint32_t INFS :1; //Interface Non-fatal Error Status: Indicates that the HBA encountered an error on the Serial ATA interface but was able to continue operation. Refer to section 6.1.2.
        uint32_t IFS :1; //Interface Fatal Error Status: Indicates that the HBA encountered an error on the Serial ATA interface which caused the transfer to stop. Refer to section 6.1.2.
        uint32_t HBDS :1; //Host Bus Data Error Status: Indicates that the HBA encountered a data error (uncorrectable ECC / parity) when reading from or writing to system memory.
        uint32_t HBFS :1; //Host Bus Fatal Error Status: Indicates that the HBA encountered a host bus error that it cannot recover from, such as a bad software pointer. In PCI, such an indication would be a target or master abort.
        uint32_t TFES :1; //Task File Error Status: This bit is set whenever the status register is updated by the device and the error bit (bit 0) is set.
        uint32_t CPDS :1; //Cold Port Detect Status: When set, a device status has changed as detected by the cold presence detect logic. This bit can either be set due to a non-connected port receiving a device, or a connected port having its device removed. This bit is only valid if the port supports cold presence detect as indicated by PxCMD.CPD set to ?1 .
        //MSB
    };
 
    uint32_t AsUlong;
 
}  AHCI_INTERRUPT_STATUS, *PAHCI_INTERRUPT_STATUS;

// Status of the Task File Data as defined in AHCI 1.0, section 3.3.8
typedef union _AHCI_TASK_FILE_DATA_STATUS {
    struct {
        // LSB
        uint8_t ERR : 1;  // Indicates an error during the transfer.
        uint8_t CS1 : 2;  // Command specific.
        uint8_t DRQ : 1;  // Indicates a data transfer is requested.
        uint8_t CS2 : 3;  // Command specific.
        uint8_t BSY : 1;  // Indicates the interface is busy.
        // MSB
    };

    uint8_t AsUchar;  // Full byte access to the task file data status.

} AHCI_TASK_FILE_DATA_STATUS, *PAHCI_TASK_FILE_DATA_STATUS;


/* Scatter/gather descriptor entry. */

typedef struct {
    uint32_t    phys_addr;      /* Physical address. */
    uint32_t    size;           /* Entry size. */
} vds_sg;

typedef struct {
    uint32_t    region_size;    /* Region size in bytes. */
    uint32_t    offset;         /* Offset. */
    uint16_t    seg_sel;        /* Segment or selector. */
    uint16_t    resvd;          /* Reserved. */
    uint16_t    num_avail;      /* Number of entries available. */
    uint16_t    num_used;       /* Number of entries used. */
    union {
        vds_sg      sg[1];      /* S/G entry array. */
        uint32_t    pte[1];     /* Page table entry array. */
    } u;
} vds_edds;


typedef struct
{
    /** The AHCI command list as defined by chapter 4.2.2 of the Intel AHCI spec.
     *  Because the BIOS doesn't support NCQ only the first command header is defined
     *  to save memory. - Must be aligned on a 1K boundary.
     */
    uint32_t        aCmdHdr[0x8];
    /** Align the next structure on a 128 byte boundary. */
    uint8_t         abAlignment1[0x60];
    /** The command table of one request as defined by chapter 4.2.3 of the Intel AHCI spec.
     *  Must be aligned on 128 byte boundary.
     */
    uint8_t         abCmd[0x40];
    /** The ATAPI command region.
     *  Located 40h bytes after the beginning of the CFIS (Command FIS).
     */
    uint8_t         abAcmd[0x20];
    /** Align the PRDT structure on a 128 byte boundary. */
    uint8_t         abAlignment2[0x20];
    /** Physical Region Descriptor Table (PRDT) array. In other
     *  words, a scatter/gather descriptor list.
     */
    ahci_prdt       aPrdt[16];
    /** Memory for the received command FIS area as specified by chapter 4.2.1
     *  of the Intel AHCI spec. This area is normally 256 bytes big but to save memory
     *  only the first 96 bytes are used because it is assumed that the controller
     *  never writes to the UFIS or reserved area. - Must be aligned on a 256byte boundary.
     */
    uint8_t         abFisRecv[0x60];
    /** Base I/O port for the index/data register pair. */
    uint16_t        iobase;
    /** Current port which uses the memory to communicate with the controller. */
    uint8_t         cur_port;
    /** Current PRD index (for pre/post skip). */
    uint8_t         cur_prd;
    /** Physical address of the sink buffer (for pre/post skip). */
    uint32_t        sink_buf_phys;
    /** Saved high bits of EAX. */
    uint16_t        saved_eax_hi;
    /** VDS EDDS DMA buffer descriptor structure. */
    vds_edds        edds;
    vds_sg          edds_more_sg[NUM_EDDS_SG - 1];
} ahci_t;

/* Virtual DMA Services (VDS) */

#define VDS_FLAGS_OFS   0x7B    /* Offset of VDS flag byte in BDA. */
#define VDS_PRESENT     0x20    /* The VDS present bit. */

/* The DMA descriptor data structure. */

typedef struct {
    uint32_t    region_size;    /* Region size in bytes. */
    uint32_t    offset;         /* Offset. */
    uint16_t    seg_sel;        /* Segment selector. */
    uint16_t    buf_id;         /* Buffer ID. */
    uint32_t    phys_addr;      /* Physical address. */
} vds_dds;


/* The extended DDS for scatter/gather. Note that the EDDS contains either
 * S/G descriptors or x86-style PTEs.
 */

typedef struct {
    uint32_t NP:5;                   //0-4
    uint32_t SXS:1;                  //5
    uint32_t EMS:1;                  //6
    uint32_t CCCS:1;                 //7
    uint32_t NCS:5;                  //8-12
    uint32_t PSC:1;                  //13
    uint32_t SSC:1;                  //14
    uint32_t PMD:1;                  //15
    uint32_t FBSS:1;                 //16
    uint32_t SPM:1;                  //17
    uint32_t SAM:1;                  //18
    uint32_t RES1:1;                 //19
    uint32_t ISS:4;                  //20-23
    uint32_t SCLO:1;                 //24
    uint32_t SAL:1;                  //25
    uint32_t SALP:1;                 //26
    uint32_t SSS:1;                  //27
    uint32_t SMPS:1;                 //28
    uint32_t SSNTF:1;                //29
    uint32_t SNCQ:1;                 //30
    uint32_t S64A:1;                 //31 - Support 64-bit addressing
} ahcicaps_t;

/*Portions from: http://wiki.osdev.org/AHCI*/
typedef enum
{
	FIS_TYPE_REG_H2D	= 0x27,	// Register FIS - host to device
	FIS_TYPE_REG_D2H	= 0x34,	// Register FIS - device to host
	FIS_TYPE_DMA_ACT	= 0x39,	// DMA activate FIS - device to host
	FIS_TYPE_DMA_SETUP	= 0x41,	// DMA setup FIS - bidirectional
	FIS_TYPE_DATA		= 0x46,	// Data FIS - bidirectional
	FIS_TYPE_BIST		= 0x58,	// BIST activate FIS - bidirectional
	FIS_TYPE_PIO_SETUP	= 0x5F,	// PIO setup FIS - device to host
	FIS_TYPE_DEV_BITS	= 0xA1,	// Set device bits FIS - device to host
} FIS_TYPE;

//A host to device register FIS is used by the host to send command or control to a device
//it contains the IDE registers such as command, LBA, device, feature, count and control. 
//An ATA command is constructed in this structure and issued to the device. All reserved fields in an FIS should be cleared to zero. 
typedef struct tagFIS_REG_H2D
{
	// uint32_t 0
	uint8_t	fis_type;	// FIS_TYPE_REG_H2D
 
	uint8_t	pmport:4;	// Port multiplier
	uint8_t	rsv0:3;		// Reserved
	uint8_t	c:1;		// 1: Command, 0: Control
 
	uint8_t	command;	// Command register
	uint8_t	featurel;	// Feature register, 7:0
 
	// uint32_t 1
	uint8_t	lba0;		// LBA low register, 7:0
	uint8_t	lba1;		// LBA mid register, 15:8
	uint8_t	lba2;		// LBA high register, 23:16
	uint8_t	device;		// Device register
 
	// uint32_t 2
	uint8_t	lba3;		// LBA register, 31:24
	uint8_t	lba4;		// LBA register, 39:32
	uint8_t	lba5;		// LBA register, 47:40
	uint8_t	featureh;	// Feature register, 15:8
 
	// uint32_t 3
	uint8_t	countl;		// Count register, 7:0
	uint8_t	counth;		// Count register, 15:8
	uint8_t	icc;		// Isochronous command completion
	uint8_t	control;	// Control register
 
	// uint32_t 4
	uint8_t	rsv1[4];	// Reserved
} FIS_REG_H2D;

//A device to host register FIS is used by the device to notify the host that some ATA register has changed. 
//It contains the updated task files such as status, error and other registers. 
typedef struct tagFIS_REG_D2H
{
	// uint32_t 0
	uint8_t	fis_type;    // FIS_TYPE_REG_D2H
 
	uint8_t	pmport:4;    // Port multiplier
	uint8_t	rsv0:2;      // Reserved
	uint8_t	i:1;         // Interrupt bit
	uint8_t	rsv1:1;      // Reserved
 
	uint8_t	status;      // Status register
	uint8_t	error;       // Error register
 
	// uint32_t 1
	uint8_t	lba0;        // LBA low register, 7:0
	uint8_t	lba1;        // LBA mid register, 15:8
	uint8_t	lba2;        // LBA high register, 23:16
	uint8_t	device;      // Device register
 
	// uint32_t 2
	uint8_t	lba3;        // LBA register, 31:24
	uint8_t	lba4;        // LBA register, 39:32
	uint8_t	lba5;        // LBA register, 47:40
	uint8_t	rsv2;        // Reserved
 
	// uint32_t 3
	uint8_t	countl;      // Count register, 7:0
	uint8_t	counth;      // Count register, 15:8
	uint8_t	rsv3[2];     // Reserved
 
	// uint32_t 4
	uint8_t	rsv4[4];     // Reserved
} FIS_REG_D2H;

//Data FIS - Used by the host or device to send data payload. The data size can be varied. 
typedef struct tagFIS_DATA
{
	// uint32_t 0
	uint8_t	fis_type;	// FIS_TYPE_DATA
 
	uint8_t	pmport:4;	// Port multiplier
	uint8_t	rsv0:4;		// Reserved
 
	uint8_t	rsv1[2];	// Reserved
 
	// uint32_t 1 ~ N
	uint32_t data[1];	// Payload
} FIS_DATA;

//PIO Setup FIS - Used by the device to tell the host that it’s about to send or ready to receive a PIO data payload. 
typedef struct tagFIS_PIO_SETUP
{
	// uint32_t 0
	uint8_t	fis_type;	// FIS_TYPE_PIO_SETUP
 
	uint8_t	pmport:4;	// Port multiplier
	uint8_t	rsv0:1;		// Reserved
	uint8_t	d:1;		// Data transfer direction, 1 - device to host
	uint8_t	i:1;		// Interrupt bit
	uint8_t	rsv1:1;
 
	uint8_t	status;		// Status register
	uint8_t	error;		// Error register
 
	// uint32_t 1
	uint8_t	lba0;		// LBA low register, 7:0
	uint8_t	lba1;		// LBA mid register, 15:8
	uint8_t	lba2;		// LBA high register, 23:16
	uint8_t	device;		// Device register
 
	// uint32_t 2
	uint8_t	lba3;		// LBA register, 31:24
	uint8_t	lba4;		// LBA register, 39:32
	uint8_t	lba5;		// LBA register, 47:40
	uint8_t	rsv2;		// Reserved
 
	// uint32_t 3
	uint8_t	countl;		// Count register, 7:0
	uint8_t	counth;		// Count register, 15:8
	uint8_t	rsv3;		// Reserved
	uint8_t	e_status;	// New value of status register
 
	// uint32_t 4
	uint16_t	tc;		// Transfer count
	uint8_t	rsv4[2];	// Reserved
} FIS_PIO_SETUP;

//DMA Setup FIS - Device to Host
typedef struct tagFIS_DMA_SETUP
{
	// uint32_t 0
	uint8_t	fis_type;	// FIS_TYPE_DMA_SETUP
 
	uint8_t	pmport:4;	// Port multiplier
	uint8_t	rsv0:1;		// Reserved
	uint8_t	d:1;		// Data transfer direction, 1 - device to host
	uint8_t	i:1;		// Interrupt bit
	uint8_t	a:1;            // Auto-activate. Specifies if DMA Activate FIS is needed
 
        uint8_t    rsved[2];       // Reserved
 
	//uint32_t 1&2
 
        uint64_t   DMAbufferID;    // DMA Buffer Identifier. Used to Identify DMA buffer in host memory. SATA Spec says host specific and not in Spec. Trying AHCI spec might work.
 
        //uint32_t 3
        uint32_t   rsvd;           //More reserved
 
        //uint32_t 4
        uint32_t   DMAbufOffset;   //Byte offset into buffer. First 2 bits must be 0
 
        //uint32_t 5
        uint32_t   TransferCount;  //Number of bytes to transfer. Bit 0 must be 0
 
        //uint32_t 6
        uint32_t   resvd;          //Reserved
 
} FIS_DMA_SETUP;

typedef volatile struct tagHBA_PORT
{
	uint32_t	clb;		// 0x00, command list base address, 1K-byte aligned
	uint32_t	clbu;		// 0x04, command list base address upper 32 bits
	uint32_t	fb;		// 0x08, FIS base address, 256-byte aligned
	uint32_t	fbu;		// 0x0C, FIS base address upper 32 bits
	//uint32_t	pxis;		// 0x10, interrupt status
        AHCI_INTERRUPT_STATUS pxis;
	//uint32_t	ie;		// 0x14, interrupt enable
        AHCI_INTERRUPT_ENABLE ie;
	//uint32_t	cmd;		// 0x18, command and status
        AHCI_COMMAND    cmd;
	uint32_t	rsv0;		// 0x1C, Reserved
	//uint32_t	tfd;		// 0x20, task file data
        AHCI_TASK_FILE_DATA_STATUS tfd;
	uint32_t	sig;		// 0x24, signature
	uint32_t	ssts;		// 0x28, SATA status (SCR0:SStatus)
        AHCI_SERIAL_ATA_CONTROL sctl;   // 0x2C, SATA control (SCR0:SControl))
	//uint32_t	serr;		// 0x30, SATA error (SCR1:SError)
        AHCI_SERIAL_ATA_ERROR_ERROR serr;
		//CLR 11/24/2024 - removed commented AHCI_SERIAL_ATA_ERROR_DIAGNOSTICS
	uint32_t	sact;		// 0x34, SATA active (SCR3:SActive)
	uint32_t	ci;		// 0x38, command issue
	uint32_t	sntf;		// 0x3C, SATA notification (SCR4:SNotification)
	uint32_t	fbs;		// 0x40, FIS-based switch control
	uint32_t	rsv1[11];	// 0x44 ~ 0x6F, Reserved
	uint32_t	vendor[4];	// 0x70 ~ 0x7F, vendor specific
} hba_port_t;

typedef volatile struct tagHBA_MEM
{
	// 0x00 - 0x2B, Generic Host Control
	//uint32_t	cap;		// 0x00, Host capability
        AHCI_HBA_CAPABILITIES cap;
	//uint32_t	ghc;		// 0x04, Global host control
        AHCI_Global_HBA_CONTROL ghc;
	uint32_t	is;		// 0x08, Interrupt status
	uint32_t	pi;		// 0x0C, Port implemented
	uint32_t	vs;		// 0x10, Version
	uint32_t	ccc_ctl;	// 0x14, Command completion coalescing control
	uint32_t	ccc_pts;	// 0x18, Command completion coalescing ports
	uint32_t	em_loc;		// 0x1C, Enclosure management location
	uint32_t	em_ctl;		// 0x20, Enclosure management control
	uint32_t	cap2;		// 0x24, Host capabilities extended
	uint32_t	bohc;		// 0x28, BIOS/OS handoff control and status
 
	// 0x2C - 0x9F, Reserved
	uint8_t	rsv[0xA0-0x2C];
 
	// 0xA0 - 0xFF, Vendor specific registers
	uint8_t	vendor[0x100-0xA0];
 
	// 0x100 - 0x10FF, Port control registers
	volatile hba_port_t	ports[32];	// 1 ~ 32
} HBA_MEM;
 
typedef struct tagHBA_CMD_HEADER
{
	// DW0
	uint8_t	cfl:5;		// Command FIS length in DWORDS, 2 ~ 16
	uint8_t	a:1;		// ATAPI
	uint8_t	w:1;		// Write, 1: H2D, 0: D2H
	uint8_t	p:1;		// Prefetchable
 
	uint8_t	r:1;		// Reset
	uint8_t	b:1;		// BIST
	uint8_t	c:1;		// Clear busy upon R_OK
	uint8_t	rsv0:1;		// Reserved
	uint8_t	pmp:4;		// Port multiplier port
 
	uint16_t	prdtl;		// Physical region descriptor table length in entries
 
	// DW1
	volatile
	uint32_t	prdbc;		// Physical region descriptor byte count transferred
 
	// DW2, 3
	union {
        struct {
            uint32_t ctba;   // Lower 32 bits
            uint32_t ctbau;  // Upper 32 bits
        };
        uintptr_t* ctba_64;    // Combined 64-bit value
    };
 
	// DW4 - 7
	uint32_t	rsv1[4];	// Reserved
} HBA_CMD_HEADER;

typedef struct tagHBA_PRDT_ENTRY
{
     union {
		struct {
			uint32_t    dba;        // Data base address
			uint32_t    dbau;       // Data base address upper 32 bits
		};
		uintptr_t* dba_64;
	 };
     uint32_t    rsv0;       // Reserved
 
     // DW3
     uint32_t    dbc:22;     // Byte count, 4M max
     uint32_t    rsv1:9;     // Reserved
     uint32_t    i:1;        // Interrupt on completion
} HBA_PRDT_ENTRY;

typedef struct tagHBA_CMD_TBL
{
	// 0x00
	uint8_t	cfis[64];	// Command FIS
 
	// 0x40
	uint8_t	acmd[16];	// *ATAPI command*, 12 or 16 bytes
 
	// 0x50
	uint8_t	rsv[48];	// Reserved
 
	// 0x80
	HBA_PRDT_ENTRY	prdt_entry[1];	// Physical region descriptor table entries, 0 ~ 65535
} HBA_CMD_TBL;

typedef volatile struct tagHBA_FIS
{
	// 0x00
	FIS_DMA_SETUP	dsfis;		// DMA Setup FIS
	uint8_t		pad0[4];
 
	// 0x20
	FIS_PIO_SETUP	psfis;		// PIO Setup FIS
	uint8_t		pad1[12];
 
	// 0x40
            FIS_REG_D2H	rfis;		// Register – Device to Host FIS
	uint8_t		pad2[4];
 
	// 0x58
	uint16_t         sdbfis;		// Set Device Bit FIS
 
	// 0x60
	uint8_t		ufis[64];
 
	// 0xA0
	uint8_t		rsv[0x100-0xA0];
} HBA_FIS;

int ahci_check_type(const hba_port_t *port, uint32_t* sig);
void ahci_probe_ports(HBA_MEM *abar);
void printAHCICaps();
bool init_AHCI();
void ahci_port_rebase(hba_port_t *port, int portno, uintptr_t remapBase);
void start_cmd(hba_port_t *port);
void ahci_stop_cmd(volatile hba_port_t *port);
void ahciIdentify(hba_port_t* port, int deviceType);
int ata_find_cmdslot(const hba_port_t *port);
void waitForPortIdle(hba_port_t *port);
void ahciSetCurrentDisk(hba_port_t* port);
int ahciRead(hba_port_t* port, int sector, uint8_t* buffer, int sector_count);
int ahciBlockingRead28(uint32_t sector, uint8_t *buffer, uint32_t sector_count);
int ahciBlockingWrite28(uint32_t sector, uint8_t *buffer, uint32_t sector_count);
void ahci_port_activate_device(HBA_MEM* h, hba_port_t* p);
void ahci_enable_port(HBA_MEM* ad, int pt);

int ahci_lba_read(block_device_info_t* device, uint64_t sector, void* buffer, uint64_t sector_count);

extern volatile HBA_MEM* kABARs;
#endif	/* AHCI_H */
