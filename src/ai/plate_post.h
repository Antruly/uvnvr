#pragma once

#include <string>

namespace uvnvr {

// Normalizes a raw licence-plate string for mainland China blue/green plates:
//   * full-width ASCII is folded to half-width;
//   * spaces, interpuncts and dashes are removed;
//   * ASCII letters are upper-cased;
//   * in the serial segment I becomes 1 and O becomes 0, while the issuing
//     authority letter keeps O (京O is a real plate series).
// The province character is preserved. Invalid input still goes through the
// same folding; use IsValidPlate() to decide whether the result is usable.
std::string NormalizePlate(const std::string& raw);

// True when the (already normalized, or raw and normalizable) value is one
// province character, one issuing-authority letter (A-Z except I) and five or
// six serial characters (A-Z or 0-9).
bool IsValidPlate(const std::string& plate);

} // namespace uvnvr
