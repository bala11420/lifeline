#include "doctest.h"
#include "lifeline/dedup.h"

using namespace lifeline;

TEST_CASE("StreamTracker never rejects a legitimately late record") {
  StreamTracker t;
  // Critical seq 101 arrives first because of priority; records 1..100 arrive afterwards.
  CHECK(t.accept(101));
  CHECK(t.cumulative() == 0);  // nothing below 101 is acknowledged yet
  for (uint64_t s = 1; s <= 100; s++) CHECK(t.accept(s));
  CHECK(t.cumulative() == 101);
  CHECK(t.out_of_order() == 0);
}

TEST_CASE("StreamTracker suppresses duplicates") {
  StreamTracker t;
  CHECK(t.accept(1));
  CHECK_FALSE(t.accept(1));
  CHECK(t.accept(3));
  CHECK_FALSE(t.accept(3));
  CHECK(t.cumulative() == 1);
  CHECK(t.accept(2));
  CHECK(t.cumulative() == 3);
  CHECK_FALSE(t.accept(2));
  CHECK_FALSE(t.accept(0));
}

TEST_CASE("VersionGate: newer wins, replays and stale values lose") {
  VersionGate g;
  CHECK(g.accept(1, 5));
  CHECK_FALSE(g.accept(1, 5));  // replay
  CHECK_FALSE(g.accept(1, 4));  // stale
  CHECK(g.accept(1, 6));
  CHECK(g.accept(2, 1));        // new boot epoch beats any version of the old epoch
  CHECK_FALSE(g.accept(1, 99)); // old epoch can never win again
}

TEST_CASE("StreamTracker in-order mode refuses to apply out of order") {
  StreamTracker t;
  using O = StreamTracker::Order;
  CHECK(t.accept_in_order(1) == O::Applied);
  CHECK(t.accept_in_order(1) == O::Duplicate);
  CHECK(t.accept_in_order(3) == O::Gap);  // do not apply; reply cumulative()==1 so the node resends
  CHECK(t.cumulative() == 1);
  CHECK(t.expected() == 2);
  CHECK(t.accept_in_order(2) == O::Applied);
  CHECK(t.accept_in_order(3) == O::Applied);
  CHECK(t.accept_in_order(0) == O::Duplicate);
}
