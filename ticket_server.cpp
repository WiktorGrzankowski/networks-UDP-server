#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <fstream>
#include <iostream>
#include <vector>

#include "err.hpp"

#define BUFFER_SIZE 10

char shared_buffer[BUFFER_SIZE];

typedef struct eventS event;

struct eventS {
    std::string description;
    uint16_t tickets_available;
};

uint16_t read_port(char *string) {
    errno = 0;
    unsigned long port = strtoul(string, NULL, 10);
    PRINT_ERRNO();
    if (port > UINT16_MAX) {
        fatal("%ul is not a valid port number", port);
    }

    return (uint16_t)port;
}

int bind_socket(uint16_t port) {
    int socket_fd = socket(AF_INET, SOCK_DGRAM, 0);  // creating IPv4 UDP socket
    ENSURE(socket_fd > 0);
    // after socket() call; we should close(sock) on any execution path;

    struct sockaddr_in server_address;
    server_address.sin_family = AF_INET;  // IPv4
    server_address.sin_addr.s_addr =
        htonl(INADDR_ANY);  // listening on all interfaces
    server_address.sin_port = htons(port);

    // bind the socket to a concrete address
    CHECK_ERRNO(bind(socket_fd, (struct sockaddr *)&server_address,
                     (socklen_t)sizeof(server_address)));

    return socket_fd;
}

size_t read_message(int socket_fd, struct sockaddr_in *client_address,
                    char *buffer, size_t max_length) {
    socklen_t address_length = (socklen_t)sizeof(*client_address);
    int flags = 0;  // we do not request anything special
    errno = 0;
    ssize_t len = recvfrom(socket_fd, buffer, max_length, flags,
                           (struct sockaddr *)client_address, &address_length);
    if (len < 0) {
        PRINT_ERRNO();
    }
    return (size_t)len;
}

void send_message(int socket_fd, const struct sockaddr_in *client_address,
                  const char *message, size_t length) {
    socklen_t address_length = (socklen_t)sizeof(*client_address);
    int flags = 0;
    ssize_t sent_length =
        sendto(socket_fd, message, length, flags,
               (struct sockaddr *)client_address, address_length);
    ENSURE(sent_length == (ssize_t)length);
}

void check_port_num(char *port_c) {
    std::string port_string = std::string(port_c);
    for (char &c : port_string) {
        if (!isdigit(c)) fatal("Wrong port number provided");
    }
    int value = atoi(port_c);
    if (value < 1024 || value > 65535) fatal("Wrong port number provided");
}

void check_timeout_value(char *timeout_c) {
    std::string timeout_string = std::string(timeout_c);
    for (char &c : timeout_string) {
        if (!isdigit(c)) fatal("Wrong timeout value provided");
    }
    int value = atoi(timeout_c);
    if (value < 1 || value > 86400) fatal("Wrong timeout value provided");
}

void read_input(int argc, char *argv[], uint16_t *port_num, uint16_t *timeout,
                char **filename) {
    if (argc < 3) {
        fatal("Not enough arguments provided");
    }
    bool filename_found = false, port_num_found = false, timeout_found = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = std::string(argv[i]);

        if (arg == "-f" && !filename_found) {
            *filename = argv[++i];
            if (access(*filename, F_OK) != 0) fatal("Wrong filename provided");
            filename_found = true;
        } else if (arg == "-p" && !port_num_found) {
            if (argc < i + 2) fatal("Not enough parameters provided");
            check_port_num(argv[++i]);
            *port_num = atoi(argv[i]);
            port_num_found = true;
        } else if (arg == "-t" && !timeout_found) {
            if (argc < i + 2) fatal("Not enough parameters provided");
            check_timeout_value(argv[++i]);
            *timeout = atoi(argv[i]);
            timeout_found = true;
        } else {
            // error
            fatal("Wrong argument provided");
        }
    }
}

std::vector<event> read_events(char *filename) {
    FILE *fp;
    fp = fopen(filename, "r");
    std::ifstream file(filename);
    std::vector<event> events;
    if (file.is_open()) {
        std::string line;
        while (std::getline(file, line)) {
            std::string description = line;
            std::getline(file, line);
            uint16_t tickets_available = stoi(line);
            events.push_back(event({description, tickets_available}));
        }
        file.close();
    }
    fclose(fp);
    return events;
}

/*
 * argv[1] - -f file -  nazwa pliku z opisem wydarzeń poprzedzona opcjonalnie
 * ścieżką wskazującą, gdzie szukać tego pliku, obowiązkowy; argv[2] - -p port –
 * port, na którym nasłuchuje, opcjonalny, domyślnie 2022; argv[3] - -t timeout
 * – limit czasu w sekundach, opcjonalny, wartość z zakresu od 1 do 86400,
 * domyślnie 5.
 */
int main(int argc, char *argv[]) {
    uint16_t port_num = 2022;
    uint16_t timeout = 5;
    char *filename;
    read_input(argc, argv, &port_num, &timeout, &filename);
    std::cout << filename << " " << port_num << " " << timeout << '\n';

    std::vector<event> events = read_events(filename);
    for (auto &ev : events) {
        std::cout << ev.description << " " << ev.tickets_available << '\n';
    }

    return 0;

    // uint16_t port = read_port(argv[1]);
    // printf("Listening on port %u\n", port);

    // memset(shared_buffer, 0, sizeof(shared_buffer));

    // int socket_fd = bind_socket(port);

    // struct sockaddr_in client_address;
    // size_t read_length;
    // do {
    //     read_length = read_message(socket_fd, &client_address, shared_buffer,
    //     sizeof(shared_buffer)); char* client_ip =
    //     inet_ntoa(client_address.sin_addr); uint16_t client_port =
    //     ntohs(client_address.sin_port); printf("received %zd bytes from
    //     client %s:%u: '%.*s'\n", read_length, client_ip, client_port,
    //            (int) read_length, shared_buffer); // note: we specify the
    //            length of the printed string
    //     send_message(socket_fd, &client_address, shared_buffer, read_length);

    // } while (read_length > 0);
    // printf("finished exchange\n");

    // CHECK_ERRNO(close(socket_fd));

    // return 0;
}
