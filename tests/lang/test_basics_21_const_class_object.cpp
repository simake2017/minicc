// =============================================================================
// 测试：const 类对象的端到端支持（BUGS.md B34）
// =============================================================================
// 理论点：
//   · `const Box_int b;` 的声明类型是 Const(Box_int) —— CodeGen/Sema 的类性
//     判定（查类表、isClass()）此前不看穿这层 Const 包壳，四散出错：
//       ① Sema inferMember 误报 "Cannot access member on non-class type
//          'const ClassA'"（拒收合法程序）；
//       ② CodeGen visit(VarDeclStmt) 查类表落空 ⇒ const 类对象掉标量路径：
//          无 RAII 构造/析构，读成员被当成"变量存指针再解引用"（实测 SIGSEGV）；
//       ③ 平凡拷贝判据 type->equals(initType) 对 const 目标不成立 ⇒ 拷贝初始化
//          静默退化成默认构造（读出来是 0，静默算错）。
//   · 判据归一：stripTopCvRef / bareForClass —— const 只管写检查（未做，
//     ROADMAP 已列），类性/查找一律剥壳。成员【查找】不受对象 cv 影响
//     （[class.mfct.non-static]）。
// 预期行为：rc=0，运行返回 0；既有用例逐字节零漂移（tmpl_50 除外——它此前
//   正落在错误的回退路径上，修复后走正确的字段偏移路径，汇编改善、行为不变，
//   有意漂移已重刷基线）。
// 对照 clang：clang++-18 -std=c++20 同样 rc=0、返回 0。
// =============================================================================

// ── GoogleTest 入口（仅 CLion 单用例 Run/Debug；minicc 编译时本块被条件编译抹掉）──
#ifdef MINICC_GTEST_TEST
#include <gtest/gtest.h>
#define main minicc_gtest_body
static int minicc_gtest_body();
TEST(Integration, test_basics_21_const_class_object) { minicc_gtest_body(); }
#endif

class Counter {
public:
    int n;
    Counter() { n = 1; }
    int get() const { return n; }
};

class Widget {
public:
    int w;
    Widget(int x) { w = x; }
};

template<class T>
struct Box {
    T value;
    Box() { value = 0; }   // 用户提供默认构造 —— 否则 const 对象默认初始化 clang 拒收
};

int main() {
    // 语句层 const 声明（parseStatement 2227 分支，KwConst ∈ isTypeKeyword）
    const int x = 1;

    // const 对象 + 默认构造 + const 成员函数调用
    const Counter c;
    // const 源 → const 目标的平凡拷贝
    const Counter d = c;
    // const 对象 + 带参构造
    const Widget g(7);
    // const 模板类对象 + 零初始化 + 成员读
    const Box<int> b;

    int sum = x + c.get() + d.get() + g.w + b.value;   // 1+1+1+7+0 = 10
    return sum - 10;
}
