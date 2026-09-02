#pragma once

// Ported from `execution/prebuilt_orders.py`. Kalshi's order endpoint expresses direction as an
// outcome choice at a price rather than a buy/sell action, so a template is a ticker and a side and
// there is no `action` field.

#include <array>
#include <cstddef>
#include <string>
#include <unordered_map>

#include "hotpath/protocol.hpp"

namespace hotpath {

struct OrderTemplate {
    std::string ticker;
    Direction outcome_side = Direction::Yes;

    bool operator==(const OrderTemplate&) const = default;
};

// Both sides of a ticker are built together and handed out by reference. A ticker first seen on a
// fire pays one insertion; every later wake on it is a lookup. Mapped values are reference-stable
// across rehash, so a template handed to a dispatch outlives the next insert.
class OrderTemplates {
public:
    [[nodiscard]] const OrderTemplate& acquire(const std::string& ticker, Direction side);

    [[nodiscard]] std::size_t size() const noexcept { return by_ticker_.size(); }

private:
    std::unordered_map<std::string, std::array<OrderTemplate, 2>> by_ticker_;
};

}  // namespace hotpath
