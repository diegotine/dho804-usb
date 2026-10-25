#pragma once
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace dho804 {
constexpr unsigned int ioctl_kind = 91;
inline void failure(const std::string& operation, int code) {
    const char* category = (code == ETIMEDOUT || code == ETIME) ? "TIMEOUT: " :
        (code == EBUSY || code == EAGAIN) ? "BUSY: " :
        (code == EACCES || code == EPERM) ? "PERMISSION: " : "IO: ";
    throw std::runtime_error(std::string(category) + operation + ": " + std::strerror(code));
}
struct Backend {
    virtual ~Backend() = default;
    virtual std::string read(size_t count) = 0;
    virtual size_t write(const char* data, size_t count) = 0;
    virtual void timeout(uint32_t value) = 0;
    virtual void clear() = 0;
    virtual void close() noexcept = 0;
};
class PosixBackend final : public Backend {
    int fd_ = -1;
public:
    explicit PosixBackend(const std::string& path) {
        fd_ = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (fd_ < 0) failure("open " + path, errno);
        if (::flock(fd_, LOCK_EX | LOCK_NB) < 0) {
            const int error = errno;
            ::close(fd_); fd_ = -1;
            failure("flock " + path, error);
        }
        const uint8_t enabled = 1;
        // Optional, same behavior as the validated driver; never CLEAR.
        ::ioctl(fd_, _IOW(ioctl_kind, 11, uint8_t), &enabled);
        ::ioctl(fd_, _IOW(ioctl_kind, 25, uint8_t), &enabled);
    }
    ~PosixBackend() override { close(); }
    std::string read(size_t count) override {
        std::string data(count, '\0');
        const ssize_t n = ::read(fd_, data.data(), count);
        if (n < 0) failure("read USBTMC", errno);
        if (!n) throw std::runtime_error("IO: USBTMC response ended without data");
        data.resize(static_cast<size_t>(n));
        return data;
    }
    size_t write(const char* data, size_t count) override {
        const ssize_t n = ::write(fd_, data, count);
        if (n < 0) failure("write USBTMC", errno);
        if (!n) throw std::runtime_error("IO: USBTMC accepted zero bytes");
        return static_cast<size_t>(n);
    }
    void timeout(uint32_t value) override {
        if (::ioctl(fd_, _IOW(ioctl_kind, 10, uint32_t), &value) < 0)
            failure("set USBTMC timeout", errno);
    }
    void clear() override {
        if (::ioctl(fd_, _IO(ioctl_kind, 2)) < 0) failure("clear USBTMC", errno);
    }
    void close() noexcept override {
        if (fd_ >= 0) {
            ::ioctl(fd_, _IO(ioctl_kind, 36));
            ::flock(fd_, LOCK_UN);
            ::close(fd_); fd_ = -1;
        }
    }
};
// 0: plain text/indefinite block; SIZE_MAX: incomplete header.
inline size_t block_size(const std::string& data) {
    if (data.empty() || data[0] != '#') return 0;
    if (data.size() < 2) return SIZE_MAX;
    const int digits = data[1] - '0';
    if (digits < 0 || digits > 9) throw std::runtime_error("IO: invalid IEEE header");
    if (!digits) return 0;
    const size_t header = 2 + static_cast<size_t>(digits);
    if (data.size() < header) return SIZE_MAX;
    size_t count = 0;
    for (size_t i = 2; i < header; ++i) {
        if (data[i] < '0' || data[i] > '9') throw std::runtime_error("IO: invalid IEEE length");
        const size_t digit = static_cast<size_t>(data[i] - '0');
        if (count > (SIZE_MAX - digit) / 10) throw std::runtime_error("IO: IEEE length overflow");
        count = count * 10 + digit;
    }
    if (count > SIZE_MAX - header) throw std::runtime_error("IO: IEEE length overflow");
    return header + count;
}
class Transport {
    std::unique_ptr<Backend> io_;
    mutable std::mutex mutex_;
    std::string pending_;
    size_t chunk_;
    uint32_t timeout_ = 5000;
    bool closed_ = false;
    void require_open() const {
        if (closed_) throw std::runtime_error("IO: USBTMC transport closed");
    }
    std::string read_chunk() {
        require_open();
        std::string data = io_->read(chunk_);
        if (data.empty() || data.size() > chunk_) throw std::runtime_error("IO: invalid USB read size");
        return data;
    }
    size_t write_unlocked(const std::string& data) {
        require_open();
        size_t sent = 0;
        while (sent < data.size()) {
            const size_t n = io_->write(data.data() + sent, data.size() - sent);
            if (!n || n > data.size() - sent) throw std::runtime_error("IO: invalid USB write size");
            sent += n;
        }
        return sent;
    }
    std::string read_unlocked() {
        if (!pending_.empty()) { std::string data; data.swap(pending_); return data; }
        std::string data = read_chunk();
        size_t expected = block_size(data);
        while (expected == SIZE_MAX || (expected && data.size() < expected)) {
            // Only reserve a bounded increment; a corrupt header cannot force a giant allocation.
            data += read_chunk();
            expected = block_size(data);
        }
        return data;
    }
public:
    Transport(std::unique_ptr<Backend> io, uint32_t timeout, size_t chunk)
        : io_(std::move(io)), chunk_(chunk) {
        if (!chunk || chunk > static_cast<size_t>(std::numeric_limits<ssize_t>::max()))
            throw std::invalid_argument("chunk_size must be positive and fit ssize_t");
        set_timeout(timeout);
    }
    Transport(const std::string& path, uint32_t timeout, size_t chunk)
        : Transport(std::make_unique<PosixBackend>(path), timeout, chunk) {}
    ~Transport() { close(); }
    void set_timeout(uint32_t value) {
        if (!value || value > 3600000) throw std::invalid_argument("timeout outside 1..3600000 ms");
        std::lock_guard<std::mutex> lock(mutex_);
        if (!closed_) io_->timeout(value);
        timeout_ = value;
    }
    uint32_t timeout() const { std::lock_guard<std::mutex> lock(mutex_); return timeout_; }
    void set_chunk(size_t value) {
        if (!value || value > static_cast<size_t>(std::numeric_limits<ssize_t>::max()))
            throw std::invalid_argument("invalid chunk size");
        std::lock_guard<std::mutex> lock(mutex_); chunk_ = value;
    }
    bool closed() const { std::lock_guard<std::mutex> lock(mutex_); return closed_; }
    size_t write(const std::string& data) {
        std::lock_guard<std::mutex> lock(mutex_); return write_unlocked(data);
    }
    std::string read_raw() {
        std::lock_guard<std::mutex> lock(mutex_); return read_unlocked();
    }
    std::string read_bytes(size_t count) {
        std::lock_guard<std::mutex> lock(mutex_);
        while (pending_.size() < count) pending_ += read_chunk();
        std::string out = pending_.substr(0, count); pending_.erase(0, count); return out;
    }
    std::string query(const std::string& command) {
        std::lock_guard<std::mutex> lock(mutex_);
        write_unlocked(command); return read_unlocked();
    }
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_); require_open(); pending_.clear(); io_->clear();
    }
    void close() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!closed_) { pending_.clear(); io_->close(); closed_ = true; }
    }
};
inline bool expects_reply(const std::string& command) {
    char quote = 0;
    for (size_t i = 0; i < command.size(); ++i) {
        const char c = command[i];
        if (quote) {
            if (c == quote) {
                if (i + 1 < command.size() && command[i + 1] == quote) ++i;
                else quote = 0;
            }
        } else if (c == '\'' || c == '"') quote = c;
        else if (c == '?') return true;
    }
    return false;
}
using Stop = bool (*)(void*);
inline bool stopping(Stop stop, void* context) { return stop && stop(context); }
inline bool send_all(int fd, const std::string& data, Stop stop, void* context) {
    size_t sent = 0;
    auto progress = std::chrono::steady_clock::now();
    while (sent < data.size()) {
        if (stopping(stop, context)) return false;
        if (std::chrono::steady_clock::now() - progress >= std::chrono::seconds(5))
            throw std::runtime_error("SOCKET: client stalled for 5 seconds");
        pollfd p{fd, POLLOUT, 0};
        const int ready = ::poll(&p, 1, 500);
        if (ready < 0) { if (errno == EINTR) continue; throw std::runtime_error("SOCKET: poll failed"); }
        if (!ready) continue;
        const ssize_t n = ::send(fd, data.data() + sent, std::min(size_t(65536), data.size() - sent), MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
        if (n <= 0) throw std::runtime_error("SOCKET: client disconnected");
        sent += static_cast<size_t>(n); progress = std::chrono::steady_clock::now();
    }
    return true;
}
inline void serve_client(int fd, Transport& instrument, Stop stop, void* context) {
    std::string pending;
    auto process = [&](std::string line) {
        while (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) return true;
        if (expects_reply(line)) return send_all(fd, instrument.query(line + "\n"), stop, context);
        instrument.write(line + "\n"); return !stopping(stop, context);
    };
    while (!stopping(stop, context)) {
        pollfd p{fd, POLLIN, 0};
        const int ready = ::poll(&p, 1, 500);
        if (ready < 0) { if (errno == EINTR) continue; throw std::runtime_error("SOCKET: poll failed"); }
        if (!ready) continue;
        char buffer[65536];
        const ssize_t n = ::recv(fd, buffer, sizeof(buffer), MSG_DONTWAIT);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
        if (n < 0) throw std::runtime_error("SOCKET: recv failed");
        if (!n) { if (!pending.empty() && !stopping(stop, context)) process(pending); return; }
        pending.append(buffer, static_cast<size_t>(n));
        size_t end;
        while ((end = pending.find('\n')) != std::string::npos) {
            if (stopping(stop, context)) return;
            std::string line = pending.substr(0, end); pending.erase(0, end + 1);
            if (!process(std::move(line))) return;
        }
    }
}
} // namespace dho804
