#pragma once
#include <ArduinoJson.h>
#include <string>

#include "printer_protocol.hpp"

namespace bambu {
    using std::string;

    namespace msg {

        inline string serialize_print_command(const string& command, uint32_t sequence_id) {
            JsonDocument doc;
            JsonObject print = doc["print"].to<JsonObject>();
            print["command"] = command;
            print["sequence_id"] = std::to_string(sequence_id);
            std::string serialized;
            serializeJson(doc, serialized);
            return serialized;
        }

        // G-code 由 ArduinoJson 负责转义换行、引号和反斜杠。
        inline string runGcode(const string& code, uint32_t sequence_id) {
            JsonDocument doc;
            JsonObject print = doc["print"].to<JsonObject>();
            print["command"] = "gcode_line";
            print["param"] = printer_protocol::normalize_gcode(code);
            print["sequence_id"] = std::to_string(sequence_id);
            std::string serialized;
            serializeJson(doc, serialized);
            return serialized;
        }

        inline string get_status_with_sequence(uint32_t sequence_id) {
            JsonDocument doc;
            JsonObject pushing = doc["pushing"].to<JsonObject>();
            pushing["command"] = "pushall";
            pushing["sequence_id"] = std::to_string(sequence_id);
            std::string serialized;
            serializeJson(doc, serialized);
            return serialized;
        }
    }//msg


/*
已知A1/A1mini 1.04固件下
使用Mqtt发送热床调节M140,热端调节M104均无效
旧版本固件或拓竹自家软件发送有效
因拓竹网络层闭源,测试需要抓包,较为繁琐
现阶段调温都用M190,M109
*/

}//bambu
