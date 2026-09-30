#include <array>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <thread>
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
