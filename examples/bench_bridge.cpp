// Cliente de bancada: usa a ponte USB existente, sem abrir USBTMC diretamente.
// g++ -O2 -std=c++17 -Wall -Wextra -Wpedantic -pthread bench_bridge.cpp -o bench_bridge
// ./bench_bridge --self-test | ./bench_bridge 5556 /caminho/captura_ch1 [pontos_por_janela]
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <unistd.h>
#include <fcntl.h>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;
static double seconds(Clock::time_point a) {
    return std::chrono::duration<double>(Clock::now() - a).count();
}
static double cpu_seconds() {
    rusage r{};
    if (getrusage(RUSAGE_SELF, &r)) throw std::runtime_error("getrusage falhou");
    return r.ru_utime.tv_sec + r.ru_stime.tv_sec
        + (r.ru_utime.tv_usec + r.ru_stime.tv_usec) / 1e6;
}
struct Session {
    int fd;
    size_t received = 0;
    double receive_seconds = 0;
    explicit Session(int descriptor) : fd(descriptor) {
        // A ponte existente acumula a resposta USB inteira antes de enviar TCP.
        timeval timeout{120, 0};
        if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ||
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout))) {
            ::close(fd);
            throw std::runtime_error("setsockopt falhou");
        }
    }
    ~Session() { ::close(fd); }
    Session(const Session&) = delete;
    void send_command(const std::string& command) {
        std::string wire = command + "\n";
        size_t offset = 0;
        while (offset < wire.size()) {
            auto n = ::send(fd, wire.data() + offset, wire.size() - offset, MSG_NOSIGNAL);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) throw std::runtime_error("envio falhou: " + std::string(strerror(errno)));
            offset += size_t(n);
        }
    }
    void exact(void* dest, size_t size) {
        auto* out = static_cast<char*>(dest);
        size_t offset = 0;
        const auto began = Clock::now();
        while (offset < size) {
            if (seconds(began) > 120) throw std::runtime_error("prazo total de leitura excedido");
            auto start = Clock::now();
            auto n = ::recv(fd, out + offset, size - offset, 0);
            receive_seconds += seconds(start);
            if (n < 0 && errno == EINTR) continue;
            if (n < 0) throw std::runtime_error("recepcao falhou: " + std::string(strerror(errno)));
            if (!n) throw std::runtime_error("ponte encerrou a resposta antes do fim");
            offset += size_t(n);
            received += size_t(n);
        }
    }
    std::string query(const std::string& command) {
        send_command(command);
        std::string response;
        while (response.size() < 4096) {
            char c;
            exact(&c, 1);
            if (c == '\n') {
                if (!response.empty() && response.back() == '\r') response.pop_back();
                if (response.empty()) throw std::runtime_error("resposta vazia para " + command);
                return response;
            }
            response += c;
        }
        throw std::runtime_error("resposta textual longa demais");
    }
    void block(char* payload, size_t expected, bool verbose = true) {
        char header[2];
        exact(header, 2);
        if (header[0] != '#' || header[1] < '1' || header[1] > '9')
            throw std::runtime_error("cabecalho IEEE invalido");
        std::string length(size_t(header[1] - '0'), ' ');
        exact(length.data(), length.size());
        if (length.find_first_not_of("0123456789") != std::string::npos)
            throw std::runtime_error("tamanho IEEE invalido");
        size_t declared = std::stoull(length);
        if (verbose) std::cout << "payload_declarado_bytes=" << declared << std::endl;
        if (declared != expected)
            throw std::runtime_error("tamanho recebido difere da RAM solicitada: " + length);
        exact(payload, expected);
        char terminator;
        exact(&terminator, 1);
        if (terminator == '\r') exact(&terminator, 1);
        if (terminator != '\n') throw std::runtime_error("terminador IEEE invalido");
    }
};

