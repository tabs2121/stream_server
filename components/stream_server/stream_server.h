/* Copyright (C) 2020-2022 Oxan van Leeuwen
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
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "esphome/core/component.h"
#include "esphome/components/socket/socket.h"

#include <vector>
#include <map>
#include <memory>
#include <string>
#include <functional>

class StreamServerComponent : public esphome::Component {
public:
    void setup() override;
    void loop() override;
    void dump_config() override;
    void on_shutdown() override;

    float get_setup_priority() const override {
        return esphome::setup_priority::AFTER_WIFI;
    }

    // Konfiguration
    void set_port(uint16_t port) { port_ = port; }
    void set_no_tcp_delay(bool on) { notcpdelay = on; }

    // 🔁 Bidirektionale API
    void send_to_all(const uint8_t* data, size_t len);
    void send_to_client(const std::string& id, const uint8_t* data, size_t len);

    // Callback bei empfangenen Daten
    std::function<void(const uint8_t*, size_t, const std::string&)> on_data_received;

protected:
    struct Client {
        Client(std::unique_ptr<esphome::socket::Socket> socket, std::string id);

        std::unique_ptr<esphome::socket::Socket> socket;
        std::string identifier;
        bool disconnected{ false };

        uint8_t buffer[256];
        size_t offset{ 0 };

        std::vector<uint8_t> send_buffer;
        uint32_t last_activity{ 0 };
    };

    void accept_();
    void read_();
    void write_();
    void cleanup_();

    std::unique_ptr<esphome::socket::Socket> socket_;
    uint16_t port_{ 502 };
    bool notcpdelay{ true };

    std::vector<Client> clients_;
};