#include "transport.hpp"
#include <arpa/inet.h>
#include <cmath>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <netinet/tcp.h>
#include <sstream>
#include <sys/un.h>

namespace fs = std::filesystem;
namespace {
volatile std::sig_atomic_t stopped = 0;
void on_signal(int) { stopped = 1; }
bool stop_requested(void*) { return stopped != 0; }
struct Fd {
    int value;
    explicit Fd(int fd) : value(fd) { if (fd < 0) dho804::failure("socket", errno); }
    ~Fd() { if (value >= 0) ::close(value); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
};
struct Options {
    bool serve = false, probe = false;
    std::string device, host = "127.0.0.1", log = "INFO";
    unsigned port = 5556, timeout = 5000, discovery_timeout = 1000, attempts = 0;
    double window = 120;
};
unsigned number(const std::string& s, unsigned minimum, unsigned maximum) {
    if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos)
        throw std::invalid_argument("invalid integer: " + s);
    const auto value = std::stoull(s);
    if (value < minimum || value > maximum) throw std::invalid_argument("integer outside range: " + s);
    return static_cast<unsigned>(value);
}
Options options(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--serve") { o.serve = true; continue; }
        if (arg == "--probe") { o.probe = true; continue; }
        if (arg == "--help") {
            std::cout << "DHO804 native USBTMC bridge (no Python runtime)\n"
                "--serve | --probe [--device PATH] [--host 127.0.0.1|localhost|::1]\n"
                "[--port 5556] [--timeout-ms 5000] [--discovery-timeout-ms 1000]\n"
                "[--discovery-attempts 0] [--discovery-window-seconds 120] [--log-level INFO]\n";
            std::exit(0);
        }
        if (i + 1 == argc) throw std::invalid_argument("missing value: " + arg);
        const std::string value = argv[++i];
        if (arg == "--device") o.device = value;
        else if (arg == "--host") o.host = value;
        else if (arg == "--port") o.port = number(value, 1, 65535);
        else if (arg == "--timeout-ms") o.timeout = number(value, 1, 3600000);
        else if (arg == "--discovery-timeout-ms") o.discovery_timeout = number(value, 1, 3600000);
        else if (arg == "--discovery-attempts") o.attempts = number(value, 0, 1000000);
        else if (arg == "--discovery-window-seconds") {
            size_t end; o.window = std::stod(value, &end);
            if (end != value.size() || !std::isfinite(o.window)) throw std::invalid_argument("invalid discovery window");
        } else if (arg == "--log-level") o.log = value;
        else throw std::invalid_argument("unknown option: " + arg);
    }
    if (o.serve == o.probe) throw std::invalid_argument("choose --serve or --probe");
    if (o.host != "127.0.0.1" && o.host != "localhost" && o.host != "::1")
        throw std::invalid_argument("loopback host required");
    if (o.attempts > 1 && o.window <= 0) throw std::invalid_argument("positive discovery window required");
    return o;
}
std::string trim(const std::string& s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
}
std::string text_file(const fs::path& path) {
    std::ifstream file(path);
    if (!file) throw std::runtime_error("cannot read " + path.string());
    std::string s; std::getline(file, s); return trim(s);
}
bool identity_valid(const std::string& idn) {
    std::istringstream in(idn); std::string item; std::vector<std::string> fields;
    while (std::getline(in, item, ',')) fields.push_back(trim(item));
    if (fields.size() != 4 || fields[2].empty() || fields[3].empty()) return false;
    for (size_t i = 0; i < 2; ++i) for (char& c : fields[i]) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return fields[0] == "RIGOL TECHNOLOGIES" && fields[1] == "DHO804" && idn.back() != ',';
}
struct Instrument {
    std::unique_ptr<dho804::Transport> transport;
    std::string path, idn;
};
Instrument discover(const Options& o) {
    std::vector<std::pair<unsigned, std::string>> paths;
    if (!o.device.empty()) paths.emplace_back(0, o.device);
    else for (const auto& entry : fs::directory_iterator("/dev")) {
        const std::string name = entry.path().filename();
        if (name.rfind("usbtmc", 0) != 0 || name.size() == 6) continue;
        const std::string suffix = name.substr(6);
        if (suffix.find_first_not_of("0123456789") != std::string::npos) continue;
        paths.emplace_back(number(suffix, 0, 1000000), entry.path());
    }
    std::sort(paths.begin(), paths.end());
    std::vector<Instrument> matches;
    std::string errors;
    for (const auto& candidate : paths) {
        try {
            if (o.device.empty()) {
                const auto parent = fs::canonical(fs::path("/sys/class/usbmisc") / fs::path(candidate.second).filename() / "device").parent_path();
                if (text_file(parent / "idVendor") != "1ab1" || text_file(parent / "idProduct") != "044d") continue;
            }
            auto instrument = std::make_unique<dho804::Transport>(candidate.second, o.device.empty() ? o.discovery_timeout : o.timeout, 4000);
            const std::string idn = trim(instrument->query("*IDN?\n"));
            if (identity_valid(idn)) matches.push_back({std::move(instrument), candidate.second, idn});
            else errors += candidate.second + ": unexpected identity; ";
        } catch (const std::exception& e) { errors += candidate.second + ": " + e.what() + "; "; }
    }
    if (matches.size() > 1) throw std::runtime_error("multiple DHO804 devices; use --device");
    if (matches.empty()) throw std::runtime_error("no usable DHO804: " + errors);
    matches[0].transport->set_timeout(o.timeout);
    return std::move(matches[0]);
}
void notify(const std::string& message) {
    const char* path = std::getenv("NOTIFY_SOCKET");
    if (!path || !*path) return;
    sockaddr_un address{}; address.sun_family = AF_UNIX;
    const size_t length = std::strlen(path);
    if (length >= sizeof(address.sun_path)) return;
    std::memcpy(address.sun_path, path, length);
    if (path[0] == '@') address.sun_path[0] = '\0';
    const int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return;
    ::sendto(fd, message.data(), message.size(), MSG_NOSIGNAL,
             reinterpret_cast<sockaddr*>(&address), static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + length + (path[0] == '@' ? 0 : 1)));
    ::close(fd);
}
int run(const Options& o) {
    if (o.probe) { auto found = discover(o); std::cout << found.idn << '\n'; return 0; }
    const bool ipv6 = o.host == "::1";
    Fd server(::socket(ipv6 ? AF_INET6 : AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    int yes = 1;
    if (::setsockopt(server.value, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) dho804::failure("SO_REUSEADDR", errno);
    int bound;
    if (ipv6) {
        sockaddr_in6 address{}; address.sin6_family = AF_INET6; address.sin6_addr = in6addr_loopback; address.sin6_port = htons(static_cast<uint16_t>(o.port));
        bound = ::bind(server.value, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    } else {
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = htons(static_cast<uint16_t>(o.port));
        bound = ::bind(server.value, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    }
    if (bound < 0) dho804::failure("bind", errno);
    if (::listen(server.value, 1) < 0) dho804::failure("listen", errno);
    Instrument current;
    unsigned remaining = o.attempts;
    auto next = std::chrono::steady_clock::now();
    const auto interval = std::chrono::duration<double>(o.attempts > 1 ? o.window / (o.attempts - 1) : 0);
    auto connect = [&] {
        try { current = discover(o); std::cerr << current.idn << '\n'; notify("STATUS=DHO804 connected"); return true; }
        catch (const std::exception& e) { std::cerr << e.what() << '\n'; notify("STATUS=Waiting for DHO804"); return false; }
    };
    std::cerr << "Native bridge ready on " << o.host << ':' << o.port << '\n';
    notify("READY=1\nSTATUS=Waiting for DHO804");
    while (!stopped) {
        if (!current.transport && remaining && std::chrono::steady_clock::now() >= next) {
            --remaining;
            if (connect()) remaining = 0;
            next = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(interval);
        }
        pollfd p{server.value, POLLIN, 0}; const int ready = ::poll(&p, 1, 500);
        if (ready < 0) { if (errno == EINTR) continue; dho804::failure("accept poll", errno); }
        if (!ready) continue;
        const int accepted = ::accept4(server.value, nullptr, nullptr, SOCK_CLOEXEC);
        if (accepted < 0 && errno == EINTR) continue;
        Fd client(accepted);
        if (!current.transport && !connect()) continue;
        remaining = 0;
        ::setsockopt(client.value, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
        try { dho804::serve_client(client.value, *current.transport, stop_requested, nullptr); }
        catch (const std::exception& e) {
            std::cerr << "Session stopped without retry: " << e.what() << '\n';
            current.transport.reset(); notify("STATUS=Waiting for new client after error"); continue;
        }
        current.transport.reset();
        if (!stopped) {
            try { current.transport = std::make_unique<dho804::Transport>(current.path, o.timeout, 4000); }
            catch (const std::exception& e) { std::cerr << e.what() << '\n'; }
        }
    }
    notify("STOPPING=1"); return 0;
}
} // namespace
int main(int argc, char** argv) {
    try {
        const Options o = options(argc, argv);
        struct sigaction action{}; action.sa_handler = on_signal; sigemptyset(&action.sa_mask);
        if (sigaction(SIGINT, &action, nullptr) < 0 || sigaction(SIGTERM, &action, nullptr) < 0) dho804::failure("sigaction", errno);
        return run(o);
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 2; }
}
