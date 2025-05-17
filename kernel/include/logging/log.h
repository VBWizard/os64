#ifndef LOG_H
#define LOG_H

#include <stdint.h>
#include <stddef.h>
#include "smp.h"

//Set to sizeof(log_entry_t)*10 to enable buffer full processing

#define LOG_BUFFER_SIZE (1 * 1024 * 1024) // 1MB per core
#define MAX_LOG_MESSAGE_SIZE 256
#define MAX_BATCH_SIZE 1000  // Process up to 10 messages before sleeping
#define LOGD_SLEEP_TICKS (TICKS_PER_SECOND)  // Sleep for 1 second
#define LOGD_FLUSH_WAIT_TICKS 100
typedef struct log_entry {
    uint64_t timestamp;
    uint64_t tick_count;
    uint16_t core_id;
    uint8_t log_level;
    uint8_t category;
	uint64_t threadID;
    char message[MAX_LOG_MESSAGE_SIZE];
} log_entry_t;

typedef struct log_buffer {
    log_entry_t *entries;
    size_t head;
    size_t tail;
    size_t capacity;
    volatile uint32_t lock;
} log_buffer_t;

extern log_buffer_t core_log_buffers[MAX_CPUS];
extern bool kLoggingInitialized;
void logging_queueing_init();
void dump_log_buffer(uint16_t core);
void log_store_entry(uint16_t core, uint64_t tick_count, uint8_t priority, uint8_t category, bool continued, const char *message);
void logd_thread();

#endif // LOG_H
