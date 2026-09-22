#include "ipc.h"

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

int ipc_write(int fd, const void* buf, size_t n) {
    const char* p = static_cast<const char*>(buf);
    size_t left = n;
    while (left > 0) {
        ssize_t w = write(fd, p, left);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (w == 0) {
            return -1;
        }
        p += static_cast<size_t>(w);
        left -= static_cast<size_t>(w);
    }
    return 0;
}

int ipc_read(int fd, void* buf, size_t n) {
    char* p = static_cast<char*>(buf);
    size_t left = n;
    while (left > 0) {
        ssize_t r = read(fd, p, left);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (r == 0) {
            return -1;
        }
        p += static_cast<size_t>(r);
        left -= static_cast<size_t>(r);
    }
    return 0;
}

static int set_blocking(int fd) {
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
}

int ipc_create_fifos() {
    if (mkfifo(IPC_PIPE_REQ, 0666) < 0 && errno != EEXIST) {
        return -1;
    }
    if (mkfifo(IPC_PIPE_RESP, 0666) < 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

int ipc_open_server(IpcChannel* ch) {
    ch->fd_req = -1;
    ch->fd_resp = -1;

    // O_RDWR no Linux evita bloqueio no open e EOF quando o cliente sai.
    int req = open(IPC_PIPE_REQ, O_RDWR);
    if (req < 0) {
        return -1;
    }
    int resp = open(IPC_PIPE_RESP, O_RDWR);
    if (resp < 0) {
        close(req);
        return -1;
    }

    ch->fd_req = req;
    ch->fd_resp = resp;
    return 0;
}

int ipc_open_client(IpcChannel* ch) {
    ch->fd_req = -1;
    ch->fd_resp = -1;

    for (int i = 0; i < 50; i++) {
        int req = open(IPC_PIPE_REQ, O_WRONLY | O_NONBLOCK);
        if (req >= 0) {
            int resp = open(IPC_PIPE_RESP, O_RDONLY | O_NONBLOCK);
            if (resp >= 0) {
                if (set_blocking(req) < 0 || set_blocking(resp) < 0) {
                    close(req);
                    close(resp);
                    return -1;
                }
                ch->fd_req = req;
                ch->fd_resp = resp;
                return 0;
            }
            close(req);
        }
        usleep(100000);
    }
    return -1;
}

void ipc_close(IpcChannel* ch) {
    if (ch->fd_req >= 0) {
        close(ch->fd_req);
        ch->fd_req = -1;
    }
    if (ch->fd_resp >= 0) {
        close(ch->fd_resp);
        ch->fd_resp = -1;
    }
}

void ipc_remove_fifos() {
    unlink(IPC_PIPE_REQ);
    unlink(IPC_PIPE_RESP);
}
