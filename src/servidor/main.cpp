#include "ipc.h"
#include "protocol.h"

#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <iterator>
#include <poll.h>
#include <pthread.h>
#include <queue>
#include <string>
#include <unistd.h>
#include <vector>

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int) {
    g_stop = 1;
}

// ========== Banco ==========

struct Banco {
    std::vector<Registro> rows;
    pthread_mutex_t mutex;
};

static const char* BANCO_PATH = "banco.json";
static const char* BANCO_TMP_PATH = "banco.json.tmp";

static void banco_init(Banco* b) {
    pthread_mutex_init(&b->mutex, nullptr);
}

static void banco_destroy(Banco* b) {
    pthread_mutex_destroy(&b->mutex);
}

static void skip_ws(const std::string& s, size_t* i) {
    while (*i < s.size() && std::isspace(static_cast<unsigned char>(s[*i]))) {
        (*i)++;
    }
}

static std::string json_escape(const char* s) {
    std::string out;
    for (; *s != '\0'; s++) {
        if (*s == '"' || *s == '\\') {
            out += '\\';
            out += *s;
        } else if (*s == '\n') {
            out += "\\n";
        } else {
            out += *s;
        }
    }
    return out;
}

static bool parse_json_string(const std::string& s, size_t* i, std::string* out) {
    skip_ws(s, i);
    if (*i >= s.size() || s[*i] != '"') {
        return false;
    }
    (*i)++;
    out->clear();
    while (*i < s.size()) {
        char c = s[*i];
        if (c == '"') {
            (*i)++;
            return true;
        }
        if (c == '\\' && *i + 1 < s.size()) {
            char n = s[*i + 1];
            if (n == '"' || n == '\\') {
                *out += n;
            } else if (n == 'n') {
                *out += '\n';
            }
            *i += 2;
            continue;
        }
        *out += c;
        (*i)++;
    }
    return false;
}

static bool parse_json_object(const std::string& s, size_t* i, Registro* row) {
    skip_ws(s, i);
    if (*i >= s.size() || s[*i] != '{') {
        return false;
    }
    (*i)++;
    std::memset(row, 0, sizeof(*row));
    bool got_id = false;
    bool got_nome = false;

    while (true) {
        skip_ws(s, i);
        if (*i < s.size() && s[*i] == '}') {
            (*i)++;
            break;
        }

        std::string key;
        if (!parse_json_string(s, i, &key)) {
            return false;
        }
        skip_ws(s, i);
        if (*i >= s.size() || s[*i] != ':') {
            return false;
        }
        (*i)++;
        skip_ws(s, i);

        if (key == "id") {
            char* end = nullptr;
            long v = std::strtol(s.c_str() + *i, &end, 10);
            if (end == s.c_str() + *i) {
                return false;
            }
            *i = static_cast<size_t>(end - s.c_str());
            row->id = static_cast<int>(v);
            got_id = true;
        } else if (key == "nome") {
            std::string nome;
            if (!parse_json_string(s, i, &nome)) {
                return false;
            }
            std::strncpy(row->nome, nome.c_str(), sizeof(row->nome) - 1);
            got_nome = true;
        } else {
            return false;
        }

        skip_ws(s, i);
        if (*i < s.size() && s[*i] == ',') {
            (*i)++;
        }
    }
    return got_id && got_nome;
}

static int banco_save_unlocked(const Banco* b) {
    FILE* fp = std::fopen(BANCO_TMP_PATH, "w");
    if (!fp) {
        return -1;
    }
    std::fprintf(fp, "[\n");
    for (size_t i = 0; i < b->rows.size(); i++) {
        std::fprintf(fp, "  {\"id\": %d, \"nome\": \"%s\"}", b->rows[i].id,
                     json_escape(b->rows[i].nome).c_str());
        if (i + 1 < b->rows.size()) {
            std::fprintf(fp, ",");
        }
        std::fprintf(fp, "\n");
    }
    std::fprintf(fp, "]\n");
    if (std::fclose(fp) != 0) {
        return -1;
    }
    if (std::rename(BANCO_TMP_PATH, BANCO_PATH) != 0) {
        return -1;
    }
    return 0;
}

