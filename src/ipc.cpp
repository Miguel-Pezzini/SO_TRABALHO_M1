#include "ipc.h"

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

int ipc_write(int file_descriptor, const void* buffer, size_t size) {
    const char* cursor = static_cast<const char*>(buffer);
    size_t remaining = size;
    while (remaining > 0) {
        ssize_t bytes_written = write(file_descriptor, cursor, remaining);
        if (bytes_written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (bytes_written == 0) {
            return -1;
        }
        cursor += static_cast<size_t>(bytes_written);
        remaining -= static_cast<size_t>(bytes_written);
    }
    return 0;
}

int ipc_read(int file_descriptor, void* buffer, size_t size) {
    char* cursor = static_cast<char*>(buffer);
    size_t remaining = size;
    while (remaining > 0) {
        ssize_t bytes_read = read(file_descriptor, cursor, remaining);
        if (bytes_read < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (bytes_read == 0) {
            return -1;
        }
        cursor += static_cast<size_t>(bytes_read);
        remaining -= static_cast<size_t>(bytes_read);
    }
    return 0;
}

static int set_blocking(int file_descriptor) {
    int flags = fcntl(file_descriptor, F_GETFL);
    if (flags < 0) {
        return -1;
    }
    return fcntl(file_descriptor, F_SETFL, flags & ~O_NONBLOCK);
}

int ipc_create_fifos() {
    if (mkfifo(IPC_PIPE_REQUEST, 0666) < 0 && errno != EEXIST) {
        return -1;
    }
    if (mkfifo(IPC_PIPE_RESPONSE, 0666) < 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

int ipc_open_server(IpcChannel* channel) {
    channel->request_fd = -1;
    channel->response_fd = -1;

    int request_fd = open(IPC_PIPE_REQUEST, O_RDWR);
    if (request_fd < 0) {
        return -1;
    }
    int response_fd = open(IPC_PIPE_RESPONSE, O_RDWR);
    if (response_fd < 0) {
        close(request_fd);
        return -1;
    }

    channel->request_fd = request_fd;
    channel->response_fd = response_fd;
    return 0;
}

int ipc_open_client(IpcChannel* channel) {
    channel->request_fd = -1;
    channel->response_fd = -1;

    for (int attempt = 0; attempt < 50; attempt++) {
        int request_fd = open(IPC_PIPE_REQUEST, O_WRONLY | O_NONBLOCK);
        if (request_fd >= 0) {
            int response_fd = open(IPC_PIPE_RESPONSE, O_RDONLY | O_NONBLOCK);
            if (response_fd >= 0) {
                if (set_blocking(request_fd) < 0 || set_blocking(response_fd) < 0) {
                    close(request_fd);
                    close(response_fd);
                    return -1;
                }
                channel->request_fd = request_fd;
                channel->response_fd = response_fd;
                return 0;
            }
            close(request_fd);
        }
        usleep(100000);
    }
    return -1;
}

void ipc_close(IpcChannel* channel) {
    if (channel->request_fd >= 0) {
        close(channel->request_fd);
        channel->request_fd = -1;
    }
    if (channel->response_fd >= 0) {
        close(channel->response_fd);
        channel->response_fd = -1;
    }
}

void ipc_remove_fifos() {
    unlink(IPC_PIPE_REQUEST);
    unlink(IPC_PIPE_RESPONSE);
}
