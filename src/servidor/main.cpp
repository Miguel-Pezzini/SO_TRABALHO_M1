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

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int) {
    g_stop = 1;
}

struct Banco {
    std::vector<Registro> rows;
    pthread_mutex_t mutex;
};

static void banco_init(Banco* b) {
    pthread_mutex_init(&b->mutex, nullptr);
}

static void banco_destroy(Banco* b) {
    pthread_mutex_destroy(&b->mutex);
}

static void copy_nome(char* dst, const char* src) {
    std::strncpy(dst, src, sizeof(Registro::nome) - 1);
    dst[sizeof(Registro::nome) - 1] = '\0';
}

static Registro* banco_find(Banco* b, int id) {
    for (Registro& row : b->rows) {
        if (row.id == id) {
            return &row;
        }
    }
    return nullptr;
}

static void set_msg(Response* r, int ok, const char* msg) {
    r->ok = ok;
    std::strncpy(r->msg, msg, sizeof(r->msg) - 1);
    r->msg[sizeof(r->msg) - 1] = '\0';
}

static Response banco_exec(Banco* b, const Request* req) {
    Response resp{};
    pthread_mutex_lock(&b->mutex);

    switch (req->op) {
    case Op::INSERT: {
        if (banco_find(b, req->id)) {
            set_msg(&resp, 0, "id ja existe");
            break;
        }
        Registro row{};
        row.id = req->id;
        copy_nome(row.nome, req->nome);
        b->rows.push_back(row);
        set_msg(&resp, 1, "ok");
        break;
    }
    case Op::DELETE: {
        Registro* row = banco_find(b, req->id);
        if (!row) {
            set_msg(&resp, 0, "id nao encontrado");
            break;
        }
        b->rows.erase(b->rows.begin() + (row - b->rows.data()));
        set_msg(&resp, 1, "ok");
        break;
    }
    case Op::SELECT: {
        Registro* row = banco_find(b, req->id);
        if (!row) {
            set_msg(&resp, 0, "id nao encontrado");
            break;
        }
        resp.row = *row;
        set_msg(&resp, 1, "ok");
        break;
    }
    case Op::UPDATE: {
        Registro* row = banco_find(b, req->id);
        if (!row) {
            set_msg(&resp, 0, "id nao encontrado");
            break;
        }
        copy_nome(row->nome, req->nome);
        resp.row = *row;
        set_msg(&resp, 1, "ok");
        break;
    }
    default:
        set_msg(&resp, 0, "operacao invalida");
        break;
    }

    pthread_mutex_unlock(&b->mutex);
    return resp;
}

struct Job {
    Request req;
};

struct Pool {
    int nthreads;
    std::vector<pthread_t> threads;
    std::queue<Job> jobs;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int stop;

    Banco* banco;
    int fd_resp;
    pthread_mutex_t io_mutex;
    FILE* log;
};

static const char* op_nome(Op op) {
    switch (op) {
    case Op::INSERT:
        return "INSERT";
    case Op::DELETE:
        return "DELETE";
    case Op::SELECT:
        return "SELECT";
    case Op::UPDATE:
        return "UPDATE";
    }
    return "?";
}

static void log_op(FILE* fp, const Request* req, const Response* resp) {
    time_t now = time(nullptr);
    struct tm tmv {};
    localtime_r(&now, &tmv);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);
    std::fprintf(fp, "%s %s id=%d nome=%s %s (%s)\n", ts, op_nome(req->op),
                 req->id, req->nome, resp->ok ? "OK" : "ERRO", resp->msg);
    std::fflush(fp);
}

