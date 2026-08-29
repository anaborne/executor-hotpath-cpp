#include <cstdio>
#include <string_view>

#include "hotpath/version.hpp"

int main() {
    const std::string_view v = hotpath::version();
    std::printf("executor-hotpath %.*s\n", static_cast<int>(v.size()), v.data());
    return 0;
}
