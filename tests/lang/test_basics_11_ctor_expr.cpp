// =============================================================================
// 测试：表达式级类模板构造调用 `Box<int>(7)` / `Box(9)`（CTAD）与平凡拷贝/赋值
// =============================================================================
// 理论点：
//   · [expr.type.conv]/1：函数式写法 `Box<int>(7)` 是**显式类型转换**表达式，
//     语义 = 以 7 为实参构造一个 Box_int 临时对象，表达式结果是该对象（左值）。
//     此前 minicc 的 Parser 能建出 CallExpr(VarExpr(Box)+显式模板实参)，
//     但 inferCall 一路落到"Undefined variable 'Box'"——缺口全在语义层。
//   · [dcl.type.class.deduct]（CTAD）：`Box(9)` 由构造实参反推类模板形参，
//     与函数模板推导是同一套合一算法的反向使用；语句级 `Box<int> b(9);`
//     与表达式级 `Box(9)` 共用 deduceCtadCore。
//   · [class.copy.elision]：拷贝初始化 `Box<int> b = Box<int>(7);` 允许
//     省略临时对象、直接构造进 b 的槽（clang 默认这么做）。本实现把它
//     **升级成直接初始化**（[ctor] ★ 日志）：临时对象永不物化。
//   · 平凡拷贝（[class.copy]/6 的合成拷贝构造）：用户没写拷贝构造时，
//     合成构造的语义就是逐字节复制对象表示 ⇒ `Box<int> b = a;` 按布局复制。
//   · 合成的拷贝赋值（[class.copy.assign]）：本语言没有 operator=，类实例
//     赋值 `b = a;` 的语义同样是按内存复制对象表示。
//   · 全表达式结束析构临时对象（[class.dtor]/2）：`b = Box<int>(9);` 的
//     临时对象在 memcpy 进 b 之后才析构 —— m_suppressTempDtor 协作。
//   · ★ 自研链接器只内置 malloc/free：对象复制用 rep movsb 内联展开
//     （[builtin] 阶段注册的 memcpy 只是原型，没有定义可绑）。
// 预期行为：六个检查点全部命中，运行返回 0。
// 对照 clang：clang++-18 -std=c++20 编译运行同样 rc=0、返回 0。
// 回归背景（docs/BUGS.md B30）：修复前 ① 报 Undefined variable 'Box'（rc=1）；
//   ② `Box<int> b = a;` 掉到"默认构造"分支发不存在的 0 参符号 ⇒ 链接期
//   undefined reference（编译期全绿的错配）；③ `b = a;` 被当标量赋值只写
//   8 字节且源地址错 —— 类对象赋值静默算错。另有 cloneStmt 漏搬 ctorArgs
//   的伴随 bug（模板体内的变量声明直接初始化和顶层行为不一致）。
// =============================================================================

// ── GoogleTest 入口（仅 CLion 单用例 Run/Debug；minicc 编译时本块被条件编译抹掉）──
#ifdef MINICC_GTEST_TEST
#include <gtest/gtest.h>
#define main minicc_gtest_body
static int minicc_gtest_body();
TEST(Integration, test_basics_11_ctor_expr) { minicc_gtest_body(); }
#endif

template<class T>
struct Box {
    T v;
    Box(T x) { v = x; }
    int get() { return v; }
};

int main() {
    int acc = 0;

    Box<int> e1 = Box<int>(7);    // ① 拷贝初始化 + 拷贝省略升级
    acc = acc + e1.get() - 7;

    auto e2 = Box<int>(7);        // ② auto 反推 + 构造表达式
    acc = acc + e2.get() - 7;

    Box<int> e3(3);               // ③ 类实例赋值，RHS 是构造临时对象
    e3 = Box<int>(9);
    acc = acc + e3.get() - 9;

    auto e4 = Box(9);             // ④ CTAD：表达式级推导指引
    acc = acc + e4.get() - 9;

    Box<int> s1a(7);              // ⑤ 平凡拷贝初始化（同类左值）
    Box<int> s1 = s1a;
    acc = acc + s1.v - 7;

    Box<int> s2a(7);              // ⑥ 合成的拷贝赋值
    Box<int> s2(1);
    s2 = s2a;
    acc = acc + s2.v - 7;

    return acc;                   // 全中 ⇒ 0
}
