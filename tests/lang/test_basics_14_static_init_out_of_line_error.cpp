// =============================================================================
// 测试（错误用例）：非 const 静态成员的类内初始化（[class.static.data]/3）
// =============================================================================
// 理论点：
//   · [class.static.data]/3：只有 const 整型族 / constexpr 的静态数据成员
//     才允许类内初始化；`static int count = 3;` 是 ill-formed —— 非 const
//     成员的存储只能来自类外定义。
// 预期行为：rc=1，报错文案
//   non-const static data member must be initialized out of line
// 对照 clang：clang++-18 -std=c++20 同样 rc=1，报错逐字相同（探针校准）。
// 回归背景（docs/BUGS.md B31）：修复前 Parser 对所有 static 数据成员一律
//   "not implemented" 硬性拒收，根本走不到这条判据。
// =============================================================================

struct A {
    static int count = 3;   // ✗ 非 const：必须类外定义
};

int main() { return 0; }
