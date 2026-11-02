// =============================================================================
// 测试（正例）：嵌套命名空间的限定名调用 `A::B::f()`（B38）
// =============================================================================
// 理论点：
//   · [basic.scope.namespace] 命名空间可以嵌套，作用域是【路径】不是单层：
//     `namespace A { namespace B { … } }` 里 B 的全名是 `A::B`。
//   · [basic.lookup.qual]：限定名 `A::B::f` 沿 A → B 的 DeclContext 链查找。
//   · ★ 修复前的症状（docs/BUGS.md B38）：前缀化只取【本层】名字
//     （processNamespaceDecl 用 decl->name 而不是 m_currentNamespace）
//     ⇒ `A::B::f` 被注册成 `B::f` ⇒ 使用名与注册名对不上
//     ⇒ `Undefined variable 'A::B::f'`（链接期前就报错）。
//   · 判据单点（红线 3）：前缀化只有一处（processNamespaceDecl），
//     命名空间里的函数 / 类 / 全局变量 / 枚举 / 模板 共用同一句。
// 预期行为：rc=0，运行返回 0。
// 对照 clang：clang++-18 -std=c++20 同样 rc=0、返回 0。
// =============================================================================

// ── GoogleTest 入口（仅 CLion 单用例 Run/Debug；minicc 编译时本块被条件编译抹掉）──
#ifdef MINICC_GTEST_TEST
#include <gtest/gtest.h>
#define main minicc_gtest_body
static int minicc_gtest_body();
TEST(Integration, test_basics_29_nested_namespace_qualified_call) { minicc_gtest_body(); }
#endif

namespace A {
namespace B {
int f() { return 7; }          // 全名应是 "A::B::f"
struct S { int x; };           // 全名应是 "A::B::S"
}   // namespace B
}   // namespace A

int main() {
    if (A::B::f() != 7) return 1;      // ★ 限定名调用：注册名必须是 "A::B::f"
    A::B::S s;
    s.x = 3;
    if (s.x != 3) return 2;            // ★ 限定名类型：注册名必须是 "A::B::S"
    return 0;
}