static void* worker_fn(void* arg) {
    Pool* p = static_cast<Pool*>(arg);
    while (true) {
        pthread_mutex_lock(&p->mutex);
        while (p->jobs.empty() && !p->stop) {
            pthread_cond_wait(&p->cond, &p->mutex);
        }
        if (p->stop && p->jobs.empty()) {
            pthread_mutex_unlock(&p->mutex);
            break;
        }
        Job job = p->jobs.front();
        p->jobs.pop();
        pthread_mutex_unlock(&p->mutex);

        Response resp = banco_exec(p->banco, &job.req);

        pthread_mutex_lock(&p->io_mutex);
        log_op(p->log, &job.req, &resp);
        ipc_write(p->fd_resp, &resp, sizeof(resp));
        pthread_mutex_unlock(&p->io_mutex);
    }
    return nullptr;
}

static int pool_start(Pool* p, int nthreads, Banco* banco, int fd_resp, FILE* log) {
    p->nthreads = nthreads;
    p->stop = 0;
    p->banco = banco;
    p->fd_resp = fd_resp;
    p->log = log;
    pthread_mutex_init(&p->mutex, nullptr);
    pthread_cond_init(&p->cond, nullptr);
    pthread_mutex_init(&p->io_mutex, nullptr);

    p->threads.resize(static_cast<size_t>(nthreads));
    for (int i = 0; i < nthreads; i++) {
        if (pthread_create(&p->threads[static_cast<size_t>(i)], nullptr, worker_fn, p) !=
            0) {
            p->nthreads = i;
            return -1;
        }
    }
    return 0;
}

static void pool_submit(Pool* p, const Request* req) {
    pthread_mutex_lock(&p->mutex);
    p->jobs.push(Job{*req});
    pthread_cond_signal(&p->cond);
    pthread_mutex_unlock(&p->mutex);
}

static void pool_stop(Pool* p) {
    pthread_mutex_lock(&p->mutex);
    p->stop = 1;
    pthread_cond_broadcast(&p->cond);
    pthread_mutex_unlock(&p->mutex);

    for (int i = 0; i < p->nthreads; i++) {
        pthread_join(p->threads[static_cast<size_t>(i)], nullptr);
    }

    pthread_mutex_destroy(&p->mutex);
    pthread_cond_destroy(&p->cond);
    pthread_mutex_destroy(&p->io_mutex);
}

int main(int argc, char** argv) {
    int nthreads = 4;
    if (argc >= 2) {
        nthreads = std::atoi(argv[1]);
        if (nthreads < 1) {
            nthreads = 1;
        }
        if (nthreads > 32) {
            nthreads = 32;
        }
    }

    struct sigaction sa {};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    if (ipc_create_fifos() < 0) {
        std::perror("mkfifo");
        return 1;
    }

    IpcChannel ch {};
    if (ipc_open_server(&ch) < 0) {
        std::perror("open fifos");
        return 1;
    }

    FILE* log = std::fopen("log.txt", "a");
    if (!log) {
        std::perror("log.txt");
        ipc_close(&ch);
        return 1;
    }

    Banco banco;
    banco_init(&banco);

    Pool pool {};
    if (pool_start(&pool, nthreads, &banco, ch.fd_resp, log) < 0) {
        std::cerr << "falha ao criar threads\n";
        banco_destroy(&banco);
        std::fclose(log);
        ipc_close(&ch);
        return 1;
    }

    std::cout << "servidor pronto (" << nthreads << " threads)\n"
              << "pipes: " << IPC_PIPE_REQ << " , " << IPC_PIPE_RESP << "\n"
              << "Ctrl+C para encerrar\n";

    while (!g_stop) {
        struct pollfd pfd {};
        pfd.fd = ch.fd_req;
        pfd.events = POLLIN;
        int pr = poll(&pfd, 1, 200);
        if (pr < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::perror("poll");
            break;
        }
        if (pr == 0) {
            continue;
        }

        Request req {};
        if (ipc_read(ch.fd_req, &req, sizeof(req)) < 0) {
            if (g_stop) {
                break;
            }
            continue;
        }
        pool_submit(&pool, &req);
    }

    std::cout << "encerrando...\n";
    pool_stop(&pool);
    banco_destroy(&banco);
    std::fclose(log);
    ipc_close(&ch);
    ipc_remove_fifos();
    return 0;
}