static void self_test() {
    const std::string payload("\0\n\r#\xff\x01", 6);
    for (const auto& wire : {std::string("#16") + payload + "\n",
                            std::string("#16") + payload + "\r\n",
                            std::string("#16") + payload.substr(0, 3),
                            std::string("#15") + payload,
                            std::string("#x6") + payload}) {
        int pair[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair)) throw std::runtime_error("socketpair");
        std::thread writer([&] {
            for (char c : wire) {
                if (send(pair[1], &c, 1, MSG_NOSIGNAL) != 1) break;
            }
            ::close(pair[1]);
        });
        bool passed = false;
        {
            Session s(pair[0]);
            std::vector<char> result(payload.size());
            try {
                s.block(result.data(), result.size());
                passed = std::string(result.begin(), result.end()) == payload;
            } catch (const std::exception&) { }
        }
        writer.join();
        const bool valid = wire == "#16" + payload + "\n" || wire == "#16" + payload + "\r\n";
        if (passed != valid) throw std::runtime_error("teste de bloco falhou");
    }
    std::cout << "5 testes de fragmentacao, truncamento e terminador passaram\n";
}

int main(int argc, char** argv) {
    std::cout << std::fixed << std::setprecision(6) << std::unitbuf;
    try {
        if (argc == 2 && std::string(argv[1]) == "--self-test") { self_test(); return 0; }
        if (argc < 3 || argc > 4)
            throw std::runtime_error("uso: bench_bridge PORTA PREFIXO_SAIDA [PONTOS_POR_JANELA]");
        const int port = std::stoi(argv[1]);
        if (port < 1 || port > 65535) throw std::runtime_error("porta invalida");
        const std::string prefix = argv[2];
        const std::string path = prefix + ".raw";
        if (access(path.c_str(), F_OK) == 0) throw std::runtime_error("arquivo ja existe");
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) throw std::runtime_error("socket falhou");
        Session s(fd);
        int no_delay = 1;
        if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay)))
            throw std::runtime_error("TCP_NODELAY falhou");
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(uint16_t(port));
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)))
            throw std::runtime_error("conexao falhou: " + std::string(strerror(errno)));
        auto began = Clock::now();
        const auto identity = s.query("*IDN?");
        std::cout << "instrumento=" << identity << '\n';
        if (identity.rfind("RIGOL TECHNOLOGIES,DHO804,", 0) != 0)
            throw std::runtime_error("instrumento inesperado");
        const bool was_running = s.query(":TRIGger:STATus?") != "STOP";
        if (was_running) s.send_command(":STOP");
        auto stopped = Clock::now();
        while (s.query(":TRIGger:STATus?") != "STOP") {
            if (seconds(stopped) > 3) throw std::runtime_error("nao confirmou STOP");
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
        }
        double depth = std::stod(s.query(":ACQuire:MDEPth?"));
        if (!std::isfinite(depth) || depth < 1 || depth > 25000000 || std::floor(depth) != depth)
            throw std::runtime_error("profundidade de memoria invalida");
        const size_t points = size_t(depth);
        const size_t window_points = argc == 4 ? std::stoull(argv[3]) : points;
        if (!window_points || window_points > points)
            throw std::runtime_error("janela fora da profundidade da RAM");
        s.send_command(":WAVeform:SOURce CHANnel1");
        s.send_command(":WAVeform:MODE RAW");
        s.send_command(":WAVeform:FORMat WORD");
        s.send_command(":WAVeform:STARt 1");
        s.send_command(":WAVeform:STOP " + std::to_string(window_points));
        if (s.query(":WAVeform:MODE?") != "RAW" || s.query(":WAVeform:FORMat?") != "WORD" ||
            s.query(":WAVeform:SOURce?") != "CHAN1" ||
            std::stoull(s.query(":WAVeform:STARt?")) != 1 ||
            std::stoull(s.query(":WAVeform:STOP?")) != window_points)
            throw std::runtime_error("configuracao RAW/WORD/janela nao confirmada");
        const auto preamble = s.query(":WAVeform:PREamble?");
        std::vector<char> payload(points * 2);
        std::cout << "pontos_solicitados=" << points << "\npontos_por_janela=" << window_points
                  << "\npreparacao_s=" << seconds(began) << '\n';
        const auto transfer = Clock::now();
        const double cpu_before = cpu_seconds();
        const double recv_before = s.receive_seconds;
        const size_t bytes_before = s.received;
        size_t completed_points = 0;
        size_t windows = 0;
        auto last_progress = Clock::now();
        try {
            while (completed_points < points) {
                size_t count = std::min(window_points, points - completed_points);
                if (completed_points) {
                    // Aumentar STOP antes de START evita uma janela invertida.
                    s.send_command(":WAVeform:STOP " + std::to_string(completed_points + count)
                        + "\n:WAVeform:STARt " + std::to_string(completed_points + 1)
                        + "\n:WAVeform:DATA?");
                } else {
                    s.send_command(":WAVeform:DATA?");
                }
                s.block(payload.data() + completed_points * 2, count * 2, window_points == points);
                completed_points += count;
                ++windows;
                if (seconds(last_progress) >= 2 || completed_points == points) {
                    std::cout << "progresso_pontos=" << completed_points
                              << "; decorrido_s=" << seconds(transfer) << '\n';
                    last_progress = Clock::now();
                }
            }
        } catch (...) {
            std::cerr << "captura_incompleta; transferencia_s=" << seconds(transfer)
                      << "; pontos_completos=" << completed_points
                      << "; bytes_recebidos_da_ponte=" << s.received - bytes_before
                      << "; sem repetir consulta ou enviar RUN apos falha de transporte\n";
            throw;
        }
        const double transfer_seconds = seconds(transfer);
        const double cpu_used = cpu_seconds() - cpu_before;
        const double receive_used = s.receive_seconds - recv_before;
        std::cout << "pontos_recebidos=" << payload.size() / 2
                  << "\njanelas=" << windows
                  << "\ntransferencia_s=" << transfer_seconds
                  << "\ncliente_cpu_s=" << cpu_used
                  << "\ncliente_em_recv_s=" << receive_used << '\n';
        if (was_running) {
            s.send_command(":RUN");
            auto resumed = Clock::now();
            while (s.query(":TRIGger:STATus?") == "STOP") {
                if (seconds(resumed) > 3) throw std::runtime_error("nao confirmou RUN");
                std::this_thread::sleep_for(std::chrono::milliseconds(40));
            }
        }
        const auto save = Clock::now();
        int output = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (output < 0) throw std::runtime_error("nao abriu arquivo de captura");
        size_t written = 0;
        while (written < payload.size()) {
            auto n = ::write(output, payload.data() + written, payload.size() - written);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { ::close(output); throw std::runtime_error("arquivo incompleto: " + path); }
            written += size_t(n);
        }
        const int sync_result = fsync(output);
        const int close_result = ::close(output);
        if (sync_result || close_result) throw std::runtime_error("falha de persistencia: " + path);
        const double save_seconds = seconds(save);
        std::ofstream meta(prefix + ".txt");
        meta << std::setprecision(12) << "complete=true\nformat=WORD_LE_U16\nchannel=1\npoints=" << points
             << "\nwindows=" << windows << "\nwindow_points=" << window_points
             << "\nbytes=" << payload.size() << "\nidn=" << identity << "\npreamble=" << preamble
             << "\ntransfer_seconds=" << transfer_seconds << "\nclient_cpu_seconds=" << cpu_used
             << "\nclient_recv_seconds=" << receive_used << "\nsave_fsync_seconds=" << save_seconds << '\n';
        meta.close();
        if (!meta) throw std::runtime_error("falha ao gravar metadados");
        std::cout << "gravacao_fsync_s=" << save_seconds << "\ntotal_s=" << seconds(began)
                  << "\narquivo=" << path << "\ncaptura_completa=true\n";
    } catch (const std::exception& e) {
        std::cerr << "ERRO: " << e.what() << '\n';
        return 1;
    }
}
