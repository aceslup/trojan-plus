
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

#include "tundev.h"

#include <boost/asio/buffer.hpp>
#include <boost/asio/ip/address_v4.hpp>
#include <cassert>
#include <functional>

#include <misc/ipv4_proto.h>
#include <misc/udp_proto.h>

#include "proto/ipv4_header.h"
#include "proto/ipv6_header.h"

#include "core/log.h"
#include "core/service.h"
#include "tun/dnsserver.h"
#include "tun/lwip_tcp_client.h"
#include "tun/tunlocalsession.h"
#include "tun/tunproxysession.h"
#include "tun/tunsession.h"

using namespace std;
using namespace boost::asio::ip;

// clang-format off
static const uint32_t mask_values[] = {
  0x80000000, 0xC0000000, 0xE0000000, 0xF0000000,
  0xF8000000, 0xFC000000, 0xFE000000, 0xFF000000,
  0xFF800000, 0xFFC00000, 0xFFE00000, 0xFFF00000,
  0xFFF80000, 0xFFFC0000, 0xFFFE0000, 0xFFFF0000,
  0xFFFF8000, 0xFFFFC000, 0xFFFFE000, 0xFFFFF000,
  0xFFFFF800, 0xFFFFFC00, 0xFFFFFE00, 0xFFFFFF00,
  0xFFFFFF80, 0xFFFFFFC0, 0xFFFFFFE0, 0xFFFFFFF0,
  0xFFFFFFF8, 0xFFFFFFFC, 0xFFFFFFFE, 0xFFFFFFFF,
};
// clang-format on

static void tcp_remove(struct tcp_pcb* pcb_list) {
    struct tcp_pcb* pcb  = pcb_list;
    struct tcp_pcb* pcb2 = nullptr;

    while (pcb != nullptr) {
        pcb2 = pcb;
        pcb  = pcb->next;
        tcp_abort(pcb2);
    }
}

TUNDev* TUNDev::sm_tundev = nullptr;

TUNDev::TUNDev(Service* _service, const std::string& _tun_name, const std::string& _ipaddr, const std::string& _netmask,
  uint16_t _mtu, int _outside_tun_fd)
    : m_netif_configured(false),
      m_tcp_listener(nullptr),
      m_service(_service),
      m_tun_fd(_outside_tun_fd),
      m_is_outside_tun_fd(_outside_tun_fd != -1),
      m_mtu(_mtu),
      m_quitting(false),
      m_boost_sd(_service->get_io_context()) {

    assert(sm_tundev == nullptr);
    sm_tundev = this;

    if (m_tun_fd == -1) {
#ifdef __linux__
        // open TUN device, check detail information:
        // https://www.kernel.org/doc/Documentation/networking/tuntap.txt
        if ((m_tun_fd = open("/dev/net/tun", O_RDWR)) < 0) {
            throw runtime_error("[tun] error opening device");
        }

        struct ifreq ifr {};
        ifr.ifr_flags = IFF_NO_PI | IFF_TUN;
        snprintf(ifr.ifr_name, IFNAMSIZ, "%s", _tun_name.c_str());

        if (ioctl(m_tun_fd, TUNSETIFF, (void*)&ifr) < 0) {
            throw runtime_error("[tun] error configuring device");
        }

        _log_with_date_time("[tun] /dev/net/tun ifr.ifr_mtu: " + to_string(ifr.ifr_mtu), Log::WARN);
#else
        throw logic_error("[tun] cannot enable tun run type in NON-linux system ! " + _tun_name);
#endif //__linux__
    }

    m_boost_sd.assign(m_tun_fd);

    // init lwip
    lwip_init();

    // make addresses for netif
    ip4_addr_t addr;
    addr.addr = make_address_v4(_ipaddr).to_uint();

    ip4_addr_t netmask;
    netmask.addr = make_address_v4(_netmask).to_uint();

    ip4_addr_t gw;
    ip4_addr_set_any(&gw);

    // init netif
    if (netif_add(&m_netif, &addr, &netmask, &gw, nullptr, (netif_init_fn)static_netif_init_func,
          (netif_input_fn)static_netif_input_func) == nullptr) {
        throw runtime_error("[tun] netif_add failed");
    }

    // set netif up
    netif_set_up(&m_netif);

    // set netif link up, otherwise ip route will refuse to route
    netif_set_link_up(&m_netif);

    // set netif pretend TCP
    netif_set_pretend_tcp(&m_netif, 1);

    // set netif default
    netif_set_default(&m_netif);

    m_netif_configured = true;

    // init listener
    struct tcp_pcb* l = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (l == nullptr) {
        throw runtime_error("[tun] tcp_new_ip_type failed");
    }

    // bind listener
    if (tcp_bind_to_netif(l, "ho0") != ERR_OK) {
        tcp_close(l);
        throw runtime_error("[tun] tcp_bind_to_netif failed");
    }

    tcp_bind_netif(l, &m_netif);

    // listen listener
    m_tcp_listener = tcp_listen(l);
    if (m_tcp_listener == nullptr) {
        tcp_close(l);
        throw runtime_error("[tun] tcp_listen failed");
    }

    tcp_arg(m_tcp_listener, this);

    // setup listener accept handler
    tcp_accept(m_tcp_listener, static_listener_accept_func);

    async_read();
}

