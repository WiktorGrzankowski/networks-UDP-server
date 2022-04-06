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
#include <map>
#include <ctime>
#include <random>

#include "err.hpp"

#define BUFFER_SIZE 80
#define MAX_UDP_DATAGRAM_SIZE 65535
#define MAX_MESSAGE_LENGTH 65435
#define COOKIE_LENGTH 48
#define MAX_BYTE_VALUE 255
#define BAD_REQUEST 255
#define MIN_RESERVATION_ID 1000000
#define MAX_EVENT_ID 999999
#define TICKET_LENGTH 7

char shared_buffer[BUFFER_SIZE];


typedef struct eventStruct event;

struct eventStruct {
    uint32_t event_id; // little endian
    char description_length;
    std::string description;
    uint16_t tickets_available; // litle endian
};

typedef struct reservationStruct reservation;

struct reservationStruct {
    uint32_t reservation_id; // little endian
    uint32_t event_id; // little endian
    uint16_t ticket_count; // litlle endian
    std::string cookie; // 48 bytes
    uint64_t expiration_time;
    std::vector<std::string> tickets;
    bool tickets_received;
};

/*
 * Key: event_id in little endian
 * Value: event struct
*/
using eventsMap = std::map<uint32_t, event>;

/*
 * Key: reservation_id in little endian
 * Value: reservation struct
*/
using reservationsMap = std::map<uint32_t, reservation>;

