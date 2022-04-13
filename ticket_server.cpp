#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>
#include <map>
#include <ctime>
#include <random>
#include <queue>

#define BUFFER_SIZE 80
#define SEND_EVENTS_ID 2
#define SEND_RESERVATION_ID 4
#define SEND_TICKETS_ID 6
#define BAD_REQUEST 255
#define MAX_UDP_DATAGRAM_SIZE 65507
#define MAX_PORT_NUM 65535
#define MIN_PORT_NUM 0
#define MIN_TIMEOUT_VALUE 1
#define MAX_TIMEOUT_VALUE 86400
#define COOKIE_LENGTH 48
#define MAX_BYTE_VALUE 255
#define MIN_RESERVATION_ID 1000000
#define MAX_EVENT_ID 999999
#define TICKET_LENGTH 7
#define MAX_TICKETS_PER_RESERVATION 9357

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

class Cookie {
private:
    std::string value;

public:
    Cookie() {
        static const int range_from = 33;
        static const int range_to = 126;
        static std::random_device rand_dev;
        static std::mt19937 generator(rand_dev());
        static std::uniform_int_distribution<int> distr(range_from, range_to);
        std::string cookie(COOKIE_LENGTH, '\0');
        for (int i = 0; i < COOKIE_LENGTH; ++i)
            cookie[i] = char(distr(generator));

        value = cookie;
    }

    Cookie(const char *buffer) {
        std::string cookie(COOKIE_LENGTH, '\0');
        for (int i = 0; i < COOKIE_LENGTH; ++i) {
            cookie[i] = buffer[i + 5];
        }
        value = cookie;
    }

    std::string get_value() {
        return value;
    }

    bool operator==(const Cookie &other) const {
        return value.compare(other.value) == 0;
    }
};

class TicketGenerator {
private:
    char last_ticket[TICKET_LENGTH];

public:
    TicketGenerator() {
        for (char &i : last_ticket)
            i = '0';
    }

    std::string generate_next_ticket() {
        for (char &i : last_ticket) {
            if (i == 'Z') {
                i = '0';
            } else if (i == '9') {
                i = 'A';
                break;
            } else {
                i++;
                break;
            }
        }
        return std::string(last_ticket);
    }
};

typedef struct reservationStruct Reservation;

typedef struct eventStruct Event;

struct reservationStruct {
    uint32_t reservation_id; // little endian
    uint32_t event_id; // little endian
    uint16_t ticket_count; // little endian
    Cookie cookie; // 48 bytes
    uint64_t expiration_time; // little endian
    std::vector<std::string> tickets;
    bool tickets_received;
};

struct eventStruct {
    uint32_t event_id; // little endian
    char description_length;
    std::string description;
    uint16_t tickets_available; // little endian
    std::queue<Reservation> unreceived_reservations;
};


// Key: event_id in little endian
// Value: event struct
using EventsMap = std::map<uint32_t, Event>;


// Key: reservation_id in little endian
// Value: reservation struct
using ReservationsMap = std::map<uint32_t, Reservation>;

using MessageInfo = std::pair<std::string, uint32_t>;

// Global generator, one for the entire server.
TicketGenerator ticketGenerator;

// Calculate event or reservation id from message stored in shared_buffer.
uint32_t calc_id() {
    uint32_t potential_id = 0;
    uint32_t multiplier = 1;
    for (int i = 4; i >= 1; --i) {
        potential_id += multiplier *
                        ((int) ((shared_buffer[i] + (MAX_BYTE_VALUE + 1)) %
                                (MAX_BYTE_VALUE + 1)));
        multiplier *= (MAX_BYTE_VALUE + 1);
    }
    return potential_id;
}

