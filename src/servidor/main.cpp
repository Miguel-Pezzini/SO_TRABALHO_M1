#include "ipc.h"
#include "protocol.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <poll.h>
#include <pthread.h>
#include <queue>
#include <unistd.h>
#include <vector>

static volatile sig_atomic_t stop_requested = 0;

static void on_signal(int) {
    stop_requested = 1;
}

struct Banco {
    std::vector<Registro> rows;
    pthread_mutex_t mutex;
};

static void banco_init(Banco* banco) {
    pthread_mutex_init(&banco->mutex, nullptr);
}

static void banco_destroy(Banco* banco) {
    pthread_mutex_destroy(&banco->mutex);
}

static void copy_nome(char* destination, const char* source) {
    std::strncpy(destination, source, sizeof(Registro::nome) - 1);
    destination[sizeof(Registro::nome) - 1] = '\0';
}

static Registro* banco_find(Banco* banco, int id) {
    for (Registro& row : banco->rows) {
        if (row.id == id) {
            return &row;
        }
    }
    return nullptr;
}

static void set_message(Response* response, int ok, const char* message) {
    response->ok = ok;
    std::strncpy(response->message, message, sizeof(response->message) - 1);
    response->message[sizeof(response->message) - 1] = '\0';
}

static Response banco_exec(Banco* banco, const Request* request) {
    Response response{};
    pthread_mutex_lock(&banco->mutex);

    switch (request->operation) {
    case Operation::INSERT: {
        if (banco_find(banco, request->id)) {
            set_message(&response, 0, "id ja existe");
            break;
        }
        Registro row{};
        row.id = request->id;
        copy_nome(row.nome, request->nome);
        banco->rows.push_back(row);
        set_message(&response, 1, "ok");
        break;
    }
    case Operation::DELETE: {
        Registro* row = banco_find(banco, request->id);
        if (!row) {
            set_message(&response, 0, "id nao encontrado");
            break;
        }
        banco->rows.erase(banco->rows.begin() + (row - banco->rows.data()));
        set_message(&response, 1, "ok");
        break;
    }
    case Operation::SELECT: {
        Registro* row = banco_find(banco, request->id);
        if (!row) {
            set_message(&response, 0, "id nao encontrado");
            break;
        }
        response.row = *row;
        set_message(&response, 1, "ok");
        break;
    }
    case Operation::UPDATE: {
        Registro* row = banco_find(banco, request->id);
        if (!row) {
            set_message(&response, 0, "id nao encontrado");
            break;
        }
        copy_nome(row->nome, request->nome);
        response.row = *row;
        set_message(&response, 1, "ok");
        break;
    }
    default:
        set_message(&response, 0, "operacao invalida");
        break;
    }

    pthread_mutex_unlock(&banco->mutex);
    return response;
}

struct Job {
    Request request;
};

struct Pool {
    int thread_count;
    std::vector<pthread_t> threads;
    std::queue<Job> jobs;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    int stop;

    Banco* banco;
    int response_fd;
    pthread_mutex_t io_mutex;
    FILE* log_file;
};

static const char* operation_name(Operation operation) {
    switch (operation) {
    case Operation::INSERT:
        return "INSERT";
    case Operation::DELETE:
        return "DELETE";
    case Operation::SELECT:
        return "SELECT";
    case Operation::UPDATE:
        return "UPDATE";
    }
    return "?";
}

static void log_operation(FILE* log_file, const Request* request, const Response* response) {
    time_t now = time(nullptr);
    struct tm local_time {};
    localtime_r(&now, &local_time);
    char timestamp[32];
    std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &local_time);
    std::fprintf(log_file, "%s %s id=%d nome=%s %s (%s)\n", timestamp,
                 operation_name(request->operation), request->id, request->nome,
                 response->ok ? "OK" : "ERRO", response->message);
    std::fflush(log_file);
}

static void* worker(void* argument) {
    Pool* pool = static_cast<Pool*>(argument);
    while (true) {
        pthread_mutex_lock(&pool->mutex);
        while (pool->jobs.empty() && !pool->stop) {
            pthread_cond_wait(&pool->condition, &pool->mutex);
        }
        if (pool->stop && pool->jobs.empty()) {
            pthread_mutex_unlock(&pool->mutex);
            break;
        }
        Job job = pool->jobs.front();
        pool->jobs.pop();
        pthread_mutex_unlock(&pool->mutex);

        Response response = banco_exec(pool->banco, &job.request);

        pthread_mutex_lock(&pool->io_mutex);
        log_operation(pool->log_file, &job.request, &response);
        ipc_write(pool->response_fd, &response, sizeof(response));
        pthread_mutex_unlock(&pool->io_mutex);
    }
    return nullptr;
}

