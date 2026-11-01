// picture.c — a picture fetched and decoded on a worker (picture.h).

#include "picture.h"
#include "diag.h"
#include "fetch_job.h"
#include "os64/fmt.h"
#include "os64/mem.h"
#include "os64/slurp.h"
#include "os64/str.h"

// Negotiated image types, then anything: a server that labels a GIF
// as text/plain still sent a GIF, and the decoder reads the bytes, not the
// label. An SVG MIME type selects XML validation. A browser preset asks
// with its browser's list instead
// (agent.c; way_fetch_hooks puts it in).
#define PICTURE_ACCEPT "image/svg+xml, " OS64_IMAGE_ACCEPT ", */*;q=0.5"
_Static_assert(sizeof(PICTURE_ACCEPT) <= OS64_FETCH_ACCEPT_MAX,
               "libfetch refuses an Accept longer than OS64_FETCH_ACCEPT_MAX");

// XML candidates are validated by the namespace-aware parser. Looking for an
// arbitrary <svg substring would accept HTML wrappers and comments as SVG.
static bool xml_candidate(const uint8_t *bytes, size_t len)
{
    if (len >= 2 && ((bytes[0] == 0xff && bytes[1] == 0xfe) ||
                     (bytes[0] == 0xfe && bytes[1] == 0xff)))
        return true;
    size_t at = len >= 3 && bytes[0] == 0xef && bytes[1] == 0xbb && bytes[2] == 0xbf ? 3 : 0;
    while (at < len && (bytes[at] == ' ' || bytes[at] == '\t' || bytes[at] == '\r' || bytes[at] == '\n'))
        ++at;
    return at < len && bytes[at] == '<';
}
static bool svg_type(const char *s)
{
    const char *type = "image/svg+xml";
    if (!s) return false;
    while (*type) {
        char c = *s++;
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c != *type++) return false;
    }
    return !*s || *s == ';' || *s == ' ' || *s == '\t';
}
static os64_image_status_t svg_failure(svg_status_t st, yonder_picture_t *p)
{
    const char *why = st == SVG_CANCELLED ? "cancelled" :
                      st == SVG_LIMIT ? "SVG exceeded its limits" :
                      st == SVG_NO_MEMORY ? "no memory for SVG" :
                      st == SVG_UNSUPPORTED ? "unsupported SVG document" : "malformed SVG document";
    os64_strcopy(p->why, sizeof(p->why), why);
    return st == SVG_NO_MEMORY ? OS64_IMAGE_NO_MEMORY :
           st == SVG_LIMIT ? OS64_IMAGE_LIMIT :
           st == SVG_UNSUPPORTED ? OS64_IMAGE_UNSUPPORTED :
           st == SVG_CANCELLED ? OS64_IMAGE_IO_ERROR : OS64_IMAGE_MALFORMED;
}
// A display GIF keeps its sequence; a Canvas source decodes its default
// frame. SVG keeps a tree rather than fixed-size pixels. Raster formats retain libimage's signature-based decoding contract.
static os64_image_status_t decode(const uint8_t *bytes, size_t len, const char *type,
                                   const yonder_picture_job_t *job, bool (*cancelled)(void *),
                                   void *context, yonder_picture_t *p)
{
    if (cancelled && cancelled(context))
        return svg_failure(SVG_CANCELLED, p);
    if (job->svg_enabled && (svg_type(type) || xml_candidate(bytes, len))) {
        svg_status_t st = yonder_svg_document_parse(bytes, len, cancelled, context, &p->svg, NULL);
        if (st == SVG_OK)
            st = yonder_svg_document_size(p->svg, job->dark, job->reduced_motion,
                                           cancelled, context, &p->width, &p->height);
        if (st != SVG_OK) {
            yonder_svg_document_free(p->svg);
            p->svg = NULL;
            return svg_failure(st, p);
        }
        p->kind = YONDER_PICTURE_SVG;
        p->cost = yonder_svg_document_cost(p->svg);
        return OS64_IMAGE_OK;
    }
    if (!job->canvas_request && len >= 4 && os64_memcmp(bytes, "GIF8", 4) == 0) {
        os64_image_sequence_t *seq = NULL;
        if (os64_image_sequence_decode(bytes, len, &seq) == OS64_IMAGE_OK) {
            const os64_image_frame_t *f = os64_image_sequence_frame(seq);
            if (f->frame_count > 1) {
                // libimage's account of a handle (GIF_ANIMATION.md): the
                // copied input, the canvas, a byte a pixel of staging, and
                // at most a canvas-sized restore rectangle.
                p->kind = YONDER_PICTURE_SEQUENCE;
                p->sequence = seq;
                p->cost = len + (size_t)f->width * f->height * 9u;
                return OS64_IMAGE_OK;
            }
        }
        os64_image_sequence_free(seq);
    }
    os64_image_status_t st = os64_image_decode(bytes, len, &p->image);
    p->cost = (size_t)p->image.width * p->image.height * 4u;
    if (st == OS64_IMAGE_UNKNOWN_FORMAT)
        p->format = yonder_diag_image_format(bytes, len);
    else if (st == OS64_IMAGE_UNSUPPORTED && len >= 12 &&
             os64_memcmp(bytes, "RIFF", 4) == 0 && os64_memcmp(bytes + 8, "WEBP", 4) == 0)
        p->format = "webp";
    if (st == OS64_IMAGE_UNSUPPORTED && p->format && os64_strcmp(p->format, "webp") == 0 &&
        len >= 30 && os64_memcmp(bytes + 12, "VP8X", 4) == 0 && (bytes[20] & 2))
        os64_strcopy(p->why, sizeof(p->why), "animated");
    else if (st != OS64_IMAGE_OK && st != OS64_IMAGE_UNKNOWN_FORMAT)
        os64_snprintf(p->why, sizeof(p->why), "would not decode (%s)", os64_image_status_name(st));
    return st;
}

