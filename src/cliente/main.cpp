#include "ipc.h"
#include "protocol.h"

#include <cctype>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>

static std::string trim(const std::string& text) {
    size_t start = 0;
    while (start < text.size() &&
           std::isspace(static_cast<unsigned char>(text[start]))) {
        start++;
    }
    size_t end = text.size();
    while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        end--;
    }
    return text.substr(start, end - start);
}

static std::string upper(std::string text) {
    for (char& character : text) {
        character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
    }
    return text;
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
static int parse_line(const std::string& line, Request* request) {
    std::istringstream input_stream(line);
    std::string command;
    if (!(input_stream >> command)) {
        return -1;
    }
    command = upper(command);

    if (command == "QUIT" || command == "EXIT") {
        return 1;
    }
    if (command == "HELP") {
        return 2;
    }

    Operation operation;
    bool needs_nome = false;
    if (command == "INSERT") {
        operation = Operation::INSERT;
        needs_nome = true;
    } else if (command == "DELETE") {
        operation = Operation::DELETE;
    } else if (command == "SELECT") {
        operation = Operation::SELECT;
    } else if (command == "UPDATE") {
        operation = Operation::UPDATE;
        needs_nome = true;
    } else {
        return -1;
    }

    std::memset(request, 0, sizeof(*request));
    request->operation = operation;
    if (!(input_stream >> request->id)) {
        return -1;
    }
    if (needs_nome) {
        std::string nome;
        std::getline(input_stream, nome);
        nome = trim(nome);
        if (nome.empty()) {
            return -1;
        }
        std::strncpy(request->nome, nome.c_str(), sizeof(request->nome) - 1);
    }
    return 0;
}

static void print_response(const Request& request, const Response& response) {
    if (!response.ok) {
        std::cout << "erro: " << response.message << "\n";
        return;
    }
    if (request.operation == Operation::SELECT || request.operation == Operation::UPDATE) {
        std::cout << response.row.id << " " << response.row.nome << "\n";
        return;
    }
    std::cout << response.message << "\n";
}

int main() {
    std::cout << "aguardando servidor...\n";
    IpcChannel channel {};
    if (ipc_open_client(&channel) < 0) {
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

        Request request {};
        int kind = parse_line(line, &request);
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

        if (ipc_write(channel.request_fd, &request, sizeof(request)) < 0) {
            std::cerr << "falha ao enviar. o servidor caiu?\n";
            break;
        }

        Response response {};
        if (ipc_read(channel.response_fd, &response, sizeof(response)) < 0) {
            std::cerr << "falha ao receber resposta.\n";
            break;
        }
        print_response(request, response);
    }

    ipc_close(&channel);
    return 0;
}
