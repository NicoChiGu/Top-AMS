#include <ArduinoJson.h>

#include <cassert>
#include <iostream>
#include <string>

#include "bambu.hpp"

int main() {
    const std::string source = "G1 X1\\path\r\nM117 \"ready\"\n\n";
    const std::string payload = bambu::msg::runGcode(source, 42);

    JsonDocument parsed;
    assert(deserializeJson(parsed, payload) == DeserializationError::Ok);
    assert(parsed["print"]["command"].as<std::string>() == "gcode_line");
    assert(parsed["print"]["sequence_id"].as<std::string>() == "42");
    assert(parsed["print"]["param"].as<std::string>() ==
           "G1 X1\\path\nM117 \"ready\"\n");

    const std::string round_trip = parsed["print"]["param"].as<std::string>();
    assert(round_trip.back() == '\n');
    assert(round_trip.size() == 1 || round_trip[round_trip.size() - 2] != '\n');

    const std::string command = bambu::msg::serialize_print_command("resume", 43);
    parsed.clear();
    assert(deserializeJson(parsed, command) == DeserializationError::Ok);
    assert(parsed["print"]["command"].as<std::string>() == "resume");
    assert(parsed["print"]["sequence_id"].as<std::string>() == "43");

    std::cout << "Bambu JSON protocol host tests passed\n";
    return 0;
}
