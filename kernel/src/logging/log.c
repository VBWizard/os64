#include "log.h"
#include "serial_logging.h"
#include "io.h"
#include "panic.h"
#include "kernel.h"
#include "smp.h"
#include "kmalloc.h"
#include "CONFIG.h"
#include "sprintf.h"
#include "memset.h"
#include "smp_core.h"
#include "task.h"
#include "scheduler.h"  //for scheduler_wake_task
#include "signals.h"
// TODO: Implement dump_log_buffer() to handle emergency log flushes when buffer is full in place of panicking

extern task_t* kLogDTask;
extern volatile uint64_t kTicksSinceStart;
log_buffer_t core_log_buffers[MAX_CPUS];
bool kLoggingInitialized = false;
extern struct limine_smp_response *kLimineSMPInfo;
// Ensures only one logd worker processes the buffers at a time
_Atomic uint32_t kLogDWorkLock = 0;

void log_store_entry(uint16_t core, uint64_t ticks, uint8_t priority, uint8_t category, bool continued, const char *message)
{
	core_local_storage_t *cls = get_core_local_storage();

	if (!kLoggingInitialized)
		return;
	if (core >= MAX_CPUS) panic("Invalid core ID in log_store_entry: %u", core);

	log_buffer_t *buffer = &core_log_buffers[core];

    log_entry_t *entry = &buffer->entries[buffer->head];
    entry->ticks = ticks;
    // TSC is valid for cross-core comparison on QEMU (single host TSC source).
    // On real hardware, requires invariant TSC (CPUID 0x80000007 EDX bit 8).
    __asm__ volatile("rdtsc" : "=a"(*(uint32_t*)&entry->tsc), "=d"(*((uint32_t*)&entry->tsc + 1)));
    entry->core_id = core;
    entry->log_level = priority;
    entry->category = category;
    entry->continued = continued;
	if (kSMPInitDone)
		entry->threadID = cls->currentThread->threadID;
	else
		entry->threadID = 0;
    snprintf(entry->message, MAX_LOG_MESSAGE_SIZE, "%s", message);
    entry->message[MAX_LOG_MESSAGE_SIZE-1] = '\0';
    buffer->head = (buffer->head + 1) % buffer->capacity;

    //If the log buffer is *full* then attempt to flush the buffer directly. If that fails, put the current thread to sleep
    //so that logd has a chance to wake up and flush the buffer.
    //NOTE: Putting the thread to sleep is *a bad idea* because the scheduler calls printd() a bunch of times, and putting the scheduler
    //to sleep to start another thread? That just makes no sense.
    while ((buffer->head + 1) % buffer->capacity == buffer->tail)
        //Attempt to execute logd flushing method
        if (!logd_thread(false))
            //If that fails, throw a panic for now until we figure out a better approach
            panic("log_store_entry: logd buffer for core %u is full", core);
}

void logging_queueing_init() {
    for (int i = 0; i <  (int)kLimineSMPInfo->cpu_count; i++) {
        // Allocate memory for each core's log buffer
        core_log_buffers[i].entries = (log_entry_t *)kmalloc(LOG_BUFFER_SIZE); 
        core_log_buffers[i].capacity = LOG_BUFFER_SIZE / sizeof(log_entry_t);
        core_log_buffers[i].head = 0;  // Initialize head pointer
        core_log_buffers[i].tail = 0;  // Initialize tail pointer
        core_log_buffers[i].lock = 0;  // Initialize lock
    }
	kLoggingInitialized = true;
}

