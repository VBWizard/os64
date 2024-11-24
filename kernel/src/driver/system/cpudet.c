/*
 * Copyright (c) 2006-2007 -  http://brynet.biz.tm - <brynet@gmail.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL
 * THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/* You need to include a file with  fairly(ish) compliant printf prototype, Decimal and String support like %s and %d and this is truely all you need! */
#include <cpuid.h>
#include "driver/system/cpudet.h"
#include "sprintf.h"
#include "CONFIG.h"
#include "serial_logging.h"

/* Required Declarations */
int do_intel(void);
int do_amd(void);
void printregs(int eax, int ebx, int ecx, int edx);
cpuinfo_t kcpuInfo;
cpuid_features_t kCPUFeatures;

void identifyCPUFeatures(cpuid_features_t* cpuFeatures)
{
    uint32_t eax, ebx, ecx, edx;

    __cpuid(1, eax, ebx, cpuFeatures->cpuid_feature_bits_2.cpuid_feature_bits_ecx_reg, cpuFeatures->cpuid_feature_bits.cpuid_features_edx_reg);
    __cpuid(7, eax, cpuFeatures->cpuid_extended_feature_bits_3.cpuid_extended_feature_bits_ebx_reg , ecx, edx);
}

/* Simply call this function detect_cpu(); */
int detect_cpu(void) { /* or main() if your trying to port this as an independant application */
	unsigned int ebx, unused;
        __get_cpuid(0, &unused, &ebx, &unused, &unused);
	switch(ebx) {
		case 0x756e6547: /* Intel Magic Code */
		do_intel();
		break;
		case 0x68747541: /* AMD Magic Code */
		do_amd();
		break;
		default:
		printd(DEBUG_BOOT, "Unknown x86 CPU Detected\n");
		__asm__("pushf\n pop rax\n and rax, %0\n push rax\n popf\n":: "i"(0x00000000FFDFFFFF));
		break;
	}
	identifyCPUFeatures(&kCPUFeatures);
	return 0;
}

/* Intel Specific brand list */
char *Intel[] = {
	"Brand ID Not Supported.", 
	"Intel(R) Celeron(R) processor", 
	"Intel(R) Pentium(R) III processor", 
	"Intel(R) Pentium(R) III Xeon(R) processor", 
	"Intel(R) Pentium(R) III processor", 
	"Reserved", 
	"Mobile Intel(R) Pentium(R) III processor-M", 
	"Mobile Intel(R) Celeron(R) processor", 
	"Intel(R) Pentium(R) 4 processor", 
	"Intel(R) Pentium(R) 4 processor", 
	"Intel(R) Celeron(R) processor", 
	"Intel(R) Xeon(R) Processor", 
	"Intel(R) Xeon(R) processor MP", 
	"Reserved", 
	"Mobile Intel(R) Pentium(R) 4 processor-M", 
	"Mobile Intel(R) Pentium(R) Celeron(R) processor", 
	"Reserved", 
	"Mobile Genuine Intel(R) processor", 
	"Intel(R) Celeron(R) M processor", 
	"Mobile Intel(R) Celeron(R) processor", 
	"Intel(R) Celeron(R) processor", 
	"Mobile Geniune Intel(R) processor", 
	"Intel(R) Pentium(R) M processor", 
	"Mobile Intel(R) Celeron(R) processor"
};

/* This table is for those brand strings that have two values depending on the processor signature. It should have the same number of entries as the above table. */
char *Intel_Other[] = {
	"Reserved", 
	"Reserved", 
	"Reserved", 
	"Intel(R) Celeron(R) processor", 
	"Reserved", 
	"Reserved", 
	"Reserved", 
	"Reserved", 
	"Reserved", 
	"Reserved", 
	"Reserved", 
	"Intel(R) Xeon(R) processor MP", 
	"Reserved", 
	"Reserved", 
	"Intel(R) Xeon(R) processor", 
	"Reserved", 
	"Reserved", 
	"Reserved", 
	"Reserved", 
	"Reserved", 
	"Reserved", 
	"Reserved", 
	"Reserved", 
	"Reserved"
};

