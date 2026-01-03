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

static const char* TAG = "streamserver";

using namespace esphome;

void StreamServerComponent::setup() {
    ESP_LOGCONFIG(TAG, "Setting up stream server...");

    struct sockaddr_in bind_addr = {
        .sin_len = sizeof(struct sockaddr_in),
        .sin_family = AF_INET,
        .sin_port = htons(this->port_),
        .sin_addr = {
            .s_addr = ESPHOME_INADDR_ANY,
        }
    };

    this->socket_ = socket::socket(AF_INET, SOCK_STREAM, PF_INET);

    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 20000;

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

void StreamServerComponent::accept() {
    struct sockaddr_in client_addr;
    socklen_t client_addrlen = sizeof(struct sockaddr_in);
    std::unique_ptr<socket::Socket> socket = this->socket_->accept(reinterpret_cast<struct sockaddr*>(&client_addr), &client_addrlen);
    if (!socket)
        return;

    socket->setblocking(false);
    int on = this->notcpdelay;
    socket->setsockopt(IPPROTO_TCP, TCP_NODELAY, &on, sizeof(int));
    std::string identifier = socket->getpeername();
    this->clients_.emplace_back(std::move(socket), identifier);
    ESP_LOGI(TAG, "New client #%d connected from %s", this->get_client_count(), identifier.c_str());
}

void StreamServerComponent::cleanup() {
    uint32_t now = esphome::millis();
    for (Client& client : this->clients_) {
        if (client.last_activity + this->max_inactivity_time < now) {
            client.disconnected = true;
            ESP_LOGW(TAG, "Client %s inactive for %d s", client.identifier.c_str(), (now - client.last_activity) / 1000);
        }
    }

    int count = this->get_client_count();
    auto discriminator = [](const Client& client) { return !client.disconnected; };
    auto last_client = std::partition(this->clients_.begin(), this->clients_.end(), discriminator);
    this->clients_.erase(last_client, this->clients_.end());
    if (count != this->get_client_count())
        ESP_LOGI(TAG, "%d clients connected", this->get_client_count());
}

void StreamServerComponent::read() {
}

void StreamServerComponent::write() {
    ssize_t len;
    for (Client& client : this->clients_) {
        // Header lesen (6 Bytes)
        if (client.offset < 6) {
            while ((len = client.socket->read(((uint8_t*)&client.buffer) + client.offset, 6 - client.offset)) > 0) {
                client.offset = client.offset + len;
                App.feed_wdt();
                if (client.offset >= 6)
                    break;
            }
            if (len == 0) {
                ESP_LOGI(TAG, "Client %s sent no header", client.identifier.c_str());
                client.disconnected = true;
                continue;
            }
            if (len < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    ESP_LOGE(TAG, "Client header, error %d: %s", errno, strerror(errno));
                    client.disconnected = true;
                }
                continue;
            }
            if (client.offset < 6)
                continue;
        }

        uint16_t msglen = client.buffer[4] << 8 | client.buffer[5];
        if (msglen > 250) {
            ESP_LOGE(TAG, "Message length %d > 250", msglen);
            client.disconnected = true;
            continue;
        }

        // Message lesen
        if (client.offset < 6 + msglen) {
            while ((len = client.socket->read(((uint8_t*)&client.buffer) + client.offset, (6 + msglen) - client.offset)) > 0) {
                client.offset = client.offset + len;
                App.feed_wdt();
                if (client.offset >= 6 + msglen)
                    break;
            }
            if (len == 0) {
                ESP_LOGI(TAG, "Client %s sent no message", client.identifier.c_str());
                client.disconnected = true;
                continue;
            }
            if (len < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    ESP_LOGE(TAG, "Client data, error %d: %s", errno, strerror(errno));
                    client.disconnected = true;
                }
                continue;
            }
            if (client.offset < 6 + msglen)
                continue;
        }

        client.last_activity = esphome::millis();
        ESP_LOGD(TAG, "Received %d bytes %s", client.offset, format_hex(client.buffer, client.offset).c_str());

        uint16_t transaction = client.buffer[0] << 8 | client.buffer[1];
        uint16_t protocol = client.buffer[2] << 8 | client.buffer[3];
        uint8_t unit = client.buffer[6];
        uint8_t function = client.buffer[7];

        int error = 0;
        if (protocol != 0) {
            error = 4;
            ESP_LOGE(TAG, "Protocol %d", protocol);
        }

        // READ HOLDING REGISTERS (0x03) oder READ INPUT REGISTERS (0x04)
        if ((function == 3 || function == 4) && client.offset == 12 && error == 0) {
            uint16_t address = client.buffer[8] << 8 | client.buffer[9];
            uint16_t count = client.buffer[10] << 8 | client.buffer[11];

            ESP_LOGD(TAG, "READ: Transaction %d unit %d function %d address 0x%x count %d",
                transaction, unit, function, address, count);

            if (count > 125) {
                ESP_LOGE(TAG, "Count %d > 125", count);
                error = 3;
            }

            uint8_t response[9 + count * 2];

            if (error == 0) {
                response[0] = client.buffer[0];
                response[1] = client.buffer[1];
                response[2] = client.buffer[2];
                response[3] = client.buffer[3];
                response[4] = (3 + count * 2) >> 8;
                response[5] = (3 + count * 2) & 0xFF;
                response[6] = unit;
                response[7] = function;
                response[8] = count * 2;

                for (int a = address; a < address + count; a++) {
                    int32_t val = getRegister(unit, function, a, a == address);
                    if (val > 0x10000) {
                        if (a == address) {
                            error = val & 0xF;
                            break;
                        }
                        val = 0;
                    }
                    response[9 + (a - address) * 2] = val >> 8;
                    response[9 + (a - address) * 2 + 1] = val & 0xFF;
                }
            }

            if (error == 0) {
                ESP_LOGD(TAG, "Sending response %s", format_hex(response, sizeof(response)).c_str());
                ssize_t sent = client.socket->write(response, sizeof(response));
                if (sent != sizeof(response)) {
                    ESP_LOGE(TAG, "Sending response failed");
                    client.disconnected = true;
                }
            }
            else {
                response[4] = 0;
                response[5] = 3;
                response[7] = function | 0x80;
                response[8] = error;
                ESP_LOGE(TAG, "Sending error %d", error);
                client.socket->write(response, 9);
            }
        }
        // WRITE SINGLE COIL (0x05)
        else if (function == 5 && client.offset == 12 && error == 0) {
            uint16_t address = client.buffer[8] << 8 | client.buffer[9];
            uint16_t value = client.buffer[10] << 8 | client.buffer[11];

            ESP_LOGI(TAG, "WRITE COIL: Transaction %d unit %d address 0x%x value 0x%x",
                transaction, unit, address, value);

            // Coil-Wert normalisieren (0x0000 = OFF, 0xFF00 = ON)
            if (value != 0x0000 && value != 0xFF00) {
                error = 3; // Ungültiger Wert
            }
            else {
                // Als Register speichern (0 oder 1)
                uint16_t normalized_value = (value == 0xFF00) ? 1 : 0;
                if (!setRegister(unit, 1, address, normalized_value)) {
                    error = 2; // Adresse nicht verfügbar
                }
            }

            uint8_t response[12];
            response[0] = client.buffer[0];
            response[1] = client.buffer[1];
            response[2] = client.buffer[2];
            response[3] = client.buffer[3];

            if (error == 0) {
                // Erfolg: Echo der Anfrage
                response[4] = 0;
                response[5] = 6;
                response[6] = unit;
                response[7] = function;
                response[8] = client.buffer[8];
                response[9] = client.buffer[9];
                response[10] = client.buffer[10];
                response[11] = client.buffer[11];
                ESP_LOGI(TAG, "WRITE COIL SUCCESS");
                client.socket->write(response, 12);
            }
            else {
                response[4] = 0;
                response[5] = 3;
                response[6] = unit;
                response[7] = function | 0x80;
                response[8] = error;
                ESP_LOGE(TAG, "WRITE COIL ERROR %d", error);
                client.socket->write(response, 9);
            }
        }
        // WRITE SINGLE REGISTER (0x06)
        else if (function == 6 && client.offset == 12 && error == 0) {
            uint16_t address = client.buffer[8] << 8 | client.buffer[9];
            uint16_t value = client.buffer[10] << 8 | client.buffer[11];

            ESP_LOGI(TAG, "WRITE REGISTER: Transaction %d unit %d address 0x%x value %d",
                transaction, unit, address, value);

            if (!setRegister(unit, 6, address, value)) {
                error = 2; // Adresse nicht verfügbar
            }

            uint8_t response[12];
            response[0] = client.buffer[0];
            response[1] = client.buffer[1];
            response[2] = client.buffer[2];
            response[3] = client.buffer[3];

            if (error == 0) {
                // Erfolg: Echo der Anfrage
                response[4] = 0;
                response[5] = 6;
                response[6] = unit;
                response[7] = function;
                response[8] = client.buffer[8];
                response[9] = client.buffer[9];
                response[10] = client.buffer[10];
                response[11] = client.buffer[11];
                ESP_LOGI(TAG, "WRITE REGISTER SUCCESS");
                client.socket->write(response, 12);
            }
            else {
                response[4] = 0;
                response[5] = 3;
                response[6] = unit;
                response[7] = function | 0x80;
                response[8] = error;
                ESP_LOGE(TAG, "WRITE REGISTER ERROR %d", error);
                client.socket->write(response, 9);
            }
        }
        // WRITE MULTIPLE REGISTERS (0x10)
        else if (function == 16 && error == 0) {
            uint16_t address = client.buffer[8] << 8 | client.buffer[9];
            uint16_t count = client.buffer[10] << 8 | client.buffer[11];
            uint8_t byte_count = client.buffer[12];

            ESP_LOGI(TAG, "WRITE MULTIPLE: Transaction %d unit %d address 0x%x count %d",
                transaction, unit, address, count);

            if (byte_count != count * 2 || client.offset != 13 + byte_count) {
                error = 3;
            }
            else if (count > 123) {
                error = 3;
            }
            else {
                for (int i = 0; i < count && error == 0; i++) {
                    uint16_t value = client.buffer[13 + i * 2] << 8 | client.buffer[13 + i * 2 + 1];
                    if (!setRegister(unit, 16, address + i, value)) {
                        error = 2;
                        break;
                    }
                }
            }

            uint8_t response[12];
            response[0] = client.buffer[0];
            response[1] = client.buffer[1];
            response[2] = client.buffer[2];
            response[3] = client.buffer[3];

            if (error == 0) {
                response[4] = 0;
                response[5] = 6;
                response[6] = unit;
                response[7] = function;
                response[8] = client.buffer[8];
                response[9] = client.buffer[9];
                response[10] = client.buffer[10];
                response[11] = client.buffer[11];
                ESP_LOGI(TAG, "WRITE MULTIPLE SUCCESS");
                client.socket->write(response, 12);
            }
            else {
                response[4] = 0;
                response[5] = 3;
                response[6] = unit;
                response[7] = function | 0x80;
                response[8] = error;
                ESP_LOGE(TAG, "WRITE MULTIPLE ERROR %d", error);
                client.socket->write(response, 9);
            }
        }
        else {
            ESP_LOGE(TAG, "Unsupported function %d or unexpected length %d", function, client.offset);
            uint8_t response[9];
            response[0] = client.buffer[0];
            response[1] = client.buffer[1];
            response[2] = 0;
            response[3] = 0;
            response[4] = 0;
            response[5] = 3;
            response[6] = unit;
            response[7] = function | 0x80;
            response[8] = 1; // Illegal function
            client.socket->write(response, 9);
        }

        client.offset = 0;
    }
}

