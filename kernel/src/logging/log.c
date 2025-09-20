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

void log_store_entry(uint16_t core, uint64_t tick_count, uint8_t priority, uint8_t category, bool continued, const char *message) 
{
	core_local_storage_t *cls = get_core_local_storage();

	if (!kLoggingInitialized)
		return;
	if (core >= MAX_CPUS) panic("Invalid core ID in log_store_entry: %u", core);
    
	log_buffer_t *buffer = &core_log_buffers[core];
    
    log_entry_t *entry = &buffer->entries[buffer->head];
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

bool logd_thread(bool daemon) {
    log_buffer_t *buffer;
    thread_t *self = get_core_local_storage()->currentThread;
    bool nonDaemonRunSuccess = false;

    while (1) {
        int processed_logs = 0;

        // Try-lock: if another CPU is already flushing, skip work this tick
        if (!__sync_lock_test_and_set(&kLogDWorkLock, 1))
        {
            nonDaemonRunSuccess = true;
            if (kLoggingInitialized)
            {
                for (int core = 0; core < kMPCoreCount; core++)
                {
                    __asm__("pause\n");
                    buffer = &core_log_buffers[core];

                    /* Skip cores whose buffers are not yet allocated */
                    if (!buffer->entries)
                        continue;

                    /* Process up to MAX_BATCH_SIZE entries for this core */
                    while (buffer->head != buffer->tail &&
                           processed_logs < MAX_BATCH_SIZE)
                    {
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

                        // memset(entry->message, 0, MAX_LOG_MESSAGE_SIZE);
                        entry->message[0] = '\0';
                        buffer->tail = (buffer->tail + 1) % buffer->capacity;
                        processed_logs++;
                    }
                }
            }
            __sync_lock_release(&kLogDWorkLock);
        }
        if (!daemon)
            return nonDaemonRunSuccess;
        sigaction(SIGSLEEP, NULL, kTicksSinceStart + LOGD_SLEEP_TICKS, self);
    }
}


