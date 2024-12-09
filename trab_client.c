#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>

// A cada 128 bytes atualiza o arquivo .part
#define CHUNK_SIZE 128
#define BUFFER_SIZE 4096

int is_remote_path(const char *path) {
    return strchr(path, ':') != NULL; 
}

int parse_remote_path(const char *path, char *host, size_t host_sz, char *rpath, size_t rpath_sz) {
    const char *colon = strchr(path, ':');
    if (!colon) return -1;
    size_t hlen = colon - path;
    if (hlen >= host_sz) return -1;
    strncpy(host, path, hlen);
    host[hlen] = '\0';

    const char *file_path = colon+1; 
    if (strlen(file_path)+1 > rpath_sz) return -1;
    strcpy(rpath, file_path);
    return 0;
}

#define DEFAULT_PORT 8190

// Função para ler offset do .part no modo SEND
static off_t read_send_part_offset(const char *part_file) {
    int fd = open(part_file, O_RDONLY);
    if (fd < 0) return 0; // sem .part ou erro, consideramos offset 0

    char buf[64];
    ssize_t r = read(fd, buf, sizeof(buf)-1);
    close(fd);
    if (r <= 0) return 0;
    buf[r] = '\0';
    return atoll(buf);
}

// Função para atualizar .part no modo SEND
static void update_send_part_offset(const char *part_file, off_t offset) {
    int fd = open(part_file, O_WRONLY|O_CREAT|O_TRUNC, 0666);
    if (fd>=0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%lld\n", (long long)offset);
        write(fd, buf, strlen(buf));
        close(fd);
    }
}

// Função para remover .part ao terminar envio
static void remove_part_file(const char *part_file) {
    unlink(part_file);
}

