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

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/core/util.h"
#include "esphome/core/application.h"
#include "esphome/components/network/util.h"
#include "esphome/components/socket/socket.h"

#include <algorithm>
#include <cstring>

static const char* TAG = "streamserver";

using namespace esphome;

void StreamServerComponent::setup() {
    ESP_LOGCONFIG(TAG, "Setting up stream server...");

    struct sockaddr_in bind_addr = {
        .sin_len = sizeof(struct sockaddr_in),
        .sin_family = AF_INET,
        .sin_port = htons(this->port_),
        .sin_addr = {.s_addr = ESPHOME_INADDR_ANY }
    };

    this->socket_ = socket::socket(AF_INET, SOCK_STREAM, PF_INET);

    struct timeval timeout { 0, 20000 }; // 20ms

#ifdef ESP8266
    this->socket_->setsockopt(SOL_SOCKET, LWIP_SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));
#else
    this->socket_->setsockopt(SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));
#endif

    this->socket_->bind(reinterpret_cast<struct sockaddr*>(&bind_addr), sizeof(struct sockaddr_in));
    this->socket_->listen(8);
}

void StreamServerComponent::loop() {
    this->accept();
    this->read();
    this->write();
    this->cleanup();
}

// -------------------- Client Management --------------------

void StreamServerComponent::accept() {
    struct sockaddr_in client_addr;
    socklen_t client_addrlen = sizeof(client_addr);
    std::unique_ptr<socket::Socket> sock = this->socket_->accept(reinterpret_cast<struct sockaddr*>(&client_addr), &client_addrlen);
    if (!sock) return;

    sock->setblocking(false);
    int on = this->notcpdelay;
    sock->setsockopt(IPPROTO_TCP, TCP_NODELAY, &on, sizeof(int));

    std::string identifier = sock->getpeername();
    this->clients_.emplace_back(std::move(sock), identifier);

    ESP_LOGI(TAG, "New client #%d connected from %s", this->get_client_count(), identifier.c_str());
}

void StreamServerComponent::cleanup() {
    uint32_t now = esphome::millis();
    for (auto& client : this->clients_) {
        if (client.last_activity + this->max_inactivity_time < now)
            client.disconnected = true;
    }

    auto discriminator = [](const Client& c) { return !c.disconnected; };
    auto last_client = std::partition(this->clients_.begin(), this->clients_.end(), discriminator);
    this->clients_.erase(last_client, this->clients_.end());
}

// -------------------- Reading --------------------

void StreamServerComponent::read() {
    for (auto& client : this->clients_) {
        ssize_t len = client.socket->read(client.buffer + client.offset, sizeof(client.buffer) - client.offset);
        if (len > 0) {
            client.offset += len;
            client.last_activity = esphome::millis();

            // Daten an Callback weiterleiten
            if (this->on_data_received)
                this->on_data_received(client.buffer, client.offset, client.identifier);

            client.offset = 0; // Buffer zurücksetzen, du kannst hier auch Streaming-spezifisch anpassen
        }
    }
}

// -------------------- Writing --------------------

void StreamServerComponent::write() {
    for (auto& client : this->clients_) {
        if (!client.send_buffer.empty()) {
            ssize_t sent = client.socket->write(client.send_buffer.data(), client.send_buffer.size());
            if (sent > 0)
                client.send_buffer.erase(client.send_buffer.begin(), client.send_buffer.begin() + sent);
            else if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
                client.disconnected = true;
        }
    }
}

void StreamServerComponent::send_to_all(const uint8_t* data, size_t len) {
    for (auto& client : this->clients_)
        client.send_buffer.insert(client.send_buffer.end(), data, data + len);
}

void StreamServerComponent::send_to_client(const std::string& client_id, const uint8_t* data, size_t len) {
    for (auto& client : this->clients_) {
        if (client.identifier == client_id)
            client.send_buffer.insert(client.send_buffer.end(), data, data + len);
    }
}

// -------------------- Registers (Modbus-like) --------------------

void StreamServerComponent::setRegisterUint16(uint8_t unit, uint8_t function, uint16_t address, uint16_t value, uint16_t maxage) {
    registers_[{unit, function, address}] = { value, maxage == 0 ? 0 : esphome::millis() + maxage };
}

void StreamServerComponent::setRegisterSint32(uint8_t unit, uint8_t function, uint16_t address, int32_t value, uint16_t maxage) {
    uint32_t expiration = (maxage == 0 ? 0 : esphome::millis() + maxage);

    registers_[{ unit, function, address}] = { (uint16_t)(value & 0xFFFF), expiration };
    registers_[{ unit, function, (uint16_t)(address + 1) }] = { (uint16_t)(value >> 16), expiration };
}

int32_t StreamServerComponent::getRegister(uint8_t unit, uint8_t function, uint16_t address, bool main) {
    if (function != 3 && function != 4) return 0x10001;

    auto reg = registers_.find({ unit, function, address });
    if (reg != registers_.end())
        if (reg->second.expiration == 0 || reg->second.expiration > esphome::millis())
            return reg->second.value;
        else return 0x10002;
    else return main ? 0x10002 : 0x10002;
}

// -------------------- Config & Shutdown --------------------

void StreamServerComponent::dump_config() {
    ESP_LOGCONFIG(TAG, "Stream Server:");
    std::string ip_str = "";
    for (auto& ip : network::get_ip_addresses())
        if (ip.is_set())
            ip_str += " " + ip.str();
    ESP_LOGCONFIG(TAG, "  Address:%s", ip_str.c_str());
    ESP_LOGCONFIG(TAG, "  Port: %u", this->port_);
}

void StreamServerComponent::on_shutdown() {
    for (auto& client : this->clients_)
        client.socket->shutdown(SHUT_RDWR);
}

// -------------------- Client Constructor --------------------

StreamServerComponent::Client::Client(std::unique_ptr<esphome::socket::Socket> socket, std::string identifier)
    : socket(std::move(socket)), identifier{ identifier } {
}
