/*
 * This file is part of the Trojan Plus project.
 * Trojan is an unidentifiable mechanism that helps you bypass GFW.
 * Trojan Plus is derived from original trojan project and writing 
 * for more experimental features.
 * Copyright (C) 2020 The Trojan Plus Group Authors.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef IPV6_HEADER_HPP
#define IPV6_HEADER_HPP

#include <iostream>
#include <algorithm>
#include <boost/asio/ip/address_v4.hpp>
#include <gsl/gsl>

#include "core/utils.h"

// Packet header for IPv4.
//
// The wire format of an IPv4 header is:
// https://en.wikipedia.org/wiki/IPv6_packet#Fixed_header

namespace trojan{
    
class ipv6_header {
public:
    enum {
        HEADER_FIXED_LENGTH = 40,
    };

    void clear() { rep_.fill(0); }
    gsl::span<uint8_t> raw() { return rep_; }

    [[nodiscard]] uint8_t version() const { return (rep_[0] >> half_byte_shift_4_bits) & half_byte_mask_0xF; }
    [[nodiscard]] uint16_t payload_length() const { return decode(4, 5); }

    friend std::istream &operator>>(std::istream &is, ipv6_header &header) {
        is.read((char *)(header.rep_.data()), HEADER_FIXED_LENGTH);
        return is;
    }

private:
    [[nodiscard]] uint16_t decode(int a, int b) const {
        return (rep_.at(a) << one_byte_shift_8_bits) + rep_.at(b);
    }

    void encode(int a, int b, uint16_t n) {
        rep_.at(a) = static_cast<uint8_t>(n >> one_byte_shift_8_bits);
        rep_.at(b) = static_cast<uint8_t>(n & one_byte_mask_0xFF);
    }

    std::array<uint8_t, HEADER_FIXED_LENGTH>  rep_{};
};

}

#endif  // IPV6_HEADER_HPP