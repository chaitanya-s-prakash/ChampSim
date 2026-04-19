#include <catch.hpp>

#include "dram_stats.h"
#include "stats_printer.h"

namespace
{
std::vector<std::string> default_lines()
{
  return {"test_channel READ_REQUESTS:          0",
          "  WRITE_REQUESTS:          0",
          "  BYTES_RETURNED:          0",
          "  BYTES_TRANSFERRED:          0",
          "  BANDWIDTH UTILIZATION: -",
          "  DEMAND_REQUESTS:          0",
          "  DEMAND_TIER_ACCESSES:          0",
          "  AVERAGE DEMAND ACCESS LATENCY: - cycles",
          "  AVERAGE RQ OCCUPANCY: -",
          "  AVERAGE WQ OCCUPANCY: -",
          "  AVERAGE TOTAL QUEUE OCCUPANCY: -",
          "  AVERAGE RQ OCCUPANCY RATIO: -",
          "  AVERAGE WQ OCCUPANCY RATIO: -",
          "  AVERAGE TOTAL QUEUE OCCUPANCY RATIO: -",
          "  PEAK RQ OCCUPANCY:          0",
          "  PEAK WQ OCCUPANCY:          0",
          "  PEAK TOTAL QUEUE OCCUPANCY:          0",
          "  PEAK RQ OCCUPANCY RATIO: -",
          "  PEAK WQ OCCUPANCY RATIO: -",
          "  PEAK TOTAL QUEUE OCCUPANCY RATIO: -",
          "test_channel RQ ROW_BUFFER_HIT:          0",
          "  ROW_BUFFER_MISS:          0",
          "  AVG DBUS CONGESTED CYCLE: -",
          "test_channel WQ ROW_BUFFER_HIT:          0",
          "  ROW_BUFFER_MISS:          0",
          "  FULL:          0",
          "test_channel REFRESHES ISSUED: -"};
}
} // namespace

TEST_CASE("An empty DRAM stats prints zero")
{
  dram_stats given{};
  given.name = "test_channel";

  REQUIRE_THAT(champsim::plain_printer::format(given), Catch::Matchers::RangeEquals(default_lines()));
}

TEST_CASE("The DRAM RQ row buffer hit counter increments the printed stats")
{
  dram_stats given{};
  given.name = "test_channel";
  given.RQ_ROW_BUFFER_HIT = 255;

  auto expected = default_lines();
  expected.at(20) = "test_channel RQ ROW_BUFFER_HIT:        255";

  REQUIRE_THAT(champsim::plain_printer::format(given), Catch::Matchers::RangeEquals(expected));
}

TEST_CASE("The DRAM RQ row buffer miss counter increments the printed stats")
{
  dram_stats given{};
  given.name = "test_channel";
  given.RQ_ROW_BUFFER_MISS = 255;

  auto expected = default_lines();
  expected.at(21) = "  ROW_BUFFER_MISS:        255";

  REQUIRE_THAT(champsim::plain_printer::format(given), Catch::Matchers::RangeEquals(expected));
}

TEST_CASE("The DRAM WQ row buffer hit counter increments the printed stats")
{
  dram_stats given{};
  given.name = "test_channel";
  given.WQ_ROW_BUFFER_HIT = 255;

  auto expected = default_lines();
  expected.at(23) = "test_channel WQ ROW_BUFFER_HIT:        255";

  REQUIRE_THAT(champsim::plain_printer::format(given), Catch::Matchers::RangeEquals(expected));
}

TEST_CASE("The DRAM WQ row buffer miss counter increments the printed stats")
{
  dram_stats given{};
  given.name = "test_channel";
  given.WQ_ROW_BUFFER_MISS = 255;

  auto expected = default_lines();
  expected.at(24) = "  ROW_BUFFER_MISS:        255";

  REQUIRE_THAT(champsim::plain_printer::format(given), Catch::Matchers::RangeEquals(expected));
}

TEST_CASE("The DRAM WQ full counter increments the printed stats")
{
  dram_stats given{};
  given.name = "test_channel";
  given.WQ_FULL = 255;

  auto expected = default_lines();
  expected.at(25) = "  FULL:        255";

  REQUIRE_THAT(champsim::plain_printer::format(given), Catch::Matchers::RangeEquals(expected));
}

TEST_CASE("The DRAM dbus congestion counters increment the printed stats")
{
  dram_stats given{};
  given.name = "test_channel";
  given.dbus_cycle_congested = 100;
  given.dbus_count_congested = 100;

  auto expected = default_lines();
  expected.at(22) = "  AVG DBUS CONGESTED CYCLE: 1";

  REQUIRE_THAT(champsim::plain_printer::format(given), Catch::Matchers::RangeEquals(expected));
}

TEST_CASE("The DRAM refresh counters increment the printed stats")
{
  dram_stats given{};
  given.name = "test_channel";
  given.refresh_cycles = 100;

  auto expected = default_lines();
  expected.at(26) = "test_channel REFRESHES ISSUED:        100";

  REQUIRE_THAT(champsim::plain_printer::format(given), Catch::Matchers::RangeEquals(expected));
}
