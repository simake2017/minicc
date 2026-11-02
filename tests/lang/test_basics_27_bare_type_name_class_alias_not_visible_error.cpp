// =============================================================================
// 测试（负例）：类作用域的类型别名在【全局】裸用 ⇒ 不可见（B37）
// =============================================================================
// 理论点：
//   · [basic.scope.class]：类作用域的名字只在类体内（含成员函数体）可见 ——
//     `struct A { using B = int; };` 里的 B 在类外不可见
//     （clang: use of undeclared identifier 'B'）。
//   · ★ 这是"嵌套类同名"问题的可观测形态：名字登记表的 scope 键只到【命名空间】
//     一级，类体内的名字被登记在外层作用域（spec 异常 1 记为"宽方向"）⇒
//     裸名查询跨作用域取并集 ⇒ 全局裸用 B 被误判成"这里有类型 B"。
//   · ★ B37 的修复：可见性裁决下沉到 Sema（用它自己的符号表 + 类作用域回退），
//     判据单点（红线 3）。
// 预期行为：rc=1，报错文案与 clang 逐字对齐：
//   [Semantic Error] 29:5: use of undeclared identifier 'B'
// 对照 clang：clang++-18 -std=c++20 同样 rc=1，位置同为 29:5。
// =============================================================================

// ── GoogleTest 入口（仅 CLion 单用例 Run/Debug；minicc 编译时本块被条件编译抹掉）──
#ifdef MINICC_GTEST_TEST
#include <gtest/gtest.h>
#define main minicc_gtest_body
static int minicc_gtest_body();
TEST(Integration, test_basics_27_bare_type_name_class_alias_not_visible_error) { minicc_gtest_body(); }
#endif

struct A { using B = int; };   // B 只在 A 内可见

int main() {
    B;                      // ★ 类外看不见 A::B ⇒ 必须报错（clang 逐字）
    return 0;
}
