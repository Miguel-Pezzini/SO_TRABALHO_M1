#ifndef IPC_H
#define IPC_H

#include "protocol.h"

#include <cstddef>

#define IPC_PIPE_REQ  "/tmp/db_req"
#define IPC_PIPE_RESP "/tmp/db_resp"

struct IpcChannel {
    int fd_req;
    int fd_resp;
};

int ipc_create_fifos();
int ipc_open_server(IpcChannel* ch);
int ipc_open_client(IpcChannel* ch);
void ipc_close(IpcChannel* ch);
void ipc_remove_fifos();

int ipc_write(int fd, const void* buf, size_t n);
int ipc_read(int fd, void* buf, size_t n);

#endif