int64_t yonder_picture_run(void *job, bool (*cancelled)(void *ctx), void *ctx, void **out)
{
    yonder_picture_job_t *j = job;
    yonder_picture_t *p = os64_calloc(1, sizeof(*p));
    if (p == NULL)
        return -1;
    *out = p;
    if (cancelled && cancelled(ctx)) {
        p->status = svg_failure(SVG_CANCELLED, p);
        return 0;
    }
    // A data: picture is its own address, decoded here as Fetch decodes
    // one (fetch_job.h): nothing is fetched or read. Its type is what the
    // address says, which picks SVG as a server's Content-Type would.
    if (j->data_url != NULL) {
        uint8_t *bytes = NULL;
        size_t len = 0;
        char type[WAY_MIME_SERIALIZED_MAX];
        if (!yonder_fetch_data_url(j->data_url, OS64_IMAGE_CAP_DEFAULT, &bytes, &len, type, sizeof(type))) {
            p->status = OS64_IMAGE_MALFORMED;
            os64_strcopy(p->why, sizeof(p->why), "its data: address is malformed, too big, or no memory");
        } else if (len == 0) {
            p->status = OS64_IMAGE_NO_FILE;
            os64_strcopy(p->why, sizeof(p->why), "its data: address is empty");
        } else {
            p->status = decode(bytes, len, type, j, cancelled, ctx, p);
            p->origin_clean = true;
        }
        os64_free(bytes);
        return p->status == OS64_IMAGE_OK ? 1 : 0;
    }
    // A picture on a page read from disk is a file beside it.
    if (os64_strlen(j->url) > 7 && os64_memcmp(j->url, "file://", 7) == 0) {
        size_t len = 0;
        uint8_t *bytes = NULL;
        p->status = OS64_IMAGE_IO_ERROR;
        if (os64_slurp(j->url + 7, OS64_IMAGE_CAP_DEFAULT, &bytes, &len) == OS64_SLURP_OK) {
            p->status = decode(bytes, len, NULL, j, cancelled, ctx, p);
            /* Local pages may sample local fixtures; a network page may not
             * acquire file pixels through this worker's Canvas request. */
            p->origin_clean=(os64_strlen(j->hooks.referrer)>=7&&!os64_memcmp(j->hooks.referrer,"file://",7));
            os64_free(bytes);
        } else {
            os64_strcopy(p->why, sizeof(p->why), "could not be read");
        }
        return p->status == OS64_IMAGE_OK ? 1 : 0;
    }
    if(j->canvas_request) {
        yonder_fetch_job_t request={.agent=j->agent,.identity=j->hooks.identity,.url=j->url,
            .jar=j->hooks.jar,.max_body=OS64_IMAGE_CAP_DEFAULT};
        request.request.mode=j->canvas_request==1?WAY_MODE_NO_CORS:WAY_MODE_CORS;
        request.request.credentials=j->canvas_request==2?WAY_CREDENTIALS_SAME_ORIGIN:WAY_CREDENTIALS_INCLUDE;
        request.request.redirect=WAY_REDIRECT_FOLLOW;
        request.request.policy=WAY_POLICY_STRICT_ORIGIN_WHEN_CROSS_ORIGIN;
        way_origin_of(j->hooks.referrer,0,&request.request.origin);
        os64_strcopy(request.request.method,sizeof(request.request.method),"GET");
        os64_strcopy(request.referrer,sizeof(request.referrer),j->hooks.referrer);
        yonder_fetched_t *got=NULL;
        yonder_fetch_image_run(&request,cancelled,ctx,(void **)&got);
        p->status=OS64_IMAGE_IO_ERROR;
        if(got&&got->ok&&got->status>=200&&got->status<300&&got->has_body) {
            p->status=decode(got->body,got->body_len,NULL,j,cancelled,ctx,p);
            p->origin_clean=got->type!=WAY_TYPE_OPAQUE;
        } else os64_strcopy(p->why,sizeof(p->why),got&&got->why[0]?got->why:"image fetch failed");
        yonder_fetched_free(got);return p->status==OS64_IMAGE_OK?1:0;
    }
    os64_fetch_options_t opt = {0};
    opt.user_agent = j->agent;
    opt.accept = j->svg_enabled ? PICTURE_ACCEPT : OS64_IMAGE_ACCEPT ", */*;q=0.5";
    j->hooks.cancelled = cancelled;
    j->hooks.cancel_ctx = ctx;
    way_whole_t body;
    p->status = OS64_IMAGE_IO_ERROR;
    if (!way_fetch_whole(&j->hooks, j->url, &opt, OS64_IMAGE_CAP_DEFAULT, &body)) {
        os64_snprintf(p->why, sizeof(p->why), "did not fetch (%s)", os64_fetch_status_name(body.fetch));
        return 0;
    }
    // An error's body is the server's page about the error, not the picture.
    if (cancelled && cancelled(ctx)) {
        os64_strcopy(p->why, sizeof(p->why), "cancelled");
    } else if (body.status != 0 && (body.status < 200 || body.status >= 300)) {
        p->status = OS64_IMAGE_NO_FILE;
        os64_snprintf(p->why, sizeof(p->why), "HTTP %d", (int)body.status);
    } else if (body.len == 0) {
        // Nothing came to decode — a 204, or a reply that was empty — which
        // is the server's answer, not a format nobody decodes.
        p->status = OS64_IMAGE_NO_FILE;
        if (body.status == 204 || body.status == 205)
            os64_snprintf(p->why, sizeof(p->why), "HTTP %d, no picture", (int)body.status);
        else
            os64_strcopy(p->why, sizeof(p->why), "an empty reply");
    } else {
        p->status = decode(body.bytes, body.len, body.content_type, j, cancelled, ctx, p);
    }
    os64_free(body.bytes);
    return p->status == OS64_IMAGE_OK ? 1 : 0;
}

void yonder_picture_release(void *job, void *product)
{
    yonder_picture_t *p = product;
    if (p != NULL) {
        yonder_svg_document_free(p->svg);
        os64_image_free(&p->image);
        os64_image_sequence_free(p->sequence);
        os64_free(p);
    }
    if (job != NULL)
        os64_free(((yonder_picture_job_t *)job)->data_url);
    os64_free(job);
}
