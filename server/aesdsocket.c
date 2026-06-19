#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <syslog.h>
#include <signal.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <sys/stat.h>

#define PORT 9000

#define DATA_FILE "/var/tmp/aesdsocketdata"

static int server_fd;

void signal_handler(int sig)
{
    syslog(LOG_INFO, "Caught signal, exiting");

    if (server_fd > 0)
        close(server_fd);

    remove("/var/tmp/aesdsocketdata");
    closelog();
    exit(0);
}



int main(int argc, char *argv[]){
int daemon_mode = 0;

 //printf("START\n"); //debug

if (argc == 2 && strcmp(argv[1], "-d") == 0)
{
    daemon_mode = 1;
}


    int client_fd;
    struct sockaddr_in addr;
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    openlog("aesdsocket", LOG_PID, LOG_USER);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

if (daemon_mode)
{
    pid_t pid = fork();

    if (pid < 0)
        return -1;

    if (pid > 0)
        exit(0);   // parent exits

    if (setsid() < 0)
        return -1;

    chdir("/");
    umask(0);

    // redirect stdio
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
}



    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) return -1;
        //printf("SOCKET OK\n");//debug

    int opt = 1;
    
setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0){
    //perror("bind"); //debug
    return -1;
}
    
//printf("BIND OK\n"); //debug


    if (listen(server_fd, 5) < 0)
    {
    //perror("listen");
    return -1;
}

	//printf("LISTEN OK\n"); //debug

    while (1)
{
    client_fd = accept(server_fd,
                       (struct sockaddr*)&client_addr,
                       &client_len);

    if (client_fd < 0)
        continue;

    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));

    syslog(LOG_INFO, "Accepted connection from %s", ip);

   char buffer[1024];
int bytes;
    FILE *fp;

fp = fopen(DATA_FILE, "a");
if (!fp)
{
    syslog(LOG_ERR, "Failed to open data file");
    close(client_fd);
    continue;
}

while ((bytes = recv(client_fd, buffer, sizeof(buffer), 0)) > 0)
{
    fwrite(buffer, 1, bytes, fp);

    if (memchr(buffer, '\n', bytes) != NULL)
    {
        break;
    }
}

fflush(fp);
fclose(fp);

    fp = fopen(DATA_FILE, "r");
    if (fp)
    {
        while ((bytes = fread(buffer, 1, sizeof(buffer), fp)) > 0)
        {
            send(client_fd, buffer, bytes, 0);
        }
        fclose(fp);
    }

    syslog(LOG_INFO, "Closed connection from %s", ip);
    close(client_fd);
}
    


}
