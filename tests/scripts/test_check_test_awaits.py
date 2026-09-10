"""Regression cases for the source guard, independent of the C++ build."""

from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from check_test_awaits import violations


class AwaitAssertionGuardTests(unittest.TestCase):
    def test_each_runtime_assertion_rejects_await(self):
        for macro in (
            "REQUIRE", "REQUIRE_FALSE", "REQUIRE_THROWS", "REQUIRE_THROWS_AS",
            "REQUIRE_THROWS_WITH", "REQUIRE_THROWS_MATCHES", "REQUIRE_NOTHROW",
            "REQUIRE_THAT", "CHECK", "CHECK_FALSE", "CHECK_THROWS",
            "CHECK_THROWS_AS", "CHECK_THROWS_WITH", "CHECK_THROWS_MATCHES",
            "CHECK_NOTHROW", "CHECK_THAT",
        ):
            with self.subTest(macro=macro):
                self.assertEqual(violations(macro + "((co_await read()) == 4096);"),
                                 [(macro, 1)])

    def test_multiline_lambda_and_parentheses(self):
        self.assertEqual(violations('''
            REQUIRE /* comment between macro and argument */ (
                (co_await bdev_io([&] {
                    return ::pwrite(fd, patch.data(), patch.size(),
                                    data.size() + 1024);
                })) == 4096);
            CHECK_FALSE(!(co_await read()));
        '''), [("REQUIRE", 2), ("CHECK_FALSE", 7)])

    def test_nested_assertion_and_lambda_body(self):
        self.assertEqual(violations('''REQUIRE(([&]() {
            CHECK(co_await read());
            co_return true;
        })());'''), [("REQUIRE", 1), ("CHECK", 2)])

    def test_comments_and_literals_do_not_affect_nesting(self):
        self.assertEqual(violations(r'''
            // REQUIRE(co_await example());
            /* CHECK_FALSE((co_await example())); */
            auto text = "REQUIRE(co_await example()) \"";
            auto raw = u8R"demo(CHECK(co_await example()); ")demo";
            auto ch = '\'';
            REQUIRE(text == "co_await )");
            CHECK(raw != R"(co_await REQUIRE())");
            REQUIRE(ch != '(');
            CHECK(1'000 == 1000);
            const auto result = co_await read();
            REQUIRE(result == 4096);
            CHECK_FALSE(co_awaited);
            MY_REQUIRE(co_await read());
        '''), [])

    def test_ignored_text_cannot_hide_real_await(self):
        self.assertEqual(violations(r'''
            REQUIRE(compare(R"tag() /* ) */ " co_await)tag", "escaped \\") &&
                    (co_await read()) == 1'000);
            const auto result = co_await next();
            CHECK(result);
        '''), [("REQUIRE", 2)])

    def test_await_after_assertion_is_outside_it(self):
        self.assertEqual(violations("REQUIRE(ready()); co_await read();"), [])


if __name__ == "__main__":
    unittest.main()
