#include "decorations.h"
#include "os64/decoration.h"
#include "gui/gui_internal.h"
#include "gui/window.h"
#include "memory/kmalloc.h"
#include "memory/memcpy.h"
#include "memory/memset.h"

struct decoration_pending {
    spinlock_t lock;
    bool busy;
    uint8_t *bytes;
    size_t length, used;
    uint64_t expected;
};
static uint64_t s_generation, s_fingerprint;
static uint32_t s_bytes;

decoration_pending_t *decorations_begin(void)
{
    decoration_pending_t *p=kmalloc(sizeof(*p));
    if (p) memset(p,0,sizeof(*p));
    return p;
}

void decorations_discard(decoration_pending_t *p)
{
    /* The file-object pin keeps this close behind its in-flight operations. */
    if (!p) return;
    kfree(p->bytes);
    kfree(p);
}

int decorations_status(char *out, size_t capacity)
{
    if (!out || capacity<OS64_DECOR_STATUS_MAX) return -1;
    uint64_t flags=spinlock_acquire_irqsave(&kGuiLock);
    os64_decor_status_t status={s_generation,s_fingerprint,s_bytes};
    int length=(int)os64_decor_status_write(out,&status);
    spinlock_release_irqrestore(&kGuiLock,flags);
    return length;
}

int decorations_write(decoration_pending_t *p, const void *bytes, size_t length)
{
    if (!p || !bytes || length<sizeof(os64_decor_command_t) ||
        length>sizeof(os64_decor_command_t)+OS64_DECOR_DATA_MAX) return -1;
    os64_decor_command_t cmd;
    memcpy(&cmd,bytes,sizeof(cmd));
    if (cmd.data_bytes!=length-sizeof(cmd)) return -1;
    uint64_t flags=spinlock_acquire_irqsave(&p->lock);
    if (p->busy) {spinlock_release_irqrestore(&p->lock,flags);return -1;}
    p->busy=true;
    spinlock_release_irqrestore(&p->lock,flags);
    /* busy serializes the complete operation without allocating under a
     * spinlock. A shared descriptor's racing command fails without changing
     * staging. Close waits for the file pin held by this syscall. */
    int result=-1;
    if (cmd.command==OS64_DECOR_BEGIN && !p->bytes && !cmd.offset && !cmd.data_bytes &&
        cmd.total_bytes>=OS64_DECOR_LEGACY_HEADER_BYTES && cmd.total_bytes<=OS64_DECOR_BYTES_MAX) {
        uint8_t *data=kmalloc(cmd.total_bytes);
        if (data) {
            p->bytes=data;p->length=cmd.total_bytes;p->used=0;p->expected=cmd.expected_generation;
            result=(int)length;
        }
    } else if (p->bytes && cmd.total_bytes==p->length && cmd.expected_generation==p->expected &&
        cmd.offset==p->used) {
        if (cmd.command==OS64_DECOR_DATA && cmd.data_bytes && cmd.data_bytes<=p->length-p->used) {
            memcpy(p->bytes+p->used,(const uint8_t *)bytes+sizeof(cmd),cmd.data_bytes);
            p->used+=cmd.data_bytes;result=(int)length;
        } else if (cmd.command==OS64_DECOR_COMMIT && !cmd.data_bytes && p->used==p->length) {
            os64_decor_view_t view;
            if (os64_decor_validate(p->bytes,p->length,&view)) {
                /* Hash private validated staging outside the GUI lock. Publish the
                 * identifier with the assets so readers see one generation. */
                uint64_t fingerprint=os64_decor_fingerprint(p->bytes,p->length);
                void *retired=NULL;
                uint64_t gui_flags=spinlock_acquire_irqsave(&kGuiLock);
                if (s_generation==p->expected && s_generation!=UINT64_MAX && wm_set_decoration(&view,&retired)) {
                    ++s_generation;s_fingerprint=fingerprint;s_bytes=(uint32_t)p->length;
                    p->bytes=NULL;p->length=p->used=0;result=(int)length;
                }
                spinlock_release_irqrestore(&kGuiLock,gui_flags);
                kfree(retired);
            }
        }
    }
    flags=spinlock_acquire_irqsave(&p->lock);
    p->busy=false;
    spinlock_release_irqrestore(&p->lock,flags);
    return result;
}
