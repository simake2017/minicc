// =============================================================================
// 测试：全局/命名空间内的类型别名（using / typedef → 模板实例）
// =============================================================================
// 理论点：
//   · [dcl.typedef]/1：别名声明**不产生新类型**，只是既有类型的另一个名字。
//     故 `using IntBox = Box<int>;` 的语义 = 在声明点把 `Box<int>` **解析成
//     成品类型**（实例 Box_int）再登记 —— 对照 clang：TypedefNameDecl 的底层
//     类型在声明点就完成了决议（Sema::getTypeName / BuildTypedefNameExpr）。
//   · 命名空间成员的名字遵循与类/函数相同的前缀化约定（processNamespaceDecl）：
//     `namespace N { using IntBox = ...; }` 注册的名字必须是 `N::IntBox`，
//     类外使用点 `N::IntBox b;` 带着限定名来查，裸名必然查无此名。
// 预期行为：三个别名的变量都按实例类型布局分配、成员可读写，运行返回 0。
// 对照 clang：clang++-18 -std=c++20 编译运行同样 rc=0。
// 回归背景（docs/BUGS.md B29）：修复前 processTypeAliasDecl 把【未实例化】的
//   模板 id 原样存进符号表（`IntBox b; b.v` 对着蓝图名 "Box" 查字段 ⇒
//   "No member 'v' in class 'Box'"），且注册名不带命名空间前缀（类外
//   `N::IntBox` ⇒ unknown type name）—— 合法程序被拒收。
// =============================================================================

// ── GoogleTest 入口（仅 CLion 单用例 Run/Debug；minicc 编译时本块被条件编译抹掉）──
#ifdef MINICC_GTEST_TEST
#include <gtest/gtest.h>
#define main minicc_gtest_body
static int minicc_gtest_body();
TEST(Integration, test_decl_03_global_alias) { minicc_gtest_body(); }
#endif

template<class T>
struct Box {
    T v;
};

using IntBox = Box<int>;          // ① 全局 using 别名 → 模板实例
typedef Box<int> IntBoxTypedef;   // ② 全局 typedef 别名（同一路径）

namespace N {
    using IntBox = Box<int>;      // ③ 命名空间内别名 → 名字须前缀化为 N::IntBox
}

int main() {
    IntBox a;                     // ① 全局 using
    a.v = 1;
    IntBoxTypedef b;              // ② 全局 typedef
    b.v = 2;
    N::IntBox c;                  // ③ 命名空间前缀使用点
    c.v = 3;
    // 三者都是同一个实例 Box_int（别名不是新类型）：
    // 1 + 2 + 3*10 - 33 = 0
    return a.v + b.v + c.v * 10 - 33;
}
