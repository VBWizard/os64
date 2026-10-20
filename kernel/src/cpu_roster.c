// cpu_roster.c — number the CPUs (cpu_roster.h has the rules and the why).

#include "cpu_roster.h"

bool cpu_roster_build(const uint32_t *lapic_ids, uint32_t listed,
                      uint32_t bsp_lapic_id, uint32_t cap, uint32_t max_cpus,
                      uint32_t *slot_of_cpu,
                      uint8_t apic_to_cpu[CPU_ROSTER_APIC_IDS],
                      cpu_roster_report_t *report)
{
	if (max_cpus == 0 || max_cpus >= CPU_ROSTER_NO_CPU)
		return false;
	if (bsp_lapic_id > CPU_ROSTER_APIC_MAX)
		return false;

	cpu_roster_report_t r = { .listed = listed };

	// First pass: which entries are usable. An ID the xAPIC cannot address is
	// refused; a repeated ID keeps its first entry, because two CPUs cannot
	// answer to one address and the bootloader woke only one of them.
	// slot_by_apic doubles as the "seen" set.
	uint32_t slot_by_apic[CPU_ROSTER_APIC_IDS];
	bool     seen[CPU_ROSTER_APIC_IDS] = { false };
	for (uint32_t i = 0; i < listed; i++)
	{
		uint32_t id = lapic_ids[i];
		if (id > CPU_ROSTER_APIC_MAX)
		{
			r.too_wide++;
			continue;
		}
		if (seen[id])
		{
			r.duplicates++;
			continue;
		}
		seen[id] = true;
		slot_by_apic[id] = i;
	}
	if (!seen[bsp_lapic_id])
		return false;

	uint32_t limit = max_cpus;
	if (cap != 0 && cap < limit)
		limit = cap;

	// The BSP is CPU 0; everyone else in ascending ID order, which walking
	// the seen set from 0 upward IS — no sort needed.
	uint32_t count = 0;
	slot_of_cpu[count++] = slot_by_apic[bsp_lapic_id];
	for (uint32_t id = 0; id < CPU_ROSTER_APIC_IDS; id++)
	{
		if (!seen[id] || id == bsp_lapic_id)
			continue;
		if (count < limit)
			slot_of_cpu[count++] = slot_by_apic[id];
		else
			r.parked++;
	}
	r.count = count;
	r.parked_by_max = r.parked > 0 && (cap == 0 || cap > max_cpus);

	for (uint32_t id = 0; id < CPU_ROSTER_APIC_IDS; id++)
		apic_to_cpu[id] = CPU_ROSTER_NO_CPU;
	r.apic_lowest = bsp_lapic_id;
	r.apic_highest = bsp_lapic_id;
	for (uint32_t cpu = 0; cpu < count; cpu++)
	{
		uint32_t id = lapic_ids[slot_of_cpu[cpu]];
		apic_to_cpu[id] = (uint8_t)cpu;
		if (id < r.apic_lowest)
			r.apic_lowest = id;
		if (id > r.apic_highest)
			r.apic_highest = id;
	}
	r.bsp_not_lowest = r.apic_lowest < bsp_lapic_id;
	// Numbered IDs are distinct, so lowest 0 and highest count-1 means every
	// ID in between is taken — and with the BSP at APIC 0 the ascending order
	// makes CPU n APIC n throughout.
	r.dense = bsp_lapic_id == 0 && r.apic_lowest == 0 && r.apic_highest == count - 1;

	*report = r;
	return true;
}
