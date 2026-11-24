#include "ai/plate_post.h"

#include <cstdio>
#include <string>

namespace {

int failures = 0;

void Expect(const std::string& name, const std::string& actual, const std::string& expected) {
  if (actual == expected)
    return;
  std::fprintf(stderr, "FAIL %s: got '%s', expected '%s'\n", name.c_str(), actual.c_str(),
               expected.c_str());
  ++failures;
}

void ExpectBool(const std::string& name, bool actual, bool expected) {
  if (actual == expected)
    return;
  std::fprintf(stderr, "FAIL %s: got %s, expected %s\n", name.c_str(), actual ? "true" : "false",
               expected ? "true" : "false");
  ++failures;
}

} // namespace

int main() {
  using uvnvr::IsValidPlate;
  using uvnvr::NormalizePlate;

  // The two worked examples from the specification.
  Expect("lowercase O in authority, middle dot", NormalizePlate("京o·A1O2I"), "京OA1021");
  Expect("space and I in serial", NormalizePlate("粤B I2345"), "粤B12345");

  // Full-width ASCII and lowercase are folded.
  Expect("full-width", NormalizePlate("京Ａ１２３４５"), "京A12345");
  Expect("lowercase", NormalizePlate("粤b12345"), "粤B12345");
  Expect("dashes and spaces", NormalizePlate(" 京-A-1 2 3 4 5 "), "京A12345");
  Expect("bullet", NormalizePlate("京A•12345"), "京A12345");

  // Serial I/O mapping, but the authority O stays O.
  Expect("serial O", NormalizePlate("京A1O234"), "京A10234");
  Expect("authority O", NormalizePlate("京o12345"), "京O12345");
  Expect("six-character serial", NormalizePlate("粤BD12345"), "粤BD12345");

  // Validity.
  ExpectBool("valid five", IsValidPlate("京A12345"), true);
  ExpectBool("valid six", IsValidPlate("粤BD12345"), true);
  ExpectBool("valid authority O", IsValidPlate("京O12345"), true);
  ExpectBool("invalid authority I", IsValidPlate("京I12345"), false);
  ExpectBool("invalid serial count", IsValidPlate("京A1234"), false);
  ExpectBool("invalid province", IsValidPlate("A12345"), false);
  ExpectBool("empty", IsValidPlate(""), false);
  ExpectBool("invalid symbol", IsValidPlate("京A1234#"), false);

  // Normalization is idempotent.
  const std::string once = NormalizePlate("京o·A1O2I");
  Expect("idempotent", NormalizePlate(once), once);

  if (failures == 0) {
    std::printf("ai_plate_post_test: all checks passed\n");
    return 0;
  }
  std::fprintf(stderr, "ai_plate_post_test: %d checks failed\n", failures);
  return 1;
}
