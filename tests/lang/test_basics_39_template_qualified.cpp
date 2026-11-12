// 预期返回 0：模板限定链读取常量，静态调用按普通函数约定传参。
// 考察 [basic.lookup.qual]、[class.static]：嵌套类型、引用实参与类值返回。
template<class T> struct Box {
    struct Inner {
        static const int b = 3;
        struct Deep { static const int c = 9; };
        static int add(int x) { return x + 4; }
    };
    static int make() { return 4; }
    static void bump(int& x) { x = x + 1; }
};
struct Result { int value; };
template<class T> struct Factory {
    static Result make(int x) { Result r; r.value = x; return r; }
};
Box<int>* observe(Box<int>* box, int& count) { count = count + 1; return box; }
#ifdef MINICC_GTEST_TEST
#include <gtest/gtest.h>
#define main minicc_test_basics_39_main
static int minicc_test_basics_39_main();
TEST(Integration, test_basics_39_template_qualified) {
    EXPECT_EQ(minicc_test_basics_39_main(), 0);
}
#endif
int main() {
    if (Box<int>::Inner::b + Box<int>::Inner::Deep::c != 12) return 1;
    if (Box<int>::make() != 4) return 2;
    if (Box<int>::Inner::add(2) != 6) return 3;
    int x = 5;
    Box<int>::bump(x);
    if (x != 6) return 4;
    Result r = Factory<int>::make(7);
    if (r.value != 7) return 5;
    Box<int> box;
    int count = 0;
    if (observe(&box, count)->make() != 4) return 6;
    if (count != 1) return 7;
    return 0;
}
