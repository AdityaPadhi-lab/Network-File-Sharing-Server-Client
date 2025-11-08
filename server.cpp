#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
static const size_t BUF_SZ = 64 * 1024;

static std::string root_dir;
static std::mutex cout_mx;

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

static bool safe_join(const fs::path& rel, fs::path& out_abs) {
    fs::path base = fs::weakly_canonical(root_dir);
    fs::path tgt  = fs::weakly_canonical(base / rel);
    if (std::mismatch(base.begin(), base.end(), tgt.begin()).first == base.end()) {
        out_abs = tgt;
        return true;
    }
    return false;
}

static void handle_list(int fd, const std::string& arg) {
    fs::path p;
    if (!safe_join(arg.empty() ? "." : fs::path(arg), p) || !fs::exists(p) || !fs::is_directory(p)) {
        send_line(fd, "ERR Invalid path");
        return;
    }
    send_line(fd, "OK");
    for (auto& e : fs::directory_iterator(p)) {
        std::string type = e.is_directory() ? "dir" : "file";
        uintmax_t sz = 0;
        if (e.is_regular_file()) { std::error_code ec; sz = fs::file_size(e.path(), ec); if (ec) sz = 0; }
        std::ostringstream line;
        line << e.path().filename().string() << "\t" << type << "\t" << sz;
        send_line(fd, line.str());
    }
    send_line(fd, "END");
}

static void handle_get(int fd, const std::string& arg) {
    fs::path p;
    if (!safe_join(fs::path(arg), p) || !fs::exists(p) || !fs::is_regular_file(p)) {
        send_line(fd, "ERR Not found");
        return;
    }
    std::error_code ec;
    uintmax_t sz = fs::file_size(p, ec);
    if (ec) { send_line(fd, "ERR Cannot stat"); return; }

    std::ifstream ifs(p, std::ios::binary);
    if (!ifs) { send_line(fd, "ERR Cannot open"); return; }

    std::ostringstream hdr; hdr << "OK " << sz;
    if (!send_line(fd, hdr.str())) return;

    std::vector<char> buf(BUF_SZ);
    uintmax_t left = sz;
    while (left > 0) {
        size_t chunk = (size_t)std::min<uintmax_t>(BUF_SZ, left);
        if (!ifs.read(buf.data(), chunk)) break;
        if (!send_all(fd, buf.data(), chunk)) return;
        left -= chunk;
    }
}

static void handle_put(int fd, std::istringstream& iss) {
    std::string rel; uint64_t sz=0;
    if (!(iss >> rel >> sz)) { send_line(fd, "ERR Bad header"); return; }

    fs::path p;
    if (!safe_join(fs::path(rel), p)) { send_line(fd, "ERR Path outside root"); return; }

    fs::create_directories(p.parent_path());
    std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
    if (!ofs) { send_line(fd, "ERR Cannot open"); return; }

    std::vector<char> buf(BUF_SZ);
    uint64_t left = sz;
    while (left > 0) {
        size_t chunk = (size_t)std::min<uint64_t>(BUF_SZ, left);
        if (!recv_all(fd, buf.data(), chunk)) { send_line(fd, "ERR Read fail"); return; }
        ofs.write(buf.data(), chunk);
        if (!ofs) { send_line(fd, "ERR Write fail"); return; }
        left -= chunk;
    }
    send_line(fd, "OK");
}

static void client_thread(int cfd, sockaddr_storage addr) {
    char host[NI_MAXHOST], serv[NI_MAXSERV];
    if (getnameinfo((sockaddr*)&addr, sizeof(addr), host, sizeof(host), serv, sizeof(serv),
                    NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
        std::lock_guard<std::mutex> lk(cout_mx);
        std::cerr << "[+] client " << host << ":" << serv << "\n";
    }

    std::string line;
    while (recv_line(cfd, line)) {
        std::istringstream iss(line);
        std::string cmd; iss >> cmd;
        if (cmd == "LIST") {
            std::string path; std::getline(iss, path);
            if (!path.empty() && path[0] == ' ') path.erase(0, 1);
            handle_list(cfd, path);
        } else if (cmd == "GET") {
            std::string path; iss >> path;
            handle_get(cfd, path);
        } else if (cmd == "PUT") {
            handle_put(cfd, iss);
        } else if (cmd == "QUIT") {
            break;
        } else {
            send_line(cfd, "ERR Unknown");
        }
    }
    close(cfd);
    std::lock_guard<std::mutex> lk(cout_mx);
    std::cerr << "[-] client disconnected\n";
}

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0] << " <bind_ip> <port> <root_dir>\n";
        return 1;
    }

    std::string bind_ip = argv[1];
    std::string port = argv[2];
    root_dir = argv[3];

    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) { perror("socket"); return 1; }

    int yes = 1;
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in sin{}; 
    sin.sin_family = AF_INET; 
    sin.sin_port = htons((uint16_t)std::stoi(port));
    if (inet_pton(AF_INET, bind_ip.c_str(), &sin.sin_addr) != 1) { 
        std::cerr << "Bad IP\n"; 
        return 1; 
    }

    if (bind(sfd, (sockaddr*)&sin, sizeof(sin)) < 0) { perror("bind"); return 1; }
    if (listen(sfd, 64) < 0) { perror("listen"); return 1; }

    std::cerr << "[*] Serving " << fs::weakly_canonical(root_dir) << " on " 
              << bind_ip << ":" << port << "\n";

    while (true) {
        sockaddr_storage addr{}; socklen_t alen = sizeof(addr);
        int cfd = accept(sfd, (sockaddr*)&addr, &alen);
        if (cfd < 0) { perror("accept"); continue; }
        std::thread(client_thread, cfd, addr).detach();
    }
}
