#ifndef IPC_H
#define IPC_H

#include "protocol.h"

#include <cstddef>

#define IPC_PIPE_REQUEST  "/tmp/db_req"
#define IPC_PIPE_RESPONSE "/tmp/db_resp"

struct IpcChannel {
    int request_fd;
    int response_fd;
};

int ipc_create_fifos();
int ipc_open_server(IpcChannel* channel);
int ipc_open_client(IpcChannel* channel);
void ipc_close(IpcChannel* channel);
void ipc_remove_fifos();

int ipc_write(int file_descriptor, const void* buffer, size_t size);
int ipc_read(int file_descriptor, void* buffer, size_t size);

#endif
