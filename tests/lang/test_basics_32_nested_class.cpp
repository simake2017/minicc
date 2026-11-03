// =============================================================================
// 测试（正例）：嵌套类（[class.nest]，Stage 3 批 4 —— 嵌套类落地）
// =============================================================================
// 理论点：
//   · [class.nest]/1：嵌套类【是外层类的成员】，没有特殊地位 —— 它有外层类的
//     访问权（能访问外层类的 private 成员），但【没有】外层类的隐式 this
//     （外层类的实例字段必须经显式对象访问：`o.val`）。
//   · [basic.scope.class]/2.1：类名被插入到【本类作用域】里 ⇒ `struct A { A* p; };`
//     里的 A 指本类（自引用指针）—— 本实现此前【拒收这类合法程序】。
//   · [basic.lookup.unqual]：名字查找由内向外 —— 类作用域 → 命名空间链 → 全局。
//     ★ 本测试的核心断言：类 A 里的嵌套类 B 与全局 B 【是两个实体】，
//     A 的成员函数里的裸名 `B` 必须是 A::B（clang 同款）。此前全局符号表先命中
//     ⇒ 静默绑到全局 B（不报错，只是算错）。
//   · [dcl.type.elab]/2：`new` 后面跟的是 type-id，type-id 可以是限定名 ⇒
//     `new A::B(6)`（嵌套类只能这样 new）。
//   · [basic.lookup.qual]：限定名调用 `B::sm()` 的限定者按【可见性】解析 ——
//     嵌套类的【可见名】("B") ≠ 【登记名】("A::B") ⇒ 调用点要按登记名查表。
//   · [class.member.lookup]：嵌套类是外层类的成员 ⇒ 嵌套类里能看见外层类的
//     static 成员（裸名）。
//   · 名字前缀化（learn/49 的"作用域即身份"）：嵌套类的登记名 = 外层类登记名 +
//     "::" + 裸名 ⇒ 类表键 / 符号表键 / mangling / 汇编符号全带作用域，
//     与全局 B 互不干扰（两个类型、两个符号、两个 vtable）。
// 预期行为：rc=0，运行返回 0（acc 全部检查点抵消）。
// 对照 clang：clang++-18 -std=c++20 编译运行同样 rc=0、返回 0。
// 边界（本测试【不】覆盖，见 docs/learn/53 §5）：
//   · 嵌套类写在【类模板】里（蓝图不克隆嵌套类 ⇒ 字段类型解析不到）；
//   · 类外定义嵌套类成员（`struct A::B { … };`）—— Parser 尚未支持类外成员定义；
//   · 嵌套类按值返回（`A::B f();`）—— 与普通类同款，属"按值返回/拷贝省略"缺口。
// =============================================================================

// ── GoogleTest 入口（仅 CLion 单用例 Run/Debug；minicc 编译时本块被条件编译抹掉）──
#ifdef MINICC_GTEST_TEST
#include <gtest/gtest.h>
#define main minicc_gtest_body
static int minicc_gtest_body();
TEST(Integration, test_basics_32_nested_class) { minicc_gtest_body(); }
#endif

// ── ① 嵌套类 vs 同名全局类：两个实体，互不干扰 ──
struct A {
    struct B {                       // 嵌套类：登记名 "A::B"
        int v;
        B() { v = 1; }              // 默认构造
        B(int x) { v = x; }         // 带参构造
        int get() { return v; }
    };
    int use() {                     // ★ 这里的裸名 B 必须是 A::B，不是全局 B
        B b(7);
        return b.get();
    }
    static int stat() { return 5; }
};
struct B {                          // ★ 同名全局类：与 A::B 是两个类型
    int w;
    int get() { return w; }
};

// ── ② 自引用指针字段（[basic.scope.class]/2.1：类名在本类作用域内可见）──
struct Node {
    Node* next;                     // ★ "Node" 指本类
    int v;
};

// ── ③ 嵌套类看见外层类的 static 成员（嵌套类是外层类的成员）──
struct Outer {
    static int sval;
    struct Inner {
        int get(Outer& o) { return o.val + sval; }   // 裸名 sval ⇒ 外层类的 static
        static int stat() { return sval * 2; }
    };
    int val;
    int use() { return Inner::stat() + 1; }           // ★ 限定名调用：可见名 B ⇒ 登记名 A::B
};
int Outer::sval = 10;

// ── ④ 命名空间里的嵌套类（登记名 "N::A::B"）──
namespace N {
struct A {
    struct B { int v; };
    B b;
};
}

// ── ⑤ 深层嵌套（A::B::C）──
struct Deep {
    struct B {
        struct C { int v; };
        C c;
    };
    B b;                              // 外层类持有一个 B（字段类型解析要能看见嵌套类）
};

int main() {
    int acc = 0;

    // ① 同名共存：限定名与全局名各指各的
    A::B ab;                          // ★ 限定名可达
    ab.v = 3;
    B gb;                             // ★ 全局 B
    gb.w = 4;
    acc = acc + ab.get() - 3;         // 3 —— 命中 A::B 的字段
    acc = acc + gb.get() - 4;         // 4 —— 命中全局 B 的字段
    if (ab.get() == gb.get()) return 1;   // ★ 两个实体：值不该相等（3 vs 4）

    A a;
    acc = acc + a.use() - 7;          // ★ 成员函数里的裸名 B ⇒ A::B（不是全局 B）

    A::B ab2(9);                     // 带参构造
    acc = acc + ab2.get() - 9;

    // ② 自引用指针字段
    Node n1;
    Node n2;
    n1.v = 5;
    n2.v = 6;
    n1.next = &n2;
    acc = acc + n1.next->v - 6;       // ★ 自引用指针可用

    // ③ 嵌套类看见外层类的 static 成员
    Outer o;
    o.val = 1;
    Outer::Inner in;
    acc = acc + in.get(o) - 11;       // 1 + 10
    acc = acc + o.use() - 21;         // sval*2 + 1 = 21

    // ④ 命名空间里的嵌套类
    N::A na;
    N::A::B nb;                       // ★ 登记名 "N::A::B"
    nb.v = 8;
    acc = acc + nb.v - 8;
    na.b.v = 12;
    acc = acc + na.b.v - 12;

    // ⑤ 深层嵌套
    Deep::B::C dc;                    // ★ 三级限定名
    dc.v = 13;
    acc = acc + dc.v - 13;
    Deep d;
    d.b.c.v = 14;
    acc = acc + d.b.c.v - 14;

    // ⑥ new 限定名类型（[dcl.type.elab]/2）
    A::B* p = new A::B(6);            // ★ 嵌套类只能这样 new
    acc = acc + p->get() - 6;

    return acc;
}
