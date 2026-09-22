#include "ipc.h"
#include "protocol.h"

#include <cctype>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>

static std::string trim(const std::string& s) {
    size_t a = 0;
    while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) {
        a++;
    }
    size_t b = s.size();
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) {
        b--;
    }
    return s.substr(a, b - a);
}

static std::string upper(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return s;
}

static void print_help() {
    std::cout << "comandos:\n"
              << "  INSERT <id> <nome>\n"
              << "  DELETE <id>\n"
              << "  SELECT <id>\n"
              << "  UPDATE <id> <nome>\n"
              << "  QUIT\n";
}

// 0 = request, 1 = quit, 2 = help, -1 = erro de parse
static int parse_line(const std::string& line, Request* req) {
    std::istringstream iss(line);
    std::string cmd;
    if (!(iss >> cmd)) {
        return -1;
    }
    cmd = upper(cmd);

    if (cmd == "QUIT" || cmd == "EXIT") {
        return 1;
    }
    if (cmd == "HELP") {
        return 2;
    }

    Op op;
    bool needs_nome = false;
    if (cmd == "INSERT") {
        op = Op::INSERT;
        needs_nome = true;
    } else if (cmd == "DELETE") {
        op = Op::DELETE;
    } else if (cmd == "SELECT") {
        op = Op::SELECT;
    } else if (cmd == "UPDATE") {
        op = Op::UPDATE;
        needs_nome = true;
    } else {
        return -1;
    }

    std::memset(req, 0, sizeof(*req));
    req->op = op;
    if (!(iss >> req->id)) {
        return -1;
    }
    if (needs_nome) {
        std::string nome;
        std::getline(iss, nome);
        nome = trim(nome);
        if (nome.empty()) {
            return -1;
        }
        std::strncpy(req->nome, nome.c_str(), sizeof(req->nome) - 1);
    }
    return 0;
}

static void print_response(const Request& req, const Response& resp) {
    if (!resp.ok) {
        std::cout << "erro: " << resp.msg << "\n";
        return;
    }
    if (req.op == Op::SELECT || req.op == Op::UPDATE) {
        std::cout << resp.row.id << " " << resp.row.nome << "\n";
        return;
    }
    std::cout << resp.msg << "\n";
}

int main() {
    std::cout << "aguardando servidor...\n";
    IpcChannel ch {};
    if (ipc_open_client(&ch) < 0) {
        std::cerr << "nao foi possivel abrir os pipes. o servidor esta rodando?\n";
        return 1;
    }

    std::cout << "conectado. digite HELP para os comandos.\n";

    std::string line;
    while (std::cout << "> " && std::getline(std::cin, line)) {
        line = trim(line);
        if (line.empty()) {
            continue;
        }

        Request req {};
        int kind = parse_line(line, &req);
        if (kind == 1) {
            break;
        }
        if (kind == 2) {
            print_help();
            continue;
        }
        if (kind < 0) {
            std::cout << "comando invalido. digite HELP.\n";
            continue;
        }

        if (ipc_write(ch.fd_req, &req, sizeof(req)) < 0) {
            std::cerr << "falha ao enviar. o servidor caiu?\n";
            break;
        }

        Response resp {};
        if (ipc_read(ch.fd_resp, &resp, sizeof(resp)) < 0) {
            std::cerr << "falha ao receber resposta.\n";
            break;
        }
        print_response(req, resp);
    }

    ipc_close(&ch);
    return 0;
}
