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

#include "stream_server.h"

#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include "esphome/components/network/util.h"

#include <algorithm>
#include <cstring>

static const char* TAG = "streamserver";

using namespace esphome;

void StreamServerComponent::setup() {
    ESP_LOGCONFIG(TAG, "Starting bidirectional TCP stream server");

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    addr.sin_addr.s_addr = ESPHOME_INADDR_ANY;

    socket_ = socket::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    timeval timeout{ 0, 20000 };
    socket_->setsockopt(SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    socket_->bind((sockaddr*)&addr, sizeof(addr));
    socket_->listen(4);
}

void StreamServerComponent::loop() {
    accept_();
    read_();
    write_();
    cleanup_();
}

void StreamServerComponent::accept_() {
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);

    auto sock = socket_->accept((sockaddr*)&addr, &len);
    if (!sock)
        return;

    sock->setblocking(false);
    int on = notcpdelay;
    sock->setsockopt(IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));

    std::string id = sock->getpeername();
    clients_.emplace_back(std::move(sock), id);

    ESP_LOGI(TAG, "Client connected: %s", id.c_str());
}

void StreamServerComponent::read_() {
    for (auto& c : clients_) {
        ssize_t r = c.socket->read(c.buffer, sizeof(c.buffer));
        if (r <= 0)
            continue;

        c.last_activity = millis();

        ESP_LOGD(TAG, "RX %d bytes from %s", r, c.identifier.c_str());
        ESP_LOGV(TAG, "%s", format_hex(c.buffer, r).c_str());

        // 🔁 Callback
        if (on_data_received)
            on_data_received(c.buffer, r, c.identifier);

        // 🔁 AUTOMATISCHE ANTWORT (Echo + ACK)
        const char* ack = "OK\n";
        c.send_buffer.insert(c.send_buffer.end(), ack, ack + strlen(ack));
    }
}

void StreamServerComponent::write_() {
    for (auto& c : clients_) {
        if (c.send_buffer.empty())
            continue;

        ssize_t w = c.socket->write(c.send_buffer.data(), c.send_buffer.size());
        if (w > 0)
            c.send_buffer.erase(c.send_buffer.begin(), c.send_buffer.begin() + w);
    }
}

void StreamServerComponent::cleanup_() {
    clients_.erase(
        std::remove_if(clients_.begin(), clients_.end(),
            [](auto& c) { return c.disconnected; }),
        clients_.end());
}

void StreamServerComponent::send_to_all(const uint8_t* data, size_t len) {
    for (auto& c : clients_)
        c.send_buffer.insert(c.send_buffer.end(), data, data + len);
}

void StreamServerComponent::send_to_client(const std::string& id, const uint8_t* data, size_t len) {
    for (auto& c : clients_)
        if (c.identifier == id)
            c.send_buffer.insert(c.send_buffer.end(), data, data + len);
}

void StreamServerComponent::dump_config() {
    ESP_LOGCONFIG(TAG, "Bidirectional TCP Stream Server");
    ESP_LOGCONFIG(TAG, "  Port: %u", port_);
}

void StreamServerComponent::on_shutdown() {
    for (auto& c : clients_)
        c.socket->shutdown(SHUT_RDWR);
}

StreamServerComponent::Client::Client(std::unique_ptr<socket::Socket> sock, std::string id)
    : socket(std::move(sock)), identifier(std::move(id)) {
    last_activity = millis();
}