/* Intel-specific information */
int do_intel(void) {
	unsigned int eax=0, ebx=0, ecx=0, edx=0, max_eax=0, signature, unused;
	sprintf(kcpuInfo.vendor,"Intel");
        __get_cpuid(1, &eax, &ebx, &unused, &unused);
	kcpuInfo.model = (eax >> 4) & 0xf;
	kcpuInfo.family = (eax >> 8) & 0xf;
	kcpuInfo.type = (eax >> 12) & 0x3;
	kcpuInfo.brand = ebx & 0xff;
	kcpuInfo.stepping = eax & 0xf;
	signature = eax;
	switch(kcpuInfo.type) {
		case 0:
		sprintf(kcpuInfo.type_name,"Original OEM");
		break;
		case 1:
		sprintf(kcpuInfo.type_name,"Overdrive");
		break;
		case 2:
		sprintf(kcpuInfo.type_name,"Dual-capable");
		break;
		case 3:
		sprintf(kcpuInfo.type_name,"Reserved");
		break;
		default:
		sprintf(kcpuInfo.type_name,"Unidentified");
		break;
	}
	switch(kcpuInfo.family) {
		case 3:
		sprintf(kcpuInfo.family_name,"i386");
		break;
		case 4:
		sprintf(kcpuInfo.family_name,"i486");
		break;
		case 5:
		sprintf(kcpuInfo.family_name,"Pentium");
		break;
		case 6:
		sprintf(kcpuInfo.family_name,"Pentium Pro");
		break;
		case 15:
		sprintf(kcpuInfo.family_name,"Pentium 4");
		break;
		default:
		sprintf(kcpuInfo.family_name, "Unidentified");
		break;
	}
	if(kcpuInfo.family == 15) {
		kcpuInfo.extended_family = (eax >> 20) & 0xff;
	}
	switch(kcpuInfo.family) {
		case 3:
		break;
		case 4:
		switch(kcpuInfo.model) {
			case 0:
			case 1:
			sprintf(kcpuInfo.model_name,"DX");
			break;
			case 2:
			sprintf(kcpuInfo.model_name,"SX");
			break;
			case 3:
			sprintf(kcpuInfo.model_name,"487/DX2");
			break;
			case 4:
			sprintf(kcpuInfo.model_name,"SL");
			break;
			case 5:
			sprintf(kcpuInfo.model_name,"SX2");
			break;
			case 7:
			sprintf(kcpuInfo.model_name,"Write-back enhanced DX2");
			break;
			case 8:
			sprintf(kcpuInfo.model_name,"DX4");
			break;
		}
		break;
		case 5:
		switch(kcpuInfo.model) {
			case 1:
			sprintf(kcpuInfo.model_name,"60/66");
			break;
			case 2:
			sprintf(kcpuInfo.model_name,"75-200");
			break;
			case 3:
			sprintf(kcpuInfo.model_name,"for 486 system");
			break;
			case 4:
			sprintf(kcpuInfo.model_name,"MMX");
			break;
		}
		break;
		case 6:
		switch(kcpuInfo.model) {
			case 1:
			sprintf(kcpuInfo.model_name,"Pentium Pro");
			break;
			case 3:
			sprintf(kcpuInfo.model_name,"Pentium II Model 3");
			break;
			case 5:
			sprintf(kcpuInfo.model_name,"Pentium II Model 5/Xeon/Celeron");
			break;
			case 6:
			sprintf(kcpuInfo.model_name,"Celeron");
			break;
			case 7:
			sprintf(kcpuInfo.model_name,"Pentium III/Pentium III Xeon - external L2 cache");
			break;
			case 8:
			sprintf(kcpuInfo.model_name,"Pentium III/Pentium III Xeon - internal L2 cache");
			break;
		}
		break;
		case 15:
		break;
		default:
		break;
	}
	__get_cpuid(0x80000000, &max_eax, &unused, &unused, &unused);
	/* Quok said: If the max extended eax value is high enough to support the processor brand string
	(values 0x80000002 to 0x80000004), then we'll use that information to return the brand information. 
	Otherwise, we'll refer back to the brand tables above for backwards compatibility with older processors. 
	According to the Sept. 2006 Intel Arch Software Developer's Guide, if extended eax values are supported, 
	then all 3 values for the processor brand string are supported, but we'll test just to make sure and be safe. */
	if(max_eax >= 0x80000004) {
		if(max_eax >= 0x80000002) {
			__get_cpuid(0x80000002, &eax, &ebx, &ecx, &edx);
			printregs(eax, ebx, ecx, edx);
		}
		if(max_eax >= 0x80000003) {
			__get_cpuid(0x80000003, &eax, &ebx, &ecx, &edx);
			printregs(eax, ebx, ecx, edx);
		}
		if(max_eax >= 0x80000004) {
			__get_cpuid(0x80000004, &eax, &ebx, &ecx, &edx);
			printregs(eax, ebx, ecx, edx);
		}
	} else if(kcpuInfo.brand > 0) {
		if(kcpuInfo.brand < 0x18) {
			if(signature == 0x000006B1 || signature == 0x00000F13) {
				sprintf(kcpuInfo.brand_name,"%s\n", Intel_Other[kcpuInfo.brand]);
			} else {
				sprintf(kcpuInfo.brand_name, "%s\n", Intel[kcpuInfo.brand]);
			}
		} else {
			sprintf(kcpuInfo.brand_name, "Reserved\n");
		}
	}
	return 0;
}

