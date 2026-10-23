// =============================================================================
// 测试：模板名登记表消歧（BUGS.md B31，ROADMAP 主线 I 第 1 项）
// =============================================================================
// 理论点：
//   · [stmt.ambig] 的表达式版：`a<b>(c)` 里 IDENT '<' 既可能是 template-id
//     （foo<int>(x)）也可能是小于号（a<b 再 >(c)）。旧消歧只看【试探能不能
//     空跑出实参表 + '>' 后跟 '(' 或 '::'】——`a<b>(c)` 全变量时"实参表"竟
//     也空跑成功（b 被当成类型名），误判成模板调用，Sema 阶段才炸：
//     拒收合法程序（clang rc=0）。
//   · 修复（对照 clang Parser::isTemplateName / Sema::isTemplateName 的
//     "问 Sema 一个 bit"做法）：Parser 构造时前向扫描 token 流建【只读
//     模板名登记表】（类/别名/函数模板、推导指引、成员模板、模板模板形参
//     全收），消歧第一层闸 = 查表。名字不在表 ⇒ 直接按比较运算解析，
//     连试探都不做。真错误仍由 Sema 报 —— 登记表只做语法 hint，不打破分层。
//   · 保守方向：名字在表中【也】可能是比较（变量可遮蔽模板名），仍走
//     试探-回滚；试不出来回退比较。反方向（漏登记 ⇒ 真 template-id 被
//     当成比较链）才是错的 —— 故 std::declval 这类 Sema 内建模板名
//     （不经 token 流）以种子名单收进表。
// 预期行为：rc=0，运行返回 0；真模板调用（函数模板 / 类型限定静态成员访问）
//   逐字节不受影响（既有 150 份基线零漂移钉死）。
// 对照 clang：clang++-18 -std=c++20 同样 rc=0、返回 0。
// =============================================================================

// ── GoogleTest 入口（仅 CLion 单用例 Run/Debug；minicc 编译时本块被条件编译抹掉）──
#ifdef MINICC_GTEST_TEST
#include <gtest/gtest.h>
#define main minicc_gtest_body
static int minicc_gtest_body();
TEST(Integration, test_basics_20_template_id_disambiguation) { minicc_gtest_body(); }
#endif

// ── 真模板（消歧必须照旧走通的三种形态）──
template<class T> T identity(T x) { return x; }

template<class T>
struct Trait { static const int value = 5; };

class Box { public: int v; };

int main() {
    int a = 1;
    int b = 2;
    int c = 3;
    // ★ B31 现场：全变量比较链。修复前被误判为"模板 a 的调用"⇒ rc=1。
    //   左结合：(a<b)>(c) = (1<2)>(3) = 1>3 = 0
    int r = a < b > (c);
    // 无空格形态同理
    int d = 3;
    int e = 2;
    int f = 1;
    int s = d<e>(f);   // (3<2)>(1) = 0>1 = 0

    // 真模板调用：函数模板（显式实参）
    int g = identity<int>(6);
    // 真类型限定访问：Trait<int>::value（static const 折叠）
    int h = Trait<int>::value;
    // 类模板直接初始化（parseType 路径，不经过本消歧，一并回归）
    Box bx;
    bx.v = 7;

    int sum = r + s + g + h + bx.v;   // 0+0+6+5+7 = 18
    return sum - 18;
}
