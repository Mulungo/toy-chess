#include "timeit.hpp"
#include <catch2/catch_test_macros.hpp>

TEST_CASE("timeit") {

  std::vector<int> ones(1000000, 1);

  INFO(timeit::timeit([&]() {
    int64_t sum = 0;
    for (auto x : ones) { sum += x; }
    return sum;
  }));
  SUCCEED();
}
