#include <gtest/gtest.h>

#include <cstring>

#include "neasant/version.h"

// The inception baseline: the library links, and its one function answers.
TEST(Version, ReportsProjectVersion) {
  ASSERT_NE(neasant::version(), nullptr);
  EXPECT_STREQ(neasant::version(), "0.0.0");
}

TEST(Version, IsDottedTriple) {
  const char* v = neasant::version();
  int dots = 0;
  for (const char* p = v; *p != '\0'; ++p) {
    if (*p == '.') {
      ++dots;
    } else {
      EXPECT_TRUE(*p >= '0' && *p <= '9') << "unexpected character in version: " << v;
    }
  }
  EXPECT_EQ(dots, 2) << "version is not major.minor.patch: " << v;
}
