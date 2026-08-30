#include "hotpath/orders.hpp"

#include <array>
#include <cstddef>
#include <string>
#include <utility>

#include "hotpath/protocol.hpp"

namespace hotpath {
namespace {

constexpr std::size_t side_index(Direction side) noexcept {
    return side == Direction::Yes ? 0U : 1U;
}

std::array<OrderTemplate, 2> both_sides(const std::string& ticker) {
    return {OrderTemplate{.ticker = ticker, .outcome_side = Direction::Yes},
            OrderTemplate{.ticker = ticker, .outcome_side = Direction::No}};
}

}  // namespace

void OrderTemplates::prebuild(const std::string& ticker) {
    by_ticker_.try_emplace(ticker, both_sides(ticker));
}

const OrderTemplate& OrderTemplates::acquire(const std::string& ticker, Direction side) {
    auto found = by_ticker_.find(ticker);
    if (found == by_ticker_.end()) {
        found = by_ticker_.try_emplace(ticker, both_sides(ticker)).first;
    }
    return found->second[side_index(side)];
}

}  // namespace hotpath
