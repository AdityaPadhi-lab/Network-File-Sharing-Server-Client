#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

static const size_t BUF_SZ = 64 * 1024;

static bool send_all(int fd, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(fd, data + sent, len - sent, 0);
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

static bool recv_all(int fd, char* data, size_t len) {
    size_t got = 0;
    while (got < len) {
        ssize_t n = ::recv(fd, data + got, len - got, 0);
        if (n <= 0) return false;
        got += (size_t)n;
    }
    return true;
}

static bool recv_line(int fd, std::string& out) {
    out.clear();
    char c;
    while (true) {
        ssize_t n = ::recv(fd, &c, 1, 0);
        if (n <= 0) return false;
        if (c == '\n') break;
        out.push_back(c);
        if (out.size() > 1'000'000) return false;
    }
    return true;
}

static bool send_line(int fd, const std::string& s) {
    std::string t = s + "\n";
    return send_all(fd, t.data(), t.size());
}

static int connect_to(const std::string& host, const std::string& port) {
    addrinfo hints{}, *res;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0) return -1;
    int fd = -1;
    for (auto p = res; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

static void cmd_ls(int fd, const std::string& path) {
    send_line(fd, path.empty() ? "LIST" : ("LIST " + path));
    std::string line;
    if (!recv_line(fd, line)) { std::cout << "Disconnected\n"; return; }
    if (line != "OK") { std::cout << line << "\n"; return; }
    while (recv_line(fd, line)) {
        if (line == "END") break;
        std::cout << line << "\n";
    }
}

static void cmd_get(int fd, const std::string& remote, const std::string& local) {
    send_line(fd, "GET " + remote);
    std::string line;
    if (!recv_line(fd, line)) { std::cout << "Disconnected\n"; return; }
    std::istringstream iss(line);
    std::string ok; uint64_t sz=0;
    iss >> ok >> sz;
    if (ok != "OK") { std::cout << line << "\n"; return; }

    std::ofstream ofs(local.empty() ? remote.substr(remote.find_last_of("/\\")+1) : local, std::ios::binary|std::ios::trunc);
    if (!ofs) { std::cout << "Cannot open local file\n"; return; }

    std::vector<char> buf(BUF_SZ);
    uint64_t left = sz;
    while (left > 0) {
        size_t chunk = (size_t)std::min<uint64_t>(BUF_SZ, left);
        if (!recv_all(fd, buf.data(), chunk)) { std::cout << "Read fail\n"; return; }
        ofs.write(buf.data(), chunk);
        left -= chunk;
    }
    std::cout << "Downloaded " << sz << " bytes\n";
}

static void cmd_put(int fd, const std::string& local, const std::string& remote) {
    std::ifstream ifs(local, std::ios::binary);
    if (!ifs) { std::cout << "Local file not found\n"; return; }
    ifs.seekg(0, std::ios::end);
    uint64_t sz = (uint64_t)ifs.tellg();
    ifs.seekg(0);

    std::ostringstream hdr; hdr << "PUT " << remote << " " << sz;
    if (!send_line(fd, hdr.str())) return;

    std::vector<char> buf(BUF_SZ);
    uint64_t left = sz;
    while (left > 0) {
        size_t chunk = (size_t)std::min<uint64_t>(BUF_SZ, left);
        ifs.read(buf.data(), chunk);
        if (!send_all(fd, buf.data(), chunk)) { std::cout << "Send fail\n"; return; }
        left -= chunk;
    }
    std::string line;
    if (recv_line(fd, line)) std::cout << line << "\n";
}

static void help() {
    std::cout <<
      "Commands:\n"
      "  ls [path]\n"
      "  get <remote> [local]\n"
      "  put <local> <remote>\n"
      "  quit\n";
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <server_ip> <port>\n";
        return 1;
    }
    int fd = connect_to(argv[1], argv[2]);
    if (fd < 0) { std::cerr << "connect failed\n"; return 1; }
    std::cout << "Connected. Type 'help' for commands.\n";

    std::string line;
    while (true) {
        std::cout << "> ";
        if (!std::getline(std::cin, line)) break;
        std::istringstream iss(line);
        std::string cmd; iss >> cmd;
        if (cmd == "help") help();
        else if (cmd == "ls") { std::string p; std::getline(iss, p); if(!p.empty()&&p[0]==' ') p.erase(0,1); cmd_ls(fd, p); }
        else if (cmd == "get") { std::string r,l; iss>>r>>l; if(r.empty()){std::cout<<"usage: get <remote> [local]\n";} else cmd_get(fd,r,l); }
        else if (cmd == "put") { std::string l,r; iss>>l>>r; if(l.empty()||r.empty()){std::cout<<"usage: put <local> <remote>\n";} else cmd_put(fd,l,r); }
        else if (cmd == "quit" || cmd == "exit") { send_line(fd, "QUIT"); break; }
        else if (cmd.empty()) { continue; }
        else std::cout << "Unknown. Try 'help'.\n";
    }
    close(fd);
    return 0;
}
