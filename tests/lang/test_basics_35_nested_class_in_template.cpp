// =============================================================================
// 测试（正例）：类模板里的嵌套类（实例化步骤 5.8 —— 克隆嵌套类）
// =============================================================================
// 理论点：
//   · [class.nest]/1：嵌套类是其外层类的成员，**没有特殊地位** —— 外层类是
//     类模板时，嵌套类同样是**模板的成员** ⇒ 必须随实例化一起克隆
//     （[temp.inst]/5：实例化一个类模板，等价于实例化它的每个成员声明）。
//   · [temp.subst]：嵌套类的字段/方法里出现的**外层形参** T 与外层类自己的
//     字段一样要替换 —— `struct Inner { T v; }` 在 Box<int> 里必须变成
//     `int v`（不替换 ⇒ 字段大小 0、偏移错、"看起来能跑"但全是错的）。
//   · learn/49 的"作用域即身份"：嵌套类的登记名 = 外层类登记名 + "::" + 裸名
//     ⇒ `Box<int>::Inner` 的登记名是 **"Box_int::Inner"** —— 与全局 Inner、
//     与 Box_double::Inner 三者互不干扰（三个类型、三套符号、三个 vtable）。
//   · [class.nest] 的嵌套套嵌套：A::B::C 三级限定名同样成立
//     （`Deep<int>::A::B` ⇒ 登记名 "Deep_int::A::B"）。
//   · [basic.scope.class]/2.1：嵌套类的名字在**外层类作用域内**可见 ⇒
//     外层类可以拿嵌套类当字段类型（`Holder<T>::Cell c;`）。
// 预期行为：rc=0，运行返回 0（acc 全部检查点抵消）。
// 对照 clang：clang++-18 -std=c++20 编译运行同样 rc=0、返回 0。
// 边界（本测试【不】覆盖）：
//   · 嵌套类**按值返回**（`Box<int>::Inner f();`）—— 属"按值返回/拷贝省略"
//     缺口（docs/ROADMAP.md §0.1 批 0-1，与本批 0-2 是两件事）；
//   · 类外定义嵌套类成员（`struct Box<int>::Inner { … };`）—— Parser 未支持
//     任何类外成员定义（连非模板的也没有）；
//   · 嵌套类写在**别名模板**里 —— 别名不解糖成类，没有"实例化"可言
//     （[temp.alias]/1）。
// =============================================================================

// ── GoogleTest 入口（仅 CLion 单用例 Run/Debug；minicc 编译时本块被条件编译抹掉）──
#ifdef MINICC_GTEST_TEST
#include <gtest/gtest.h>
#define main minicc_gtest_body
static int minicc_gtest_body();
TEST(Integration, test_basics_35_nested_class_in_template) { minicc_gtest_body(); }
#endif

// ── ① 类模板里的嵌套类：字段/方法随外层形参替换 ────────────────────────────
template <class T> struct Box {
    struct Inner {
        T v;                        // ★ 外层形参 ⇒ Box<int> 里是 int
        Inner(T x) : v(x) {}        // 带参构造（形参类型也随替换）
        T get() { return v; }       // 方法体里的 v 也随替换
    };
};

// ── ② 外层类持有嵌套类型的字段（嵌套类名在类作用域内可见）──────────────────
template <class T> struct Holder {
    struct Cell { T v; };
    Cell c;                        // ★ 字段类型是本类的嵌套类
};

// ── ③ 嵌套套嵌套（三级限定名）─────────────────────────────────────────────
template <class T> struct Deep {
    struct A {
        struct B { T v; };
        B b;                        // ★ A 持有 B
    };
};

int main() {
    int acc = 0;

    // ① 限定名取到嵌套类：构造 / 字段 / 方法三条路都通
    Box<int>::Inner i(3);
    acc = acc + i.get() - 3;         // ★ 方法体里的 T 已替换成 int
    i.v = 5;
    acc = acc + i.v - 5;             // ★ 字段类型已替换（不是 T、不是 0 字节）

    // ② 第二个实例 ⇒ 【另一个】嵌套类（登记名 Box_double::Inner，符号另一套）
    //    ★ 两套符号若撞名，汇编期就会报 "symbol already defined" ⇒ 本用例
    //      同时钉住"不同实例的嵌套类是不同实体"。
    Box<double>::Inner d(2);
    d.v = 2;

    // ③ 外层类持有嵌套类型的字段
    Holder<int> h;
    h.c.v = 4;
    acc = acc + h.c.v - 4;

    // ④ 嵌套套嵌套：三级限定名
    Deep<int>::A::B b;
    b.v = 6;
    acc = acc + b.v - 6;
    Deep<int>::A a;
    a.b.v = 8;
    acc = acc + a.b.v - 8;

    return acc;
}