static int banco_load(Banco* b) {
    std::ifstream in(BANCO_PATH);
    if (!in) {
        return banco_save_unlocked(b);
    }

    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    size_t i = 0;
    skip_ws(content, &i);
    if (i >= content.size() || content[i] != '[') {
        std::cerr << "banco.json invalido, iniciando vazio\n";
        b->rows.clear();
        return banco_save_unlocked(b);
    }
    i++;

    std::vector<Registro> loaded;
    skip_ws(content, &i);
    if (i < content.size() && content[i] == ']') {
        i++;
    } else {
        while (true) {
            Registro row {};
            if (!parse_json_object(content, &i, &row)) {
                std::cerr << "banco.json invalido, iniciando vazio\n";
                b->rows.clear();
                return banco_save_unlocked(b);
            }
            loaded.push_back(row);
            skip_ws(content, &i);
            if (i < content.size() && content[i] == ',') {
                i++;
                continue;
            }
            if (i < content.size() && content[i] == ']') {
                i++;
                break;
            }
            std::cerr << "banco.json invalido, iniciando vazio\n";
            b->rows.clear();
            return banco_save_unlocked(b);
        }
    }

    b->rows = std::move(loaded);
    return 0;
}

static int banco_find(Banco* b, int id) {
    for (size_t i = 0; i < b->rows.size(); i++) {
        if (b->rows[i].id == id) {
            return static_cast<int>(i);
        }
    }
    return -1;
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
        if (banco_find(b, req->id) >= 0) {
            set_msg(&resp, 0, "id ja existe");
            break;
        }
        Registro row{};
        row.id = req->id;
        std::strncpy(row.nome, req->nome, sizeof(row.nome) - 1);
        b->rows.push_back(row);
        if (banco_save_unlocked(b) < 0) {
            std::perror("banco.json");
        }
        set_msg(&resp, 1, "ok");
        break;
    }
    case Op::DELETE: {
        int i = banco_find(b, req->id);
        if (i < 0) {
            set_msg(&resp, 0, "id nao encontrado");
            break;
        }
        b->rows.erase(b->rows.begin() + static_cast<size_t>(i));
        if (banco_save_unlocked(b) < 0) {
            std::perror("banco.json");
        }
        set_msg(&resp, 1, "ok");
        break;
    }
    case Op::SELECT: {
        int i = banco_find(b, req->id);
        if (i < 0) {
            set_msg(&resp, 0, "id nao encontrado");
            break;
        }
        resp.row = b->rows[static_cast<size_t>(i)];
        set_msg(&resp, 1, "ok");
        break;
    }
    case Op::UPDATE: {
        int i = banco_find(b, req->id);
        if (i < 0) {
            set_msg(&resp, 0, "id nao encontrado");
            break;
        }
        std::strncpy(b->rows[static_cast<size_t>(i)].nome, req->nome,
                     sizeof(b->rows[0].nome) - 1);
        b->rows[static_cast<size_t>(i)].nome[sizeof(b->rows[0].nome) - 1] = '\0';
        resp.row = b->rows[static_cast<size_t>(i)];
        if (banco_save_unlocked(b) < 0) {
            std::perror("banco.json");
        }
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

// ========== Pool de threads ==========

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
        ipc_send_response(p->fd_resp, &resp);
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

// ========== Main ==========

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
    if (banco_load(&banco) < 0) {
        std::perror("banco.json");
        banco_destroy(&banco);
        std::fclose(log);
        ipc_close(&ch);
        return 1;
    }

    Pool pool {};
    if (pool_start(&pool, nthreads, &banco, ch.fd_resp, log) < 0) {
        std::cerr << "falha ao criar threads\n";
        banco_destroy(&banco);
        std::fclose(log);
        ipc_close(&ch);
        return 1;
    }

    std::cout << "servidor pronto (" << nthreads << " threads, "
              << banco.rows.size() << " registros em banco.json)\n"
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
        if (ipc_recv_request(ch.fd_req, &req) < 0) {
            if (g_stop) {
                break;
            }
            continue;
        }
        pool_submit(&pool, &req);
    }

    std::cout << "encerrando...\n";
    pool_stop(&pool);
    pthread_mutex_lock(&banco.mutex);
    banco_save_unlocked(&banco);
    pthread_mutex_unlock(&banco.mutex);
    banco_destroy(&banco);
    std::fclose(log);
    ipc_close(&ch);
    ipc_remove_fifos();
    return 0;
}
