/*
 * This file is part of the Trojan Plus project.
 * Trojan is an unidentifiable mechanism that helps you bypass GFW.
 * Trojan Plus is derived from original trojan project and writing 
 * for more experimental features.
 * Copyright (C) 2017-2020  The Trojan Authors.
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

#ifndef _SOCKS5ADDRESS_H_
#define _SOCKS5ADDRESS_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/streambuf.hpp>

class SOCKS5Address {
public:
    enum AddressType {
        IPv4 = 1,
        DOMAINNAME = 3,
        IPv6 = 4
    } address_type;
    std::string address;
    uint16_t port;

    [[nodiscard]]
    bool parse(const std::string_view &data, size_t &address_len);
    static void generate(boost::asio::streambuf& buf, const boost::asio::ip::udp::endpoint &endpoint);
};

#endif // _SOCKS5ADDRESS_H_
