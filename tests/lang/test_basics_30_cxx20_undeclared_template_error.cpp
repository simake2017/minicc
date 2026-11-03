// =============================================================================
// 测试（负例）：C++20 的 UndeclaredTemplate 规则 —— 未声明的名字 + '<' 按模板 id 解释
// =============================================================================
// 理论点：
//   · [temp.names]/3 与 C++20（P0846R0）：名字在【当前作用域】查不到、且后面
//     跟着 '<' 时，解析器【必须假定】它是模板名 —— clang 的
//     Sema::ClassifyName 的 UndeclaredTemplate 分支：
//     "we're required to assume that this is a template name"。
//   · ★ 本实现此前靠"名字登记表"（前向扫 token 流猜出来的【跨作用域并集】）
//     决定要不要试探实参表 ⇒ 未声明的名字压根不进试探 ⇒ `unknown<int>(y)`
//     被当成比较链 ⇒ 报 "Undefined variable 'unknown'"（clang 只报 'y' 那一处）。
//     换成活作用域表 + C++20 假定后：模板 id 路径生效 ⇒ 实参 'y' 未声明 ⇒
//     报 "use of undeclared identifier 'y'"（与 clang 逐字，连列号都一样）。
//   · ★ 另一处受益：`a<b>c;` 现在按【声明】解释（clang 的 AST 同样是
//     DeclStmt/VarDecl 'c'，不是比较链）—— 此前 minicc 走比较链报
//     "Undefined variable 'a'"。
// 预期行为：rc=1，报错文案与 clang 逐字对齐：
//   [Semantic Error] 33:18: use of undeclared identifier 'y'
//   （日志里另有一行 "[parse] template-id: unknown<1 explicit arg(s)>" ——
//     证明走的是模板 id 路径，不是比较链。）
// 对照 clang：clang++-18 -std=c++20 同样 rc=1、同样文案、同样列号 33:18。
// =============================================================================

// ── GoogleTest 入口（仅 CLion 单用例 Run/Debug；minicc 编译时本块被条件编译抹掉）──
#ifdef MINICC_GTEST_TEST
#include <gtest/gtest.h>
#define main minicc_gtest_body
static int minicc_gtest_body();
TEST(Integration, test_basics_30_cxx20_undeclared_template) { minicc_gtest_body(); }
#endif

int main() {
    unknown<int>(y);   // ★ 未声明的名字 + '<' ⇒ 按模板 id 解释；'y' 未声明 ⇒ 报错
    return 0;
}