// Print one entry from a buffer and advance its tail.
// Always drains immediately-following continued entries from the same buffer
// so multi-chunk messages are never split by the interleave logic.
static void logd_drain_one(log_buffer_t *buffer)
{
    // Worst-case formatted output: 20 (%lu ticks) + 22 ((0x%016lx threadID))
    // + 9 (AP65535:) + 1 (space) + 255 (message) + NUL = 308 bytes.
    // Use 512 to stay well clear even if metadata is corrupted.
    char print_buf2[MAX_LOG_MESSAGE_SIZE + 256];
    log_entry_t *entry = &buffer->entries[buffer->tail];

    if (!entry->continued)
        snprintf(print_buf2, sizeof(print_buf2),
                 "%lu (0x%04lx) AP%u: %s",
                 entry->ticks,
                 entry->threadID,
                 entry->core_id,
                 entry->message);
    else
        snprintf(print_buf2, sizeof(print_buf2), "%s", entry->message);

    serial_print_string(print_buf2);
    entry->message[0] = '\0';
    buffer->tail = (buffer->tail + 1) % buffer->capacity;

    // Drain any continuation chunks that belong to this entry before yielding
    while (buffer->head != buffer->tail &&
           buffer->entries[buffer->tail].continued)
    {
        log_entry_t *cont = &buffer->entries[buffer->tail];
        snprintf(print_buf2, sizeof(print_buf2), "%s", cont->message);
        serial_print_string(print_buf2);
        cont->message[0] = '\0';
        buffer->tail = (buffer->tail + 1) % buffer->capacity;
    }
}

bool logd_thread(bool daemon) {
    thread_t *self = get_core_local_storage()->currentThread;
    bool nonDaemonRunSuccess = false;
    static uint32_t drain_pass = 0;

    while (1) {
        int processed_logs = 0;

        // Try-lock: if another CPU is already flushing, skip this wakeup
        if (!__sync_lock_test_and_set(&kLogDWorkLock, 1))
        {
            if (kLoggingInitialized)
            {
                // k-way merge: on each step pick the core whose oldest entry
                // has the lowest TSC, print it, repeat until all buffers empty.
                // Only logd updates tail pointers so no per-buffer lock needed.
                bool any;
                do {
                    any = false;
                    int   best_core = -1;
                    uint64_t best_tsc  = UINT64_MAX;

                    for (int c = 0; c < kMPCoreCount; c++)
                    {
                        log_buffer_t *buf = &core_log_buffers[c];
                        if (!buf->entries || buf->head == buf->tail)
                            continue;
                        uint64_t t = buf->entries[buf->tail].tsc;
                        if (t < best_tsc) {
                            best_tsc  = t;
                            best_core = c;
                            any = true;
                        }
                    }

                    if (best_core >= 0) {
                        logd_drain_one(&core_log_buffers[best_core]);
                        processed_logs++;
                    }
                } while (any);
            }
            nonDaemonRunSuccess = processed_logs > 0;
            drain_pass++;

            // Print queue depths directly to serial after every pass, so we
            // can monitor buffer pressure without adding to the ring buffers
            // themselves (which would skew the numbers). One line per drain
            // (~2/sec) is cheap, and per-pass visibility is what cracked the
            // 2026-07 slow-walk investigation. AP is the core the pass ran
            // on — kworker drains from AP1 every 2s through this same code,
            // so without it the stats can't tell the two drainers apart.
            {
                char stats[128];
                int pos = snprintf(stats, sizeof(stats),
                    "[logd] AP%u tick=%lu pass=%u drained=%d",
                    get_core_local_storage()->apic_id, kTicksSinceStart, drain_pass, processed_logs);
                for (int c = 0; c < kMPCoreCount && pos < (int)sizeof(stats) - 32; c++) {
                    log_buffer_t *b = &core_log_buffers[c];
                    size_t used = (b->head >= b->tail)
                        ? b->head - b->tail
                        : b->capacity - b->tail + b->head;
                    pos += snprintf(stats + pos, sizeof(stats) - pos,
                        " AP%d=%lu/%lu", c, (unsigned long)used, (unsigned long)b->capacity);
                }
                snprintf(stats + pos, sizeof(stats) - pos, "\n");
                serial_print_string(stats);
            }

            __sync_lock_release(&kLogDWorkLock);
        }
        if (!daemon)
            return nonDaemonRunSuccess;
        // Sleep only when all queues were empty; wake up periodically regardless
        sigaction(SIGSLEEP, NULL, kTicksSinceStart + LOGD_SLEEP_TICKS, self);
    }
}

