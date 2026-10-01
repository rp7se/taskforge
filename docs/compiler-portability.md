# Compiler portability

TaskForge CI currently validates GCC and Clang builds. Production targets use
the warning baseline `-Wall -Wextra -Wpedantic`; CI enables warnings as errors
for those targets only.

Tests, benchmarks, and test helpers retain the normal warning baseline and are
not subject to a repository-wide `-Werror` requirement. `-Werror` is an
internal CI policy, not a requirement imposed on downstream users.

The checks cover the current CI toolchains; they do not promise support for all
GCC or Clang versions.
