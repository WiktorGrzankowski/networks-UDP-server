#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdarg.h>

#include <fstream>
#include <iostream>
#include <vector>
#include <map>
#include <ctime>
#include <random>
#include <queue>

#define BUFFER_SIZE 80
#define MAX_UDP_DATAGRAM_SIZE 65507
#define MAX_PORT_NUM 65535
#define MIN_PORT_NUM 0
#define MAX_TIMEOUT_VALUE 86400
#define COOKIE_LENGTH 48
#define MAX_BYTE_VALUE 255
#define BAD_REQUEST 255
#define MIN_RESERVATION_ID 1000000
#define MAX_EVENT_ID 999999
#define TICKET_LENGTH 7
#define MAX_TICKETS_PER_RESERVATION 9357

// Evaluate `x`: if non-zero, describe it as a standard error code and exit with an error.
#define CHECK(x)                                                          \
    do {                                                                  \
        int err = (x);                                                    \
        if (err != 0) {                                                   \
            fprintf(stderr, "Error: %s returned %d in %s at %s:%d\n%s\n", \
                #x, err, __func__, __FILE__, __LINE__, strerror(err));    \
            exit(EXIT_FAILURE);                                           \
        }                                                                 \
    } while (0)

// Evaluate `x`: if false, print an error message and exit with an error.
#define ENSURE(x)                                                         \
    do {                                                                  \
        bool result = (x);                                                \
        if (!result) {                                                    \
            fprintf(stderr, "Error: %s was false in %s at %s:%d\n",       \
                #x, __func__, __FILE__, __LINE__);                        \
            exit(EXIT_FAILURE);                                           \
        }                                                                 \
    } while (0)

// Check if errno is non-zero, and if so, print an error message and exit with an error.
#define PRINT_ERRNO()                                                  \
    do {                                                               \
        if (errno != 0) {                                              \
            fprintf(stderr, "Error: errno %d in %s at %s:%d\n%s\n",    \
              errno, __func__, __FILE__, __LINE__, strerror(errno));   \
            exit(EXIT_FAILURE);                                        \
        }                                                              \
    } while (0)


// Set `errno` to 0 and evaluate `x`. If `errno` changed, describe it and exit.
#define CHECK_ERRNO(x)                                                             \
    do {                                                                           \
        errno = 0;                                                                 \
        (void) (x);                                                                \
        PRINT_ERRNO();                                                             \
    } while (0)

// Print an error message and exit with an error.
void fatal(const char *fmt, ...) {
    va_list fmt_args;

    fprintf(stderr, "Error: ");
    va_start(fmt_args, fmt);
    vfprintf(stderr, fmt, fmt_args);
    va_end(fmt_args);
    fprintf(stderr, "\n");
    exit(EXIT_FAILURE);
}

char shared_buffer[BUFFER_SIZE];

typedef struct reservationStruct Reservation;

typedef struct eventStruct Event;

struct reservationStruct {
    uint32_t reservation_id; // little endian
    uint32_t event_id; // little endian
    uint16_t ticket_count; // litlle endian
    std::string cookie; // 48 bytes
    uint64_t expiration_time;
    std::vector<std::string> tickets;
    bool tickets_received;
};

struct eventStruct {
    uint32_t event_id; // little endian
    char description_length;
    std::string description;
    uint16_t tickets_available; // litle endian
    std::queue<Reservation> unreceived_reservations;
};

/*
 * Key: event_id in little endian
 * Value: event struct
*/
using EventsMap = std::map<uint32_t, Event>;

/*
 * Key: reservation_id in little endian
 * Value: reservation struct
*/
using ReservationsMap = std::map<uint32_t, Reservation>;

// Calculate event or reservation id from message stored in shared_buffer.
uint32_t calc_id() {
    uint32_t potential_id = 0;
    uint32_t multiplier = 1;
    for (int i = 4; i >= 1; --i) {
        potential_id += multiplier * ((int) ((shared_buffer[i] + (MAX_BYTE_VALUE + 1)) % (MAX_BYTE_VALUE + 1)));
        multiplier *= (MAX_BYTE_VALUE + 1);
    }
    return potential_id;
}

// Calculate ticket count from message stored in shared_buffer.
uint16_t calc_ticket_count() {
    return (uint16_t)(((uint16_t) shared_buffer[5]) * (MAX_BYTE_VALUE + 1)) + 
            (uint16_t((shared_buffer[6] + MAX_BYTE_VALUE + 1) % (MAX_BYTE_VALUE + 1)));
}

bool message_is_get_events(size_t read_length) {
    return shared_buffer[0] == 1 && read_length == 1;
}

bool message_is_get_reservation(size_t read_length) {
    return shared_buffer[0] == 3 && read_length == 7;
}

bool message_is_get_tickets(size_t read_length) {
    return shared_buffer[0] == 5 && read_length == 53;
}

bool tickets_expired(ReservationsMap &reservations) {
    uint32_t reservation_id = calc_id(); // reading from shared_buffer
    uint64_t current_time = time(0);
    if (!reservations[reservation_id].tickets_received &&
        reservations[reservation_id].expiration_time <= current_time) 
            return true;
    
    return false;
}

// Check whether cookie from message stored in shared_buffer
// is the same as cookie of a given reservation.
bool cookies_match(std::string real_cookie) {
    for (size_t i = 0; i < real_cookie.length(); ++i) {
        if (real_cookie[i] != shared_buffer[i + 5]) {
            return false;
        }
    }
    return true;
}

// Check reservations for a given event starting from the oldest one
// and update event info if time to receive tickets expired.
void update_reservations_for_event(EventsMap &events, uint32_t event_id, ReservationsMap &reservations) {
    uint64_t current_time = time(0);
    while (events[event_id].unreceived_reservations.size() > 0) {
        if (reservations[events[event_id].unreceived_reservations.front().reservation_id].tickets_received) {
            events[event_id].unreceived_reservations.pop();
        } else if (events[event_id].unreceived_reservations.front().expiration_time <= current_time) {
            events[event_id].tickets_available += events[event_id].unreceived_reservations.front().ticket_count;
            events[event_id].unreceived_reservations.pop();
        } else {
            break;
        }
    }
}

bool tickets_arguments_are_correct(ReservationsMap &reservations) {
    uint32_t potential_reservation_id = calc_id(); 

    return potential_reservation_id >= MIN_RESERVATION_ID && 
            reservations.find(potential_reservation_id) != reservations.end() &&
            cookies_match(reservations[potential_reservation_id].cookie) &&
            (MAX_UDP_DATAGRAM_SIZE - 7) > TICKET_LENGTH * reservations[potential_reservation_id].ticket_count;
}

bool reservation_arguments_are_correct(EventsMap &events, ReservationsMap &reservations) {
    uint32_t potential_event_id = calc_id();
   
    if (potential_event_id > MAX_EVENT_ID || events.find(potential_event_id) == events.end())
        return false;
    
    update_reservations_for_event(events, potential_event_id, reservations);

    uint16_t potential_ticket_count = calc_ticket_count();
    
    return potential_ticket_count > 0 && events[potential_event_id].tickets_available >= potential_ticket_count
            && potential_ticket_count <= MAX_TICKETS_PER_RESERVATION;
}

std::string generate_cookie() {
    const int range_from  = 33;
    const int range_to    = 126;
    std::random_device                  rand_dev;
    std::mt19937                        generator(rand_dev());
    std::uniform_int_distribution<int>  distr(range_from, range_to);
    std::string cookie(COOKIE_LENGTH, '\0');
    for (int i = 0; i < COOKIE_LENGTH; ++i) 
        cookie[i] = char(distr(generator));
    
    return cookie;
}

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
                    
    if (len < 0) 
        PRINT_ERRNO();
    
    return (size_t)len;
}

// Function generates unique tickets.
// Before running out of combinations to create a unique ticket
// all possible combinations will be used ealier.
std::string generate_ticket(char *nextTicketNumber) {
    const int letters_from  = 65;
    const int letters_to    = 90;
    const int digits_from   = 48;
    const int digits_to     = 57;
    std::string ticket(TICKET_LENGTH, '\0');
    int finished_changes = 0;
    if (nextTicketNumber[finished_changes] < digits_to) {
        // still more digits to increment
        nextTicketNumber[finished_changes]++;
    }  else if (nextTicketNumber[finished_changes] == digits_to) {
        // last digit, change to first letter
        nextTicketNumber[finished_changes] = letters_from;
    } else if (nextTicketNumber[finished_changes] < letters_to) {
        // still more letters to increment
        nextTicketNumber[finished_changes]++;
    } else if (nextTicketNumber[finished_changes] == letters_to) {
        // last letter
        while (finished_changes < TICKET_LENGTH - 1 && nextTicketNumber[finished_changes] == letters_to) {
            nextTicketNumber[finished_changes] = digits_from;
            if (nextTicketNumber[finished_changes + 1] != letters_to) {
                if (nextTicketNumber[finished_changes + 1] == digits_to) {
                    nextTicketNumber[finished_changes + 1] = letters_from;
                } else {
                    nextTicketNumber[finished_changes + 1]++;
                }
                break;
            } else {
                finished_changes++;
            }
        }
    }

    for (int i = 0; i < TICKET_LENGTH; ++i) 
        ticket[i] = nextTicketNumber[i];
    
    return ticket;

}

Reservation create_reservation(uint16_t timeout, EventsMap &events, ReservationsMap &reservations, char *nextTicketNumber) {
    Reservation result;
    result.cookie = generate_cookie();
    result.event_id = calc_id();
    result.ticket_count = calc_ticket_count();
    result.reservation_id = MIN_RESERVATION_ID + reservations.size();
    result.expiration_time = time(0) + timeout;
    result.tickets_received = false;
  
    events[result.event_id].tickets_available -= result.ticket_count;

    for (int i = 0; i < result.ticket_count; ++i) {
        std::string ticket = generate_ticket(nextTicketNumber);
        result.tickets.push_back(ticket);
    }

    events[result.event_id].unreceived_reservations.push(result);

    return result;
}

// Sends a datagram with tickets from a given reservation. Called only when it is ensured that 
// such reservation exists and time to receive tickets hasn't expired.
void send_tickets(int socket_fd, const struct sockaddr_in *client_address, ReservationsMap &reservations) {
        socklen_t address_length = (socklen_t)sizeof(*client_address);
    int flags = 0;
    uint32_t reservation_id = calc_id();
    std::string tickets_message(MAX_UDP_DATAGRAM_SIZE, '\0');
    uint32_t current_index = 1;

    tickets_message[0] = char(6);

    uint32_t reservation_id_copy = htonl(reservation_id); // in big endian
    memcpy(&tickets_message[current_index], &reservation_id_copy, sizeof(reservation_id_copy));
    current_index += sizeof(reservation_id_copy);

    uint16_t tickets_count_copy = htons(reservations[reservation_id].ticket_count);
    memcpy(&tickets_message[current_index], &tickets_count_copy, sizeof(tickets_count_copy));
    current_index += sizeof(tickets_count_copy); // tickets count is sent in big endian

    for (auto ticket : reservations[reservation_id].tickets) {
        strcpy(&tickets_message[current_index], ticket.c_str());
        current_index += ticket.size();
    }

    tickets_message.resize(current_index);

    reservations[reservation_id].tickets_received = true;
    
    ssize_t sent_length = sendto(socket_fd, tickets_message.c_str(), current_index, flags, (struct sockaddr *)client_address, address_length);
    ENSURE(sent_length == (ssize_t)tickets_message.length());
    printf("[SEND TICKETS] Tickets sent successfully.\n");
}

// Sends a datagram with reservation info. Called only when it is ensured that 
// such a reservation is possible to make.
void send_reservation(int socket_fd, const struct sockaddr_in *client_address,  EventsMap &events,
                        ReservationsMap &reservations, uint16_t timeout, char *nextTicketNumber) {
    socklen_t address_length = (socklen_t)sizeof(*client_address);
    int flags = 0;

    Reservation to_be_sent = create_reservation(timeout, events, reservations, nextTicketNumber);
    reservations[to_be_sent.reservation_id] = to_be_sent;

    uint32_t current_index = 1;
    std::string reservation_message(MAX_UDP_DATAGRAM_SIZE, '\0');

    reservation_message[0] = char(4);

    uint32_t reservation_id_copy = htonl(to_be_sent.reservation_id);
    memcpy(&reservation_message[current_index], &reservation_id_copy, sizeof(reservation_id_copy));
    current_index += sizeof(reservation_id_copy);

    uint32_t event_id_copy = htonl(to_be_sent.event_id);
    memcpy(&reservation_message[current_index], &event_id_copy, sizeof(event_id_copy));
    current_index += sizeof(event_id_copy); // send in big endian

    uint16_t tickets_count_copy = htons(to_be_sent.ticket_count);
    memcpy(&reservation_message[current_index], &tickets_count_copy, sizeof(tickets_count_copy));
    current_index += sizeof(tickets_count_copy); // tickets count is sent in big endian
 
    strcpy(&reservation_message[current_index], to_be_sent.cookie.c_str());
    current_index += to_be_sent.cookie.size();

    uint64_t expiration_time_copy = htobe64(to_be_sent.expiration_time); // send in big endian
    memcpy(&reservation_message[current_index], &expiration_time_copy, sizeof(expiration_time_copy));
    current_index += sizeof(expiration_time_copy);

    reservation_message.resize(current_index);

    ssize_t sent_length = sendto(socket_fd, reservation_message.c_str(), current_index, flags, (struct sockaddr *)client_address, address_length);
    ENSURE(sent_length == (ssize_t)reservation_message.length());
    printf("[SEND RESERVATION] Reservation info sent successfully.\n");
}

// Sends a datagram with info about all events.
void send_events(int socket_fd, const struct sockaddr_in *client_address,  EventsMap &events, ReservationsMap &reservations) {
    socklen_t address_length = (socklen_t)sizeof(*client_address);
    int flags = 0;

    uint32_t current_index = 1;
    std::string events_message(MAX_UDP_DATAGRAM_SIZE, '\0'); 
    events_message[0] = char(2);
    for (auto ev : events) {
        update_reservations_for_event(events, ev.first, reservations);
        uint16_t tickets_count_copy = ev.second.tickets_available;
        tickets_count_copy = htons(tickets_count_copy); // send in big endian

        if (current_index + sizeof(ev.second.event_id) + sizeof(ev.second.tickets_available) + 
            sizeof(ev.second.description_length) + sizeof(ev.second.description) > MAX_UDP_DATAGRAM_SIZE - 100)
            break; 

        uint32_t event_id_copy = ev.second.event_id;
        event_id_copy = htonl(event_id_copy);  
        memcpy(&events_message[current_index], &event_id_copy, sizeof(event_id_copy));
        current_index += sizeof(event_id_copy); // send in big endian
        
        memcpy(&events_message[current_index], &tickets_count_copy, sizeof(tickets_count_copy));
        current_index += sizeof(tickets_count_copy); // send in big endian
        
        memcpy(&events_message[current_index], &ev.second.description_length, sizeof(ev.second.description_length));
        current_index += sizeof(ev.second.description_length); // only one byte, so big endian as well
        
        strcpy(&events_message[current_index], ev.second.description.c_str());
        current_index += ev.second.description.size();
    }
    events_message.resize(current_index);
    ssize_t sent_length = sendto(socket_fd, events_message.c_str(), current_index, flags, (struct sockaddr *)client_address, address_length);
    ENSURE(sent_length == (ssize_t)events_message.length());
    printf("[SEND EVENTS] Events info sent successfully.\n");
}

void send_bad_reservation_request(int socket_fd, const struct sockaddr_in *client_address) {
    socklen_t address_length = (socklen_t)sizeof(*client_address);
    int flags = 0;
    std::string message;
    message += char(BAD_REQUEST);
    for (int i = 1; i < 5; ++i)
        message += shared_buffer[i];
    size_t length = 5;
    ssize_t sent_length = sendto(socket_fd, message.c_str(), length, flags, (struct sockaddr *)client_address, address_length);
    ENSURE(sent_length == (ssize_t)length);
    printf("[SEND RESERVATION] Bad request.\n");
}

void send_bad_tickets_request(int socket_fd, const struct sockaddr_in *client_address) {
    socklen_t address_length = (socklen_t)sizeof(*client_address);
    int flags = 0;
    std::string message;
    message += char(BAD_REQUEST);
    for (int i = 1; i < 5; ++i)
        message += shared_buffer[i];
    size_t length = 5;
    ssize_t sent_length = sendto(socket_fd, message.c_str(), length, flags, (struct sockaddr *)client_address, address_length);
    ENSURE(sent_length == (ssize_t)length);
    printf("[SEND TICKETS] Bad request.\n");
}

void check_port_num(char *port_c) {
    std::string port_string = std::string(port_c);
    for (char &c : port_string) {
        if (!isdigit(c)) 
            fatal("Wrong port number provided");
    }
    int32_t value = atoi(port_c);
    if (value < MIN_PORT_NUM || value > MAX_PORT_NUM) 
        fatal("Wrong port number provided");
}

void check_timeout_value(char *timeout_c) {
    std::string timeout_string = std::string(timeout_c);
    for (char &c : timeout_string) {
        if (!isdigit(c)) 
            fatal("Wrong timeout value provided");
    }
    int value = atoi(timeout_c);
    if (value < 1 || value > MAX_TIMEOUT_VALUE) 
        fatal("Wrong timeout value provided");
}

void read_input(int argc, char *argv[], uint16_t *port_num, uint16_t *timeout,
                char **filename) {
    if (argc < 3) 
        fatal("Not enough arguments provided");
    
    bool filename_found = false, port_num_found = false, timeout_found = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = std::string(argv[i]);

        if (arg == "-f" && !filename_found) {
            *filename = argv[++i];
            if (access(*filename, F_OK) != 0) 
                fatal("Wrong filename provided");
            filename_found = true;
        } else if (arg == "-p" && !port_num_found) {
            if (argc < i + 2) 
                fatal("Not enough parameters provided");
            check_port_num(argv[++i]);
            *port_num = atoi(argv[i]);
            port_num_found = true;
        } else if (arg == "-t" && !timeout_found) {
            if (argc < i + 2) 
                fatal("Not enough parameters provided");
            check_timeout_value(argv[++i]);
            *timeout = atoi(argv[i]);
            timeout_found = true;
        } else {
            fatal("Wrong arguments provided");
        }
    }
}


