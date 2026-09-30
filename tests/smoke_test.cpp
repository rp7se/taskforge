#include <cassert>

#include "taskforge/taskforge.hpp"

int main() {
    assert(taskforge::project_name() == "taskforge");
    return 0;
}
