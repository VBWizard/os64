#ifndef FPU_H
#define FPU_H

#include <stdbool.h>
#include <stdint.h>

#define FPU_FXSAVE_SIZE 512

typedef struct
{
	uint8_t bytes[FPU_FXSAVE_SIZE] __attribute__((aligned(16)));
} fpu_state_t;

_Static_assert(_Alignof(fpu_state_t) == 16,
		"FXSAVE requires every FPU state image to be 16-byte aligned");

void fpu_init_this_cpu(void);
void fpu_state_init(fpu_state_t *state);
void fpu_save(fpu_state_t *state);
bool fpu_restore(fpu_state_t *state);
bool fpu_state_from_user(fpu_state_t *destination, const fpu_state_t *source);

#endif