bool StreamServerComponent::setRegister(uint8_t unit, uint8_t function, uint16_t address, uint16_t value) {
    // Callback aufrufen, wenn vorhanden
    if (this->write_callback_) {
        return this->write_callback_(unit, function, address, value);
    }

    // Alternativ: Direkt in Register schreiben (für einfache Implementierung)
    registers_[{unit, function, address}] = { value, 0 };
    ESP_LOGD(TAG, "Register set: unit=%d func=%d addr=0x%x value=%d", unit, function, address, value);
    return true;
}

void StreamServerComponent::setRegisterUint16(uint8_t unit, uint8_t function, uint16_t address, uint16_t value, uint16_t maxage) {
    registers_[{unit, function, address}] = { value, maxage == 0 ? 0 : esphome::millis() + maxage };
}

void StreamServerComponent::setRegisterSint32(uint8_t unit, uint8_t function, uint16_t address, int32_t value, uint16_t maxage) {
    uint32_t expiration = (maxage == 0 ? 0 : esphome::millis() + maxage);
    registers_[{ unit, function, address}] = { (uint16_t)(value & 0xFFFF), expiration };
    registers_[{ unit, function, (uint16_t)(address + 1) }] = { (uint16_t)(value >> 16), expiration };
}

int32_t StreamServerComponent::getRegister(uint8_t unit, uint8_t function, uint16_t address, bool main) {
    if (function != 3 && function != 4) {
        ESP_LOGW(TAG, "Function %x not available", function);
        return 0x10001;
    }

    auto reg = registers_.find({ unit, function, address });
    if (reg != registers_.end()) {
        if (reg->second.expiration == 0 || reg->second.expiration > esphome::millis())
            return reg->second.value;
        else {
            ESP_LOGW(TAG, "Value at address %x expired %d ms ago", address, esphome::millis() - reg->second.expiration);
            return 0x10002;
        }
    }
    else {
        if (main)
            ESP_LOGW(TAG, "Address %x not available", address);
        return 0x10002;
    }
}

void StreamServerComponent::dump_config() {
    ESP_LOGCONFIG(TAG, "Stream Server:");
    std::string ip_str = "";
    for (auto& ip : network::get_ip_addresses()) {
        if (ip.is_set())
            ip_str += " " + ip.str();
    }
    ESP_LOGCONFIG(TAG, "  Address:%s", ip_str.c_str());
    ESP_LOGCONFIG(TAG, "  Port: %u", this->port_);
}

void StreamServerComponent::on_shutdown() {
    for (const Client& client : this->clients_)
        client.socket->shutdown(SHUT_RDWR);
}

StreamServerComponent::Client::Client(std::unique_ptr<esphome::socket::Socket> socket, std::string identifier)
    : socket(std::move(socket)), identifier{ identifier }
{
}