/* Print Registers */
void printregs(int eax, int ebx, int ecx, int edx) {
	int j;
	char string[17];
	string[16] = '\0';
	for(j = 0; j < 4; j++) {
		string[j] = eax >> (8 * j);
		string[j + 4] = ebx >> (8 * j);
		string[j + 8] = ecx >> (8 * j);
		string[j + 12] = edx >> (8 * j);
	}
	sprintf(kcpuInfo.brand_name, "%s%s", kcpuInfo.brand_name, string);
}

/* AMD-specific information */
int do_amd(void) {
	//printf("AMD Specific Features:\n");
	unsigned int extended=0, eax=0, ebx=0, ecx=0, edx=0, unused=0;
	__get_cpuid(1, &eax, &unused, &unused, &unused);
	kcpuInfo.model = (eax >> 4) & 0xf;
	kcpuInfo.family = (eax >> 8) & 0xf;
	kcpuInfo.stepping = eax & 0xf;
	//reserved = eax >> 12;
	sprintf(kcpuInfo.vendor, "AMD");
	switch(kcpuInfo.family) 
        {
		case 4:
		sprintf(kcpuInfo.model_name, "486 Model %d", kcpuInfo.model);
		break;
		case 5:
		switch(kcpuInfo.model) {
			case 0:
			case 1:
			case 2:
			case 3:
			case 6:
			case 7:
			
                            sprintf(kcpuInfo.model_name, "K6 Model %d", kcpuInfo.model);
			break;
			case 8:
			sprintf(kcpuInfo.model_name, "K6-2 Model 8");
			break;
			case 9:
			sprintf(kcpuInfo.model_name, "K6-III Model 9");
			break;
			default:
			sprintf(kcpuInfo.model_name, "K5/K6 Model %d", kcpuInfo.model);
			break;
		}
		break;
		case 6:
		switch(kcpuInfo.model) {
			case 1:
			case 2:
			case 4:
			sprintf(kcpuInfo.model_name, "Athlon Model %d", kcpuInfo.model);
			break;
			case 3:
			sprintf(kcpuInfo.model_name, "Duron Model 3");
			break;
			case 6:
			sprintf(kcpuInfo.model_name, "Athlon MP/Mobile Athlon Model 6");
			break;
			case 7:
			sprintf(kcpuInfo.model_name, "Mobile Duron Model 7");
			break;
			default:
			sprintf(kcpuInfo.model_name, "Duron/Athlon Model %d", kcpuInfo.model);
			break;
		}
		break;
                case 15:
                    switch(kcpuInfo.model)
                    {
                        case 1:
                        case 2:
                            sprintf(kcpuInfo.model_name, "FX Series/Opertron (Piledriver)");
                            break;
                        case 10:
                        case 13:
                            sprintf(kcpuInfo.model_name, "A/R-Series/Athlon/Semperon/Firepro (Piledriver)");
                            break;
                        case 30:
                            sprintf(kcpuInfo.model_name, "Elite A-Series/R-Series/Opertron (SteamRoller)");
                            break;
                    }
                    break;
	}
	__get_cpuid(0x80000000, &extended, &unused, &unused, &unused);
	if(extended == 0) {
		return 0;
	}
	if(extended >= 0x80000002) {
		unsigned int j;
		for(j = 0x80000002; j <= 0x80000004; j++) {
			__get_cpuid(j, &eax, &ebx, &ecx, &edx);
			printregs(eax, ebx, ecx, edx);
		}
	}
	if(extended >= 0x80000007) {
		__get_cpuid(0x80000007, &unused, &unused, &unused, &edx);
		if(edx & 1) {
			printd(DEBUG_BOOT, "Temperature Sensing Diode Detected!\n");
		}
	}
	return 0;
}

unsigned long long rdtsc64(void)
{
    unsigned hi, lo;
    __asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
    return ( (unsigned long long)lo)|( ((unsigned long long)hi)<<32 );
}