// Calculate event or reservation id from message stored in shared_buffer.
uint32_t calc_id() {
    uint32_t potential_id = 0;
    uint32_t multiplier = 1;
    for (int i = 4; i >= 1; --i) {
        potential_id += multiplier * ((int) shared_buffer[i]);
        multiplier *= (MAX_BYTE_VALUE + 1);
    }
    return potential_id;
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

bool tickets_expired(reservationsMap &reservations) {
    uint32_t reservation_id = calc_id(); // reading from shared_buffer
    uint64_t current_time = time(0);
    if (!reservations[reservation_id].tickets_received &&
        reservations[reservation_id].expiration_time < current_time) {
            return true;
        }
    return false;
}


bool cookies_match(std::string real_cookie) {
    for (int i = 0; i < real_cookie.length(); ++i) {
        if (real_cookie[i] != shared_buffer[i + 5]) {
            return false;
        }
    }
    printf("dobrze jest\n");
    return true;
}


/*todo - to moze miec bledy ze wzgledu na inny endian w reservation_id */
bool tickets_arguments_are_correct(reservationsMap &reservations) {
    uint32_t potential_reservation_id = calc_id(); // works same as event, 4 bytes each
    if (potential_reservation_id < MIN_RESERVATION_ID)
        return false;
    if (reservations.find(potential_reservation_id) == reservations.end()) {
        printf("niedobry request o ticketsy = %d\n", potential_reservation_id);
        return false;
    }
    printf("chce sobie wziac cookie\n");
    // std::string potential_cookie = get_cookie();
    // printf("moje potential cookie ma wgl size %d", potential_cookie.size());
    if (!cookies_match(reservations[potential_reservation_id].cookie)) {
        printf("nie pasuje ciasteczko");
        return false;
    }
    if (MAX_UDP_DATAGRAM_SIZE < TICKET_LENGTH * reservations[potential_reservation_id].ticket_count) {
        printf("bilety nie mieszcza sie w jednym datagramie");
        return false;
    }

    printf("dobry requescik\n");
    return true;
}

bool reservation_arguments_are_correct(eventsMap &events) {
    uint32_t potential_event_id = calc_id();
    if (potential_event_id > MAX_EVENT_ID)
        return false;
    if (events.find(potential_event_id) == events.end()) {
        printf("BAAAD VERY BAD REQUEST po id = %d\n", potential_event_id);
        return false;
    } 
    // printf("Spoks request po id = %d\n", potential_event_id);
    // teraz ile biletow
    int16_t potential_ticket_count = 0;
    potential_ticket_count += (((int) shared_buffer[5]) * (MAX_BYTE_VALUE + 1)) + ((int) shared_buffer[6]);
    if (potential_ticket_count <= 0 || events[potential_event_id].tickets_available < potential_ticket_count) {
        printf("bad oj veri bad liczba biletow = %d\n", potential_ticket_count);
        return false;
    } 
    // printf("jest git\n");
    return true;
}

std::string generate_cookie() {
    const int range_from  = 33;
    const int range_to    = 126;
    std::random_device                  rand_dev;
    std::mt19937                        generator(rand_dev());
    std::uniform_int_distribution<int>  distr(range_from, range_to);
    int upper_bound = MAX_BYTE_VALUE;
    std::string cookie(COOKIE_LENGTH, '\0');
    for (int i = 0; i < COOKIE_LENGTH; ++i) {
        cookie[i] = char(distr(generator));
    }
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
                    
    if (len < 0) {
        PRINT_ERRNO();
    }
    return (size_t)len;
}


std::string generate_ticket() {
    const int letters_from  = 65;
    const int letters_to    = 90;
    const int digits_from   = 48;
    const int digits_to     = 57;
    std::random_device                  rand_dev;
    std::mt19937                        generator(rand_dev());
    std::uniform_int_distribution<int>  distr_letters(letters_from, letters_to);
    std::uniform_int_distribution<int>  distr_digits(digits_from, digits_to);
    std::uniform_int_distribution<int>  distr_choose(0, 1); // 0 for letters, 1 for digits

    std::string ticket(TICKET_LENGTH, '\0');
    for (int i = 0; i < TICKET_LENGTH; ++i) {
        if (distr_choose(generator) == 0) {
            ticket[i] = char(distr_letters(generator));
        } else {
            ticket[i] = char(distr_digits(generator));
        }
    }
    return ticket;
}

reservation create_reservation(uint16_t timeout, eventsMap &events) {
    reservation result;
    result.cookie = generate_cookie();
    result.event_id = calc_id();
    result.ticket_count = (uint16_t)(((int) shared_buffer[5]) * (MAX_BYTE_VALUE + 1)) + ((int) shared_buffer[6]);
    result.reservation_id = MIN_RESERVATION_ID + events.size();
    result.expiration_time = time(0) + timeout;
    result.tickets_received = false;
    events[result.event_id].tickets_available -= result.ticket_count;

    for (int i = 0; i < result.ticket_count; ++i) {
        std::string ticket = generate_ticket();
        result.tickets.push_back(ticket);
    }
    printf("\nstworzona rezerwacja o id = %d\n", result.reservation_id);
    return result;
}

void send_tickets(int socket_fd, const struct sockaddr_in *client_address, reservationsMap &reservations) {
        socklen_t address_length = (socklen_t)sizeof(*client_address);
    int flags = 0;
    uint32_t reservation_id = calc_id();

    std::string tickets_message(MAX_UDP_DATAGRAM_SIZE, '\0');
    uint32_t current_index = 1;
    // +1
    tickets_message[0] = char(6);
    // +4
    uint32_t reservation_id_copy = htonl(reservation_id); // in big endian
    memcpy(&tickets_message[current_index], &reservation_id_copy, sizeof(reservation_id_copy));
    current_index += sizeof(reservation_id_copy);
    // +2 - ticket count
    uint16_t tickets_count_copy = htons(reservations[reservation_id].ticket_count);
    memcpy(&tickets_message[current_index], &tickets_count_copy, sizeof(tickets_count_copy));
    current_index += sizeof(tickets_count_copy); // tickets count is sent in big endian
    // + ticket_count * 7
    for (auto ticket : reservations[reservation_id].tickets) {
        strcpy(&tickets_message[current_index], ticket.c_str());
        current_index += ticket.size();
    }
    tickets_message.resize(current_index);

    reservations[reservation_id].tickets_received = true;
    ssize_t sent_length = sendto(socket_fd, tickets_message.c_str(), current_index, flags, (struct sockaddr *)client_address, address_length);
    ENSURE(sent_length == (ssize_t)tickets_message.length());
}


// RESERVATION – message_id = 4, reservation_id, event_id, ticket_count, cookie, expiration_time,
// odpowiedź na komunikat GET_RESERVATION potwierdzająca rezerwację, zawierająca czas, do którego należy odebrać zarezerwowane bilety;
void send_reservation(int socket_fd, const struct sockaddr_in *client_address,  eventsMap &events,
                        reservationsMap &reservations, uint16_t timeout) {
    socklen_t address_length = (socklen_t)sizeof(*client_address);
    int flags = 0;

    reservation to_be_sent = create_reservation(timeout, events);
    reservations[to_be_sent.reservation_id] = to_be_sent;

    uint32_t current_index = 1;
    std::string reservation_message(MAX_UDP_DATAGRAM_SIZE, '\0');
    // +1
    reservation_message[0] = char(4);
    // +4
    uint32_t reservation_id_copy = htonl(to_be_sent.reservation_id);
    memcpy(&reservation_message[current_index], &reservation_id_copy, sizeof(reservation_id_copy));
    current_index += sizeof(reservation_id_copy);
    // +4
    uint32_t event_id_copy = htonl(to_be_sent.event_id);
    memcpy(&reservation_message[current_index], &event_id_copy, sizeof(event_id_copy));
    current_index += sizeof(event_id_copy); // send in big endian
    // +2
    uint16_t tickets_count_copy = htons(to_be_sent.ticket_count);
    memcpy(&reservation_message[current_index], &tickets_count_copy, sizeof(tickets_count_copy));
    current_index += sizeof(tickets_count_copy); // tickets count is sent in big endian
    // +48
    strcpy(&reservation_message[current_index], to_be_sent.cookie.c_str());
    current_index += to_be_sent.cookie.size();
    // +8
    uint64_t expiration_time_copy = htobe64(to_be_sent.expiration_time);
    memcpy(&reservation_message[current_index], &expiration_time_copy, sizeof(expiration_time_copy));
    current_index += sizeof(expiration_time_copy);

    reservation_message.resize(current_index);

    ssize_t sent_length = sendto(socket_fd, reservation_message.c_str(), current_index, flags, (struct sockaddr *)client_address, address_length);
    ENSURE(sent_length == (ssize_t)reservation_message.length());
}

void send_events(int socket_fd, const struct sockaddr_in *client_address,  eventsMap &events) {
    socklen_t address_length = (socklen_t)sizeof(*client_address);
    int flags = 0;

    uint32_t current_index = 1; // where to add new characters
    std::string events_message(MAX_UDP_DATAGRAM_SIZE, '\0'); 
    events_message[0] = char(2);
    for (auto ev : events) {
        uint16_t tickets_count_copy = ev.second.tickets_available;
        tickets_count_copy = htons(tickets_count_copy); // send in big endian
        if (current_index > MAX_MESSAGE_LENGTH) // no more place to write onto
            break;

        uint32_t event_id_copy = ev.second.event_id;
        event_id_copy = htonl(event_id_copy);  
        memcpy(&events_message[current_index], &event_id_copy, sizeof(event_id_copy));
        current_index += sizeof(event_id_copy); // send in big endian
        
        memcpy(&events_message[current_index], &tickets_count_copy, sizeof(tickets_count_copy));
        current_index += sizeof(tickets_count_copy); // tickets count is sent in big endian
        
        memcpy(&events_message[current_index], &ev.second.description_length, sizeof(ev.second.description_length));
        current_index += sizeof(ev.second.description_length); // only one byte, so big endian as well
        
        strcpy(&events_message[current_index], ev.second.description.c_str());
        current_index += ev.second.description.size();
    }
    events_message.resize(current_index);
    // printf("\n%s\n", events_message.c_str());
    ssize_t sent_length = sendto(socket_fd, events_message.c_str(), current_index, flags, (struct sockaddr *)client_address, address_length);
    ENSURE(sent_length == (ssize_t)events_message.length());
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
}

void send_message(int socket_fd, const struct sockaddr_in *client_address,
                  const char *message, size_t length, std::vector<event> &events) {
    socklen_t address_length = (socklen_t)sizeof(*client_address);
    int flags = 0;
    ssize_t sent_length = sendto(socket_fd, message, length, flags, (struct sockaddr *)client_address, address_length);
    ENSURE(sent_length == (ssize_t)length);
}

void check_port_num(char *port_c) {
    std::string port_string = std::string(port_c);
    for (char &c : port_string) {
        if (!isdigit(c)) fatal("Wrong port number provided");
    }
    int value = atoi(port_c);
    if (value < 0 || value > 65535) fatal("Wrong port number provided");
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

eventsMap read_events(char *filename) {
    FILE *fp;
    fp = fopen(filename, "r");
    std::ifstream file(filename);
    eventsMap events;
    uint32_t event_num = 0;
    uint32_t event_num_iter = 0;
    if (file.is_open()) {
        std::string line;
        while (std::getline(file, line)) {
            std::string description = line;
            std::getline(file, line);
            uint16_t tickets_available = stoi(line);
            // tickets_available = htons(tickets_available);
            char description_length = static_cast<char>(description.length());
            // event_num = htonl(event_num_iter);
            events[event_num_iter] = event({event_num_iter, description_length, description, tickets_available});
            // events.push_back(event({event_num, description_length, description, tickets_available}));
            event_num_iter++;
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
    uint16_t port = 2022;
    uint16_t timeout = 5;
    char *filename;
    read_input(argc, argv, &port, &timeout, &filename);
    // std::cout << filename << " " << port_num << " " << timeout << '\n';
    reservationsMap reservations;
    eventsMap events = read_events(filename);
    for (auto &ev : events) {
        std::cout << "rozmiar id = " << sizeof(ev.second.event_id) <<" rozmiar len = "<<sizeof(ev.second.description_length)<<"rozmiar desc= "<<
            ev.second.description.size() <<" sizeof tickets = " << sizeof(ev.second.tickets_available)<< "   i  ";
        std::cout << ev.second.event_id << " "<< (int)ev.second.description_length <<" "<< ev.second.description  << " " << ev.second.tickets_available << '\n';
    }

    printf("Listening on port %u\n", port);

    int socket_fd = bind_socket(port);

    struct sockaddr_in client_address;
    size_t read_length;
    do {
        read_length = read_message(socket_fd, &client_address, shared_buffer, sizeof(shared_buffer)); 
        char* client_ip = inet_ntoa(client_address.sin_addr); 
        uint16_t client_port = ntohs(client_address.sin_port); printf("received %zd bytes from"
        "client %s:%u: '%.*s'\n", read_length, client_ip, client_port,
               (int) read_length, shared_buffer); // note: we specify the length of the printed string

        if (message_is_get_events(read_length)) {
            send_events(socket_fd, &client_address, events);
        } else if (message_is_get_reservation(read_length)) {
            if (reservation_arguments_are_correct(events))
                send_reservation(socket_fd, &client_address, events, reservations, timeout);
            else 
                send_bad_reservation_request(socket_fd, &client_address);
            
            // for (int i = 0; i < read_length; ++i) {
            //     printf("%d ", shared_buffer[i]);
            // }

        } else if (message_is_get_tickets(read_length)) {
            // GET_TICKETS – message_id = 5, reservation_id, cookie, prośba o wysłanie zarezerwowanych biletów.
            if (tickets_arguments_are_correct(reservations) && !tickets_expired(reservations)) {
                // client can receive tickets
                send_tickets(socket_fd, &client_address, reservations);
            } else {
                send_bad_tickets_request(socket_fd, &client_address);
            }
        } else {
            // wrong query
            continue;
        }
        
        
        // send_message(socket_fd, &client_address, shared_buffer, read_length, events);

    } while (read_length > 0);
    printf("finished exchange\n");

    CHECK_ERRNO(close(socket_fd));

    // return 0;
}
