#include <array>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <unistd.h>

namespace {

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
    return 5;
}
