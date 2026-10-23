// =============================================================================
// 测试：static 数据成员（[class.static.data]，docs/BUGS.md B33）
// =============================================================================
// 理论点：
//   · [class.static.data]/1：static 数据成员【不属于】任何对象 —— 不占类布局
//     （sizeof(A) 不含它），本质上是"挂在类名上的全局变量"，类内只是【声明】。
//   · [class.static.data]/3：const 整型族 / constexpr 的静态成员允许类内初始化，
//     此时它是编译期常量、【无存储】，读取一律折叠成字面量（本实现：常量表
//     staticConsts + registerConstVar 双登记）；非 const 成员的存储只能来自
//     类外定义（`int A::count = 3;`），漏定义又在 odr-use ⇒ 链接期响亮报错
//     （与 clang 的 undefined reference 同形态）。
//   · 三条读取路径判据必须单点（承 B21/B22 教训）：
//       A::count   —— 限定名，Parser 粘成单名 → 符号表 "A::count"（inferVar 第 0 步）
//       a.count    —— 对象裸名：布局落空 → 符号表第二源（inferMember 普通分支回退）
//       a.A::count —— 对象限定名：resolveQualifiedField 第二源（符号表 BFS 基类链）
//     三个写入口落到【同一个】全局符号 —— 本测试用互相印证的写读序列钉死。
//   · 成员函数内裸名 `count`/`limit`：inferVar 第 2.5 步（类作用域 + 基类链 BFS）；
//     常量裸名（`limit`）还多一条路：constResolver 回调的类作用域补查
//     （ReturnStmt::visit 先 fold 后 infer，等不到改名）。
//   · [class.member.lookup]/3 隐藏：派生类【实例字段】与基类【静态成员】同名时，
//     按名字查 ⇒ 实例字段优先（Hides::s 隐藏 Base::s，两个是不同实体）。
//   · 模板类：实例化时把蓝图的 staticDataMembers 替换后挂到实例类
//     （instantiateClassTemplate 步骤 5.7），`C<int>::cid` 走既有的
//     isTypeAccess 折叠（foldStaticConst，与 std::true_type::value 同路）。
//   · 命名空间类：类外定义 `int N::S::n = 8;` 是多级限定名，校验按全名
//     "N::S" 查类注册表（m_classDecls 以全名登记）。
// 预期行为：运行返回 0（acc 全部检查点抵消）。
// 对照 clang：clang++-18 -std=c++20 编译运行同样 rc=0、返回 0。
// 回归背景（B33）：修复前类体内 `static int count;` 直接报
//   "static data members are not implemented"（Parser 硬性拒收，rc=1）。
// =============================================================================

// ── GoogleTest 入口（仅 CLion 单用例 Run/Debug；minicc 编译时本块被条件编译抹掉）──
#ifdef MINICC_GTEST_TEST
#include <gtest/gtest.h>
#define main minicc_gtest_body
static int minicc_gtest_body();
TEST(Integration, test_basics_13_static_member) { minicc_gtest_body(); }
#endif

// ── ① 类内声明 + 类外定义（唯一存储）+ 成员函数内裸名 ──
struct Counter {
    static int count;                 // 声明：不占对象内存
    static const int limit = 7;       // const 整型族类内初始化 ⇒ 编译期常量，无存储
    static constexpr int twice = 14;  // constexpr：同上
    int bump() { count = count + 1; return count; }
    int sum() { return count + limit + twice; }  // 裸名：count 走符号表、limit/twice 走常量传播
};
int Counter::count = 3;               // 类外定义：唯一存储（全局符号 Counter::count）

// ── ② 继承：静态成员沿基类链可见；实例字段按名字隐藏 static ──
struct Base {
    static int s;
    static const int cs = 11;
};
int Base::s = 2;
struct Derived : Base {
    int x;                            // Derived 自己的实例字段
};
struct Hides : Base {
    int s;                            // 实例字段【隐藏】基类静态成员 s（[class.member.lookup]/3）
};

// ── ③ 命名空间类：类外定义是多级限定名 N::S::n ──
namespace N {
struct S {
    static int n;
    static const int cn = 4;
};
}
int N::S::n = 8;

// ── ④ 模板类：实例化时静态成员（含常量表）克隆到实例类 ──
template<class T>
struct C {
    static const int cid = 5;
    T v;
    C(T x) { v = x; }
};

// ── ⑤ NTTP 实参取静态常量（常量表登记的用途之一）──
template<int N>
struct Buf {
    int get() { return N; }
};

int main() {
    int acc = 0;

    Counter c;
    acc = acc + Counter::count - 3;      // A. 类名::成员
    acc = acc + c.count - 3;             // B. 对象.成员（同一存储）
    acc = acc + c.Counter::count - 3;    // C. 对象.类名::成员（同一存储）
    c.bump();                            // 成员函数内裸名写：count = 4
    acc = acc + Counter::count - 4;
    Counter::count = 10;                 // 写入口 A
    acc = acc + c.count - 10;
    c.count = 20;                        // 写入口 B
    acc = acc + Counter::count - 20;
    c.Counter::count = 30;               // 写入口 C
    acc = acc + Counter::count - 30;
    acc = acc + c.limit - 7;             // 常量：对象裸名路径折叠
    acc = acc + c.Counter::limit - 7;    // 常量：对象限定路径折叠
    acc = acc + c.sum() - 51;            // 30 + 7 + 14 = 51（成员函数内裸名三读）

    Hides h;
    h.s = 100;                           // 实例字段（隐藏 static，两个实体互不影响）
    acc = acc + h.s - 100;
    acc = acc + h.Base::s - 2;           // 限定名穿透隐藏，拿到基类静态成员
    acc = acc + Base::s - 2;

    Derived d;
    d.x = 1;
    acc = acc + d.s - 2;                 // 裸名：沿基类链找到 Base::s
    acc = acc + d.Derived::s - 2;        // 限定名从 Derived 起 BFS，同样落到 Base::s
    acc = acc + d.cs - 11;               // 常量沿基类链折叠
    d.s = 9;                             // 写也透传到 Base 的唯一存储
    acc = acc + Base::s - 9;

    acc = acc + N::S::n - 8;             // 命名空间类的静态成员
    acc = acc + N::S::cn - 4;
    N::S::n = 12;
    acc = acc + N::S::n - 12;

    C<int> ci(7);                        // 模板类实例化
    acc = acc + ci.cid - 5;              // 对象路径：实例类的常量表
    int ce = C<int>::cid;                // isTypeAccess 路径：foldStaticConst
    acc = acc + ce - 5;                  // （该折叠现只在初始化式挂点，见 docs/learn/44 边界）
    acc = acc + ci.v - 7;

    Buf<Counter::twice> bf;              // NTTP 实参 = 静态常量（常量表全名命中）
    acc = acc + bf.get() - 14;

    return acc;                          // 全中 ⇒ 0
}
