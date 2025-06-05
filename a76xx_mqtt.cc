#include "a76xx_mqtt.h"
#include <esp_log.h>

static const char *TAG = "Ml307Mqtt";



Ml307Mqtt::Ml307Mqtt(Ml307AtModem& modem, int mqtt_id) : modem_(modem), mqtt_id_(mqtt_id) {
    event_group_handle_ = xEventGroupCreate();

    command_callback_it_ = modem_.RegisterCommandResponseCallback(
        [this](const std::string command, const std::vector<AtArgumentValue>& arguments) {
            
            // 处理 MQTT 收到消息的 URC：+CMQTTRECV: <id>, "topic", <qos>, "payload"
            if (command == "CMQTTRECV" && arguments.size() >= 4) {
                int id = arguments[0].int_value;
                std::string topic = arguments[1].string_value;
                std::string payload = arguments[3].string_value;

                if (id == mqtt_id_ && on_message_callback_) {
                    on_message_callback_(topic, payload);
                }
            }

            // 可选：处理连接成功的回调 +CMQTTCONNURC: <id>, <result>
            else if (command == "CMQTTCONNECT" && arguments.size() >= 2) {
                int id = arguments[0].int_value;
                int result = arguments[1].int_value;

                if (id == mqtt_id_) {
                    if (result == 0) {
                        connected_ = true;
                        xEventGroupSetBits(event_group_handle_, MQTT_CONNECTED_EVENT);
                    } else {
                        connected_ = false;
                        xEventGroupSetBits(event_group_handle_, MQTT_DISCONNECTED_EVENT);
                        if (on_disconnected_callback_) {
                            on_disconnected_callback_();
                        }
                    }
                    ESP_LOGI(TAG, "MQTT connect result: %s", ErrorToString(result).c_str());
                }
            }

            // 可选：断开连接 +CMQTTDISC: <id>, <result>
            else if (command == "CMQTTDISC" && arguments.size() >= 2) {
                int id = arguments[0].int_value;
                int result = arguments[1].int_value;

                if (id == mqtt_id_) {
                    connected_ = false;
                    xEventGroupSetBits(event_group_handle_, MQTT_DISCONNECTED_EVENT);
                    if (on_disconnected_callback_) {
                        on_disconnected_callback_();
                    }
                    ESP_LOGI(TAG, "MQTT disconnected: %d", result);
                }
            }

            else if (command == "CMQTTCONNLOST") {
                connected_ = false;
                xEventGroupSetBits(event_group_handle_, MQTT_DISCONNECTED_EVENT);
                if (on_disconnected_callback_) {
                    on_disconnected_callback_();
                }
            }

            else if (command == "CMQTTPUB" && arguments.size() >= 2) {
                int id = arguments[0].int_value;
                int result = arguments[1].int_value;
                if (id == mqtt_id_) {
                    if (result == 0) {
                        ESP_LOGI(TAG, "MQTT publish success");
                    } else {
                        ESP_LOGW(TAG, "MQTT publish failed: code=%d", result);
                    }
                }
            }
            //处理网络掉线
            // else if (command == "CGEV" && arguments.size() >= 1) {
            //     std::string event = arguments[0].string_value;
            //     if (event.find("NW DEACT") != std::string::npos || event.find("ME DETACH") != std::string::npos) {
            //         connected_ = false;
            //         xEventGroupSetBits(event_group_handle_, MQTT_DISCONNECTED_EVENT);
            //         if (on_disconnected_callback_) {
            //             on_disconnected_callback_();
            //         }
            //     }
            // }

            // 其他未处理命令
            else {
                ESP_LOGI(TAG, "Unhandled MQTT URC: %s", command.c_str());
            }
        }
    );
}


// Ml307Mqtt::Ml307Mqtt(Ml307AtModem& modem, int mqtt_id) : modem_(modem), mqtt_id_(mqtt_id) {
//     event_group_handle_ = xEventGroupCreate();

