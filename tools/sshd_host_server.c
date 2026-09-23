/* Loopback-only OpenSSH interoperability harness for the production engine.
 * Fixed host key and DRBG seed are test fixtures; this is not a deployable daemon. */
#define _GNU_SOURCE
#include "../userland/apps/sshd/ssh_engine.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static ssh_engine s;
static uint8_t pending[SSH_WINDOW];
static size_t pending_len;
static uint64_t rekey_bytes;
static void nonblock(int fd) { if (fd >= 0) fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK); }
/* Forwards, by the daemon's rules (sshd.c forward_open/forwards_pass): the
 * destination must RESOLVE to 127/8, whatever it is called. */
static struct { int fd; uint8_t to[SSH_FORWARD_WINDOW]; size_t to_len; uint8_t from[SSH_DATA_MAX]; size_t from_len; int ended, client_eof; } fw[SSH_FORWARDS];
static void forward_close(uint32_t i) { if (fw[i].fd >= 0) close(fw[i].fd); fw[i].fd = -1; fw[i].to_len = fw[i].from_len = 0; fw[i].ended = fw[i].client_eof = 0; }
static void forward_open(uint32_t i)
{
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, *res = NULL;
    if (getaddrinfo(s.forwards[i].host, NULL, &hints, &res) || !res) { ssh_forward_result(&s, i, 0, 2, "no such host"); return; }
    struct sockaddr_in a = *(struct sockaddr_in *)res->ai_addr; freeaddrinfo(res);
    if ((ntohl(a.sin_addr.s_addr) >> 24) != 127) {
        ssh_forward_result(&s, i, 0, 1, "forwarding reaches this machine's loopback only"); return;
    }
    a.sin_port = htons((uint16_t)s.forwards[i].port);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0 || connect(fd, (void *)&a, sizeof(a))) {
        if (fd >= 0) close(fd);
        ssh_forward_result(&s, i, 0, 2, "connection refused"); return;
    }
    forward_close(i); nonblock(fd); fw[i].fd = fd;
    ssh_forward_result(&s, i, 1, 0, "");
}
static void forwards_pass(void)
{
    for (uint32_t i = 0; i < SSH_FORWARDS; i++) {
        if (fw[i].fd < 0) continue;
        if (fw[i].to_len && !fw[i].ended) {
            ssize_t wrote = write(fw[i].fd, fw[i].to, fw[i].to_len);
            if (wrote > 0) {
                fw[i].to_len -= (size_t)wrote; memmove(fw[i].to, fw[i].to + wrote, fw[i].to_len);
                ssh_forward_consumed(&s, i, (uint32_t)wrote);
            } else if (errno != EAGAIN && errno != EINTR) { fw[i].ended = 1; fw[i].to_len = 0; }
        }
        /* The daemon has no half-close to pass on, so neither does this. */
        if (fw[i].client_eof && !fw[i].to_len) fw[i].ended = 1;
        if (!fw[i].ended && !fw[i].from_len) {
            ssize_t got = read(fw[i].fd, fw[i].from, sizeof(fw[i].from));
            if (got > 0) fw[i].from_len = (size_t)got;
            else if (!got || (errno != EAGAIN && errno != EINTR)) fw[i].ended = 1;
        }
        if (fw[i].from_len) {
            size_t sent = ssh_forward_send(&s, i, fw[i].from, fw[i].from_len);
            fw[i].from_len -= sent; memmove(fw[i].from, fw[i].from + sent, fw[i].from_len);
        }
        if (fw[i].ended && !fw[i].from_len && ssh_forward_finish(&s, i)) forward_close(i);
    }
}
static int spawn(int *input, int out[2], int shell)
{
    pid_t pid;
    if (shell) {
        struct winsize size = {.ws_row=s.rows, .ws_col=s.cols};
        pid = forkpty(input, NULL, NULL, &size);
        if (!pid) {
            const char *term = ssh_term_env(&s);
            if (term) setenv("TERM", term, 1);
            execl("/bin/sh", "sh", "-i", (char *)NULL); _exit(127);
        }
        out[0] = *input; out[1] = -1;
    } else {
        int in[2], stdout_pipe[2], stderr_pipe[2];
        if (pipe(in) || pipe(stdout_pipe) || pipe(stderr_pipe)) return -1;
        pid = fork();
        if (!pid) {
            setpgid(0, 0); dup2(in[0], 0); dup2(stdout_pipe[1], 1); dup2(stderr_pipe[1], 2);
            close(in[0]); close(in[1]); close(stdout_pipe[0]); close(stdout_pipe[1]); close(stderr_pipe[0]); close(stderr_pipe[1]);
            execl("/bin/sh", "sh", "-c", s.command, (char *)NULL); _exit(127);
        }
        close(in[0]); close(stdout_pipe[1]); close(stderr_pipe[1]);
        *input=in[1]; out[0]=stdout_pipe[0]; out[1]=stderr_pipe[0];
    }
    nonblock(*input); nonblock(out[0]); nonblock(out[1]); return pid;
}
static void serve(int sock, const char *pubfile)
{
    uint8_t seed[32]={7}, key[32]={0}; key[31]=1;
    if (!ssh_init(&s, seed, key)) exit(1);
    FILE *f=fopen(pubfile,"r"); if (!f) exit(2);
    char line[2048]; while (fgets(line,sizeof(line),f)) {
        size_t n=strcspn(line,"\r\n");
        if (ssh_authorized_line(&s,line,n)<0) exit(3);
    }
    fclose(f); nonblock(sock);
    s.forward_policy = SSH_FORWARD_LOOPBACK;
    for (uint32_t i = 0; i < SSH_FORWARDS; i++) fw[i].fd = -1;
    int input=-1, out[2]={-1,-1}, child=-1, eof=0, ended=0, status=0, closing=0;
    uint8_t net[32768], output[4096]; size_t net_len=0, net_off=0;
    unsigned next_stream=0;
    for (int turns=0; turns<120000; turns++) {
        const uint8_t *wire; size_t n=ssh_output(&s,&wire);
        if (n) {
            ssize_t wrote=send(sock,wire,n,MSG_NOSIGNAL);
            if (wrote>0) ssh_output_consume(&s,(size_t)wrote);
            else if (errno!=EAGAIN && errno!=EINTR) break;
        }
        if (s.closed || (closing && !s.kex && !ssh_forwards_live(&s))) { if (!s.out_len) break; usleep(1000); continue; }
        if (net_off==net_len) {
            ssize_t got=recv(sock,net,sizeof(net),0);
            if (!got) break;
            if (got<0 && errno!=EAGAIN && errno!=EINTR) break;
            net_off=net_len=0; if (got>0) net_len=(size_t)got;
        }
        if (net_off<net_len) {
            net_off+=ssh_receive(&s,net+net_off,net_len-net_off);
            switch (s.event) {
            case SSH_EVENT_EXEC: case SSH_EVENT_SHELL:
                child=spawn(&input,out,s.event==SSH_EVENT_SHELL); ssh_start_result(&s,child>0); break;
            case SSH_EVENT_INPUT:
                if (s.event_len>sizeof(pending)-pending_len) abort();
                memcpy(pending+pending_len,s.event_data,s.event_len); pending_len+=s.event_len; break;
            case SSH_EVENT_EOF: eof=1; break;
            case SSH_EVENT_CLOSE: closing=1; break;
            case SSH_EVENT_FORWARD_OPEN: forward_open(s.event_forward); break;
            case SSH_EVENT_FORWARD_DATA: {
                uint32_t i = s.event_forward;
                if (fw[i].fd < 0 || fw[i].ended) { ssh_forward_consumed(&s, i, (uint32_t)s.event_len); break; }
                if (s.event_len > sizeof(fw[i].to) - fw[i].to_len) abort();
                memcpy(fw[i].to + fw[i].to_len, s.event_data, s.event_len); fw[i].to_len += s.event_len; break;
            }
            case SSH_EVENT_FORWARD_EOF: fw[s.event_forward].client_eof = 1; break;
            case SSH_EVENT_FORWARD_CLOSE: forward_close(s.event_forward); break;
            case SSH_EVENT_RESIZE: {
                struct winsize size={.ws_row=s.resize_rows,.ws_col=s.resize_cols};
                int good=input<0 ? !s.started : ioctl(input,TIOCSWINSZ,&size)==0;
                ssh_resize_result(&s,good);
                break;
            }
            default: break;
            }
        }
        if (pending_len && input>=0) {
            ssize_t wrote=write(input,pending,pending_len);
            if (wrote>0) {
                pending_len-=(size_t)wrote; memmove(pending,pending+wrote,pending_len); ssh_input_consumed(&s,(uint32_t)wrote);
            } else if (errno!=EAGAIN && errno!=EINTR) { close(input); input=-1; }
        }
        if (eof && !pending_len && input>=0 && !s.pty) { close(input); input=-1; }
        unsigned first_stream=next_stream; int rotated=0;
        for (unsigned pass=0;pass<2;pass++) {
            unsigned i=(first_stream+pass)%2;
            size_t cap=sizeof(output); if (cap>s.peer_window) cap=s.peer_window;
            if (cap>s.peer_packet) cap=s.peer_packet;
            if (out[i]<0 || !s.started || s.kex || !cap || SSH_OUTPUT_CAP-s.out_len<cap+128) continue;
            ssize_t got=read(out[i],output,cap);
            if (!got || (got<0 && errno==EIO && s.pty)) { close(out[i]); out[i]=-1; }
            else if (got>0) {
                if (ssh_send_data(&s,output,(size_t)got,i)!=(size_t)got) abort();
                /* The daemon's rule: the first sender in a pass hands
                 * priority to the other stream, and only the first. */
                if (!rotated) { next_stream=i^1u; rotated=1; }
            }
        }
        if (!closing && !s.sent_close && rekey_bytes && s.established && !s.kex && (s.tx.bytes >= rekey_bytes || s.rx.bytes >= rekey_bytes)) ssh_rekey(&s);
        forwards_pass();
        if (child>0 && !ended && waitpid(child,&status,WNOHANG)==child) ended=1;
        if (ended && out[0]<0 && out[1]<0) ssh_send_exit(&s,WIFEXITED(status)?WEXITSTATUS(status):128+WTERMSIG(status));
        usleep(1000);
    }
    fprintf(stderr,"session: %s; auth=%d rx=%u tx=%u\n",s.error,s.authenticated,s.rx.seq,s.tx.seq);
    if (child>0 && !ended) { kill(-child,SIGKILL); kill(child,SIGKILL); waitpid(child,NULL,0); }
    close(sock);
}
int main(int argc,char **argv)
{
    if(argc!=3 && argc!=4) return 2;
    if(argc==4) rekey_bytes=strtoull(argv[3],NULL,10);
    signal(SIGPIPE,SIG_IGN);
    int listener=socket(AF_INET,SOCK_STREAM,0), one=1;
    setsockopt(listener,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
    struct sockaddr_in addr={.sin_family=AF_INET,.sin_port=htons((uint16_t)atoi(argv[1])),.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    if(bind(listener,(void *)&addr,sizeof(addr)) || listen(listener,4)) { perror("listen"); return 1; }
    socklen_t len=sizeof(addr); getsockname(listener,(void *)&addr,&len);
    printf("%u\n",ntohs(addr.sin_port)); fflush(stdout);
    int sock=accept(listener,NULL,NULL); close(listener);
    if(sock<0) return 1;
    serve(sock,argv[2]); return 0;
}
