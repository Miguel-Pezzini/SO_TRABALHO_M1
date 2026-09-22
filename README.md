# Banco IPC — Cliente e Servidor (Linux)

Sistema com dois processos: o **servidor** guarda um vetor de registros e atende requisições com um pool de threads; o **cliente** envia comandos por named pipes (`/tmp/db_req` e `/tmp/db_resp`).

## Requisitos

- Linux
- `g++` com C++17
- `make`

## Compilar

Na raiz do repositório:

```bash
make
```

Isso gera `bin/servidor` e `bin/cliente`.

## Rodar

Abra **dois terminais** na pasta do projeto. Suba o servidor primeiro.

**Terminal 1 — servidor**

```bash
./bin/servidor
```

Opcional: número de threads do pool (1 a 32, padrão 4):

```bash
./bin/servidor 8
```

Quando aparecer `servidor pronto`, o processo está ouvindo os pipes.

**Terminal 2 — cliente**

```bash
./bin/cliente
```

O cliente espera o servidor por alguns segundos. Se os pipes não existirem, ele encerra com erro.

Encerrar: `QUIT` no cliente; `Ctrl+C` no servidor.

## Comandos do cliente

```text
INSERT <id> <nome>
DELETE <id>
SELECT <id>
UPDATE <id> <nome>
HELP
QUIT
```

Exemplos:

```text
INSERT 7 Joao
SELECT 7
UPDATE 7 Maria Silva
DELETE 7
```

O `nome` pode ter espaços. `SELECT` e `UPDATE` bem-sucedidos imprimem `id` e `nome`.

## Persistência

O banco fica só em memória (`vector`). Ao encerrar o servidor, os registros somem.

- `log.txt`: histórico de operações (append).

## Limpar

```bash
make clean
```

Remove `bin/`, `log.txt` e os FIFOs `/tmp/db_req` e `/tmp/db_resp`.
