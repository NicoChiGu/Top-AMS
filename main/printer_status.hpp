#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace printer_status {

struct ams_status_info {
    int raw = -1;
    uint8_t main = 0xFF;
    uint8_t sub = 0xFF;
    std::string label_zh = "尚未收到打印机 AMS 状态";
    bool known = false;
};

inline ams_status_info decode_ams_status(int raw) {
    ams_status_info result;
    result.raw = raw;
    if (raw < 0 || raw > 0xFFFF)
        return result;

    result.main = static_cast<uint8_t>((raw >> 8) & 0xFF);
    result.sub = static_cast<uint8_t>(raw & 0xFF);

    if (result.main == 0x00) {
        result.known = result.sub == 0;
        result.label_zh = result.sub == 0
            ? "空闲/正常"
            : "空闲主状态的未知子步骤 " + std::to_string(result.sub);
        return result;
    }

    if (result.main == 0x01) {
        static constexpr const char* labels[] = {
            "换料流程已启动",
            "暂停换料",
            "加热喷嘴",
            "切断耗材",
            "回抽当前耗材，Top-AMS 正在退线",
            "推送新耗材",
            "外挂料模式下等待确认喷嘴是否出料",
            "冲刷旧耗材",
            "检查耗材位置/确认出料",
            "切换挤出机",
            "切换热端",
            "冷却耗材",
            "将耗材推入料路切换器",
            "从料路切换器回抽耗材",
            "切换料路",
        };
        if (result.sub < sizeof(labels) / sizeof(labels[0])) {
            result.known = true;
            result.label_zh = labels[result.sub];
        } else {
            result.label_zh = "换料流程的未知子步骤 " + std::to_string(result.sub);
        }
        return result;
    }

    if (result.sub == 0) {
        switch (result.main) {
        case 0x02:
            result.known = true;
            result.label_zh = "RFID 识别中";
            break;
        case 0x03:
            result.known = true;
            result.label_zh = "辅助送料/料路已啮合（不是进料完成）";
            break;
        case 0x04:
            result.known = true;
            result.label_zh = "AMS 校准中";
            break;
        case 0x07:
            result.known = true;
            result.label_zh = "冷拉流程中";
            break;
        case 0x10:
            result.known = true;
            result.label_zh = "AMS 自检中";
            break;
        case 0x20:
            result.known = true;
            result.label_zh = "AMS 调试流程";
            break;
        default:
            break;
        }
    }
    if (!result.known)
        result.label_zh = "未知状态：主码 " + std::to_string(result.main) +
                          "，子码 " + std::to_string(result.sub);
    return result;
}

inline bool is_idle(int raw) noexcept {
    return raw == 0;
}

inline bool is_pull_current_filament(int raw) noexcept {
    return raw == 0x0104;
}

enum class fault_severity : uint8_t {
    none = 0,
    info = 1,
    warning = 2,
    error = 3,
    critical = 4,
};

inline const char* to_string(fault_severity severity) noexcept {
    switch (severity) {
    case fault_severity::info: return "info";
    case fault_severity::warning: return "warning";
    case fault_severity::error: return "error";
    case fault_severity::critical: return "critical";
    case fault_severity::none:
    default: return "none";
    }
}

struct fault_info {
    bool active = false;
    bool interlock = false;
    bool known = true;
    std::string source = "none";
    uint32_t raw_code = 0;
    std::string display_code = "0";
    std::string label_zh = "无打印机故障";
    std::string action_zh;
    fault_severity severity = fault_severity::none;
};

inline bool operator==(const fault_info& lhs, const fault_info& rhs) {
    return lhs.active == rhs.active && lhs.interlock == rhs.interlock &&
           lhs.known == rhs.known && lhs.source == rhs.source &&
           lhs.raw_code == rhs.raw_code && lhs.display_code == rhs.display_code &&
           lhs.label_zh == rhs.label_zh && lhs.action_zh == rhs.action_zh &&
           lhs.severity == rhs.severity;
}

inline bool operator!=(const fault_info& lhs, const fault_info& rhs) {
    return !(lhs == rhs);
}

inline std::string hex8(uint32_t value) {
    char buffer[9]{};
    std::snprintf(buffer, sizeof(buffer), "%08lX", static_cast<unsigned long>(value));
    return buffer;
}

// Bambu Studio DevHMS: attr 的高 16 位是模块/实例，code 的低 16 位是消息码。
inline std::string short_hms_code(uint32_t attr, uint32_t code) {
    const uint32_t compact = (attr & 0xFFFF0000u) | (code & 0x0000FFFFu);
    return hex8(compact);
}

inline fault_info decode_known_short_code(std::string_view code, std::string_view source,
                                          uint32_t raw_code,
                                          fault_severity fallback_severity) {
    fault_info result;
    result.active = true;
    result.interlock = true;
    result.source.assign(source.data(), source.size());
    result.raw_code = raw_code;
    result.display_code.assign(code.data(), code.size());
    result.severity = fallback_severity;

    if (code == "07FF8010") {
        result.label_zh = "外挂料盘或耗材卡住";
        result.action_zh = "检查料盘是否顺畅、PTFE 管是否弯折及料路是否受阻；排除后在打印机端重试。";
    } else if (code == "07FF8007" || code == "07FFC00A") {
        result.label_zh = "等待确认喷嘴是否已有耗材挤出";
        result.action_zh = "观察喷嘴；未出料时轻推耗材并在打印机端重试，已出料时由用户在打印机端确认。";
    } else {
        result.known = false;
        result.label_zh = source == "print_error" ? "未收录的打印机错误" : "未收录的打印机 HMS";
        result.action_zh = "请在打印机或 Bambu Studio 中查看完整详情，排除故障后手动重试。";
    }
    return result;
}

inline fault_info decode_print_error(uint32_t code) {
    if (code == 0)
        return {};
    fault_info result = decode_known_short_code(hex8(code), "print_error", code,
                                                fault_severity::critical);
    if (!result.known)
        result.display_code = std::to_string(code) + " / 0x" + hex8(code);
    return result;
}

inline fault_info decode_hms(uint32_t attr, uint32_t code) {
    const uint32_t level = code >> 16;
    fault_severity severity = fault_severity::warning;
    bool interlock = true;
    switch (level) {
    case 1: severity = fault_severity::critical; break;
    case 2: severity = fault_severity::error; break;
    case 3: severity = fault_severity::warning; break;
    case 4:
        severity = fault_severity::info;
        interlock = false;
        break;
    default: severity = fault_severity::warning; break;
    }

    fault_info result = decode_known_short_code(short_hms_code(attr, code), "hms", code, severity);
    result.interlock = result.known || interlock;
    return result;
}

} // namespace printer_status
