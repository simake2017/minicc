// =============================================================================
// 测试：成员类模板与逐段模板限定名，预期退出码 0。
// 对照：clang/lib/Sema/SemaTemplateInstantiateDecl.cpp:2317:VisitClassTemplateDecl。
// [temp.mem]、[temp.inst]：外层实参先绑定，内层实参留到使用点，避免实例互相污染。
// demo：Outer<int>::Inner<long>::value_type ⇒ long。
// demo：Outer<long>::Inner<int>::outer_type ⇒ long。
// 调用方通过字段读写和静态方法验证两层绑定；clang++-18 为语义 oracle。
// =============================================================================
#ifdef MINICC_GTEST_TEST
#include <gtest/gtest.h>
#define main minicc_gtest_body
static int minicc_gtest_body();
TEST(Integration, test_tmpl_79_member_class_template) { EXPECT_EQ(minicc_gtest_body(), 0); }
#endif

template<class T>
struct Outer {
    template<class U>
    struct Inner {
        using value_type = U;
        using outer_type = T;
        T first;
        U second;
        static constexpr int value = 42;
        static int add(int x) { return x + 3; }
    };
};

int main() {
    Outer<int>::Inner<long> a;
    a.first = 7;
    a.second = 9;
    Outer<long>::Inner<int> b;
    b.first = 11;
    b.second = 13;
    Outer<int>::Inner<long>::value_type x = 5;
    Outer<long>::Inner<int>::outer_type y = 6;
    if (a.first + a.second + b.first + b.second + x + y != 51) return 1;
    if (Outer<int>::Inner<long>::value != 42) return 2;
    if (Outer<int>::Inner<long>::add(4) != 7) return 3;
    return 0;
}