//     command_callback_it_ = modem_.RegisterCommandResponseCallback([this](const std::string command, const std::vector<AtArgumentValue>& arguments) {
//         // if (command == "MQTTURC" && arguments.size() >= 2) {
//         if (command == "CMQTTRECV" && arguments.size() >= 4) {//+CMQTTRECV: 0,"SafePower/sub",3,"123"
//             if (arguments[1].int_value == mqtt_id_) {
//                 auto type = arguments[0].string_value;
//                 if (type == "conn") {
//                     if (arguments[2].int_value == 0) {
//                         xEventGroupSetBits(event_group_handle_, MQTT_CONNECTED_EVENT);
//                     } else {
//                         if (connected_) {
//                             connected_ = false;
//                             if (on_disconnected_callback_) {
//                                 on_disconnected_callback_();
//                             }
//                         }
//                         xEventGroupSetBits(event_group_handle_, MQTT_DISCONNECTED_EVENT);
//                     }
//                     ESP_LOGI(TAG, "MQTT connection state: %s", ErrorToString(arguments[2].int_value).c_str());
//                 } else if (type == "suback") {
//                 } else if (type == "publish" && arguments.size() >= 7) {
//                     auto topic = arguments[3].string_value;
//                     if (arguments[4].int_value == arguments[5].int_value) {
//                         if (on_message_callback_) {
//                             on_message_callback_(topic, modem_.DecodeHex(arguments[6].string_value));
//                         }
//                     } else {
//                         message_payload_.append(modem_.DecodeHex(arguments[6].string_value));
//                         if (message_payload_.size() >= arguments[4].int_value && on_message_callback_) {
//                             on_message_callback_(topic, message_payload_);
//                             message_payload_.clear();
//                         }
//                     }
//                 } else {
//                     ESP_LOGI(TAG, "unhandled MQTT event: %s", type.c_str());
//                 }
//             }
//         } else if (command == "MQTTSTATE" && arguments.size() == 1) {
//             connected_ = arguments[0].int_value != 3;
//             xEventGroupSetBits(event_group_handle_, MQTT_INITIALIZED_EVENT);
//         }
//     });
// }

Ml307Mqtt::~Ml307Mqtt() {
    modem_.UnregisterCommandResponseCallback(command_callback_it_);
    vEventGroupDelete(event_group_handle_);
}

bool Ml307Mqtt::Connect(const std::string broker_address, int broker_port,
                        const std::string client_id, const std::string username,
                        const std::string password) {
    broker_address_ = broker_address;
    broker_port_ = broker_port;
    client_id_ = client_id;
    username_ = username;
    password_ = password;

    EventBits_t bits;

    if (IsConnected()) {
        Disconnect();
        bits = xEventGroupWaitBits(event_group_handle_, MQTT_DISCONNECTED_EVENT,
                                   pdTRUE, pdFALSE, pdMS_TO_TICKS(MQTT_CONNECT_TIMEOUT_MS));
        if (!(bits & MQTT_DISCONNECTED_EVENT)) {
            ESP_LOGE(TAG, "Failed to disconnect previous MQTT");
            return false;
        }
    }
    // === Step 1: 启动 MQTT 服务 ===
    if (!modem_.Command("AT+CMQTTSTART", 5000)) {
        ESP_LOGE(TAG, "AT+CMQTTSTART failed");
        return false;
    }
    // === Step 2: 分配客户端 ===
    std::string accq_cmd = "AT+CMQTTACCQ=0,\"" + client_id + "\"";
    if (!modem_.Command(accq_cmd)) {
        ESP_LOGE(TAG, "AT+CMQTTACCQ failed");
        return false;
    }
    // === Step 3: 配置主题模式（使用 argtopic 参数方式 + payload_len 上报）===
    if (!modem_.Command("AT+CMQTTCFG=\"argtopic\",0,1,1")) {
        ESP_LOGE(TAG, "AT+CMQTTCFG failed");
        return false;
    }
    // === Step 4: 拼接 URI，构造 AT+CMQTTCONNECT 命令 ===
    std::string uri = (broker_port == 8883 ? "ssl://" : "tcp://") + broker_address + ":" + std::to_string(broker_port);
    std::string cmd = "AT+CMQTTCONNECT=0,\"" + uri + "\"," +
                    std::to_string(keep_alive_seconds_) + ",0";
    if (!username.empty()) {
        cmd += ",\"" + username + "\",\"" + password + "\"";
    }
    if (!modem_.Command(cmd, 5000)) {
        ESP_LOGE(TAG, "AT+CMQTTCONNECT failed");
        return false;
    }
    // === Step 5: 等待连接事件 ===
    bits = xEventGroupWaitBits(event_group_handle_,
                               MQTT_CONNECTED_EVENT | MQTT_DISCONNECTED_EVENT,
                               pdTRUE, pdFALSE, pdMS_TO_TICKS(MQTT_CONNECT_TIMEOUT_MS));
    if (!(bits & MQTT_CONNECTED_EVENT)) {
        ESP_LOGE(TAG, "MQTT CONNECT failed or timeout");
        return false;
    }
    connected_ = true;
    if (on_connected_callback_) {
        on_connected_callback_();
    }
    ESP_LOGI(TAG, "✅ MQTT connected to %s", uri.c_str());
    return true;
}

