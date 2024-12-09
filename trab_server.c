#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>

#define BUFFER_SIZE 4096
#define CHUNK_SIZE 128

typedef struct {
    int sock;
    pthread_t tid;
} client_conn_t;

static client_conn_t *clients = NULL;
static int max_clients = 0;
static int current_clients = 0;

static pthread_mutex_t client_lock = PTHREAD_MUTEX_INITIALIZER;

// Limite de taxa (bytes/segundo)
static int RATE_LIMIT = 1024;

// Controle de taxa global
static int bytes_transferred_this_second = 0;
static time_t last_time = 0;
static pthread_mutex_t rate_lock = PTHREAD_MUTEX_INITIALIZER;

// Retorna quantos bytes podem ser transferidos neste segundo por um cliente
static int get_available_bytes() {
    pthread_mutex_lock(&rate_lock);
    time_t now = time(NULL);
    if (now != last_time) {
        last_time = now;
        bytes_transferred_this_second = 0;
    }
    int remaining = RATE_LIMIT - bytes_transferred_this_second;
    pthread_mutex_unlock(&rate_lock);

    pthread_mutex_lock(&client_lock);
    int clients_count = current_clients > 0 ? current_clients : 1;
    pthread_mutex_unlock(&client_lock);

    int per_client_available = remaining / clients_count;
    if (per_client_available < 1) per_client_available = 1;
    return per_client_available;
}

// Atualiza quantos bytes foram transferidos globalmente neste segundo
static void update_bytes_transferred(int bytes) {
    pthread_mutex_lock(&rate_lock);
    bytes_transferred_this_second += bytes;
    pthread_mutex_unlock(&rate_lock);
}

// Aguarda até haver espaço disponível neste segundo para transferir ao menos 1 byte
static void wait_for_available_bytes() {
    while (1) {
        int available = get_available_bytes();
        if (available > 0) return;
        usleep(100000); // Aguarda 100ms antes de tentar de novo para não sobrecarregar o CPU
    }
}

// Função auxiliar para enviar dados respeitando o rate limit (para modo RECV - download)
static ssize_t throttled_send(int sock, const char *buffer, size_t length) {
    wait_for_available_bytes();
    int available = get_available_bytes();
    if ((int)length > available) length = available;
    ssize_t s = send(sock, buffer, length, 0);
    if (s > 0) update_bytes_transferred((int)s);
    return s;
}

// Função auxiliar para receber dados respeitando o rate limit (para modo SEND - upload)
static ssize_t throttled_recv(int sock, char *buffer, size_t length) {
    wait_for_available_bytes();
    int available = get_available_bytes();
    if ((int)length > available) length = available;
    ssize_t r = recv(sock, buffer, length, 0);
    if (r > 0) update_bytes_transferred((int)r);
    return r;
}

