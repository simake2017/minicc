// =============================================================================
// NameRegistry 实现：前向收集（B31 骨架搬迁 + type/namespace 位扩展）
// =============================================================================
// 契约与判据出处见 include/name_registry.h 头注释与
// docs/designs/parser-stage-name-recognition.design.md §3.1。
// 全文件零日志（B31 纪律：收集/查询打点会漂移全部基线）。

#include "name_registry.h"

#include <set>

namespace minicc {

void NameRegistry::add(const std::string& scope, const std::string& name,
                       unsigned bit) {
    m_entries[{scope, name}] |= bit;   // 位 OR 累加：同名跨类并集（spec 异常 2）
}

NameClassification NameRegistry::fromBits(unsigned bits) {
    NameClassification c;
    c.isType      = (bits & BitType) != 0;
    c.isNamespace = (bits & BitNamespace) != 0;
    c.isTemplate  = (bits & BitTemplate) != 0;
    // isValue 恒 false（占位，拍板 1）
    return c;
}

NameClassification NameRegistry::classify(const std::string& name) const {
    // ① 全名当裸名查全局（兼容历史书写：条目都以拆分形存储）
    if (auto it = m_entries.find({"", name}); it != m_entries.end())
        return fromBits(it->second);
    // ② 限定名：末段 split 成 (scope, tail) 精确试一次
    if (const auto sep = name.rfind("::"); sep != std::string::npos) {
        const std::string scope = name.substr(0, sep);
        const std::string tail  = name.substr(sep + 2);
        if (auto it = m_entries.find({scope, tail}); it != m_entries.end())
            return fromBits(it->second);
        // ③ B31 近似：末段对【任意】作用域命中即可（宁宽勿窄——语句层/
        //    表达式层问不出本地名，误收由 TokenCheckpoint 试探回退兜底）
        unsigned orAll = 0;
        for (const auto& [key, bits] : m_entries)
            if (key.second == tail) orAll |= bits;
        if (orAll) return fromBits(orAll);
        return NameClassification{};
    }
    // 裸名：对任意作用域 OR（同名跨命名空间取并集——宽方向，试探兜底）
    unsigned orAll = 0;
    for (const auto& [key, bits] : m_entries)
        if (key.second == name) orAll |= bits;
    return fromBits(orAll);
}

NameClassification
NameRegistry::classifyQualified(const std::string& scope,
                                const std::string& name) const {
    // Sema 侧：scope 精确相等，不做末段回退（消费点 3.4 的裁决依据）
    if (auto it = m_entries.find({scope, name}); it != m_entries.end())
        return fromBits(it->second);
    return NameClassification{};
}

bool NameRegistry::isTemplateName(const std::string& name) const {
    return classify(name).isTemplate;
}

// ─────────────────────────────────────────────────────────────────────────────
// 收集（设计 3.1 五规则；B31 parser.cpp collectTemplateNames 原样搬迁扩展）
// ─────────────────────────────────────────────────────────────────────────────
void NameRegistry::collect(const std::vector<Token>& tokens) {
    // 种子名单在构造函数里（不经 token 流的名字随表存在，collect 与否都有）。

    // 作用域栈：frame = (命名空间名, 进入深度)。scopeOf() 拼 "::" 链。
    std::vector<std::pair<std::string, int>> nsStack;
    auto scopeOf = [&]() -> std::string {
        std::string s;
        for (const auto& [n, _] : nsStack) {
            if (!s.empty()) s += "::";
            s += n;
        }
        return s;
    };
    int depth = 0;

    // template 头后的 struct/class/using 名：不重复置 type 位（`Box;` 对类模板名
    // 应保持非类型解释；模板位由 B31 规则负责）。记下 token 下标跳过。
    std::set<size_t> templatedNames;

    for (size_t i = 0; i < tokens.size(); ++i) {
        const TokenType ty = tokens[i].type;

        // ── 花括号深度维护（namespace 弹栈的依据；各分支共用）──
        if (ty == TokenType::LBrace) { ++depth; continue; }
        if (ty == TokenType::RBrace) {
            --depth;
            while (!nsStack.empty() && nsStack.back().second > depth)
                nsStack.pop_back();
            continue;
        }

        // ── 规则 1：template 头（B31 原样搬迁）────────────────────────
        if (ty == TokenType::KwTemplate) {
            size_t j = i + 1;
            if (j >= tokens.size() || tokens[j].type != TokenType::Less)
                continue;
            int d = 1;   // 跳过配对的 '<...>'（'>>' 逐 Greater 计数）
            ++j;
            while (j < tokens.size() && d > 0) {
                if (tokens[j].type == TokenType::Less) ++d;
                else if (tokens[j].type == TokenType::Greater) --d;
                ++j;
            }
            if (j >= tokens.size()) break;
            const Token& head = tokens[j];
            if (head.type == TokenType::KwStruct
                || head.type == TokenType::KwClass) {
                if (j + 1 < tokens.size()
                    && tokens[j + 1].type == TokenType::Identifier) {
                    add(scopeOf(), tokens[j + 1].text, BitTemplate);
                    templatedNames.insert(j + 1);
                }
            } else if (head.type == TokenType::KwUsing) {
                if (j + 1 < tokens.size()
                    && tokens[j + 1].type == TokenType::Identifier) {
                    add(scopeOf(), tokens[j + 1].text, BitTemplate);
                    templatedNames.insert(j + 1);
                }
            } else {
                // 函数模板 / 推导指引 / 成员模板：形参表后第一个「标识符 + '('」
                for (size_t k = j; k < tokens.size(); ++k) {
                    const Token& t = tokens[k];
                    if (t.type == TokenType::Semicolon
                        || t.type == TokenType::LBrace
                        || t.type == TokenType::Eof)
                        break;
                    if (t.type == TokenType::Identifier
                        && k + 1 < tokens.size()
                        && tokens[k + 1].type == TokenType::LParen) {
                        add(scopeOf(), t.text, BitTemplate);
                        break;
                    }
                }
            }
            continue;
        }

        // ── 规则 2：type 位 —— struct/class/enum 声明 ─────────────────
        // （template 头后的除外；enum class X 跳过中间的 KwClass）
        if (ty == TokenType::KwStruct || ty == TokenType::KwClass
            || ty == TokenType::KwEnum) {
            size_t j = i + 1;
            if (ty == TokenType::KwEnum && j < tokens.size()
                && tokens[j].type == TokenType::KwClass)
                ++j;
            if (j < tokens.size() && tokens[j].type == TokenType::Identifier) {
                if (!templatedNames.count(j))
                    add(scopeOf(), tokens[j].text, BitType);
            }
            continue;
        }

        // ── 规则 3：type 位 —— typedef（取 ';' 前最后一个标识符的近似）──
        if (ty == TokenType::KwTypedef) {
            for (size_t k = i + 1; k < tokens.size(); ++k) {
                if (tokens[k].type == TokenType::Semicolon
                    || tokens[k].type == TokenType::Eof)
                    break;
                if (tokens[k].type == TokenType::Identifier) {
                    // 区间内最后一个 Identifier：走到 ';' 才算数
                    size_t last = k;
                    for (size_t m = k + 1; m < tokens.size(); ++m) {
                        if (tokens[m].type == TokenType::Semicolon
                            || tokens[m].type == TokenType::Eof)
                            break;
                        if (tokens[m].type == TokenType::Identifier) last = m;
                    }
                    add(scopeOf(), tokens[last].text, BitType);
                    break;
                }
            }
            continue;
        }

        // ── 规则 4：type 位 —— `using X = ...;`（Ident + '=' 形才收，
        //    `using Base::f;` / `using namespace X;` 不收；template 头后的
        //    别名模板名已在规则 1 收过模板位，此处跳过）────────────────
        if (ty == TokenType::KwUsing) {
            if (i + 2 < tokens.size()
                && tokens[i + 1].type == TokenType::Identifier
                && tokens[i + 2].type == TokenType::Assign
                && !templatedNames.count(i + 1))
                add(scopeOf(), tokens[i + 1].text, BitType);
            continue;
        }

        // ── 规则 5：namespace 位 + 作用域栈 ────────────────────────────
        if (ty == TokenType::KwNamespace) {
            if (i + 1 < tokens.size()
                && tokens[i + 1].type == TokenType::Identifier) {
                const std::string& nm = tokens[i + 1].text;
                if (i + 2 < tokens.size()
                    && tokens[i + 2].type == TokenType::LBrace) {
                    // `namespace X {`：登记 + 推栈（frame 深度 = '{' 处理后的
                    // 深度；主循环 continue 到 LBrace 分支 ++depth 即到位）
                    add(scopeOf(), nm, BitNamespace);
                    nsStack.emplace_back(nm, depth + 1);
                } else if (i + 2 < tokens.size()
                           && tokens[i + 2].type == TokenType::Assign) {
                    // `namespace A = B;`（命名空间别名，主线 A 已支持）
                    add(scopeOf(), nm, BitNamespace);
                }
            }
            continue;
        }
    }
}

} // namespace minicc
