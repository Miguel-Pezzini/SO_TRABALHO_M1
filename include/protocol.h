#ifndef PROTOCOL_H
#define PROTOCOL_H

enum class Operation : int {
    INSERT = 1,
    DELETE = 2,
    SELECT = 3,
    UPDATE = 4
};

struct Registro {
    int id;
    char nome[50];
};

struct Request {
    Operation operation;
    int id;
    char nome[50];
};

struct Response {
    int ok;
    char message[128];
    Registro row;
};

#endif
