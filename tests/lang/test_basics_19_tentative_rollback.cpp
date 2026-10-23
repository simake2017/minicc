// =============================================================================
// 测试：试探-回滚不留幽灵日志行（BUGS.md B32，ROADMAP 主线 I 第 2 项）
// =============================================================================
// 理论点：
//   · template-id 消歧靠"空跑实参表再决定"：IDENT '<' 后跟 '(' 或 '::' 才算
//     template-id，否则回滚按比较运算重解析（[stmt.ambig] 的表达式版）。
//   · 修复前回滚只撤 m_pos：试探期 parseType/parseTemplateArgumentList 打的
//     [parse:type]/[parse:targ] 行留在输出里，与最终 AST 对不上（幽灵行）。
//     本项目"日志即契约"——基线曾把这 14 行幽灵固化进 5 个文件。
//   · 修复后 TokenCheckpoint 把"存档 + 回卷 + 日志缓冲"做成 RAII：试探期打点
//     进缓冲，commit 才冲刷（成功路径逐字节不变），回卷则整体丢弃。
//   · 本文件的断言在【日志】里：基线（lang__test_basics_19_tentative_rollback.log）
//     不得出现任何 [parse:targ] / 成对的 [parse:type] 试探残留 —— 配合
//     logdiff 逐字节比对钉死。
// 预期行为：rc=0，运行返回 0；日志无幽灵行（见基线）。
// 对照 clang：clang++-18 -std=c++20 同样 rc=0、返回 0。
// =============================================================================

// ── GoogleTest 入口（仅 CLion 单用例 Run/Debug；minicc 编译时本块被条件编译抹掉）──
#ifdef MINICC_GTEST_TEST
#include <gtest/gtest.h>
#define main minicc_gtest_body
static int minicc_gtest_body();
TEST(Integration, test_basics_19_tentative_rollback) { minicc_gtest_body(); }
#endif

class Box { public: int v; };

int main() {
    // ★ B32 原始现场 `a < Box > c;` 经 clang++-18 实测是【非法程序】
    //   （"'a' does not name a template but is followed by template arguments"）——
    //   按"写用例前先过 clang"的规矩，本文件只用 clang 认可的合法形态钉幽灵行。
    //   minicc 对该非法写法的宽松放行另记 B32 台账边界（等 B31 登记表落地再裁）。
    int a = 1;
    int c = 3;
    // ① 循环条件里的 i < 10：试探把 3 吃掉当 NTTP 实参 ⇒ 回滚。
    //   修复前每个 for 留 1 行幽灵 [parse:targ] NTTP。
    int sum = 0;
    for (int i = 0; i < 3; i = i + 1) sum = sum + i;
    for (int i = 0; i < 2; i = i + 1) sum = sum + i;
    // ② 连续比较 x < y < z：parsePrimaryExpr(x) 处试探（y 被当类型实参、
    //   期望 '>' 撞 '<' 抛 ParseError）⇒ 回滚按比较链重解析。
    //   修复前每次比较留 2 行幽灵 [parse:type]（base = y / final type = y）。
    int x = 1;
    int y = 2;
    int z = 3;
    a = c;                              // ①的两个变量留个使用点
    bool chained = x < y < z;           // (1<2) → true；true < 3 → true
    sum = sum + (chained ? 100 : 0);
    // ② sum = (0+1+2) + (0+1) = 4；+100 ⇒ 104
    return sum - 104;                  // 全中 ⇒ 0
}
