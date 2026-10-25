#include "transport.hpp"
#include <atomic>
#include <cassert>
#include <iostream>
#include <thread>
using namespace dho804;
struct Fake final : Backend {
    std::string data, written;
    size_t pos = 0, limit = 4000, max_read = 0;
    bool echo = false, fail = false, cleared = false, was_closed = false;
    std::string read(size_t count) override {
        max_read = std::max(max_read, count);
        if (fail) failure("injected timeout", ETIMEDOUT);
        auto result = data.substr(pos, std::min(count, limit)); pos += result.size(); return result;
    }
    size_t write(const char* src, size_t count) override {
        const size_t n = echo ? count : std::min(count, size_t(3));
        written.append(src, n);
        if (echo) { data.assign(src, n); pos = 0; }
        return n;
    }
    void timeout(uint32_t) override {}
    void clear() override { cleared = true; }
    void close() noexcept override { was_closed = true; }
};
template<class F> void throws(F f) { bool caught = false; try { f(); } catch (const std::exception&) { caught = true; } assert(caught); }
int run_tests() {
    unsigned tests = 0;
    {
        auto fake = std::make_unique<Fake>(); auto* f = fake.get();
        f->data = "#850000000" + std::string(50000000, '\x5a') + "\n";
        Transport t(std::move(fake), 5000, 4000);
        const auto response = t.query(":WAVeform:DATA?\n");
        assert(response == f->data && f->max_read == 4000 && f->written == ":WAVeform:DATA?\n" && !f->cleared); ++tests;
    }
    {
        auto fake = std::make_unique<Fake>(); fake->data = "#210abcdefghij\n"; fake->limit = 1;
        Transport t(std::move(fake), 5000, 4000);
        assert(t.read_raw() == "#210abcdefghij"); assert(t.read_bytes(1) == "\n"); ++tests;
    }
    for (const std::string data : {"#xabc", "#2xx", "#210abc", "#"}) {
        auto fake = std::make_unique<Fake>(); fake->data = data;
        Transport t(std::move(fake), 5000, 4000); throws([&] { t.read_raw(); }); ++tests;
    }
    {
        auto fake = std::make_unique<Fake>(); auto* f = fake.get(); f->data = std::string(16000, 'q');
        Transport t(std::move(fake), 5000, 4000);
        assert(t.read_bytes(2) == "qq"); assert(t.read_bytes(12345) == std::string(12345, 'q'));
        assert(t.read_raw() == std::string(3653, 'q')); assert(f->max_read == 4000); ++tests;
    }
    {
        auto fake = std::make_unique<Fake>(); auto* f = fake.get(); f->fail = true;
        Transport t(std::move(fake), 5000, 4000); throws([&] { t.query("*IDN?\n"); });
        assert(f->written == "*IDN?\n" && !f->cleared); t.close(); t.close();
        assert(t.closed() && f->was_closed); throws([&] { t.read_raw(); }); ++tests;
    }
    {
        auto fake = std::make_unique<Fake>(); fake->echo = true;
        Transport t(std::move(fake), 5000, 4000); std::vector<std::thread> threads;
        for (int i = 0; i < 4; ++i) threads.emplace_back([&, i] {
            for (int j = 0; j < 100; ++j) {
                const std::string query = std::to_string(i) + ":" + std::to_string(j) + "?\n";
                assert(t.query(query) == query);
            }
        });
        for (auto& thread : threads) thread.join();
        ++tests;
    }
    assert(expects_reply("A?;B 1") && !expects_reply("A 'it''s?'")); ++tests;
    {
        int sockets[2]; assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
        auto fake = std::make_unique<Fake>(); fake->echo = true;
        Transport t(std::move(fake), 5000, 4000);
        std::atomic<bool> stop{false}; std::exception_ptr error;
        std::thread worker([&] { try { serve_client(sockets[0], t, [](void* p) { return static_cast<std::atomic<bool>*>(p)->load(); }, &stop); } catch (...) { error = std::current_exception(); } });
        const std::string input = "*IDN?\n"; assert(::send(sockets[1], input.data(), input.size(), 0) == 6);
        char response[6]; size_t received = 0;
        while (received < sizeof(response)) { const auto n = ::recv(sockets[1], response + received, sizeof(response)-received, 0); assert(n > 0); received += size_t(n); }
        assert(std::string(response, sizeof(response)) == input);
        stop = true; worker.join(); assert(!error); ::close(sockets[0]); ::close(sockets[1]); ++tests;
    }
    {
        int sockets[2]; assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
        std::atomic<bool> stop{false}; bool completed = true;
        std::thread worker([&] { completed = send_all(sockets[0], std::string(1000000, 'x'),
            [](void* p) { return static_cast<std::atomic<bool>*>(p)->load(); }, &stop); });
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const auto start = std::chrono::steady_clock::now(); stop = true; worker.join();
        assert(!completed && std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
        ::close(sockets[0]); ::close(sockets[1]); ++tests;
    }
    {
        int sockets[2]; assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
        ::close(sockets[1]);
        throws([&] { send_all(sockets[0], "reply", nullptr, nullptr); });
        ::close(sockets[0]); ++tests;
    }
    std::cout << tests << " native transport tests passed\n";
    return 0;
}

int main() { try { return run_tests(); } catch (const std::exception& e) { std::cerr << e.what() << "\n"; return 1; } }
