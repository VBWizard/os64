// h2_absent.c — HTTP/2 for a harness in which no dial answers h2.
//
// libfetch's pool and driver call an HTTP/2 connection when a connection is
// one (pool.h). In the harnesses that script HTTP/1.1 alone no server
// answers a dial's offer with h2, so none is ever made, and each call below
// is one that cannot happen: it aborts, which is the proof that against a
// server that speaks HTTP/1.1 libfetch does nothing of HTTP/2's. net.conf
// is absent here too: HTTP/2 stays offered.
#include <stdlib.h>

#include "h2_conn.h"
#include "os64/conf.h"
#include "tls/transport.h"

h2_conn_t *h2_conn_adopt(os64_tls_client *engine, int32_t socket, const void *unread, size_t unread_len,
                         const h2_greeting_t *greeting, const h2_conn_hooks_t *hooks)
{
    (void)engine; (void)socket; (void)unread; (void)unread_len; (void)greeting; (void)hooks;
    abort();
}
// Every fetch has its greeting judged as it opens, whatever it then speaks.
bool h2_greeting_ok(const h2_greeting_t *greeting) { (void)greeting; return true; }
bool h2_conn_room(h2_conn_t *conn) { (void)conn; abort(); }
void h2_conn_condemn(h2_conn_t *conn) { (void)conn; abort(); }
size_t h2_conn_queued(h2_conn_t *conn) { (void)conn; abort(); }
size_t h2_conn_buffered(h2_conn_t *conn) { (void)conn; abort(); }
void h2_conn_release(h2_conn_t *conn) { (void)conn; abort(); }
h2_result_t h2_stream_open(h2_conn_t *conn, const h2_field_t *fields, size_t count, bool ends, bool bodiless,
                           uint32_t weight, h2_wait_t *wait, h2_stream_t **stream)
{
    (void)conn; (void)fields; (void)count; (void)ends; (void)bodiless; (void)weight; (void)wait; (void)stream;
    abort();
}
h2_result_t h2_stream_send(h2_stream_t *stream, const void *bytes, size_t len, bool ends, h2_wait_t *wait,
                           size_t *sent)
{
    (void)stream; (void)bytes; (void)len; (void)ends; (void)wait; (void)sent;
    abort();
}
h2_result_t h2_stream_head(h2_stream_t *stream, h2_wait_t *wait, h2_hpack_emit_t emit, void *ctx)
{
    (void)stream; (void)wait; (void)emit; (void)ctx;
    abort();
}
int64_t h2_stream_read(h2_stream_t *stream, void *buffer, size_t room, h2_wait_t *wait)
{
    (void)stream; (void)buffer; (void)room; (void)wait;
    abort();
}
uint32_t h2_stream_id(const h2_stream_t *stream) { (void)stream; abort(); }
h2_ending_t h2_stream_ended(h2_stream_t *stream) { (void)stream; abort(); }
void h2_stream_close(h2_stream_t *stream) { (void)stream; abort(); }
const char *h2_error_name(uint32_t code) { (void)code; abort(); }
void os64_tls_free(os64_tls_client *client) { (void)client; abort(); }
os64_tls_status_t os64_tls_transport_release(os64_tls_transport *transport, os64_tls_client **client,
                                             int32_t *handle, void *unread, size_t room, size_t *unread_len)
{
    (void)transport; (void)client; (void)handle; (void)unread; (void)room; (void)unread_len;
    abort();
}

int64_t os64_conf_find_read(const char *name, os64_conf_fn fn, void *user, char *path_out, size_t cap)
{
    (void)name; (void)fn; (void)user; (void)path_out; (void)cap;
    return OS64_CONF_NO_FILE;
}
