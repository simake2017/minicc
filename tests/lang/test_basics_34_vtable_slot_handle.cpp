// =============================================================================
// 测试（正例）：vtable 槽挂【声明句柄】而非符号串（Stage 3 批 5）
// =============================================================================
// 理论点：
//   · [class.virtual]：虚函数在 vtable 里占槽；派生类的实现【占同一个槽】
//     （不是新增槽）⇒ 槽位身份 = 方法身份。
//   · 对照 clang：vtable 的槽里放的是 **CXXMethodDecl 句柄**
//     （clang/lib/CodeGen/VTables.cpp 的 VTableComponent ⇒ GlobalDecl），
//     符号名由同一个 MangleContext 产出 ⇒ 结构上不可能出现
//     "槽里的名字"与"定义的名字"不一致。
//   · 本实现此前在**两处各算一遍**符号（Pass 1 算槽、Pass 2 算定义）——
//     两处虽共用 memberMethodSymbolName，却跑在不同时刻、看到不同状态的
//     形参类型（Pass 1 未解析 / Pass 2 已解析）⇒ 同一方法两个符号
//     ⇒ .quad 命中不到 .globl ⇒ 链接期 undefined reference（BUGS.md B45）。
//     本测试第 ① 组正是那个现场：嵌套类作虚函数重载的形参类型。
//   · 析构槽的判据：看槽指向的声明是不是析构（名字 "~X"），
//     而不是在符号串里找 "dtor" 子串 —— 子串匹配会把叫 my_dtor_helper
//     的普通虚函数误认成析构 ⇒ 少发/错发一次虚调用（BUGS.md B45 附带修）。
// 预期行为：rc=0，运行返回 0（acc 全部检查点抵消）。
// 对照 clang：clang++-18 -std=c++20 编译运行同样 rc=0、返回 0。
// 边界（本测试【不】覆盖）：
//   · 类模板实例里的虚表槽（实例化步骤 5.x 会克隆方法，但槽的句柄指向
//     实例方法 —— 已随本批一并生效，见 docs/learn/54 §6）；
//   · 虚析构 + 多继承 thunk 的组合（tests/mi/test_mi_12 已覆盖 thunk 侧）。
// =============================================================================

// ── GoogleTest 入口（仅 CLion 单用例 Run/Debug；minicc 编译时本块被条件编译抹掉）──
#ifdef MINICC_GTEST_TEST
#include <gtest/gtest.h>
#define main minicc_gtest_body
static int minicc_gtest_body();
TEST(Integration, test_basics_34_vtable_slot_handle) { minicc_gtest_body(); }
#endif

// ── ① B45 现场：嵌套类作虚函数【重载】的形参类型 ────────────────────────────
// 同名同个数的两个重载 ⇒ 符号要挂形参类型链（[over.match]）⇒
// Pass 1 看到的形参类型是未解析的 "Inner"，Pass 2 看到的是已解析的
// "Outer::Inner" ⇒ 两处算出两个符号 ⇒ 槽里的 .quad 指向没人定义的符号。
struct Outer {
    struct Inner { int v; };
    virtual int f(Inner i) { return i.v + 1; }   // ★ 形参类型是嵌套类
    virtual int f(int i) { return i + 2; }       // ★ 同名同个数的另一个重载
};

// ── ② 名字里含 "dtor" 的普通虚函数【不是】析构 ─────────────────────────────
// 旧实现在符号串里找 "dtor" 子串 ⇒ 把本方法误认成析构 ⇒ 作用域结束时
// 析构调用打在错的槽上 ⇒ 真正的析构函数根本没被调用（静默少调一次）。
int g_dtorCalls = 0;
struct Widget {
    int v;
    virtual int my_dtor_helper() { return 7; }   // ★ 含 "dtor" 但不是析构
    virtual ~Widget() { g_dtorCalls = g_dtorCalls + 1; }
};

// ── ③ 覆写：派生类占基类的同一个槽（动态分派仍然成立）────────────────────────
struct BaseV {
    virtual int who() { return 1; }
};
struct DerivedV : public BaseV {
    int who() { return 2; }                      // ★ 覆写（不写 virtual 也算）
};

int main() {
    int acc = 0;

    // ① 嵌套类形参的两个重载各自命中自己的槽
    Outer o;
    Outer::Inner in;
    in.v = 10;
    acc = acc + o.f(in) - 11;                    // 虚分派 ⇒ f(Inner) ⇒ 10+1
    acc = acc + o.f(5) - 7;                      // 虚分派 ⇒ f(int)    ⇒ 5+2

    // ② 名字含 "dtor" 的虚函数照常调用；真析构在作用域结束时跑一次
    {
        Widget w;
        w.v = 3;
        acc = acc + w.my_dtor_helper() - 7;
        acc = acc + w.v - 3;                     // 字段访问顺带钉一次
    }
    acc = acc + g_dtorCalls - 1;                 // ★ 真析构恰好被调用一次

    // ③ 动态分派：经基类指针调到派生类实现
    DerivedV dv;
    BaseV* bp = &dv;
    acc = acc + bp->who() - 2;

    return acc;
}