TUNDev::~TUNDev() {
    if (m_quitting) {
        return;
    }

    m_quitting = true;

    _log_with_date_time("[tun] destoryed, clear all tcp_clients: " + to_string(m_tcp_clients.size()) +
                          " udp_clients: " + to_string(m_udp_clients.size()),
      Log::INFO);

    for (auto& it : m_tcp_clients) {
        it->close_client(true, true);
    }
    m_tcp_clients.clear();

    for (auto& it : m_udp_clients) {
        it->set_close_from_tundev_flag();
        it->destroy();
    }
    m_udp_clients.clear();

    // free listener
    if (m_tcp_listener != nullptr) {
        tcp_close(m_tcp_listener);
    }

    // free netif
    if (m_netif_configured) {
        netif_remove(&m_netif);
        m_netif_configured = false;
    }

    tcp_remove(tcp_bound_pcbs);
    tcp_remove(tcp_active_pcbs);
    tcp_remove(tcp_tw_pcbs);

    if (m_tun_fd != -1 && !m_is_outside_tun_fd) {
        m_boost_sd.close();
    } else {
        m_boost_sd.release();
    }

    sm_tundev = nullptr;
}

err_t TUNDev::netif_init_func(struct netif* netif) const {
    netif->name[0] = 'h';
    netif->name[1] = 'o';
    netif->mtu     = m_mtu;
    netif->output  = static_netif_output_func;
    return ERR_OK;
}

err_t TUNDev::netif_input_func(struct pbuf* p, struct netif* inp) {
    uint8_t ip_version = 0;
    if (p->len > 0) {
        ip_version = (((uint8_t*)p->payload)[0] >> half_byte_shift_4_bits);
    }

    switch (ip_version) {
        case IPV4: {
            return ip_input(p, inp);
        } break;
        case IPV6: {
            // throw runtime_error("haven't supported ipv6");
        } break;
    }

    pbuf_free(p);
    return ERR_OK;
}

err_t TUNDev::netif_output_func(struct netif*, struct pbuf* p, const ip4_addr_t*) {
    if (m_quitting) {
        return ERR_OK;
    }

    if (p != nullptr) {
        if (p->next == nullptr && p->len <= m_mtu) {
            boost::system::error_code ec;
            m_boost_sd.write_some(boost::asio::buffer(p->payload, p->len), ec);
            if (ec) {
                output_debug_info_ec(ec);
            }
        } else {
            do {
                if (p->len > 0) {
                    auto* write_buff = boost::asio::buffer_cast<uint8_t*>(m_write_fill_buf.prepare(p->len));
                    memcpy(write_buff, (uint8_t*)p->payload, p->len);
                    m_write_fill_buf.commit(p->len);
                }
            } while ((p = p->next) != nullptr);

            write_to_tun();
        }
    }

    return ERR_OK;
}

bool TUNDev::is_in_ips(uint32_t ip, const Config::IPList& ips, const Config::IPSubnetList& subnet) {
    if (!ips.empty() && binary_search(ips.cbegin(), ips.cend(), ip)) {
        return true;
    }

    for (const auto& sub : subnet) {
        uint32_t net = ip & gsl::at(mask_values, sub.first);
        if (binary_search(sub.second.cbegin(), sub.second.cend(), net)) {
            return true;
        }
    }

    return false;
}