EventsMap read_events(char *filename) {
    FILE *fp;
    fp = fopen(filename, "r");
    std::ifstream file(filename);
    EventsMap events;
    uint32_t event_num_iter = 0;
    if (file.is_open()) {
        std::string line;
        while (std::getline(file, line)) {
            std::string description = line;
            std::getline(file, line);
            uint16_t tickets_available = stoi(line);
            char description_length = static_cast<char>(description.length());
            std::queue<Reservation> empty_reservations;
            events[event_num_iter] = Event({event_num_iter, description_length, description, tickets_available, empty_reservations});
            event_num_iter++;
        }
        file.close();
    }
    fclose(fp);
    return events;
}

int main(int argc, char *argv[]) {
    uint16_t port = 2022;
    uint16_t timeout = 5;
    char *filename;
    read_input(argc, argv, &port, &timeout, &filename);

    ReservationsMap reservations;
    EventsMap events = read_events(filename);

    printf("Listening on port %u\n", port);

    int socket_fd = bind_socket(port);
    char nextTicketNumber[7];
    for (int i = 0; i < TICKET_LENGTH; ++i) 
        nextTicketNumber[i] = '0'; 
    
    struct sockaddr_in client_address;
    size_t read_length;
    do {
        read_length = read_message(socket_fd, &client_address, shared_buffer, sizeof(shared_buffer)); 
        char* client_ip = inet_ntoa(client_address.sin_addr); 
        uint16_t client_port = ntohs(client_address.sin_port); 
        // printf("received %zd bytes from"
        // "client %s:%u: '%.*s'\n", read_length, client_ip, client_port,
            //    (int) read_length, shared_buffer); // note: we specify the length of the printed string

        if (message_is_get_events(read_length)) {
            send_events(socket_fd, &client_address, events, reservations);
        } else if (message_is_get_reservation(read_length)) {
            if (reservation_arguments_are_correct(events, reservations))
                send_reservation(socket_fd, &client_address, events, reservations, timeout, nextTicketNumber);
            else 
                send_bad_reservation_request(socket_fd, &client_address);
        } else if (message_is_get_tickets(read_length)) {
            if (tickets_arguments_are_correct(reservations) && !tickets_expired(reservations)) 
                send_tickets(socket_fd, &client_address, reservations);
            else 
                send_bad_tickets_request(socket_fd, &client_address);
        } 

    } while (read_length > 0);
    printf("finished exchange\n");

    CHECK_ERRNO(close(socket_fd));

    return 0;
}
