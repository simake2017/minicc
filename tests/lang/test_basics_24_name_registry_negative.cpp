// =============================================================================
// 测试（负例）：不存在的限定模板名 N::B 不再误绑全局模板 B
// =============================================================================
// 理论点：
//   · B35 的 Sema 尾剥对 `N::B<...>` 取末段查【全局】蓝图表——N::B 真存在时
//     归一正确，但 N::B 不存在而全局存在同名模板 B 时方向性错（误绑）。
//   · parser 名字识别后改为登记表 classifyQualified（scope 精确）裁决：
//     全局 B 的 scope="" 配不上 scope="N" ⇒ 响亮报错。
//   · 判据「名字在哪个作用域当真」单点：Parser 语句层闸（模板 id 层查表）、
//     表达式层闸、Sema 限定名裁决共用同一张登记表（spec 拍板 2）。
// 预期行为：rc=1，报错文案与 clang 逐字对齐（探针实测）：
//   [Semantic Error] no template named 'B' in namespace 'N'
//   （clang 另附 "did you mean simply 'B'?" 提示——本实现不做建议，见 spec OOS 5）
// =============================================================================

// ── GoogleTest 入口（仅 CLion 单用例 Run/Debug；minicc 编译时本块被条件编译抹掉）──
#ifdef MINICC_GTEST_TEST
#include <gtest/gtest.h>
#define main minicc_gtest_body
static int minicc_gtest_body();
TEST(Integration, test_basics_24_name_registry_negative) { minicc_gtest_body(); }
#endif

template<class T>
struct B { T w; };         // 全局模板 B（陷阱：N::B 不存在时会误绑它）

namespace N {}             // N 是空命名空间——里面没有 B

int main() {
    N::B<int> y;           // ★ 现场：N 里没有 B ⇒ 必须报错，不许落到全局 B
    return 0;
}