bool TUNDev::proxy_by_route(uint32_t ip) const {
    auto route = m_service->get_config().get_route();
    if (is_in_ips(ip, route._proxy_ips, route._proxy_ips_subnet)) {
        return true;
    }

    if (is_in_ips(ip, route._white_ips, route._white_ips_subnet)) {
        return false;
    }

    switch (route.proxy_type) {
        case Config::route_all:          // Controlled by route tables
        case Config::route_bypass_local: // Controlled by route tables
            return true;
        case Config::route_bypass_cn_mainland:
        case Config::route_bypass_local_and_cn_mainland: // Local ips Controlled by route tables
            return !is_in_ips(ip, route._cn_mainland_ips, route._cn_mainland_ips_subnet);
        case Config::route_gfwlist:
            return m_dns_server != nullptr && m_dns_server->is_ip_in_gfwlist(ip);
        case Config::route_cn_mainland:
            return is_in_ips(ip, route._cn_mainland_ips, route._cn_mainland_ips_subnet);
        default:
            throw logic_error("[dns] error proxy type: " + to_string((int)route.proxy_type));
    }
}

err_t TUNDev::listener_accept_func(struct tcp_pcb* newpcb, err_t err) {

    if (err != ERR_OK) {
        return err;
    }

    shared_ptr<TUNSession> session = nullptr;
    if (proxy_by_route(ntoh32(newpcb->local_ip.u_addr.ip4.addr))) {
        session = make_shared<TUNProxySession>(m_service, false);
    } else {
        session = make_shared<TUNLocalSession>(m_service, false);
    }
    auto tcp_client = make_shared<lwip_tcp_client>(newpcb, session, [this](lwip_tcp_client* client) {
        for (auto it = m_tcp_clients.begin(); it != m_tcp_clients.end(); it++) {
            if (it->get() == client) {
                m_tcp_clients.erase(it);
                break;
            }
        }
    });

    m_service->start_session(session, [this, session, tcp_client](boost::system::error_code ec) {
        if (!ec) {
            session->start();
            m_tcp_clients.emplace_back(tcp_client);
        } else {
            session->destroy();
            tcp_client->close_client(true);
        }
    });

    // start_session callback immediately and destory the session
    return session->is_destroyed() ? ERR_ABRT : ERR_OK;
}

void TUNDev::input_netif_packet(const uint8_t* data, uint16_t packet_len) {
    struct pbuf* p = pbuf_alloc(PBUF_RAW, packet_len, PBUF_POOL);
    if (p == nullptr) {
        _log_with_date_time("[tun] device read: pbuf_alloc failed", Log::ERROR);
        return;
    }

    // write packet to pbuf
    if (pbuf_take(p, (void*)data, packet_len) != ERR_OK) {
        _log_with_date_time("[tun] device read: pbuf_take failed", Log::ERROR);
        pbuf_free(p);
        return;
    }

    // pass pbuf to input
    if (m_netif.input(p, &m_netif) != ERR_OK) {
        _log_with_date_time("[tun] device read: input failed", Log::ERROR);
        pbuf_free(p);
        return;
    }
}

void TUNDev::parse_packet() {
    if (m_packet_parse_buff.empty()) {
        // need more byte for version
        return;
    }

    auto* data      = (uint8_t*)m_packet_parse_buff.c_str();
    auto data_len   = m_packet_parse_buff.length();
    auto ip_version = (data[0] >> half_byte_shift_4_bits) & half_byte_mask_0xF;

    if (ip_version == IPV4 || ip_version == IPV6) {

        uint16_t total_length = 0;

        if (ip_version == IPV4) {
            if (data_len < sizeof(struct ipv4_header)) {
                return;
            }
            struct ipv4_header ipv4_hdr;
            memcpy(&ipv4_hdr, data, sizeof(ipv4_hdr));
            total_length = ntoh16(ipv4_hdr.total_length);

            //_log_with_date_time("parse_packet length:" + to_string(data_len) + " ipv4 protocol: "
            //+
            // to_string((int)ipv4_hdr.protocol) + " total_length: " + to_string(total_length));

        } else {
            if (data_len < sizeof(struct ipv6_header)) {
                return;
            }
            struct ipv6_header ipv6_hdr;
            memcpy(&ipv6_hdr, data, sizeof(ipv6_hdr));
            total_length = ntoh16(ipv6_hdr.payload_length) + sizeof(ipv6_hdr);

            //_log_with_date_time("parse_packet length:" + to_string(data_len) + " ipv6 next header:
            //" +
            // to_string((int)ipv6_hdr.next_header) + " total_length: " + to_string(total_length));
        }

        if (total_length <= data_len) {
            auto result = try_to_process_udp_packet(data, (int)total_length);
            if (result == 0) {
                input_netif_packet(data, total_length);
            }

            if (data_len == total_length) {
                //_log_with_date_time("full packet process");
                m_packet_parse_buff.clear();
            } else {
                //_log_with_date_time("split packet process--------------");
                m_packet_parse_buff = m_packet_parse_buff.substr(total_length);
                parse_packet();
            }
        }

    } else {
        m_packet_parse_buff.clear();
    }
}

