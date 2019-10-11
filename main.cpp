#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
//#include <libaio.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include <sstream>
#include <string_view>
#include <string>
#include <vector>
#include <map>
#include <functional>

using namespace std;

const int maxactiveconns = 2000;
int conns[maxactiveconns];
int activeconns = 0;

enum ParseState {
    ParsingHeaders,
    ParsingBody
};

struct ConnData {
    string buffer = "";
    size_t pos = 0;
    bool reading = true;
    bool chunked_encoding = true;
    size_t expected_size = 0;

    ParseState State;
};

map<int, ConnData> Data;
map<string, std::function<void(ConnData&)>> Handlers;

template<class Func>
void MatchHeader(ConnData& data, const char* to_match, Func func) {
    int sz = strlen(to_match);
    for (size_t i = 0; i < data.pos; ++i) {
        if (data.buffer[i] == '\n' && strncmp(to_match, data.buffer.data() + i + 1, sz) == 0) {
            size_t j = 0;
            while (j < data.buffer.size() && data.buffer[j] != '\n') {
                ++j;
            }
            func(data.buffer.data() + i, j - i);
        }
    }
}

void Handle(ConnData& data) {
    if (!data.reading) {
        return;
    }
    if (data.State == ParsingHeaders) {
        const char* headers_marker = "\r\n\r\n";
        int headers_marker_len = strlen(headers_marker);
        while (data.pos + headers_marker_len <= data.buffer.size()) {
            if (strncmp(data.buffer.c_str() + data.pos, headers_marker, headers_marker_len) == 0) {
                data.State = ParseState::ParsingBody;
                data.pos += headers_marker_len;
                data.expected_size = data.pos;
                MatchHeader(data, "Content-Length:",
                    [&](const char* s, int) {
                        s += strlen("Content-Length:");
                        while (!isalnum(*s)) {
                            ++s;
                        }
                        data.expected_size = data.pos + atoi(s);
                    });
                MatchHeader(data, "Transfer-Encoding: chunked",
                    [&](const char*, int) {
                        data.chunked_encoding = true;
                    });
                break;
            }
            ++data.pos;
        }
    }
    if (data.State == ParsingBody) {
        if (data.chunked_encoding) {
            if (data.expected_size < data.buffer.size()) {
                data.pos = data.expected_size;
                for (size_t i = data.pos; i < data.buffer.size(); ++i) {
                    if (strncmp("\r\n", data.buffer.data() + i, 2)) {
                        char* endptr;
                        long sz = strtol(data.buffer.data() + data.pos, &endptr, 16);
                        if (sz > 0) {
                            data.expected_size = (endptr - data.buffer.data()) + sz + 4;
                        } else {
                            if (data.buffer.size() == data.pos + 5) {
                                data.reading = false;
                                data.buffer = "response";
                                //data.buffer.erase(data.buffer.begin(), data.buffer.begin() + data.pos);
                                stringstream ss;
                                ss << "HTTP/1.1 200 OK\r\nContent-Length: " << data.buffer.size() << "\r\n\r\n" << data.buffer;
                                data.buffer = ss.str();
                                data.pos = 0;
                            }
                        }
                    }
                }
            }
            //data.expected_size =
        } else if (data.buffer.size() == data.expected_size) {
            data.reading = false;
            data.buffer.erase(data.buffer.begin(), data.buffer.begin() + data.pos);
            stringstream ss;
            ss << "HTTP/1.1 200 OK\r\nContent-Length: " << data.buffer.size() << "\r\n\r\n" << data.buffer;
            data.buffer = ss.str();
            data.pos = 0;
        }
    }
}

int main() {
    int sock = socket(AF_INET6, SOCK_STREAM, 0);
    struct sockaddr_in6 inaddr;
    inaddr.sin6_family = AF_INET6;
    inaddr.sin6_addr = in6addr_any;
    inaddr.sin6_port = htons(8010);
    inaddr.sin6_scope_id = 0;
    if (::bind(sock, (struct sockaddr*)&inaddr, sizeof(inaddr)) != 0) {
        return 1;
    }
    listen(sock, 100);
    int val = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(int));
    fcntl(sock, F_SETFL, O_NONBLOCK);
    int epfd = epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event evts[maxactiveconns];
    char buffer[200];
    {
        struct epoll_event evt;
        evt.events = EPOLLIN | EPOLLET;
        evt.data.fd = sock;
        epoll_ctl(epfd, EPOLL_CTL_ADD, sock, &evt);
    }
    while (1) {
        int ready = epoll_wait(epfd, evts, maxactiveconns, -1);
        for (int i = 0; i < ready; ++i) {
            if (evts[i].data.fd == sock) {
                while (1) {
                    int fd = accept(sock, NULL, NULL);
                    if (fd > 0) {
                        fcntl(fd, F_SETFL, O_NONBLOCK);
                        struct epoll_event evt;
                        evt.events = EPOLLIN | EPOLLOUT | EPOLLET;
                        evt.data.fd = fd;
                        epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &evt);
                        conns[activeconns++] = fd;
                        Data[fd] = {};
                    } else {
                        break;
                    }
                }
            } else {
                ConnData& data = Data[evts[i].data.fd];
                if (data.reading) {
                    while (1) {
                        int rd = read(evts[i].data.fd, buffer, sizeof(buffer));
                        if (rd > 0) {
                            data.buffer.append(buffer, rd);
                        } else {
                            break;
                        }
                        Handle(data);
                    }
                }
                if (!data.reading) {
                    while (1) {
                        int wr = write(evts[i].data.fd, data.buffer.c_str() + data.pos, data.buffer.size() - data.pos);
                        if (wr > 0) {
                            data.pos += wr;
                            if (data.buffer.size() == data.pos) {
                                close(evts[i].data.fd);
                                Data.erase(evts[i].data.fd);
                                break;
                            }
                        } else {
                            break;
                        }
                    }
                }
            }
        }
    }
}