void* client_handler(void *arg) {
    int client_sock = *(int*)arg;
    free(arg);

    char header[1024];
    memset(header, 0, sizeof(header));
    if (recv(client_sock, header, sizeof(header)-1, 0) <= 0) {
        close(client_sock);
        pthread_mutex_lock(&client_lock);
        current_clients--;
        pthread_mutex_unlock(&client_lock);
        return NULL;
    }

    char *mode = strtok(header, "|");
    char *offset_str = strtok(NULL, "|");
    char *filepath = strtok(NULL, "|");
    if (!mode || !offset_str || !filepath) {
        send(client_sock, "ERROR:INVALID\n", 14, 0);
        goto finalize;
    }

    off_t offset = atoll(offset_str);

    if (strcmp(mode, "RECV") == 0) {
        // Cliente quer receber arquivo do servidor (download)
        struct stat st;
        if (stat(filepath, &st) < 0) {
            send(client_sock, "ERROR:NOT_FOUND\n", 17, 0);
            goto finalize;
        }

        off_t filesize = st.st_size;
        if (offset > filesize) offset = filesize;

        int fd = open(filepath, O_RDONLY);
        if (fd < 0) {
            send(client_sock, "ERROR:IO\n", 9, 0);
            goto finalize;
        }

        lseek(fd, offset, SEEK_SET);
        char msg[128];
        snprintf(msg, sizeof(msg), "OK|%lld\n", (long long)(filesize - offset));
        send(client_sock, msg, strlen(msg), 0);

        char buffer[BUFFER_SIZE];
        ssize_t bytes_read;
        while ((bytes_read = read(fd, buffer, BUFFER_SIZE)) > 0) {
            ssize_t sent = 0;
            while (sent < bytes_read) {
                ssize_t s = throttled_send(client_sock, buffer + sent, bytes_read - sent);
                if (s <= 0) {
                    // Erro no envio
                    break;
                }
                sent += s;
            }

            if (sent < bytes_read) {
                // Falha ao enviar todo o bloco
                break;
            }
        }

        close(fd);

    } else if (strcmp(mode, "SEND") == 0) {
        // Cliente envia arquivo ao servidor (upload)
        // Usamos um arquivo .part para gravação parcial
        char part_path[1024];
        snprintf(part_path, sizeof(part_path), "%s.part", filepath);

        struct stat part_st;
        off_t part_size = 0;
        if (stat(part_path, &part_st) == 0) {
            part_size = part_st.st_size;
            // Se quisermos, poderíamos verificar coerência part_size vs offset.
        }

        int fd = open(part_path, O_WRONLY | O_CREAT, 0666);
        if (fd < 0) {
            send(client_sock, "ERROR:IO\n", 9, 0);
            goto finalize;
        }

        if (lseek(fd, offset, SEEK_SET) < 0) {
            close(fd);
            send(client_sock, "ERROR:IO\n", 9, 0);
            goto finalize;
        }

        send(client_sock, "OK\n", 3, 0);

        char buffer[BUFFER_SIZE];
        ssize_t r;
        off_t total_received = offset;
        off_t next_chunk = ((total_received / CHUNK_SIZE) + 1) * CHUNK_SIZE;

        for (;;) {
            ssize_t read_bytes = throttled_recv(client_sock, buffer, BUFFER_SIZE);
            if (read_bytes <= 0) break; // Conexão fechada ou erro

            ssize_t written = write(fd, buffer, read_bytes);
            if (written < read_bytes) {
                // Erro de escrita
                break;
            }
            total_received += written;

            // A cada CHUNK_SIZE bytes, garante persistência
            if (total_received >= next_chunk) {
                fsync(fd);
                next_chunk += CHUNK_SIZE;
            }
        }

        fsync(fd);
        close(fd);

        // Renomeia o arquivo .part para o final
        if (rename(part_path, filepath) < 0) {
            send(client_sock, "ERROR:IO\n", 9, 0);
            goto finalize;
        }

    } else {
        send(client_sock, "ERROR:MODE\n", 11, 0);
    }

finalize:
    close(client_sock);
    pthread_mutex_lock(&client_lock);
    current_clients--;
    pthread_mutex_unlock(&client_lock);
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc < 4) {
        fprintf(stderr, "Uso: %s <porta> <max_clientes> <rate_limit>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    int port = atoi(argv[1]);
    max_clients = atoi(argv[2]);
    RATE_LIMIT = atoi(argv[3]);

    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    int optval=1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    struct sockaddr_in serv_addr;
    memset(&serv_addr,0,sizeof(serv_addr));
    serv_addr.sin_family=AF_INET;
    serv_addr.sin_port=htons(port);
    serv_addr.sin_addr.s_addr=INADDR_ANY;

    if (bind(server_sock,(struct sockaddr*)&serv_addr,sizeof(serv_addr))<0) {
        perror("bind");
        exit(EXIT_FAILURE);
    }

    if (listen(server_sock,5)<0) {
        perror("listen");
        exit(EXIT_FAILURE);
    }

    clients = malloc(sizeof(client_conn_t)*max_clients);
    if (!clients) {
        perror("malloc");
        close(server_sock);
        exit(EXIT_FAILURE);
    }
    memset(clients,0,sizeof(client_conn_t)*max_clients);

    printf("Servidor iniciado na porta %d, max clientes: %d, rate: %d bytes/s\n", port, max_clients, RATE_LIMIT);

    while (1) {
        struct sockaddr_in cli_addr;
        socklen_t cli_len = sizeof(cli_addr);
        int client_sock = accept(server_sock,(struct sockaddr*)&cli_addr,&cli_len);
        if (client_sock < 0) {
            perror("accept");
            continue;
        }

        pthread_mutex_lock(&client_lock);
        if (current_clients >= max_clients) {
            send(client_sock, "ERROR:BUSY\n", 11, 0);
            close(client_sock);
            pthread_mutex_unlock(&client_lock);
            continue;
        }
        current_clients++;
        pthread_mutex_unlock(&client_lock);

        int *arg = malloc(sizeof(int));
        if (!arg) {
            perror("malloc");
            close(client_sock);
            pthread_mutex_lock(&client_lock);
            current_clients--;
            pthread_mutex_unlock(&client_lock);
            continue;
        }
        *arg = client_sock;

        pthread_t tid;
        if (pthread_create(&tid,NULL,client_handler,arg)!=0) {
            perror("pthread_create");
            close(client_sock);
            pthread_mutex_lock(&client_lock);
            current_clients--;
            pthread_mutex_unlock(&client_lock);
            free(arg);
        } else {
            pthread_detach(tid);
        }
    }

    close(server_sock);
    free(clients);
    return 0;
}