// bool Ml307Mqtt::Connect(const std::string broker_address, int broker_port, const std::string client_id, const std::string username, const std::string password) {
//     broker_address_ = broker_address;
//     broker_port_ = broker_port;
//     client_id_ = client_id;
//     username_ = username;
//     password_ = password;

//     EventBits_t bits;
//     if (IsConnected()) {
//         // 断开之前的连接
//         Disconnect();
//         bits = xEventGroupWaitBits(event_group_handle_, MQTT_DISCONNECTED_EVENT, pdTRUE, pdFALSE, pdMS_TO_TICKS(MQTT_CONNECT_TIMEOUT_MS));
//         if (!(bits & MQTT_DISCONNECTED_EVENT)) {
//             ESP_LOGE(TAG, "Failed to disconnect from previous connection");
//             return false;
//         }
//     }

//     if (broker_port_ == 8883) {
//         if (!modem_.Command(std::string("AT+MQTTCFG=\"ssl\",") + std::to_string(mqtt_id_) + ",1")) {
//             ESP_LOGE(TAG, "Failed to set MQTT to use SSL");
//             return false;
//         }
//     }

//     // Set clean session
//     if (!modem_.Command(std::string("AT+MQTTCFG=\"clean\",") + std::to_string(mqtt_id_) + ",1")) {
//         ESP_LOGE(TAG, "Failed to set MQTT clean session");
//         return false;
//     }

//     // Set keep alive
//     if (!modem_.Command(std::string("AT+MQTTCFG=\"pingreq\",") + std::to_string(mqtt_id_) + "," + std::to_string(keep_alive_seconds_))) {
//         ESP_LOGE(TAG, "Failed to set MQTT keep alive");
//         return false;
//     }

//     // Set HEX encoding
//     if (!modem_.Command("AT+MQTTCFG=\"encoding\"," + std::to_string(mqtt_id_) + ",1,1")) {
//         ESP_LOGE(TAG, "Failed to set MQTT to use HEX encoding");
//         return false;
//     }

//     // 创建MQTT连接
//     std::string command = "AT+MQTTCONN=" + std::to_string(mqtt_id_) + ",\"" + broker_address_ + "\"," + std::to_string(broker_port_) + ",\"" + client_id_ + "\",\"" + username_ + "\",\"" + password_ + "\"";
//     if (!modem_.Command(command)) {
//         ESP_LOGE(TAG, "Failed to create MQTT connection");
//         return false;
//     }

//     // 等待连接完成
//     bits = xEventGroupWaitBits(event_group_handle_, MQTT_CONNECTED_EVENT | MQTT_DISCONNECTED_EVENT, pdTRUE, pdFALSE, pdMS_TO_TICKS(MQTT_CONNECT_TIMEOUT_MS));
//     if (!(bits & MQTT_CONNECTED_EVENT)) {
//         ESP_LOGE(TAG, "Failed to connect to MQTT broker");
//         return false;
//     }

//     connected_ = true;
//     if (on_connected_callback_) {
//         on_connected_callback_();
//     }
//     return true;
// }

bool Ml307Mqtt::IsConnected() {
    //A7680C没有此命令，所以直接返回connected_，用URC去处理
    // // 检查这个 id 是否已经连接
    // modem_.Command(std::string("AT+MQTTSTATE=") + std::to_string(mqtt_id_));
    // auto bits = xEventGroupWaitBits(event_group_handle_, MQTT_INITIALIZED_EVENT, pdTRUE, pdFALSE, pdMS_TO_TICKS(MQTT_CONNECT_TIMEOUT_MS));
    // if (!(bits & MQTT_INITIALIZED_EVENT)) {
    //     ESP_LOGE(TAG, "Failed to initialize MQTT connection");
    //     return false;
    // }
    return connected_;
}

// void Ml307Mqtt::Disconnect() {
//     if (!connected_) {
//         return;
//     }
//     modem_.Command(std::string("AT+MQTTDISC=") + std::to_string(mqtt_id_));
// }

