// Pure C++ (no ESPHome dependency) so it can be unit-tested on a PC.
// Formats a sorted list of register numbers as compact ranges, e.g. "1003-1005, 1800, 2010-2012".
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace reg_ranges {

// regs must be sorted ascending and free of duplicates. The output never exceeds budget
// characters; registers that did not fit are counted in *omitted (if given).
inline std::string format(const std::vector<uint16_t> &regs, size_t budget, size_t *omitted = nullptr) {
  std::string out;
  size_t skipped = 0;
  bool full = false;
  size_t i = 0;
  while (i < regs.size()) {
    size_t j = i;
    while (j + 1 < regs.size() && regs[j + 1] == regs[j] + 1) j++;
    char piece[24];
    const char *sep = out.empty() ? "" : ", ";
    if (j == i)
      snprintf(piece, sizeof(piece), "%s%u", sep, (unsigned) regs[i]);
    else
      snprintf(piece, sizeof(piece), "%s%u-%u", sep, (unsigned) regs[i], (unsigned) regs[j]);
    if (!full && out.size() + strlen(piece) <= budget) {
      out += piece;
    } else {
      full = true;  // once something doesn't fit, stop (keeps the output in register order)
      skipped += j - i + 1;
    }
    i = j + 1;
  }
  if (omitted) *omitted = skipped;
  return out;
}

}  // namespace reg_ranges
