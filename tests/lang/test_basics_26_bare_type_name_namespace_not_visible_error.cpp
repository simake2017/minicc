// =============================================================================
// 测试（负例）：命名空间里的类型名在【全局】裸用 ⇒ 不可见（B37）
// =============================================================================
// 理论点：
//   · [basic.lookup.unqual]：非限定名字只在【当前作用域链】上查找 ——
//     namespace N 里的 struct S 在 N 外不可见（clang: use of undeclared
//     identifier 'S'）。
//   · ★ B37 的现场：此前 Parser 拿名字登记表的 type 位（跨作用域并集）直接
//     判"声明"⇒ 该程序 rc=0 静默通过 —— 非法程序被编译放行（方向性错）。
//     修复：Parser 只给【候选】（把名字带进 AST），可见性由 Sema 的符号表裁决
//     （判据单点，红线 3）。
// 预期行为：rc=1，报错文案与 clang 逐字对齐：
//   [Semantic Error] 28:5: use of undeclared identifier 'S'
// 对照 clang：clang++-18 -std=c++20 同样 rc=1，位置同为 28:5。
// =============================================================================

// ── GoogleTest 入口（仅 CLion 单用例 Run/Debug；minicc 编译时本块被条件编译抹掉）──
#ifdef MINICC_GTEST_TEST
#include <gtest/gtest.h>
#define main minicc_gtest_body
static int minicc_gtest_body();
TEST(Integration, test_basics_26_bare_type_name_namespace_not_visible_error) { minicc_gtest_body(); }
#endif

namespace N { struct S { int x; }; }   // S 只在 N 内可见

int main() {
    S;                      // ★ 全局看不见 N::S ⇒ 必须报错（clang 逐字）
    return 0;
}
