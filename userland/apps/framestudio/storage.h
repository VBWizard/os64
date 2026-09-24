#ifndef FRAME_STUDIO_STORAGE_H
#define FRAME_STUDIO_STORAGE_H
#include "model.h"
#define FRAME_NAME_MAX 40u
#define FRAME_SAVED_MAX 128u
#define FRAME_FILE_HEADER_BYTES 800u
#define FRAME_FILE_BYTES_MAX (FRAME_FILE_HEADER_BYTES+OS64_DECOR_BYTES_MAX)
enum { FRAME_STORE_OK, FRAME_STORE_INVALID, FRAME_STORE_IO, FRAME_STORE_EXISTS, FRAME_STORE_LIMIT, FRAME_STORE_MEMORY };
typedef struct { char name[FRAME_NAME_MAX+1]; } frame_saved_t;
bool frame_name_valid(const char *);
/* Container operations own returned allocations; outputs stay empty on failure. */
int frame_encode(const frame_draft_t *,const void *,size_t,void **,size_t *);
int frame_decode(const void *,size_t,frame_draft_t *,void **,size_t *);
int frame_saved_list(frame_saved_t *,size_t);
int frame_save(const char *,const frame_draft_t *,const void *,size_t,bool replace);
/* Remove a validated collection name without decoding its possibly damaged file. */
int frame_delete(const char *);
int frame_load(const char *,frame_draft_t *,void **,size_t *);
/* Search the supplied sorted collection against a stable live fingerprint.
 * Returns 1 with owned assets and the checked generation on a match, otherwise
 * 0 with empty asset outputs and other outputs unchanged.
 * Failed/corrupt entries are skipped; a session change discards the candidate. */
int frame_load_active(const frame_saved_t *,size_t,size_t *,frame_draft_t *,void **,size_t *,uint64_t *);
#endif
