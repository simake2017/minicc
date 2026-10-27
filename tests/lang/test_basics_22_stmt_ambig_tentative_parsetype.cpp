// =============================================================================
// 测试：语句层声明前瞻升级为「试探 parseType + 模板名登记表闸」（BUGS.md B35）
// =============================================================================
// 理论点：
//   · [stmt.ambig] 的语句层：标识符开头既可能是声明（`MyClass obj;`）也可能
//     是表达式（`a < b > c;`）。旧前瞻是"手写 token 跳过人"——只认
//     `名 [<...>] [::]* [*&]*` 的固定顺序：
//       ① `N::B<Box> x;`（限定名 + 模板 id）的 `::B<Box>` 段没人跳 ⇒
//          误判表达式，Sema 报 "Undefined variable 'N::B'"（拒收合法程序）；
//       ② `a < b > c;` 全变量比较链被误判成声明 `a<b> c`（B31 的语句层
//          兄弟，报错文案自己都承认是"试探法近似"）。
//   · 修复（对照 clang TryAnnotateTypeToken + tentative parse）：
//     TokenCheckpoint 存档 → parseType() 通用解析（限定名/模板 id/嵌套/
//     typename 全收，消掉第二份残缺语法）→ 后跟标识符 ⇒ 声明；
//     ';' 且带模板 id 语法 ⇒ B16 无声明符声明。★ 登记表第一层闸：类型
//     （含嵌套限定者链）里任一模板 id 层基名不在 B31 登记表 ⇒ 全部推翻——
//     否则 `a < b > c;` 会被 parseType 语法无误地解析成"模板 id + 声明符"
//     继续误判（语法无懈可击，必须问"名字的种类"这一个 bit）。
//   · 附带：Sema resolveType 补限定名模板 id 归一（`N::B<Box>` ⇒ 蓝图裸名
//     B ——蓝图按书写名登记，实例与书写位置无关，归一到 B_int 同一缓存键）。
// 预期行为：rc=0，运行返回 0；既有 152 份日志基线逐字节零漂移。
// 对照 clang：clang++-18 -std=c++20 同样 rc=0、返回 0。
// =============================================================================

// ── GoogleTest 入口（仅 CLion 单用例 Run/Debug；minicc 编译时本块被条件编译抹掉）──
#ifdef MINICC_GTEST_TEST
#include <gtest/gtest.h>
#define main minicc_gtest_body
static int minicc_gtest_body();
TEST(Integration, test_basics_22_stmt_ambig_tentative_parsetype) { minicc_gtest_body(); }
#endif

namespace N {
template<class T>
struct B { T v; };
}   // namespace N

class Box { public: int x; };

int main() {
    // ★ B35 现场①：限定名 + 模板 id 的语句层声明（旧前瞻漏跳 `::B<Box>` 段）
    N::B<Box> y;
    y.v.x = 40;

    // ★ B35 现场②：全变量比较链（旧前瞻误判成声明 a<b> c）
    int a = 1;
    int b = 2;
    int c = 3;
    a < b > c;                 // (a<b)>c，结果弃用 —— clang 仅 -Wunused-comparison

    // 回归：普通类声明 / 模板 id 声明 / 引用声明照旧走通
    Box plain;
    plain.x = 2;
    N::B<Box> z = y;           // const 外再验一遍平凡拷贝（同类左值）
    int sum = y.v.x + plain.x + z.v.x;   // 40+2+40 = 82
    return sum - 82;
}
