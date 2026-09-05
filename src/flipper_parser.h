#pragma once

#include <Arduino.h>
#include <FS.h>

#include <cstdint>
#include <cstdlib>
#include <vector>

namespace flipper {

constexpr const char *SUPPORTED_PRESET =
    "FuriHalSubGhzPreset2FSKDev476Async";
constexpr size_t MAX_TIMINGS = 4096;
constexpr uint32_t MIN_FREQUENCY_HZ = 863000000UL;
constexpr uint32_t MAX_FREQUENCY_HZ = 870000000UL;

struct RawSignal {
  uint32_t frequency_hz{0};
  String preset;
  std::vector<int32_t> timings;
};

struct ParseState {
  bool filetype_ok{false};
  bool version_ok{false};
  bool protocol_raw{false};
  bool raw_line_seen{false};
};

inline bool parse_raw_values(const String &line, std::vector<int32_t> &values,
                             String &error) {
  constexpr char PREFIX[] = "RAW_Data:";
  if (!line.startsWith(PREFIX)) return false;

  const char *cursor = line.c_str() + strlen(PREFIX);
  while (*cursor != '\0') {
    while (*cursor == ' ' || *cursor == '\t' || *cursor == ',') ++cursor;
    if (*cursor == '\0') break;

    char *end = nullptr;
    const long parsed = strtol(cursor, &end, 10);
    if (end == cursor || parsed == 0 || labs(parsed) > 1000000L) {
      error = "Invalid RAW_Data timing";
      return false;
    }
    if (values.size() >= MAX_TIMINGS) {
      error = "The signal contains too many timings";
      return false;
    }
    values.push_back(static_cast<int32_t>(parsed));
    cursor = end;
  }
  return true;
}

inline bool parse_line(String line, RawSignal &signal, ParseState &state,
                       String &error) {
  line.trim();
  if (line.isEmpty()) return true;

  if (line == "Filetype: Flipper SubGhz RAW File") {
    state.filetype_ok = true;
  } else if (line == "Version: 1") {
    state.version_ok = true;
  } else if (line.startsWith("Frequency:")) {
    const char *value = line.c_str() + strlen("Frequency:");
    signal.frequency_hz = static_cast<uint32_t>(strtoul(value, nullptr, 10));
  } else if (line.startsWith("Preset:")) {
    signal.preset = line.substring(strlen("Preset:"));
    signal.preset.trim();
  } else if (line == "Protocol: RAW") {
    state.protocol_raw = true;
  } else if (line.startsWith("RAW_Data:")) {
    state.raw_line_seen = true;
    if (!parse_raw_values(line, signal.timings, error)) return false;
  }
  return true;
}

inline bool validate_signal(const RawSignal &signal, const ParseState &state,
                            String &error) {
  if (!state.filetype_ok || !state.version_ok || !state.protocol_raw ||
      !state.raw_line_seen || signal.timings.empty()) {
    error = "The file is not a valid Flipper RAW .sub file";
    return false;
  }
  if (signal.preset != SUPPORTED_PRESET) {
    error = "Unsupported preset. This firmware currently accepts only " +
            String(SUPPORTED_PRESET);
    return false;
  }
  if (signal.frequency_hz < MIN_FREQUENCY_HZ ||
      signal.frequency_hz > MAX_FREQUENCY_HZ) {
    error = "Frequency is outside the supported 863-870 MHz range";
    return false;
  }
  return true;
}

inline bool parse_buffer(const uint8_t *data, size_t length, RawSignal &signal,
                         String &error) {
  if (data == nullptr || length == 0) {
    error = "Uploaded signal file is empty";
    return false;
  }

  signal = RawSignal{};
  ParseState state;
  size_t line_start = 0;
  for (size_t index = 0; index <= length; ++index) {
    if (index < length && data[index] != '\n') continue;
    String line;
    line.reserve(index - line_start);
    for (size_t cursor = line_start; cursor < index; ++cursor) {
      if (data[cursor] != '\r') line += static_cast<char>(data[cursor]);
    }
    if (!parse_line(line, signal, state, error)) return false;
    line_start = index + 1;
  }
  return validate_signal(signal, state, error);
}

inline bool parse_file(fs::FS &filesystem, const String &path,
                       RawSignal &signal, String &error) {
  File file = filesystem.open(path, FILE_READ);
  if (!file) {
    error = "Cannot open the signal file";
    return false;
  }

  signal = RawSignal{};
  ParseState state;
  while (file.available()) {
    if (!parse_line(file.readStringUntil('\n'), signal, state, error)) {
      file.close();
      return false;
    }
  }
  file.close();
  return validate_signal(signal, state, error);
}

}  // namespace flipper
