# stream_server

stream_server:              # start Modbus TCP Server on port 502
  id: tcp
  port: 502


ethernet:                     # start ethernet if needed
  type: LAN8720
  phy_addr: 1
  mdc_pin: GPIO23
  mdio_pin: GPIO18
  clk_mode: GPIO0_IN
  power_pin: 5
  id: eth1


global:                        # set globals for data type
  - id: tcp_server_cmd         # tcp_server_cmd only needs if you want to close the Server by hand 
    type: uint16_t
    restore_value: yes
    initial_value: '0'

  - id: phy_connected
    type: uint16_t
    restore_value: no
    initial_value: 'false'
    
  - id: relay1_cmd
    type: uint16_t
    restore_value: yes
    initial_value: '0'


interval:                         # holds function in loop
  - interval: 200ms
    then:                          # setup write calls from Modbus TCP Server
      - lambda: |- 
              static bool initialized = false;
              if (!initialized) {
                // Callback for write action set
                id(tcp).setWriteCallback([](uint8_t unit, uint8_t function, uint16_t address, uint16_t value) -> bool {
                  
                  // === safety check at beginning ===
                  if (id(phy_connected) != 1 || id(tcp_server_cmd) != 0) {
                    ESP_LOGW("modbus", "Write rejected: phy=%d tcp_server=%d", id(phy_connected), id(tcp_server_cmd));
                    return false;  // write cancled
                  }
                  
                  ESP_LOGI("modbus", "Write callback: unit=%d func=%d addr=0x%x value=%d", unit, function, address, value);
                  
                  // Unit 1
                  if (unit == 1) {
                    // Coils (Write Single Coil - Function 5)
                    if (function == 1 || function == 5) {
                      if (address == 101) {
                        id(relay1_cmd) = value;
                        return true;
                      }
                    }

                    // Holding Registers (Write Single/Multiple Register - Function 6/16)                
                    if (function == 6 || function == 16) {
                      // Relay 1
                      if (address == 101) {
                        id(relay1_cmd) = value;
                        return true;
                      }
                    }
                  }
                
                  ESP_LOGW("modbus", "Write to unknown address: unit=%d addr=0x%x", unit, address);
                  return false; // Adress not available
                });
                
                initialized = true;
                ESP_LOGI("modbus", "Modbus server initialized with write support");
              }


  - interval: 300ms
    then:                            # checks connection before setting register for read from Modbus TCP Server
      - lambda: |-
          if (id(phy_connected) == 1 && id(tcp_server_cmd) == 0) {
            id(tcp).setRegisterUint16(1, 3, 101, id(relay1_cmd), 0);
            }
        
 - interval: 10s
    then:                              # checks Network Connection is available
      - lambda: |-
          bool is_connected = id(eth1).is_connected();
          
          if (is_connected && id(phy_connected) == 0) {
            id(phy_connected) = 1;
          }
          else if (!is_connected && id(phy_connected) == 1) {
            id(phy_connected) = 0;
          }


  - interval: 10s
    then:                            # checks Network Connection is available and TCP Server is not deactivated
      - lambda: |-
          static bool last_enabled = true;
          bool should_enable = (id(phy_connected) == 1 && id(tcp_server_cmd) == 0);
          
          if (should_enable != last_enabled) {
            if (should_enable) {
              id(tcp).enable_server();
            } else {
              id(tcp).disable_server();
            }
            last_enabled = should_enable;
          }