void Ml307Mqtt::Disconnect() {
    if (!connected_) {
        return;
    }
    // 推荐的断开超时时间：30 秒
    std::string cmd = "AT+CMQTTDISC=" + std::to_string(mqtt_id_) + ",30";
    if (!modem_.Command(cmd)) {
        ESP_LOGE(TAG, "AT+CMQTTDISC failed");
        return;
    }
    // // 等待断开事件（由 URC +CMQTTDISC:<id>,0 返回）
    // EventBits_t bits = xEventGroupWaitBits(event_group_handle_,
    //                                        MQTT_DISCONNECTED_EVENT,
    //                                        pdTRUE, pdFALSE,
    //                                        pdMS_TO_TICKS(MQTT_CONNECT_TIMEOUT_MS));
    // if (!(bits & MQTT_DISCONNECTED_EVENT)) {
    //     ESP_LOGW(TAG, "Disconnect command sent but no URC received");
    // }

    // // 强制标记为未连接
    // connected_ = false;
}

// bool Ml307Mqtt::Publish(const std::string topic, const std::string payload, int qos) {
//     if (!connected_) {
//         return false;
//     }
//     std::string command = "AT+MQTTPUB=" + std::to_string(mqtt_id_) + ",\"" + topic + "\",";
//     command += std::to_string(qos) + ",0,0,";
//     command += std::to_string(payload.size()) + "," + modem_.EncodeHex(payload);
//     return modem_.Command(command);
// }


bool Ml307Mqtt::Publish(const std::string topic, const std::string payload, int qos) {
    if (!connected_) return false;

    std::string cmd = "AT+CMQTTPUB=" + std::to_string(mqtt_id_) +
                      ",\"" + topic + "\"," + std::to_string(qos) +
                      "," + std::to_string(payload.size()) + ",0";

    // Step 1: 发送指令并等待 >
    if (!modem_.CommandWaitPrompt(cmd, '>', 2000)) {
        ESP_LOGE(TAG, "❌ Did not receive prompt >");
        return false;
    }

    // Step 2: 发 payload，不带 \r\n
    if (!modem_.WriteRaw(payload)) {
        ESP_LOGE(TAG, "❌ Failed to send payload");
        return false;
    }

    // Step 3: 等待 OK
    auto bits = xEventGroupWaitBits(modem_.GetEventGroupHandle(),
                                     AT_EVENT_COMMAND_DONE | AT_EVENT_COMMAND_ERROR,
                                     pdTRUE, pdFALSE, pdMS_TO_TICKS(3000));
    if (bits & AT_EVENT_COMMAND_DONE) {
        ESP_LOGI(TAG, "✅ MQTT publish success");
        return true;
    }

    ESP_LOGE(TAG, "❌ MQTT publish failed");
    return false;
}



// bool Ml307Mqtt::Subscribe(const std::string topic, int qos) {
//     if (!connected_) {
//         return false;
//     }
//     std::string command = "AT+MQTTSUB=" + std::to_string(mqtt_id_) + ",\"" + topic + "\"," + std::to_string(qos);
//     return modem_.Command(command);
// }
bool Ml307Mqtt::Subscribe(const std::string topic, int qos) {
    if (!connected_) {return false;}
    // 构造订阅命令，最后一个参数1代表启用URC通知，但我们这次不处理它
    std::string command = "AT+CMQTTSUB=" + std::to_string(mqtt_id_) +
                          ",\"" + topic + "\"," + std::to_string(qos) + ",1";
    return modem_.Command(command);
}

// bool Ml307Mqtt::Unsubscribe(const std::string topic) {
//     if (!connected_) {
//         return false;
//     }
//     std::string command = "AT+MQTTUNSUB=" + std::to_string(mqtt_id_) + ",\"" + topic + "\"";
//     return modem_.Command(command);
// }


bool Ml307Mqtt::Unsubscribe(const std::string topic) {
    if (!connected_) {return false;}
    // A7680C 的取消订阅命令：AT+CMQTTUNSUB=<client_index>,"topic",<qos>
    // 第三个参数 qos 设置为 1，代表启用 URC，但我们不处理它
    std::string command = "AT+CMQTTUNSUB=" + std::to_string(mqtt_id_) +
                          ",\"" + topic + "\",1";
    return modem_.Command(command);
}

std::string Ml307Mqtt::ErrorToString(int error_code) {
    switch (error_code) {
        case 0:
            return "连接成功";
        case 1:
            return "正在重连";
        case 2:
            return "断开：用户主动断开";
        case 3:
            return "断开：拒绝连接（协议版本、标识符、用户名或密码错误）";
        case 4:
            return "断开：服务器断开";
        case 5:
            return "断开：Ping包超时断开";
        case 6:
            return "断开：网络异常断开";
        case 255:
            return "断开：未知错误";
        default:
            return "未知错误";
    }
}
