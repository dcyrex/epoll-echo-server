#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <stdio.h>
#include <fcntl.h>
#include <assert.h>
#include <time.h>
#include <stdatomic.h>
#include <sys/syscall.h>
#include <stdbool.h>
#include <sys/epoll.h>
#include <errno.h>
#include <ctype.h>



const char* log_prefix(const char* func, int line) {
  struct timespec spec; clock_gettime(CLOCK_REALTIME, &spec);
  long long current_msec = spec.tv_sec * 1000L + spec.tv_nsec / 1000000;
  static _Atomic long long start_msec_storage = -1;
  long long start_msec = -1;
  if (atomic_compare_exchange_strong(&start_msec_storage, &start_msec, current_msec))
    start_msec = current_msec;
  long long delta_msec = current_msec - start_msec;
  const int max_func_len = 10;
  static __thread char prefix[100];
  sprintf(prefix, "%lld.%03lld %*s():%d    ", delta_msec / 1000, delta_msec % 1000, max_func_len, func, line);
  sprintf(prefix + max_func_len + 13, "[tid=%ld]", syscall(__NR_gettid));
  return prefix;
}
#define log_printf_impl(fmt, ...) { time_t t = time(0); dprintf(2, "%s: " fmt "%s", log_prefix(__FUNCTION__, __LINE__), __VA_ARGS__); }
// Format: <time_since_start> <func_name>:<line> : <custom_message>
#define log_printf(...) log_printf_impl(__VA_ARGS__, "")
#define SWAP(a, b) { __typeof__(a) c = (a); (a) = (b); (b) = (c); }


extern int errno;

volatile sig_atomic_t must_exit = 0;
volatile sig_atomic_t connections_opened = 0;

int init_localhost_server(char* port_str, int epoll_fd) {
  log_printf("Server started\n");

  int server_fd = socket(AF_INET, SOCK_STREAM, 0);

  fcntl(server_fd, F_SETFL, fcntl(server_fd, F_GETFL) | O_NONBLOCK);

  struct epoll_event in_ev = {.events = EPOLLIN, .data.fd = server_fd};
  epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &in_ev);

  struct sockaddr_in server_addr = {
      .sin_family = AF_INET,
      .sin_port = htons(atoi(port_str)),
      .sin_addr = inet_addr("127.0.0.1")
  };

  int bind_status = bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr));
  assert(bind_status != -1);
  log_printf("Socket is bound\n");

  int listen_status = listen(server_fd, SOMAXCONN);
  assert(listen_status != -1);
  log_printf("Listening started\n");

  return server_fd;
}

int init_epoll() {
  return epoll_create(1337);
}

void handle_sigterm(int signum) {
  log_printf("SIGTERM\n");
  must_exit = 1;
}

int reg_one_client(int server_fd, int epoll_fd) {
  int client_fd = accept(server_fd, NULL, NULL);

  if (client_fd == -1) {
    assert(errno == EAGAIN);
    log_printf("Connections queue is empty\n");
    return client_fd;
  }

  ++connections_opened;

  log_printf("Server accepted connection\n");

  fcntl(client_fd, F_SETFL, fcntl(client_fd, F_GETFL) | O_NONBLOCK);

  struct epoll_event in_ev = {.events = EPOLLIN, .data.fd = client_fd};
  epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &in_ev);
  log_printf("Client %d has been added to epoll\n", client_fd);

  return client_fd;
}

void speak_with_one_client(struct epoll_event event) {
  int client_fd = event.data.fd;

  char buffer[4096];
  int read_bytes = 0;

  log_printf("Read client message...\n");

  if ((read_bytes = read(client_fd, buffer, sizeof(buffer) - 1)) > 0) {
    buffer[read_bytes] = '\0';

    log_printf("Client message successfully readed\n");

    for (int ch = 0; ch < read_bytes; ++ch) {
      buffer[ch] = toupper(buffer[ch]);
    }

    write(client_fd, buffer, read_bytes);
  } else if (read_bytes == 0) {
    --connections_opened;
    shutdown(client_fd, SHUT_RDWR);
    close(client_fd);
    log_printf("~Client:%d\n", client_fd);
  } else {
    log_printf("~Read failed\n");
  }
}

void reg_all_clients_from_queue(int server_fd, int epoll_fd) {
  while (reg_one_client(server_fd, epoll_fd) != -1) {
  }
}

void serve_clients(int server_fd, int epoll_fd) {
  struct epoll_event pending[4096];

  while (must_exit != 1) {
    int total_clients = epoll_wait(epoll_fd, pending, 4096, -1);

    for (int client = 0; client < total_clients; ++client) {
      if (pending[client].data.fd == server_fd) {
        reg_all_clients_from_queue(server_fd, epoll_fd);
      } else if (pending[client].events & EPOLLIN) {
        speak_with_one_client(pending[client]);
      }
    }
  }
}

int main(int argc, char* argv[]) {
  log_printf("Program started\n");

  /* SIGINT and SIGTERM handler */
  struct sigaction SIGTERM_action;
  memset(&SIGTERM_action, 0, sizeof(SIGTERM_action));
  SIGTERM_action.sa_handler = handle_sigterm;
  SIGTERM_action.sa_flags = SA_RESTART;
  sigaction(SIGTERM, &SIGTERM_action, NULL);

  assert(argc == 2);

  int epoll_fd = init_epoll();
  int server_fd = init_localhost_server(argv[1], epoll_fd);

  serve_clients(server_fd, epoll_fd);

  shutdown(epoll_fd, SHUT_RDWR);
  shutdown(server_fd, SHUT_RDWR);

  close(epoll_fd);
  close(server_fd);
} 
