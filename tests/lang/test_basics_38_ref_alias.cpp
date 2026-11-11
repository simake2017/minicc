// =============================================================================
// 测试：引用的别名语义（ROADMAP §0.1；BUGS.md B6）
// =============================================================================
// 理论点：
//   · [dcl.ref]/1：引用是【别名】，不是对象 —— "A reference is an alias for an
//     object and is not itself an object"（引用声明只是给另一个对象起个名字，
//     它不占存储、不参与析构、不有自己的值类别）。
//     ⇒ `int& r = a; r = 30;` 改的是 **a**，不是 r 自己那份拷贝。
//   · ★ 旧实现把引用当【值的副本】：声明时拷一份值、读写都落在自己那份槽上
//     ⇒ `r = 30` 改的是副本 ⇒ a 全程不变 ⇒ **静默算错**（BUGS.md B6）。
//   · 引用形参同款中招：调用点传的是值，callee 的写回不到调用方
//     ⇒ `void bump(int& x){ x = x + 1; }` 之后 a 不变 ⇒ 也是静默算错。
//   · 判定依据（本实现的单点判据 isAliasBinding）：
//     【有没有被绑定对象的地址】—— 没有地址就当不了别名。
//     · 局部引用变量：初始化式可寻址 ⇒ 有地址 ⇒ 当别名；
//     · 引用形参：实参在合法程序里【必然是左值】吗？只有"可写左值引用"能保证
//       （const T& / T&& 的实参可能是右值，右值没有地址可传）⇒ 只有它走别名通道。
//       调用点与被调点用同一判据、同一份类型 ⇒ 不会"一边传地址一边当值用"。
// 预期行为：rc=0（本文件每个检查点用不同的返回码定位失败的那一处）。
// 对照 clang：clang++-18 -std=c++20 同样 rc=0。
// =============================================================================

// ── GoogleTest 入口（仅 CLion 单用例 Run/Debug；minicc 编译时本块被条件编译抹掉）──
#ifdef MINICC_GTEST_TEST
#include <gtest/gtest.h>
#define main minicc_gtest_body
static int minicc_gtest_body();
TEST(Integration, test_basics_38_ref_alias) { minicc_gtest_body(); }
#endif

// ── 被引用的普通类 ──
struct S {
    int v;
};

// ── 自由函数：引用形参（可写 ⇒ 别名通道）──
void bump(int& x) {
    x = x + 1;
}

// ── 引用形参再传给另一个引用形参（形参传形参）──
void twice(int& x) {
    x = x * 2;
}

void relay(int& x) {
    twice(x);
}

// ── const 引用形参（边界：实参可能是右值 ⇒ 本实现按值传）──
int readConst(const int& x) {
    return x;
}

// ── 成员方法：this 与引用形参同时在场 ──
struct Counter {
    int v;
    void bump(int& x) {
        x = x + 1;
        v = v + 100;
    }
};

// ── 虚方法：引用形参要走虚调用三部曲 ──
struct Base {
    virtual void bump(int& x) { x = x + 1; }
    virtual ~Base() {}
};

struct Derived : Base {
    void bump(int& x) override { x = x + 10; }
};

// ── 函数模板：引用形参（T 推成实参的左值引用）──
template <class T>
void bumpTmpl(T& x) {
    x = x + 100;
}

int main() {
    // ① 局部引用变量写穿被引用对象（B6 的核心症状）
    {
        int a = 5;
        int& r = a;
        r = 30;
        if (a != 30) return 1;
    }

    // ② 局部引用读的是【被引用对象的当前值】，不是声明时的快照
    {
        int a = 5;
        int& r = a;
        a = 7;
        if (r != 7) return 2;
    }

    // ③ 引用形参写穿到调用方的对象
    {
        int a = 5;
        bump(a);
        if (a != 6) return 3;
    }

    // ④ 引用形参再传给另一个引用形参（形参传形参）
    {
        int a = 3;
        relay(a);
        if (a != 6) return 4;
    }

    // ⑤ 引用绑定到【堆对象的成员】：地址 = 对象地址 + 字段偏移
    {
        S* p = new S();
        p->v = 5;
        int& m = p->v;
        m = 9;
        if (p->v != 9) return 5;
    }

    // ⑥ 引用绑定到【另一个引用变量】：取的是被绑定对象的地址，不是副本
    {
        int a = 1;
        int& r = a;
        int& s = r;
        s = 7;
        if (a != 7) return 6;
    }

    // ⑦ const 局部引用：仍是别名（读跟踪被引用对象）
    {
        int a = 5;
        const int& cr = a;
        a = 8;
        if (cr != 8) return 7;
    }

    // ⑧ const 引用形参：值正确（右值实参也接得住）
    {
        int a = 5;
        if (readConst(a) != 5) return 8;
        if (readConst(6) != 6) return 9;
    }

    // ⑨ 类类型的引用：写穿到对象的字段（类值即地址）
    {
        S s;
        s.v = 1;
        S& rs = s;
        rs.v = 11;
        if (s.v != 11) return 10;
    }

    // ⑩ 成员方法的引用形参（this 与引用形参同时在场）
    {
        Counter c;
        c.v = 0;
        int a = 5;
        c.bump(a);
        if (a != 6) return 11;
        if (c.v != 100) return 12;
    }

    // ⑪ 虚方法的引用形参（虚调用三部曲 + 别名通道）
    {
        Base* p = new Derived();
        int a = 5;
        p->bump(a);
        if (a != 15) return 13;
    }

    // ⑫ 函数模板的引用形参（T 推成 int& ⇒ 仍是可写左值引用）
    {
        int a = 5;
        bumpTmpl(a);
        if (a != 105) return 14;
    }

    // ⑬ 引用变量作实参传给引用形参（调用点取的是"引用持有的地址"）
    {
        int a = 1;
        int& r = a;
        bump(r);
        if (a != 2) return 15;
    }

    return 0;
}