int transfer_file(const char *src, const char *dst) {
    int max_retries = 5;
    int retry_count = 0;

try_again:
    if (retry_count > max_retries) {
        fprintf(stderr, "Falhou após várias tentativas.\n");
        return -1;
    }

    int mode_send = 0; 
    int mode_recv = 0; 
    int src_remote = is_remote_path(src);
    int dst_remote = is_remote_path(dst);

    if (src_remote && !dst_remote) {
        mode_recv = 1;
    } else if (!src_remote && dst_remote) {
        mode_send = 1;
    } else {
        fprintf(stderr, "Modo não suportado: é necessário um lado remoto e outro local.\n");
        return -1;
    }

    char host[256], rpath[1024];
    char *local_path;
    if (mode_recv) {
        // src remoto
        if (parse_remote_path(src, host, sizeof(host), rpath, sizeof(rpath)) < 0) {
            fprintf(stderr, "Caminho remoto inválido.\n");
            return -1;
        }
        local_path = (char*)dst;
    } else {
        // mode_send
        if (parse_remote_path(dst, host, sizeof(host), rpath, sizeof(rpath)) < 0) {
            fprintf(stderr, "Caminho remoto inválido.\n");
            return -1;
        }
        local_path = (char*)src;
    }

    // Verificar offset (resume)
    char part_file[1024];
    snprintf(part_file, sizeof(part_file), "%s.part", local_path);

    off_t offset = 0;
    struct stat st;
    if (mode_recv && stat(part_file, &st) == 0) {
        offset = st.st_size;
        fprintf(stdout, "Retomando a partir de %lld bytes no modo RECV\n", (long long)offset);
    } else if (mode_recv) {
        offset = 0;
    }

    // No modo SEND, agora implementamos também a retomada:
    // Vamos supor que mantemos um .part com o offset já enviado.
    if (mode_send) {
        if (stat(local_path, &st) < 0) {
            fprintf(stderr, "Arquivo local não encontrado.\n");
            return -1;
        }
        // Ler offset do .part se existir
        off_t send_offset = read_send_part_offset(part_file);
        if (send_offset > st.st_size) send_offset = st.st_size;
        offset = send_offset;
        if (offset > 0) {
            fprintf(stdout, "Retomando envio a partir de %lld bytes no modo SEND\n", (long long)offset);
        }
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return -1;
    }

    struct sockaddr_in serv_addr;
    memset(&serv_addr,0,sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(DEFAULT_PORT);

    if (inet_pton(AF_INET, host, &serv_addr.sin_addr)<=0) {
        perror("inet_pton");
        close(sock);
        return -1;
    }

    if (connect(sock,(struct sockaddr*)&serv_addr,sizeof(serv_addr))<0) {
        perror("connect");
        close(sock);
        return -1;
    }

    char header[2048];
    if (mode_recv) {
        snprintf(header, sizeof(header), "RECV|%lld|%s", (long long)offset, rpath);
    } else {
        snprintf(header, sizeof(header), "SEND|%lld|%s", (long long)offset, rpath);
    }

    if (send(sock, header, strlen(header),0)<0) {
        perror("send");
        close(sock);
        return -1;
    }

    char response[1024];
    memset(response,0,sizeof(response));
    if (recv(sock,response,sizeof(response)-1,0)<=0) {
        perror("recv");
        close(sock);
        return -1;
    }

    if (strncmp(response,"ERROR",5)==0) {
        if (strstr(response,"BUSY")) {
            fprintf(stderr,"Servidor ocupado. Tentando novamente...\n");
            close(sock);
            sleep(2);
            retry_count++;
            goto try_again;
        }
        fprintf(stderr,"Erro do servidor: %s\n", response);
        close(sock);
        return -1;
    }

    if (mode_recv) {
        // "OK|remaining_size"
        if (strncmp(response,"OK|",3)!=0) {
            fprintf(stderr,"Resposta inesperada do servidor: %s\n",response);
            close(sock);
            return -1;
        }
        char *rem_size_str = response+3;
        long long remaining = atoll(rem_size_str);

        int fd = open(part_file, O_WRONLY|O_CREAT, 0666);
        if (fd<0) {
            perror("open");
            close(sock);
            return -1;
        }
        if (offset>0) {
            lseek(fd, 0, SEEK_END);
        }

        char buf[BUFFER_SIZE];
        ssize_t r;
        off_t total_received = offset;
        off_t next_chunk = ((total_received/CHUNK_SIZE)+1)*CHUNK_SIZE;

        while (total_received < offset+remaining) {
            r = recv(sock, buf, BUFFER_SIZE,0);
            if (r<=0) {
                break;
            }
            write(fd, buf, r);
            total_received += r;

            if (total_received>=next_chunk) {
                fsync(fd);
                next_chunk += CHUNK_SIZE;
            }
        }

        close(fd);
        close(sock);

        if (total_received == offset+remaining) {
            rename(part_file, local_path);
            printf("Transferência concluída: %s\n", local_path);
        } else {
            printf("Transferência interrompida. Parcial salva em %s\n", part_file);
        }

    } else {
        // SEND mode
        // "OK\n"
        if (strncmp(response,"OK",2)!=0) {
            fprintf(stderr,"Resposta inesperada do servidor: %s\n",response);
            close(sock);
            return -1;
        }

        int fd = open(local_path,O_RDONLY);
        if (fd<0) {
            perror("open local file");
            close(sock);
            return -1;
        }

        lseek(fd, offset, SEEK_SET);

        off_t total_sent = offset;
        off_t next_chunk = ((total_sent/CHUNK_SIZE)+1)*CHUNK_SIZE;

        char buf[BUFFER_SIZE];
        ssize_t rd;
        int erro_envio = 0;
        while ((rd=read(fd,buf,BUFFER_SIZE))>0) {
            ssize_t sent=0;
            while (sent<rd) {
                ssize_t s=send(sock,buf+sent,rd-sent,0);
                if (s<=0) {
                    erro_envio = 1;
                    break;
                }
                sent+=s;
                total_sent += s;

                if (total_sent>=next_chunk) {
                    // Atualiza o .part do modo SEND
                    update_send_part_offset(part_file, total_sent);
                    next_chunk += CHUNK_SIZE;
                }
            }
            if (sent<rd) {
                // erro no envio
                erro_envio = 1;
                break;
            }
        }

        close(fd);
        close(sock);

        if (!erro_envio && total_sent == st.st_size) {
            // Transferência completa
            printf("Envio concluído.\n");
            // Remover .part pois não precisamos mais dele
            remove_part_file(part_file);
        } else {
            // Parcial salvo no .part (offset escrito)
            printf("Envio interrompido. Parcialmente enviado: %lld bytes.\n", (long long)total_sent);
        }
    }

    return 0;
}


int main(int argc, char *argv[]) {
    if (argc<3) {
        fprintf(stderr,"Uso: %s origem destino\n",argv[0]);
        exit(EXIT_FAILURE);
    }

    const char *src = argv[1];
    const char *dst = argv[2];

    if (transfer_file(src,dst)<0) {
        fprintf(stderr,"Falha na transferência.\n");
        return 1;
    }

    return 0;
}