#include "codex_read_tracker.h"
#include "unity.h"

TEST_CASE("read tracker fires once after 2000 ms", "[codex_read]")
{
    codex_read_tracker_reset();
    codex_read_tracker_visit("task:t1", "", 100);
    TEST_ASSERT_FALSE(codex_read_tracker_take_due("task:t1", "", 2099));
    TEST_ASSERT_TRUE(codex_read_tracker_take_due("task:t1", "", 2100));
    TEST_ASSERT_FALSE(codex_read_tracker_take_due("task:t1", "", 5000));
}

TEST_CASE("changing task restarts the read timer", "[codex_read]")
{
    codex_read_tracker_reset();
    codex_read_tracker_visit("task:t1", "", 100);
    TEST_ASSERT_TRUE(codex_read_tracker_take_due("task:t1", "", 2100));
    codex_read_tracker_visit("task:t2", "", 2200);
    TEST_ASSERT_FALSE(codex_read_tracker_take_due("task:t2", "", 4199));
    TEST_ASSERT_TRUE(codex_read_tracker_take_due("task:t2", "", 4200));
}

TEST_CASE("a new result message restarts the read timer", "[codex_read]")
{
    codex_read_tracker_reset();
    codex_read_tracker_visit("task:t1", "", 100);
    TEST_ASSERT_TRUE(codex_read_tracker_take_due("task:t1", "", 2100));
    codex_read_tracker_visit("task:t1", "result:t1:1", 2200);
    TEST_ASSERT_FALSE(codex_read_tracker_take_due("task:t1", "result:t1:1", 4199));
    TEST_ASSERT_TRUE(codex_read_tracker_take_due("task:t1", "result:t1:1", 4200));
}

TEST_CASE("empty message id never acknowledges", "[codex_read]")
{
    codex_read_tracker_reset();
    codex_read_tracker_visit("", "", 100);
    TEST_ASSERT_FALSE(codex_read_tracker_take_due("", "", 9000));
}

TEST_CASE("failed acknowledgement can be retried on a later visit", "[codex_read]")
{
    codex_read_tracker_reset();
    codex_read_tracker_visit("task:t1", "", 100);
    TEST_ASSERT_TRUE(codex_read_tracker_take_due("task:t1", "", 2100));
    codex_read_tracker_rearm("task:t1", "", 3000);
    TEST_ASSERT_FALSE(codex_read_tracker_take_due("task:t1", "", 4999));
    TEST_ASSERT_TRUE(codex_read_tracker_take_due("task:t1", "", 5000));
}

TEST_CASE("rearming another message does not affect the active one", "[codex_read]")
{
    codex_read_tracker_reset();
    codex_read_tracker_visit("task:t1", "", 100);
    codex_read_tracker_rearm("task:t2", "", 3000);
    TEST_ASSERT_TRUE(codex_read_tracker_take_due("task:t1", "", 2100));
}
