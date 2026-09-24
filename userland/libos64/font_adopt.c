#include "os64/font_adopt.h"

os64_font_status_t os64_font_adopt(os64_font_set_t *candidate,
    const os64_font_consumer_t *consumers, size_t count, size_t *failed_index)
{
    if (failed_index) *failed_index=SIZE_MAX;
    if (!candidate || !consumers || !count || count>OS64_FONT_CONSUMERS_MAX)
        return OS64_FONT_BAD_ARGUMENT;
    /* Copy callback descriptors before entering application code. Plans and
     * descriptor storage are bounded; the coordinator itself does not allocate. */
    os64_font_consumer_t batch[OS64_FONT_CONSUMERS_MAX];
    void *plans[OS64_FONT_CONSUMERS_MAX]={0};
    size_t barrier=SIZE_MAX;
    for (size_t n=0;n<count;n++) {
        batch[n]=consumers[n];
        if (!batch[n].prepare || !batch[n].commit || !batch[n].abort)
            return OS64_FONT_BAD_ARGUMENT;
        if (batch[n].barrier) {
            if (barrier!=SIZE_MAX) return OS64_FONT_BAD_ARGUMENT;
            barrier=n;
        }
        for (size_t j=0;j<n;j++) if (batch[n].user==batch[j].user)
            return OS64_FONT_BAD_ARGUMENT;
    }
    os64_font_status_t status=os64_font_set_retain(candidate);
    if (status!=OS64_FONT_OK) return status;
    size_t prepared=0;
    for (;prepared<count;prepared++) {
        status=batch[prepared].prepare(batch[prepared].user,candidate,&plans[prepared]);
        if (status==OS64_FONT_OK && !plans[prepared]) status=OS64_FONT_ENGINE_ERROR;
        if (status!=OS64_FONT_OK) {
            if (failed_index) *failed_index=prepared;
            goto abort;
        }
    }
    if (barrier!=SIZE_MAX) {
        status=batch[barrier].barrier(batch[barrier].user,plans[barrier]);
        if (status!=OS64_FONT_OK) {
            if (failed_index) *failed_index=barrier;
            goto abort;
        }
    }
    for (size_t n=0;n<count;n++) batch[n].commit(batch[n].user,plans[n]);
    os64_font_set_release(candidate);
    return OS64_FONT_OK;
abort:
    while (prepared) {
        prepared--;
        batch[prepared].abort(batch[prepared].user,plans[prepared]);
    }
    os64_font_set_release(candidate);
    return status;
}