static int pool_start(Pool* pool, int thread_count, Banco* banco, int response_fd,
                      FILE* log_file) {
    pool->thread_count = thread_count;
    pool->stop = 0;
    pool->banco = banco;
    pool->response_fd = response_fd;
    pool->log_file = log_file;
    pthread_mutex_init(&pool->mutex, nullptr);
    pthread_cond_init(&pool->condition, nullptr);
    pthread_mutex_init(&pool->io_mutex, nullptr);

    pool->threads.resize(static_cast<size_t>(thread_count));
    for (int index = 0; index < thread_count; index++) {
        if (pthread_create(&pool->threads[static_cast<size_t>(index)], nullptr, worker,
                           pool) != 0) {
            pool->thread_count = index;
            return -1;
        }
    }
    return 0;
}

static void pool_submit(Pool* pool, const Request* request) {
    pthread_mutex_lock(&pool->mutex);
    pool->jobs.push(Job{*request});
    pthread_cond_signal(&pool->condition);
    pthread_mutex_unlock(&pool->mutex);
}

static void pool_stop(Pool* pool) {
    pthread_mutex_lock(&pool->mutex);
    pool->stop = 1;
    pthread_cond_broadcast(&pool->condition);
    pthread_mutex_unlock(&pool->mutex);

    for (int index = 0; index < pool->thread_count; index++) {
        pthread_join(pool->threads[static_cast<size_t>(index)], nullptr);
    }

    pthread_mutex_destroy(&pool->mutex);
    pthread_cond_destroy(&pool->condition);
    pthread_mutex_destroy(&pool->io_mutex);
}

int main(int argc, char** argv) {
    int thread_count = 4;
    if (argc >= 2) {
        thread_count = std::atoi(argv[1]);
        if (thread_count < 1) {
            thread_count = 1;
        }
        if (thread_count > 32) {
            thread_count = 32;
        }
    }

    struct sigaction signal_action {};
    signal_action.sa_handler = on_signal;
    sigemptyset(&signal_action.sa_mask);
    sigaction(SIGINT, &signal_action, nullptr);
    sigaction(SIGTERM, &signal_action, nullptr);

    if (ipc_create_fifos() < 0) {
        std::perror("mkfifo");
        return 1;
    }

    IpcChannel channel {};
    if (ipc_open_server(&channel) < 0) {
        std::perror("open fifos");
        return 1;
    }

    FILE* log_file = std::fopen("log.txt", "a");
    if (!log_file) {
        std::perror("log.txt");
        ipc_close(&channel);
        return 1;
    }

    Banco banco;
    banco_init(&banco);

    Pool pool {};
    if (pool_start(&pool, thread_count, &banco, channel.response_fd, log_file) < 0) {
        std::cerr << "falha ao criar threads\n";
        banco_destroy(&banco);
        std::fclose(log_file);
        ipc_close(&channel);
        return 1;
    }

    std::cout << "servidor pronto (" << thread_count << " threads)\n"
              << "pipes: " << IPC_PIPE_REQUEST << " , " << IPC_PIPE_RESPONSE << "\n"
              << "Ctrl+C para encerrar\n";

    while (!stop_requested) {
        struct pollfd poll_descriptor {};
        poll_descriptor.fd = channel.request_fd;
        poll_descriptor.events = POLLIN;
        int poll_result = poll(&poll_descriptor, 1, 200);
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::perror("poll");
            break;
        }
        if (poll_result == 0) {
            continue;
        }

        Request request {};
        if (ipc_read(channel.request_fd, &request, sizeof(request)) < 0) {
            if (stop_requested) {
                break;
            }
            continue;
        }
        pool_submit(&pool, &request);
    }

    std::cout << "encerrando...\n";
    pool_stop(&pool);
    banco_destroy(&banco);
    std::fclose(log_file);
    ipc_close(&channel);
    ipc_remove_fifos();
    return 0;
}
