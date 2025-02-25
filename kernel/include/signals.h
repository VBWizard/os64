#ifndef SIGNALS_H
#define SIGNALS_H

#include <stdint.h>
#include <stdbool.h>

  typedef struct ssignal
    {
        void* sighandler[32];
        uint64_t sigdata[32];
        uint32_t sigmask;
        uintptr_t sigind;
        
    } signals_t;

    typedef enum esignals
    {
        SIGHALT = 1,
        SIGSLEEP = 1 << 1,
        SIGUSLEEP = 1 << 2,
        SIGINT = 1 << 3,
        SIGSEGV = 1 << 4,
        SIGSTOP = 1 << 5,
        SIGIO = 1 << 6,
        SIGKILL = 9,
        SIGCONT = 1 << 7,
		SIGLOGFLUSH = 1 << 9
    } signals;

	extern bool kProcessSignals;
	extern uint8_t signalProcTickFrequency;
	void *sigaction(int signal, uintptr_t *sigAction, uint64_t sigData, void *thread);
	void init_signals();
	
#endif
