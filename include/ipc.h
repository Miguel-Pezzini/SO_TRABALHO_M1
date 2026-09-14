#ifndef IPC_H
#define IPC_H

#include "protocol.h"

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

int ipc_send_request(int fd, const Request* req);
int ipc_recv_request(int fd, Request* req);
int ipc_send_response(int fd, const Response* resp);
int ipc_recv_response(int fd, Response* resp);

#endif