int TUNDev::handle_write_upd_data(const TUNSession* _session, string_view& data_str) {
    assert(_session->is_udp_forward_session());

    auto data_len = data_str.length();
    if (data_len == 0) {
        return 0;
    }
    const auto* data   = (const uint8_t*)data_str.data();
    auto header_length = (uint16_t)(sizeof(struct ipv4_header) + sizeof(struct udp_header));
    auto max_len       = min(numeric_limits<uint16_t>::max(), m_mtu);
    max_len            = max_len - header_length;
    if (data_len > max_len) {
        data_len = max_len;
    }

    auto local_endpoint  = _session->get_udp_local_endpoint();
    auto remote_endpoint = _session->get_udp_remote_endpoint();

    auto* local_addr  = (struct sockaddr_in*)local_endpoint.data();
    auto* remote_addr = (struct sockaddr_in*)remote_endpoint.data();

    // build IP header
    struct ipv4_header ipv4_hdr;
    ipv4_hdr.version4_ihl4           = IPV4_MAKE_VERSION_IHL(sizeof(ipv4_hdr));
    ipv4_hdr.ds                      = hton8(0);
    ipv4_hdr.total_length            = hton16(uint16_t(sizeof(ipv4_hdr) + sizeof(struct udp_header) + data_len));
    ipv4_hdr.identification          = hton16(0);
    ipv4_hdr.flags3_fragmentoffset13 = hton16(0);
    ipv4_hdr.ttl                     = hton8(Default_UDP_TTL);
    ipv4_hdr.protocol                = hton8(IPV4_PROTOCOL_UDP);
    ipv4_hdr.checksum                = hton16(0);
    ipv4_hdr.source_address          = (remote_addr->sin_addr.s_addr);
    ipv4_hdr.destination_address     = (local_addr->sin_addr.s_addr);
    ipv4_hdr.checksum                = ipv4_checksum(&ipv4_hdr, nullptr, 0);

    // build UDP header
    struct udp_header udp_hdr;
    udp_hdr.source_port = hton16(remote_endpoint.port());
    udp_hdr.dest_port   = hton16(local_endpoint.port());
    udp_hdr.length      = hton16(uint16_t(sizeof(udp_hdr) + data_len));
    udp_hdr.checksum    = hton16(0);
    udp_hdr.checksum =
      udp_checksum(&udp_hdr, data, (uint16_t)data_len, ipv4_hdr.source_address, ipv4_hdr.destination_address);

    // compose packet
    auto packat_length = header_length + data_len;
    auto* write_buf    = boost::asio::buffer_cast<uint8_t*>(m_write_fill_buf.prepare(packat_length));

    memcpy(write_buf, &ipv4_hdr, sizeof(ipv4_hdr));
    memcpy(write_buf + sizeof(ipv4_hdr), &udp_hdr, sizeof(udp_hdr));
    memcpy(write_buf + sizeof(ipv4_hdr) + sizeof(udp_hdr), data, data_len);

    m_write_fill_buf.commit(packat_length);

    _log_with_endpoint_ALL(local_endpoint, "<- " + remote_endpoint.address().to_string() + ":" +
                                             to_string(remote_endpoint.port()) + " length:" + to_string(data_len));

    write_to_tun();

    data_str = data_str.substr(data_len);
    if (data_str.length() > 0) {
        handle_write_upd_data(_session, data_str);
    }

    return 0;
}