// Calculate ticket count from message stored in shared_buffer.
// MAX_BYTE_VALUE + 1 (256) is added to both numbers to avoid
// negative numbers, since we want to read them as unsigned.
// 5th and 6th bytes contain information about ticket count, in big endian.
uint16_t calc_ticket_count() {
    return (uint16_t) (
            ((shared_buffer[5] + MAX_BYTE_VALUE + 1) % (MAX_BYTE_VALUE + 1)) *
            (MAX_BYTE_VALUE + 1)) +
           ((shared_buffer[6] + MAX_BYTE_VALUE + 1) %
            (MAX_BYTE_VALUE + 1));
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

// Check reservations for a given event starting from the oldest one
// and update event info if time to receive tickets expired.
void update_reservations_for_event(EventsMap &events, uint32_t event_id,
                                   ReservationsMap &reservations) {
    uint64_t current_time = time(nullptr);
    while (!events[event_id].unreceived_reservations.empty()) {
        Reservation oldest_reservation = events[event_id].
                                                unreceived_reservations.front();
        if (reservations[oldest_reservation.reservation_id].tickets_received) {
            events[event_id].unreceived_reservations.pop();
        } else if (
                oldest_reservation.expiration_time <= current_time) {
            events[event_id].tickets_available += oldest_reservation.
                                                                ticket_count;
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
           reservations[potential_reservation_id].cookie ==
           Cookie(shared_buffer);
}

bool reservation_arguments_are_correct(EventsMap &events,
                                       ReservationsMap &reservations) {
    uint32_t potential_event_id = calc_id();

    if (potential_event_id > MAX_EVENT_ID ||
        events.find(potential_event_id) == events.end())
        return false;

    update_reservations_for_event(events, potential_event_id, reservations);

    uint16_t potential_ticket_count = calc_ticket_count();

    return potential_ticket_count > 0 &&
           events[potential_event_id].tickets_available >=
           potential_ticket_count
           && potential_ticket_count <= MAX_TICKETS_PER_RESERVATION;
}

int bind_socket(uint16_t port) {
    int socket_fd = socket(AF_INET, SOCK_DGRAM, 0);  // creating IPv4 UDP socket
    ENSURE(socket_fd > 0);
    // after socket() call; we should close(sock) on any execution path;

    sockaddr_in server_address;
    server_address.sin_family = AF_INET;  // IPv4
    server_address.sin_addr.s_addr =
            htonl(INADDR_ANY);  // listening on all interfaces
    server_address.sin_port = htons(port);

    // bind the socket to a concrete address
    CHECK_ERRNO(bind(socket_fd, (struct sockaddr *) &server_address,
                     (socklen_t) sizeof(server_address)));

    return socket_fd;
}

size_t read_message(int socket_fd, struct sockaddr_in *client_address,
                    char *buffer, size_t max_length) {
    socklen_t address_length = (socklen_t) sizeof(*client_address);
    int flags = 0;  // we do not request anything special
    errno = 0;
    ssize_t len = recvfrom(socket_fd, buffer, max_length, flags,
                           (struct sockaddr *) client_address, &address_length);

    if (len < 0)
        PRINT_ERRNO();

    return (size_t) len;
}

Reservation create_reservation(uint16_t timeout, EventsMap &events,
                               ReservationsMap &reservations) {
    Cookie next_cookie;
    uint32_t event_id = calc_id();
    uint16_t ticket_count = calc_ticket_count();
    uint32_t reservation_id = MIN_RESERVATION_ID + reservations.size();
    uint64_t expiration_time = time(nullptr) + timeout;
    events[event_id].tickets_available -= ticket_count;

    std::vector<std::string> tickets;
    for (int i = 0; i < ticket_count; ++i) {
        std::string ticket = ticketGenerator.generate_next_ticket();
        tickets.push_back(ticket);
    }

    Reservation result = Reservation(
            {reservation_id, event_id, ticket_count, next_cookie,
             expiration_time, tickets, false});
    events[event_id].unreceived_reservations.push(result);

    return result;
}

void append_id(std::string &message, uint32_t id, uint32_t &current_index) {
    uint32_t reservation_id_copy = htonl(id); // in big endian
    memcpy(&message[current_index], &reservation_id_copy,
           sizeof(reservation_id_copy));
    current_index += sizeof(reservation_id_copy);
}

void append_ticket_count(std::string &message,
                         uint16_t tickets_count_big_endian,
                         uint32_t &current_index) {
    memcpy(&message[current_index], &tickets_count_big_endian,
           sizeof(tickets_count_big_endian));
    current_index += sizeof(tickets_count_big_endian);
}

void append_description(std::string &message, std::string &description,
                        uint32_t &current_index) {
    strcpy(&message[current_index], description.c_str());
    current_index += description.size();
}

void append_description_length(std::string &message, char description_length,
                               uint32_t &current_index) {
    memcpy(&message[current_index], &description_length,
           sizeof(description_length));
    current_index += sizeof(description_length); // only one byte, so big endian
}

void append_cookie(std::string &message,
                   Cookie cookie, uint32_t &current_index) {
    strcpy(&message[current_index], cookie.get_value().c_str());
    current_index += cookie.get_value().size();
}

void append_ticket(std::string &message, std::string &ticket,
                   uint32_t &current_index) {
    strcpy(&message[current_index], ticket.c_str());
    current_index += ticket.size();
}


MessageInfo make_tickets_message(ReservationsMap &reservations,
                                 uint32_t reservation_id) {
    std::string tickets_message(MAX_UDP_DATAGRAM_SIZE, '\0');
    uint32_t current_index = 1;
    tickets_message[0] = char(SEND_TICKETS_ID);

    append_id(tickets_message, reservation_id, current_index);

    uint16_t tickets_count_copy = htons(
            reservations[reservation_id].ticket_count);
    append_ticket_count(tickets_message, tickets_count_copy, current_index);

    for (auto ticket : reservations[reservation_id].tickets)
        append_ticket(tickets_message, ticket, current_index);

    tickets_message.resize(current_index);

    reservations[reservation_id].tickets_received = true;
    return {tickets_message, current_index};
}

// Sends a datagram with tickets from a given reservation.
// Called only when it is ensured that such reservation exists
// and time to receive tickets hasn't expired.
void send_tickets(int socket_fd, const sockaddr_in *client_address,
                  ReservationsMap &reservations) {
    socklen_t address_length = (socklen_t) sizeof(*client_address);
    int flags = 0;
    uint32_t reservation_id = calc_id();
    MessageInfo message_info = make_tickets_message(reservations,
                                                    reservation_id);

    ssize_t sent_length = sendto(socket_fd, message_info.first.c_str(),
                                 message_info.second,
                                 flags, (struct sockaddr *) client_address,
                                 address_length);
    ENSURE(sent_length == (ssize_t) message_info.first.length());
    std::cout << "[SEND TICKETS]     Tickets for reservation nr "
              << reservation_id << " sent successfully.\n";
}

MessageInfo make_reservation_message(ReservationsMap &reservations,
                                     Reservation &to_be_sent) {
    reservations[to_be_sent.reservation_id] = to_be_sent;

    uint32_t current_index = 1;
    std::string reservation_message(MAX_UDP_DATAGRAM_SIZE, '\0');

    reservation_message[0] = char(SEND_RESERVATION_ID);

    append_id(reservation_message, to_be_sent.reservation_id, current_index);

    append_id(reservation_message, to_be_sent.event_id, current_index);

    uint16_t tickets_count_copy = htons(to_be_sent.ticket_count);
    append_ticket_count(reservation_message, tickets_count_copy, current_index);

    append_cookie(reservation_message, to_be_sent.cookie, current_index);

    uint64_t expiration_time_copy = htobe64(
            to_be_sent.expiration_time); // send in big endian
    memcpy(&reservation_message[current_index], &expiration_time_copy,
           sizeof(expiration_time_copy));
    current_index += sizeof(expiration_time_copy);

    reservation_message.resize(current_index);

    return {reservation_message, current_index};
}

// Sends a datagram with reservation info. Called only when it is ensured that
// such a reservation is possible to make.
void send_reservation(int socket_fd, const sockaddr_in *client_address,
                      EventsMap &events,
                      ReservationsMap &reservations, uint16_t timeout) {
    socklen_t address_length = (socklen_t) sizeof(*client_address);
    int flags = 0;

    Reservation to_be_sent = create_reservation(timeout, events, reservations);
    MessageInfo message_info = make_reservation_message(reservations,
                                                        to_be_sent);

    ssize_t sent_length = sendto(socket_fd, message_info.first.c_str(),
                                 message_info.second, flags,
                                 (struct sockaddr *) client_address,
                                 address_length);
    ENSURE(sent_length == (ssize_t) message_info.first.length());
    std::cout << "[SEND RESERVATION] Reservation nr "
              << to_be_sent.reservation_id <<
              " for event nr " << to_be_sent.event_id
              << " sent successfully.\n";
}

uint32_t next_message_size(Event &event) {
    return sizeof(event.event_id) +
           sizeof(event.tickets_available) +
           sizeof(event.description_length) +
           event.description_length;
}

MessageInfo make_events_message(ReservationsMap &reservations,
                                EventsMap &events) {
    uint32_t current_index = 1;
    std::string events_message(MAX_UDP_DATAGRAM_SIZE, '\0');
    events_message[0] = char(SEND_EVENTS_ID);
    for (auto ev : events) {
        update_reservations_for_event(events, ev.first, reservations);
        uint16_t tickets_count_copy = ev.second.tickets_available;
        tickets_count_copy = htons(tickets_count_copy); // send in big endian

        if (current_index + next_message_size(ev.second) >
            MAX_UDP_DATAGRAM_SIZE)
            break;

        append_id(events_message, ev.second.event_id, current_index);

        append_ticket_count(events_message, tickets_count_copy, current_index);

        append_description_length(events_message, ev.second.description_length,
                                  current_index);

        append_description(events_message, ev.second.description,
                           current_index);

    }
    events_message.resize(current_index);
    return {events_message, current_index};
}


// Sends a datagram with info about all events.
void send_events(int socket_fd, const sockaddr_in *client_address,
                 EventsMap &events, ReservationsMap &reservations) {
    socklen_t address_length = (socklen_t) sizeof(*client_address);
    int flags = 0;
    MessageInfo message_info = make_events_message(reservations, events);
    ssize_t sent_length = sendto(socket_fd, message_info.first.c_str(),
                                 message_info.second, flags,
                                 (struct sockaddr *) client_address,
                                 address_length);
    ENSURE(sent_length == (ssize_t) message_info.first.length());
    std::cout << "[SEND EVENTS]      Events info sent successfully. \n";
}

void send_bad_reservation_request(int socket_fd,
                                  const struct sockaddr_in *client_address) {
    socklen_t address_length = (socklen_t) sizeof(*client_address);
    int flags = 0;
    std::string message;
    message += char(BAD_REQUEST);
    for (int i = 1; i < 5; ++i)
        message += shared_buffer[i];
    size_t length = 5;
    ssize_t sent_length = sendto(socket_fd, message.c_str(), length, flags,
                                 (struct sockaddr *) client_address,
                                 address_length);
    ENSURE(sent_length == (ssize_t) length);
    std::cout << "[SEND RESERVATION] Bad request for reservation nr "
              << calc_id() << ". \n";
}

void send_bad_tickets_request(int socket_fd,
                              const struct sockaddr_in *client_address) {
    socklen_t address_length = (socklen_t) sizeof(*client_address);
    int flags = 0;
    std::string message;
    message += char(BAD_REQUEST);
    for (int i = 1; i < 5; ++i)
        message += shared_buffer[i];
    size_t length = 5;
    ssize_t sent_length = sendto(socket_fd, message.c_str(), length, flags,
                                 (struct sockaddr *) client_address,
                                 address_length);
    ENSURE(sent_length == (ssize_t) length);
    std::cout << "[SEND TICKETS]     Bad request.\n";
}

void notify_for_pointless_message(uint16_t port_num) {
    std::cout << "[UNKNOWN MESSAGE]  Not supported message received from port"
              << port_num << ". \n";
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
    if (value < MIN_TIMEOUT_VALUE || value > MAX_TIMEOUT_VALUE)
        fatal("Wrong timeout value provided");
}

void notify_for_wrong_server_parameters(std::string reason) {
    std::cout << "[SERVER USAGE]     argv[0] -f <path to events file> "
                 "[-p <port>] [-t <timeout>]\n";
    fatal("[SERVER USAGE]     %s\n", reason.c_str());
}

void notify_for_correct_server_parameters(uint16_t port_num, uint16_t timeout,
                                          char *filename) {
    std::cout << "[SERVER USAGE]     Server starting with file = " << filename
              << ", port = "
              << port_num << ", timeout = " << timeout << ". \n";
}

void notify_for_correct_message(std::string message, uint16_t port_num) {
    std::cout << message << "Received correct message from port " << port_num
              << ". \n";
}

void notify_for_bad_request(std::string message, uint16_t port_num) {
    std::cout << message << "Received bad request from port " << port_num
              << ". \n";
}

void read_input(int argc, char *argv[], uint16_t *port_num, uint16_t *timeout,
                char **filename) {
    if (argc < 3)
        notify_for_wrong_server_parameters("Not enough arguments provided");

    bool filename_found = false, port_num_found = false, timeout_found = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = std::string(argv[i]);

        if (arg == "-f" && !filename_found) {
            *filename = argv[++i];
            if (access(*filename, F_OK) != 0)
                notify_for_wrong_server_parameters("Wrong filename provided.");
            filename_found = true;
        } else if (arg == "-p" && !port_num_found) {
            if (argc < i + 2)
                notify_for_wrong_server_parameters(
                        "Not enough parameters provided.");
            check_port_num(argv[++i]);
            *port_num = atoi(argv[i]);
            port_num_found = true;
        } else if (arg == "-t" && !timeout_found) {
            if (argc < i + 2)
                notify_for_wrong_server_parameters(
                        "Not enough parameters provided.");
            check_timeout_value(argv[++i]);
            *timeout = atoi(argv[i]);
            timeout_found = true;
        } else {
            notify_for_wrong_server_parameters("Wrong arguments provided.");
        }
    }
    notify_for_correct_server_parameters(*port_num, *timeout, *filename);
}

EventsMap read_events(char *filename) {
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
            events[event_num_iter] = Event(
                    {event_num_iter, description_length, description,
                     tickets_available, empty_reservations});
            event_num_iter++;
        }
        file.close();
    }
    return events;
}

