#include <array>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <thread>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

volatile std::sig_atomic_t term_received = 0;
std::array<pid_t, 8> spawned_children{};
int spawned_count = 0;

void note_term(int) { term_received = 1; }

void write_all(int fd, const char* data, std::size_t size) {
    std::size_t written = 0;
    while (written < size) {
        const ssize_t count = ::write(fd, data + written, size - written);
        if (count > 0) {
            written += static_cast<std::size_t>(count);
        }
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        return 2;
    }
    const std::string_view mode = argv[1];
    if (mode == "stdout") {
        std::cout << "hello stdout\n";
        return 0;
    }
    if (mode == "stderr") {
        std::cerr << "hello stderr\n";
        return 0;
    }
    if (mode == "argv") {
        for (int index = 2; index < argc; ++index) {
            if (index > 2) {
                std::cout << '|';
            }
            std::cout << argv[index];
        }
        std::cout << '\n';
        return 0;
    }
    if (mode == "exit") {
        return argc == 3 ? std::atoi(argv[2]) : 3;
    }
    if (mode == "signal") {
        std::raise(SIGTERM);
        return 4;
    }
    if (mode == "both") {
        const std::array<char, 1024> stdout_block = [] {
            std::array<char, 1024> block{};
            block.fill('o');
            return block;
        }();
        const std::array<char, 1024> stderr_block = [] {
            std::array<char, 1024> block{};
            block.fill('e');
            return block;
        }();
        for (int index = 0; index < 128; ++index) {
            write_all(STDOUT_FILENO, stdout_block.data(), stdout_block.size());
            write_all(STDERR_FILENO, stderr_block.data(), stderr_block.size());
        }
        return 0;
    }
    if (mode == "sleep" && argc == 3) {
        std::this_thread::sleep_for(std::chrono::milliseconds(std::atoi(argv[2])));
        return 0;
    }
    if (mode == "cgroup-membership") {
        std::FILE* membership = std::fopen("/proc/self/cgroup", "r");
        if (membership == nullptr) {
            return 6;
        }
        char buffer[512]{};
        while (std::fgets(buffer, sizeof(buffer), membership) != nullptr) {
            write_all(STDOUT_FILENO, buffer, std::char_traits<char>::length(buffer));
        }
        std::fclose(membership);
        return 0;
    }
    if (mode == "cpu-burn" && argc == 3) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::atoi(argv[2]));
        volatile std::uint64_t accumulator = 1;
        while (std::chrono::steady_clock::now() < until) {
            accumulator = accumulator * 1664525U + 1013904223U;
        }
        return accumulator == 0 ? 7 : 0;
    }
    if (mode == "allocate-touch" && argc == 3) {
        const std::size_t bytes = static_cast<std::size_t>(std::strtoull(argv[2], nullptr, 10));
        std::cout << "MEMORY_HELPER_STARTED\n" << std::flush;
        const rlimit unlimited_memlock{RLIM_INFINITY, RLIM_INFINITY};
        if (::setrlimit(RLIMIT_MEMLOCK, &unlimited_memlock) != 0) {
            std::perror("MEMORY_HELPER_SETMEMLOCK");
            return 8;
        }
        void* mapping = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapping == MAP_FAILED) {
            return 8;
        }
        auto* allocation = static_cast<volatile unsigned char*>(mapping);
        constexpr std::size_t page = 4096;
        constexpr std::size_t lock_chunk = 1024 * 1024;
        for (std::size_t chunk_offset = 0; chunk_offset < bytes; chunk_offset += lock_chunk) {
            const std::size_t chunk_size = bytes - chunk_offset < lock_chunk ? bytes - chunk_offset : lock_chunk;
            for (std::size_t offset = chunk_offset; offset < chunk_offset + chunk_size; offset += page) {
                // A non-zero value forces a private physical page rather than
                // retaining the anonymous shared zero page.
                allocation[offset] = static_cast<unsigned char>((offset / page) % 251 + 1);
            }
            // Keep already-grown pages resident.  Otherwise a swap-enabled
            // runner can reclaim them and avoid the memcg OOM path entirely.
            if (::mlock(const_cast<unsigned char*>(allocation) + chunk_offset, chunk_size) != 0) {
                std::perror("MEMORY_HELPER_MLOCK");
                (void)::munmap(const_cast<unsigned char*>(allocation), bytes);
                return 9;
            }
        }
        if (bytes != 0) allocation[bytes - 1] = 1;
        char line[64]{};
        const int length = std::snprintf(line, sizeof(line), "TOUCHED=%zu\n", bytes);
        write_all(STDOUT_FILENO, line, static_cast<std::size_t>(length));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        (void)::munmap(const_cast<unsigned char*>(allocation), bytes);
        return 0;
    }
    if (mode == "fork-hold" && argc == 4) {
        const int requested = std::atoi(argv[2]);
        const int hold_ms = std::atoi(argv[3]);
        std::array<pid_t, 64> children{};
        int created = 0;
        for (; created < requested && created < static_cast<int>(children.size()); ++created) {
            const pid_t child = ::fork();
            if (child == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));
                _exit(0);
            }
            if (child < 0) {
                break;
            }
            children[created] = child;
        }
        char line[64]{};
        const int length = std::snprintf(line, sizeof(line), "CREATED=%d\n", created);
        write_all(STDOUT_FILENO, line, static_cast<std::size_t>(length));
        for (int index = 0; index < created; ++index) {
            while (::waitpid(children[index], nullptr, 0) < 0 && errno == EINTR) {
            }
        }
        return 0;
    }
    if (mode == "exit-after" && argc == 4) {
        std::this_thread::sleep_for(std::chrono::milliseconds(std::atoi(argv[2])));
        return std::atoi(argv[3]);
    }
    if (mode == "print-then-sleep" && argc == 3) {
        write_all(STDOUT_FILENO, "before timeout stdout\n", 22);
        write_all(STDERR_FILENO, "before timeout stderr\n", 22);
        std::this_thread::sleep_for(std::chrono::milliseconds(std::atoi(argv[2])));
        return 0;
    }
    if (mode == "spawn-descendants" && argc == 6) {
        const int sleep_ms = std::atoi(argv[2]);
        const bool parent_exits = std::atoi(argv[3]) != 0;
        const bool ignore_term = std::atoi(argv[4]) != 0;
        const int count = std::atoi(argv[5]);
        char line[96]{};
        const int parent_size = std::snprintf(line, sizeof(line), "PARENT_PID=%ld PGID=%ld\n",
                                              static_cast<long>(::getpid()), static_cast<long>(::getpgrp()));
        write_all(STDOUT_FILENO, line, static_cast<std::size_t>(parent_size));
        for (int index = 0; index < count; ++index) {
            const pid_t descendant = ::fork();
            if (descendant == 0) {
                if (ignore_term) {
                    std::signal(SIGTERM, SIG_IGN);
                }
                const int size = std::snprintf(line, sizeof(line), "DESCENDANT_PID=%ld PGID=%ld\n",
                                               static_cast<long>(::getpid()), static_cast<long>(::getpgrp()));
                write_all(STDOUT_FILENO, line, static_cast<std::size_t>(size));
                write_all(STDERR_FILENO, "descendant stderr\n", 18);
                std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
                _exit(0);
            }
            spawned_children[spawned_count++] = descendant;
        }
        if (parent_exits) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            return 0;
        }
        if (!ignore_term) {
            std::signal(SIGTERM, note_term);
            while (!term_received) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            for (int index = 0; index < spawned_count; ++index) {
                while (::waitpid(spawned_children[index], nullptr, 0) < 0 && errno == EINTR) {
                }
            }
            return 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
        return 0;
    }
    return 5;
}