int TUNDev::try_to_process_udp_packet(uint8_t* data, int data_len) {
    uint8_t ip_version = 0;
    if (data_len > 0) {
        ip_version = (data[0] >> half_byte_shift_4_bits) & half_byte_mask_0xF;
    }

    if (ip_version == IPV4) {
        // ignore non-UDP packets
        if (data_len < (int)sizeof(struct ipv4_header) ||
            data[offsetof(struct ipv4_header, protocol)] != IPV4_PROTOCOL_UDP) {
            return 0;
        }

        // parse IPv4 header
        struct ipv4_header ipv4_hdr;
        if (ipv4_check(data, data_len, &ipv4_hdr, &data, &data_len) == 0) {
            return 1;
        }

        // parse UDP
        struct udp_header udp_hdr;
        if (udp_check(data, data_len, &udp_hdr, &data, &data_len) == 0) {
            return 1;
        }

        // verify UDP checksum
        uint16_t checksum_in_packet = udp_hdr.checksum;
        udp_hdr.checksum            = 0;
        uint16_t checksum_computed =
          udp_checksum(&udp_hdr, data, data_len, ipv4_hdr.source_address, ipv4_hdr.destination_address);
        if (checksum_in_packet != checksum_computed) {
            return 1;
        }

        auto local_endpoint = udp::endpoint(
          make_address_v4((address_v4::uint_type)ntoh32(ipv4_hdr.source_address)), ntoh16(udp_hdr.source_port));

        auto remote_endpoint = udp::endpoint(
          make_address_v4((address_v4::uint_type)ntoh32(ipv4_hdr.destination_address)), ntoh16(udp_hdr.dest_port));

        _log_with_endpoint_ALL(local_endpoint, " -> " + remote_endpoint.address().to_string() + ":" +
                                                 to_string(remote_endpoint.port()) +
                                                 " [tun] length:" + to_string(data_len));

        for (auto& it : m_udp_clients) {
            if (it->try_to_process_udp(local_endpoint, remote_endpoint, data, data_len)) {
                return 1;
            }
        }

        shared_ptr<TUNSession> session = nullptr;
        if (proxy_by_route(ntoh32(ipv4_hdr.destination_address))) {
            session = make_shared<TUNProxySession>(m_service, true);
        } else {
            session = make_shared<TUNLocalSession>(m_service, true);
        }
        session->set_udp_connect(local_endpoint, remote_endpoint);
        session->set_write_to_lwip([this](const TUNSession* _se, string_view* _data) {
            assert(_data != nullptr);
            return handle_write_upd_data(_se, *_data);
        });

        session->set_close_callback([this](TUNSession* _session) {
            for (auto it = m_udp_clients.begin(); it != m_udp_clients.end(); it++) {
                if (it->get() == _session) {
                    m_udp_clients.erase(it);
                    break;
                }
            }
        });

        session->out_async_send(data, data_len, [](boost::system::error_code) {}); // send as buf
        m_udp_clients.emplace_back(session);

        _log_with_endpoint(local_endpoint,
          "TUNDev start to connected " + remote_endpoint.address().to_string() + ":" +
            to_string(remote_endpoint.port()),
          Log::INFO);

        m_service->start_session(session, [session, local_endpoint, remote_endpoint](boost::system::error_code ec) {
            if (!ec) {
                session->start();
            } else {
                output_debug_info_ec(ec);
                session->destroy();
            }
        });

        return 1;
    }

    if (ip_version == IPV6) {
        // TODO
    }

    return 0;
}

void TUNDev::write_to_tun() {
    if (m_quitting) {
        return;
    }

    while (m_write_fill_buf.size() > 0) {
        boost::system::error_code ec;
        size_t wrote = 0;
        if (m_write_fill_buf.size() > m_mtu) {
            auto copied = boost::asio::buffer_copy(m_writing_buf.prepare(m_mtu), m_write_fill_buf.data(), m_mtu);
            m_writing_buf.commit(copied);

            wrote = m_boost_sd.write_some(m_writing_buf.data(), ec);
            m_writing_buf.consume(m_writing_buf.size());
        } else {
            wrote = m_boost_sd.write_some(m_write_fill_buf.data(), ec);
        }

        if (!ec && wrote > 0) {
            m_write_fill_buf.consume(wrote);
        } else {
            m_write_fill_buf.consume(m_write_fill_buf.size());
        }
    }
}

void TUNDev::async_read() {
    m_sd_read_buffer.consume(m_sd_read_buffer.size());
    m_boost_sd.async_read_some(m_sd_read_buffer.prepare(m_mtu), [this](boost::system::error_code ec, size_t data_len) {
        if (m_quitting) {
            return;
        }

        if (!ec) {
            m_sd_read_buffer.commit(data_len);

            const auto* data = boost::asio::buffer_cast<const char*>(m_sd_read_buffer.data());
            m_packet_parse_buff.append(data, data_len);

            parse_packet();
        }

        async_read();
    });

    // sleep for test
    //::sleep(1);
}