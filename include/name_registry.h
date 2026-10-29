#pragma once
// =============================================================================
// 名字登记表（Name Registry）——parser 阶段语义识别的唯一机制
// =============================================================================
// 对应 spec：docs/specs/parser-stage-name-recognition.spec.md（2026-10-23 签字）
// 对应设计：docs/designs/parser-stage-name-recognition.design.md
// 对照 clang：Sema::ClassifyName（clang/lib/Sema/SemaDecl.cpp:912）——parser 侧只保留只读投影
// 学习文档：docs/learn/47
//
// 管线位置：预处理器 → Lexer → 【collect：构造期前向收集】→ Parser（只读查询）
//           → Sema（只读查询）→ …
//
// 对照 clang（~/cppproject/llvm-project/）：
//   NameClassification 骨架 ⇐ Sema::ClassifyName 的返回形状
//                             （clang/lib/Sema/SemaDecl.cpp:912）
//   isTemplateName()        ⇐ Sema::isTemplateName
//                             （clang/lib/Sema/SemaTemplate.cpp:179）
//   collect() 前向扫描      ⇐ clang 【没有】这步——它查活符号表（Scope/DeclContext
//                             链）。本实现因红线「语法期零语义」（Parser 不持 Sema
//                             句柄，B16/B21 确立、B31 首个实现）改为构造期收集
//                             只读投影：信息面只有「名字是哪类」几个 bit，
//                             没有类型实体/重载集/偏移。
//
// 分层红线（spec 否决记录 1）：本表是 Sema 符号表的【投影】而非替代品——
//   消费点一律「位命中才走新支」，查不到 = 未知 = 宁宽勿窄（B31）。
// =============================================================================

#include "token.h"
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace minicc {

// =============================================================================
// 名字分类结果（对齐 clang NameClassification 的骨架，spec 拍板 1）
// =============================================================================
// ★ 空值语义（钉死）：查询未命中 = 返回默认对象（四位全 false），
//   【不是】"存在但 bits 为空"。消费点一律按「位未命中」处置，
//   禁止由「全 false」反向推断"它不是类型"（spec 异常 1）。
// ★ isValue 恒 false：值名（变量/函数）不经 token 流前向收集（OOS 2），
//   该位仅作骨架占位，【不参与任何判定】。
struct NameClassification {
    bool isType = false;       // class/struct/enum/typedef/using/别名 声明的名字
    bool isNamespace = false;  // `namespace X {` 与 `namespace A = B;` 的名字
    bool isTemplate = false;   // template 头家族（类/别名/函数模板、推导指引、
                               // 成员模板、模板模板形参 —— B31 全集原样搬迁）
    bool isValue = false;      // ★ 恒 false（占位；见头注释）
};

// =============================================================================
// NameRegistry：写一次（collect），之后全程只读；Parser 与 Sema 各持 const 访问
// =============================================================================
// 键 = (scope, name)。scope 空串 = 全局作用域；嵌套命名空间拼 "N1::N2"。
// 同名跨类合法（`int S; struct S;`）——同一 (scope,name) 的位做 OR 累加，
// 故 value 语义用「位并集」表达，不用单选枚举（spec 异常 2）。
class NameRegistry {
public:
    // 种子名单：Sema 内建的模板名（不经 token 流）随表存在——declval
    // （[declval]/1，inferCall 内建；漏收症状 = test_tmpl_35..39 当场 rc=1，
    // B31 实证）。std::void_t 不收（只在类型位置按名识别，不走本消歧）。
    NameRegistry() { add("", "declval", BitTemplate); }

    // 构造期一次性前向收集（spec §2 步骤 1 / 设计 3.1）：
    //   · template 头（B31 规则原样搬迁）：形参表后 struct/class/using 取紧跟
    //     标识符；其它形态取第一个「标识符 + '('」
    //   · type 位：struct/class/enum 声明（template 头后的除外）、typedef
    //     （取 ';' 前最后一个标识符的近似）、`using X = ...`（Ident + '=' 形）
    //   · namespace 位：`namespace X {`（推作用域栈）、`namespace A = B;`
    //   · 作用域栈按花括号深度配对；类体/函数体只改深度不入栈（类内名字
    //     归属外层命名空间——局部类误登记为全局是宽方向，spec 异常 1）
    //   · declval 种子（Sema 内建，不经 token 流）
    // ★ 不打点：一行收集日志会漂移全部基线（154+ 份，B31 实证）。
    void collect(const std::vector<Token>& tokens);

    // Parser 侧查询（消费点 3.2 / 3.3）：全名命中，或取 '::' 后末段命中
    // （B31 近似原样保留——语句层/表达式层有本地名（局部类型/形参），
    //   精确作用域查询会漏收合法程序，见设计否决记录 4）。
    NameClassification classify(const std::string& name) const;

    // Sema 侧查询（消费点 3.4）：scope 必须精确相等——限定名 N::B 裁决的
    // 唯一依据；配不上 = 响亮报错，杜绝裸尾剥误绑全局 B。
    NameClassification classifyQualified(const std::string& scope,
                                         const std::string& name) const;

    // = classify(name).isTemplate（B31 isKnownTemplateName 的升级入口）。
    bool isTemplateName(const std::string& name) const;

    bool empty() const { return m_entries.empty(); }

private:
    // 位掩码（与 NameClassification 字段一一对应）
    enum : unsigned {
        BitType      = 1u << 0,
        BitNamespace = 1u << 1,
        BitTemplate  = 1u << 2,
        // BitValue 刻意不存在：value 位恒 false（拍板 1）
    };

    std::map<std::pair<std::string, std::string>, unsigned> m_entries;

    void add(const std::string& scope, const std::string& name, unsigned bit);
    static NameClassification fromBits(unsigned bits);
};

} // namespace minicc