int main(int argc, char *argv[]) {
    uint16_t port = 2022;
    uint16_t timeout = 5;
    char *filename;
    read_input(argc, argv, &port, &timeout, &filename);

    ReservationsMap reservations;
    EventsMap events = read_events(filename);

    int socket_fd = bind_socket(port);

    sockaddr_in client_address;
    size_t read_length;
    do {
        read_length = read_message(socket_fd, &client_address, shared_buffer,
                                   sizeof(shared_buffer));

        if (message_is_get_events(read_length)) {
            notify_for_correct_message("[GET EVENTS]       ", port);
            send_events(socket_fd, &client_address, events, reservations);
        } else if (message_is_get_reservation(read_length)) {
            if (reservation_arguments_are_correct(events, reservations)) {
                notify_for_correct_message("[GET RESERVATION]  ", port);
                send_reservation(socket_fd, &client_address, events,
                                 reservations, timeout);
            } else {
                notify_for_bad_request("[GET RESERVATION]  ", port);
                send_bad_reservation_request(socket_fd, &client_address);
            }
        } else if (message_is_get_tickets(read_length)) {
            if (tickets_arguments_are_correct(reservations) &&
                !tickets_expired(reservations)) {
                notify_for_correct_message("[GET TICKETS]      ", port);
                send_tickets(socket_fd, &client_address, reservations);
            } else {
                notify_for_bad_request("[GET TICKETS]      ", port);
                send_bad_tickets_request(socket_fd, &client_address);
            }
        } else {
            notify_for_pointless_message(port);
        }
    } while (read_length > 0);
    std::cout << "[SERVER USAGE]     Finished exchange\n";

    CHECK_ERRNO(close(socket_fd));

    return 0;
}
