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

void log_store_entry(uint16_t core, uint64_t tick_count, uint8_t priority, uint8_t category, bool continued, const char *message) 
{
	core_local_storage_t *cls = get_core_local_storage();

	if (!kLoggingInitialized)
		return;
	if (core >= MAX_CPUS) panic("Invalid core ID in log_store_entry: %u", core);
    
	log_buffer_t *buf = &core_log_buffers[core];
    
    log_entry_t *entry = &buf->entries[buf->head];
    entry->timestamp = kTicksSinceStart;
    entry->tick_count = tick_count;
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
    buf->head = (buf->head + 1) % buf->capacity;
    
	if ((buf->head + 1) % buf->capacity == buf->tail) 
	{
		//TODO: Fix this issue
		//FOR NOW JUST PANIC.  Scheduler calls printd() which causes the lock this code is in to block *in the scheduler*
		/*		scheduler_wake_isleep_task(kLogDTask);  // Wake logd immediately
	
		// Wait while the buffer is still full, sleeping briefly to avoid wasting CPU cycles
		while ((buf->head + 1) % buf->capacity == buf->tail) {
			sigaction(SIGSLEEP, NULL, kTicksSinceStart + LOGD_FLUSH_WAIT_TICKS, NULL);
		}
		*/	
		panic("LOGD: Log buffer full, can't continue");
	}
	
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

void logd_thread() {
    log_buffer_t *buffer;
    thread_t *self = get_core_local_storage()->currentThread;

    while (1) {
        int processed_logs = 0;

        if (kLoggingInitialized) {
            for (int core = 0; core < kMPCoreCount; core++) {
                buffer = &core_log_buffers[core];

                /* Skip cores whose buffers are not yet allocated */
                if (!buffer->entries)
                    continue;

                /* Process up to MAX_BATCH_SIZE entries for this core */
                while (buffer->head != buffer->tail &&
                       processed_logs < MAX_BATCH_SIZE) {
                    log_entry_t *entry = &buffer->entries[buffer->tail];
                    char print_buf2[300];

                    if (entry->continued)
                    {
                        // Just continue printing the message without prefixing formatting
                        snprintf(print_buf2,
                                 MAX_LOG_MESSAGE_SIZE,
                                 "%s",
                                 entry->message);
                    }
                    else
                        snprintf(print_buf2,
                                 MAX_LOG_MESSAGE_SIZE,
                                 "%u (0x%04x) AP%u: %s",
                                 entry->timestamp,
                                 entry->threadID,
                                 entry->core_id,
                                 entry->message);
                    serial_print_string(print_buf2);

                    memset(entry->message, 0, MAX_LOG_MESSAGE_SIZE);
                    buffer->tail = (buffer->tail + 1) % buffer->capacity;
                    processed_logs++;
                }
            }
        }

        sigaction(SIGSLEEP, NULL, kTicksSinceStart + LOGD_SLEEP_TICKS, self);
    }
}


