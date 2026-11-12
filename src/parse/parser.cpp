// 递归下降解析器：Token 流 → TranslationUnit；名字种类由 Sema 的活作用域表提供。
// 优先级通过文法分层表达：`1 + 2 * 3` → Add(1, Mul(2, 3))。
// 声明/表达式与模板 id/比较表达式的歧义使用 TokenCheckpoint 试探，失败时回滚。
// 参考：clang/lib/Parse/ParseDecl.cpp、ParseExpr.cpp、ParseTemplate.cpp。

#include "parser.h"
//  Sema 活句柄的完整定义只在这里出现 —— 对照 clang：
// clang/lib/Parse/Parser.cpp 引入 clang/Sema/Sema.h，而 clang Parser.h 只
// 前向声明 `class Sema;` 并持有 `Sema &Actions;`（clang Parser.h:639）。
#include "semantic_analyzer.h"
#include <cstdint>     // uint64_t —— 字面量按无符号 64 位解析（[lex.icon]/2 的表）
#include <format>
#include <iostream>
#include <stdexcept>   // std::runtime_error —— 字面量超 64 位时报错

namespace minicc {

// 持有 Token 流与 Sema 名字查询接口；游标从首个 Token 开始。
Parser::Parser(std::vector<Token> tokens)
    : m_tokens(std::move(tokens)),
      m_ownedSema(std::make_unique<SemanticAnalyzer>()) {
    m_sema = m_ownedSema.get();
}

// 析构：unique_ptr<SemanticAnalyzer> 的完整类型只在本文件可见 ⇒ 在此定义。
Parser::~Parser() = default;

// 声明点将名字登记到 Sema 的活作用域表，避免 Parser 维护另一份名字事实。
// 参考：clang/lib/Parse/ParseDecl.cpp 的 Actions 声明回调。
void Parser::declareName(const std::string& name, unsigned bits) {
    if (!m_sema) return;
    m_sema->declareParseName(name, bits);
}

void Parser::openScope(ScopeKind kind, const std::string& name, unsigned bits) {
    if (!m_sema) return;
    m_sema->openParseScope(kind, name, bits);
}

void Parser::closeScope() {
    if (!m_sema) return;
    m_sema->leaveParseScope();
}

// 名字种类只查询 Sema 的活作用域表；模板名假定规则由表达式消歧入口处理。
// 参考：clang/lib/Sema/SemaExpr.cpp:Sema::ClassifyName。
unsigned Parser::nameBits(const std::string& name) const {
    if (!m_sema) return 0;
    // 限定名与裸名统一入口：表那一侧负责逐段解析限定者（不外溢）。
    return m_sema->classifyParseQualified(name);
}

// [temp.names]：已登记的模板名可接实参表；未查到的裸名后接 `<` 时也允许试探。
// 限定名不套用裸名假定；实参表试探失败则回滚为比较表达式。
bool Parser::isTemplateNameHere(const std::string& name) const {
    const unsigned bits = nameBits(name);
    if (bits & ScopeBit::Template) return true;
    if (bits != 0) return false;   // 声明过（变量可遮蔽模板名）⇒ 不假定
    if (name.find("::") != std::string::npos) return false;   // 限定名不假定
    return check(TokenType::Less);
}

// 语句层链闸用：实参表已解析完 ⇒ '<' 必然存在 ⇒ C++20 假定直接适用。
bool Parser::isTemplateNameInTemplateId(const std::string& name) const {
    const unsigned bits = nameBits(name);
    if (bits & ScopeBit::Template) return true;
    return bits == 0;   // 查不到 ⇒ 按 C++20 假定是模板名
}

// ─────────────────────────────────────────────────────────────────────────────
// Token 流操作
// ─────────────────────────────────────────────────────────────────────────────
// ── 前瞻原语：返回游标所指 Token，不移动 m_pos。所有分支决策的信息来源。──
const Token& Parser::current() const {
    return m_tokens[m_pos];
}

// ── peek 与 current 实现相同，保留两个名字是为了调用点语义清晰：
//    current() = "我正要处理的 Token"，peek() = "只瞄一眼决定走哪条路"。──
const Token& Parser::peek() const {
    return m_tokens[m_pos];
}

// ── 消费并推进游标。末尾哨兵 Eof 保证越界前必先 isAtEnd()，故不递增。──
// 游标推进示意：m_pos=2 → advance() 返回 tok[2]，m_pos=3
//     [int][main][(][)][{]...
//                ^ m_pos 移动前        ^ 移动后
const Token& Parser::advance() {
    const Token& tok = m_tokens[m_pos];
    if (!isAtEnd()) m_pos++;
    return tok;
}

// 模板形参按作用域由内向外查找，支持同名遮蔽。
// `T` 命中类型形参时建 TemplateParam；NTTP 名仍交实参分流或 Sema 判断。
const TemplateParam* Parser::lookupTemplateParam(const std::string& name) const {
    for (const TemplateParamFrame* f = m_currentFrame; f; f = f->parent) {
        for (size_t i = 0; i < f->count; i++) {
            const TemplateParam& p = *f->owner->templateParams[i];
            if (p.name == name) return &p;   //  现取，扩容后依然有效
        }
    }
    return nullptr;
}

// ── 前瞻判断（LL(1) 的核心动作）：只比较类型，不消费。──
bool Parser::check(TokenType t) const {
    return current().type == t;
}

// ── "尝试消费"：命中则吃掉并返回 true；未命中不动游标、返回 false。
//    用于文法中的可选项（如 `['else' stmt]`、`['=' expr]`）。──
bool Parser::match(TokenType t) {
    if (check(t)) {
        advance();
        return true;
    }
    return false;
}

// ── "强制消费"：语法上此处必须是 t，否则报错（panic：抛异常中止编译单元）。
//    与 match 的对比：match 容忍缺失（可选文法），expect 不容忍（必选文法）。──
const Token& Parser::expect(TokenType t, const std::string& msg) {
    if (check(t)) {
        return advance();
    }
    errorAt(current(), std::format("{}: expected {}, got '{}'",
        msg, tokenTypeName(t), current().text));
}

// ── 流末尾判断：越过数组边界 或 撞到 Eof 哨兵（Lexer 保证末尾有 Eof）。──
bool Parser::isAtEnd() const {
    return m_pos >= m_tokens.size() || current().is(TokenType::Eof);
}

// ─────────────────────────────────────────────────────────────────────────────
// 错误处理
// ─────────────────────────────────────────────────────────────────────────────
// ── 错误报告（当前 Token 处出错）──
[[noreturn]] void Parser::error(const std::string& msg) const {
    errorAt(current(), msg);
}

// ── 错误报告（指定 Token 处出错）。
//    错误恢复策略：panic 模式 —— 抛 std::runtime_error 直接中止本编译单元，
//    不做 Token 级跳过/同步恢复。调用方（main/驱动）捕获后打印信息并退出。
//    教学取舍：换取实现极简与错误信息确定性（每处语法错误恰好一条报告）。──
[[noreturn]] void Parser::errorAt(const Token& tok, const std::string& msg) const {
    throw ParseError(
        std::format("[Parse Error] {} at '{}': {}",
            tok.location.toString(), tok.text, msg));
}

// [dcl.type.simple]：整型说明符由多个关键字组成，如 `unsigned long long`。
// 统一收集后组合类型，避免将 `long int` 的 int 误认成声明符。
TypePtr Parser::parseBuiltinTypeSpecifierSeq() {
    int nChar = 0, nShort = 0, nLong = 0, nSigned = 0, nUnsigned = 0, nInt = 0;
    std::string spelling;   // 原文拼写，仅用于日志（顺序到了规范化段就没用了）

    // ── ① 收集段 ──
    while (true) {
        std::string kw;
        if (check(TokenType::KwChar))          { kw = "char";     nChar++; }
        else if (check(TokenType::KwShort))    { kw = "short";    nShort++; }
        else if (check(TokenType::KwLong))     { kw = "long";     nLong++; }
        else if (check(TokenType::KwSigned))   { kw = "signed";   nSigned++; }
        else if (check(TokenType::KwUnsigned)) { kw = "unsigned"; nUnsigned++; }
        else if (check(TokenType::KwInt))      { kw = "int";      nInt++; }
        else break;                            // 序列到此为止（下一个是名字/'*'/'&'…）
        advance();
        if (!spelling.empty()) spelling += ' ';
        spelling += kw;
    }

    // ── ② 规范化段：先挡掉所有非法组合，再查表 ──
    // 非法组合一律响亮报错 —— 静默归一成某个类型会让 `unsigned double` 这类
    // 无意义写法一路编到底（在 CodeGen 才炸，且炸点离根因很远）。
    const std::string where = std::format("'{}'", spelling);
    auto bad = [&](const std::string& why) -> void {
        error(std::format("invalid type specifier sequence {}: {}", where, why));
    };

    if (nSigned + nUnsigned > 1)  bad("cannot be both 'signed' and 'unsigned'");
    if (nShort > 1)               bad("'short' appears more than once");
    if (nLong > 2)                bad("'long' appears more than once (at most 'long long')");
    if (nInt > 1)                 bad("'int' appears more than once");
    if (nLong > 0 && nShort > 0)  bad("cannot be both 'short' and 'long'");
    if (nChar > 1)                bad("'char' appears more than once");
    if (nChar > 0) {
        // char 只接受 signed / unsigned 这两种修饰（[basic.fundamental]/7）
        if (nShort > 0 || nLong > 0 || nInt > 0) bad("'char' cannot be combined with 'short'/'long'/'int'");
    } else if (nChar == 0 && nShort == 0 && nLong == 0 && nInt == 0
               && nSigned == 0 && nUnsigned == 0) {
        // 防御：调用约定保证至少有一个关键字；走到这里说明调用点写错了
        bad("no type specifier");
    }

    const bool uns = nUnsigned == 1;
    TypePtr base;
    if (nChar > 0) {
        //  char / signed char / unsigned char 是【三个不同类型】
        base = uns ? Type::makeUChar()
                   : (nSigned > 0 ? Type::makeSChar() : Type::makeChar());
    } else if (nShort > 0) {
        base = uns ? Type::makeUShort() : Type::makeShort();
    } else if (nLong == 2) {
        base = uns ? Type::makeULongLong() : Type::makeLongLong();
    } else if (nLong == 1) {
        base = uns ? Type::makeULong() : Type::makeLong();
    } else {
        // 没有长度修饰：`unsigned` ≡ `unsigned int`，`signed` ≡ `signed int` ≡ `int`
        base = uns ? Type::makeUInt() : Type::makeInt();
    }

    logParse(
        "  [parse:type] type-specifier-seq: '{}' ⇒ base = {}  ({})\n",
        spelling, base->toString(),
        (nUnsigned ? "无符号，Itanium 编码与有符号版不同" : "有符号"));
    return base;
}

// [dcl.type]：将类型说明符及指针、引用、const 后缀建成类型树。
// `const int*` → Pointer(Const(Int))；`int* const` → Const(Pointer(Int))。
// 参考：clang/lib/Parse/ParseDecl.cpp:Parser::ParseDeclarationSpecifiers。
TypePtr Parser::parseType() {
    // ── Step 1: 处理 const / constexpr 前缀（例如 const int, constexpr int）──
    bool isConst = false;
    if (check(TokenType::KwConst)) {
        isConst = true;
        advance();
        logParse("  [parse:type] ✦ const prefix detected\n");
    }
    // constexpr：[dcl.constexpr] —— 隐含 const（"implies const"），且初始化式
    // 必须可常量求值。类型层面按 const 处理；constexpr 性本身经
    // m_pendingConstexpr 交给声明节点（Sema 强制求值并登记常量表）。
    if (check(TokenType::KwConstexpr)) {
        advance();
        isConst = true;
        m_pendingConstexpr = true;
        logParse("  [parse:type] ✦ constexpr prefix detected (implies const)\n");
    }

    // [temp.res]：`typename T::type` 的前缀消除依赖限定名的类型歧义；成员查找留到替换后。
    if (check(TokenType::KwTypename)) {
        advance();
        logParse("  [parse:type] ✦ typename prefix (依赖限定名消歧)\n");
    }

    // ── Step 2: 解析基础类型（基本类型 / 标识符 / 命名空间限定类型）──
    TypePtr base;

    // 整型家族的多关键字写法（unsigned long long int / signed char / short …）：
    // 只接【以 short/long/signed/unsigned/char 开头】的序列；单体写法 int/double/
    // bool/void/auto 仍走下面的分支快筛 —— 既有代码的日志因此逐字节不变。
    if (check(TokenType::KwShort) || check(TokenType::KwLong)
        || check(TokenType::KwSigned) || check(TokenType::KwUnsigned)
        || check(TokenType::KwChar)) {
        base = parseBuiltinTypeSpecifierSeq();
    }
    else if (match(TokenType::KwInt)) {
        base = Type::makeInt();          // 样例: int
        logParse("  [parse:type] base = int\n");
    }
    else if (match(TokenType::KwDouble)) {
        base = Type::makeDouble();       // 样例: double
        logParse("  [parse:type] base = double\n");
    }
    else if (match(TokenType::KwBool)) {
        base = Type::makeBool();         // 样例: bool
        logParse("  [parse:type] base = bool\n");
    }
    else if (match(TokenType::KwVoid)) {
        base = Type::makeVoid();         // 样例: void
        logParse("  [parse:type] base = void\n");
    }
    else if (match(TokenType::KwAuto)) {
        base = Type::makeAuto();         // 样例: auto
        logParse("  [parse:type] base = auto\n");
    }
    else if (check(TokenType::KwDecltype)) {
        // [dcl.type.decltype]：保留操作数是否被额外括号包住，以区分声明类型与值类别规则。
        // `decltype(x)` 取声明类型；`decltype((x))` 对左值产生引用类型。
        auto kwLoc = advance().location;   // 消费 'decltype'
        expect(TokenType::LParen, "Expected '(' after 'decltype'");

        // 外层 '(' 之后若紧跟 '('，说明原文是 decltype((...))，
        // 即操作数自身被括号包住 —— 走"表达式类型"那套规则。
        bool paren = check(TokenType::LParen);

        ExprPtr operand = parseExpression();
        expect(TokenType::RParen, "Expected ')' to close 'decltype'");

        base = Type::makeDecltype(operand, paren);
        base->name = "decltype";
        logParse(
            "  [parse:type] base = decltype({}) [deferred: 不求值，留待替换阶段]\n",
            paren ? "(e)" : "e");
        (void)kwLoc;
    }
    else if (check(TokenType::Identifier)) {
        // 样例: "MyClass", 模板形参 "T", 命名空间限定名 "std::string" 或 "A::B::Type"
        std::string name = advance().text;

        // [temp.res]：`typename T::type` 的限定者依赖于类型形参 T。
        // 保留限定者的类型结构，不能扁平化成无法替换的名字字符串。
        const TemplateParam* qualParam = lookupTemplateParam(name);
        if (qualParam && qualParam->kind == TemplateParamKind::Type
            && check(TokenType::ColonColon)) {
            TypePtr qual = Type::makeTemplateParam(name);
            logParse(
                "  [parse:type] ★ 依赖限定名: {}::...（限定者是模板形参）\n", name);
            while (check(TokenType::ColonColon)) {
                advance();
                const Token& memberTok = expect(TokenType::Identifier,
                    "Expected member type name after '::'");
                TypePtr nested = Type::makeClass(memberTok.text);
                nested->nestedQualifier = qual;
                qual = nested;
                logParse("  [parse:type] ★ nested name: {}\n",
                    qual->toString());
            }
            base = qual;
            // 到此为止：不 return，落到下面的 Step 2.5（const）与 Step 3
            // （后缀循环），于是 `const typename T::type&` / `typename T::type*`
            // 这些组合也自动成立。
        }
        else {
            while (check(TokenType::ColonColon)) {
                advance();
                name += "::" + expect(TokenType::Identifier, "Expected type name after '::'").text;
            }
            // 查询模板形参作用域 —— 裸标识符若命中当前 template<...>
            // 声明的类型形参，建成 TemplateParam 节点而非 Class 节点。
            // 对应 clang: clang/lib/Sema/SemaTemplate.cpp:Sema::ActOnTypeParameter / ActOnType
            // 在模板上下文中把 T 解析为 TemplateTypeParmType。
            // 注意：只认不含 '::' 的裸名（std::T 这种限定名不可能是模板形参）。
            const TemplateParam* tp = (name.find("::") == std::string::npos)
                                    ? lookupTemplateParam(name) : nullptr;
            // 仅类型形参建 TemplateParam；NTTP 名保留为待语义解析的名字。
            // 例如 `template<int N> using A = Buf<N>;` 中 N 是值实参，不是类型形参。
            if (tp && tp->kind == TemplateParamKind::Type) {
                base = Type::makeTemplateParam(name);
                logParse("  [parse:type] base = {} (template param, scope hit)\n", name);
            }
            else {
                base = Type::makeClass(name);
                logParse("  [parse:type] base = {} (class/tparam)\n", name);
                // 模板 id：`Box<int>`、`Buf<4>`、`List<Box<int>>`。
                // 实参的类型/值分流及嵌套闭合由 parseTemplateArgumentList 统一处理。
                if (check(TokenType::Less)) {
                    base->templateArgs = parseTemplateArgumentList();
                    logParse(
                        "  [parse:type] ★ template-id: {}\n", base->toString());
                }

                // `S<int>::type` 保留限定者与成员名，供 Sema 在具体类中查找成员类型。
                // 纯限定名链 `A::B::C` 则由前面的名字解析处理。
                while (check(TokenType::ColonColon)) {
                    advance();
                    const Token& memberTok = expect(TokenType::Identifier,
                        "Expected member type name after '::'");
                    TypePtr nested = Type::makeClass(memberTok.text);
                    nested->nestedQualifier = base;
                    base = nested;
                    logParse("  [parse:type] ★ nested name: {}\n",
                        base->toString());
                }
            }
        }
    }
    else {
        error("Expected type name");
    }

    // 前缀 const 必须先于指针后缀应用：`const int*` 指向 const int，指针本身可写。
    if (isConst) {
        base = Type::makeConst(base);
        logParse("  [parse:type] const applied to base → {}\n", base->toString());
        isConst = false;   // 已消费，Step 4 不再重复套
    }

    // 后缀作用于已建好的类型，可链式组合：`int**`、`int&`、`int&&`、`int* const`。
    while (true) {
        if (match(TokenType::Star)) {
            // 指针: T*
            base = Type::makePointer(base);
            logParse("  [parse:type] suffix * → {}\n", base->toString());
        }
        else if (check(TokenType::AmpAmp)) {
            // 右值引用: T&&
            // 注意：AmpAmp 是 &&（逻辑与 Token），在类型上下文中表示右值引用
            advance();
            base = Type::makeRValueReference(base);
            logParse("  [parse:type] suffix && → {} (rvalue ref)\n",
                base->toString());
        }
        else if (check(TokenType::Ampersand)) {
            // 左值引用: T&
            advance();
            base = Type::makeLValueReference(base);
            logParse("  [parse:type] suffix & → {} (lvalue ref)\n",
                base->toString());
        }
        else if (check(TokenType::KwConst)) {
            // [dcl.type.cv]：const 修饰当前已建类型。
            // `int const` 与 `const int` 等价；`int* const` 修饰指针而非所指对象。
            advance();
            base = Type::makeConst(base);
            logParse(
                "  [parse:type] suffix const → {} (const-qualified declarator)\n",
                base->toString());
        }
        else {
            break; // 没有更多后缀修饰符
        }
    }

    logParse("  [parse:type] ★ final type = {}\n", base->toString());
    return base;
}

// [lex.icon]：解读整数字面量原文，保留数值、进制及后缀形态。
// 例如 `0x1F` 与 `4UL`；Lexer 只负责切分原文，类型选择在这里完成。
struct IntLiteralValue {
    int64_t value = 0;
    TypePtr type;                 // 形态；恒非空
};

static IntLiteralValue parseIntLiteral(const std::string& text, SourceLocation loc) {
    // ── ① 进制 + 数字段 ──
    size_t i = 0;
    int    base = 10;
    if (text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16; i = 2;
    } else if (text.size() >= 2 && text[0] == '0' && (text[1] == 'b' || text[1] == 'B')) {
        base = 2;  i = 2;
    } else if (text.size() >= 2 && text[0] == '0') {
        base = 8;  i = 1;         // `0` 自己也算八进制（值相同，形态无关紧要）
    }

    std::string digits;
    for (; i < text.size(); i++) {
        const char c = text[i];
        if (c == '\'') continue;                       // C++14 数字分隔符：跳过
        if (base == 16 ? std::isxdigit(static_cast<unsigned char>(c))
                       : (c >= '0' && c <= '9')) digits += c;
        else break;                                    // 后缀从这里开始
    }
    // ── ② 后缀 ──
    const std::string suffix = text.substr(i);
    bool hasU = false, hasZ = false;
    int  nLong = 0;
    for (char c : suffix) {
        if (c == 'u' || c == 'U') hasU = true;
        else if (c == 'l' || c == 'L') nLong++;
        else if (c == 'z' || c == 'Z') hasZ = true;
    }

    // ── ③ 值：按无符号 64 位解析，再按位模式存进 int64_t ──
    // 用 stoull 而非 stoll：0xFFFFFFFFFFFFFFFF 这类字面量超过 INT64_MAX，
    // stoll 会抛 overflow；而它的位模式（-1）正是要存的东西。
    uint64_t u = 0;
    try {
        u = digits.empty() ? 0 : std::stoull(digits, nullptr, base);
    } catch (const std::exception&) {
        // 试探只捕获 ParseError；字面量溢出也归为语法错误，其他异常不得被吞掉。
        throw ParseError(std::format(
            "integer literal '{}' is too large to represent in 64 bits", text));
    }

    // ── ④ 查 [lex.icon]/2 的表 ──
    // `fits*` 谓词按"该类型能否精确表示这个值"判断 —— 十进制字面量按数值比，
    // 非十进制按位模式比（此处两者对本题的取值域恰好同形，故共用一组谓词）。
    const bool fitsI32 = u <= 0x7FFFFFFFull;
    const bool fitsU32 = u <= 0xFFFFFFFFull;
    const bool fitsI64 = u <= 0x7FFFFFFFFFFFFFFFull;

    TypePtr ty;
    if (hasZ) {
        ty = Type::makeULong();                        // size_t 后缀；LP64 下 size_t = unsigned long
    } else if (hasU && nLong >= 2) {
        ty = Type::makeULongLong();
    } else if (hasU && nLong == 1) {
        ty = Type::makeULong();
    } else if (hasU) {
        ty = fitsU32 ? Type::makeUInt() : Type::makeULong();
    } else if (nLong >= 2) {
        ty = fitsI64 ? Type::makeLongLong() : Type::makeULongLong();
    } else if (nLong == 1) {
        // 十进制：long → long long；非十进制：long → unsigned long → …
        ty = fitsI64 ? Type::makeLong()
                     : (base == 10 ? Type::makeLongLong() : Type::makeULong());
    } else if (base == 10) {
        // 十进制无后缀：int → long → long long（绝不取无符号）
        ty = fitsI32 ? Type::makeInt() : (fitsI64 ? Type::makeLong() : Type::makeLongLong());
    } else {
        // 非十进制无后缀：int → unsigned int → long → unsigned long → long long → …
        ty = fitsI32 ? Type::makeInt()
                     : (fitsU32 ? Type::makeUInt()
                                : (fitsI64 ? Type::makeLong() : Type::makeULong()));
    }

    //  本函数【不打印日志】：它被每一条整数表达式调用，逐字面量打点会把日志淹掉，
    // 而"字数形态"这件事的可观测点在两处更合适的地方：
    // ① NTTP 实参 —— 调用点现有的 [parse:targ] 行会带上形态；
    // ② 表达式   —— [infer] IntLiteral(4) → long 那行已说明类型。
    (void)loc;
    return {static_cast<int64_t>(u), ty};
}

// escapeCharText：把【已翻译过转义】的字符字面量正文还原成可打印形式。
// 词法期 `'\n'` 的正文已经是真正的换行符（0x0A），直接塞进日志会把那一行
// 拦腰截断（`char literal '` + 换行 + `' (10)`），既没法读、也让 logdiff
// 的逐行对比失去意义。故回吐时把不可打印字符重新转义。
// demo: escapeCharText("\n")  ⇒ "\\n"    escapeCharText("a") ⇒ "a"
static std::string escapeCharText(const std::string& raw) {
    std::string out;
    for (char ch : raw) {
        switch (ch) {
            case '\n': out += "\\n";  break;
            case '\t': out += "\\t";  break;
            case '\r': out += "\\r";  break;
            case '\0': out += "\\0";  break;
            case '\\': out += "\\\\"; break;
            case '\'': out += "\\'";  break;
            default:   out += ch;     break;
        }
    }
    return out;
}

// 表达式延续判据：二元/逻辑运算符、三元 '?' 都会把「字面量」扩展成
// 「常量表达式」。 '(' 不算 —— `4(x)` 不是合法表达式，别贪心。
//  Less/Greater【刻意不收】：`Buf<4>` 的收尾 `>` 在词法上与小于/大于
// 同 token，收进来会让每条裸字面量实参都误走表达式路径（基线漂移）。
// 要比较就加括号：`Buf<(1<2)>`（'(' 开头的分支不受此限）。
bool Parser::isExprContinuation(TokenType t) {
    switch (t) {
        case TokenType::Plus:       case TokenType::Minus:
        case TokenType::Star:       case TokenType::Slash:
        case TokenType::Percent:
        case TokenType::EqualEqual: case TokenType::BangEqual:
        case TokenType::LessEqual:  case TokenType::GreaterEqual:
        case TokenType::AmpAmp:     case TokenType::PipePipe:
        case TokenType::Question:
            return true;
        default:
            return false;
    }
}

// [temp.arg]：统一解析模板实参表，区分类型、值及待求值表达式。
// `Box<int>` 含类型实参；`Buf<4>` 含值实参；嵌套模板复用同一入口。
// 参考：clang/lib/Parse/ParseTemplate.cpp:Parser::ParseTemplateArgumentList。
std::vector<TemplateArg> Parser::parseTemplateArgumentList() {
    expect(TokenType::Less, "Expected '<' before template arguments");

    std::vector<TemplateArg> args;
    if (!check(TokenType::Greater)) {
        do {
            // 整数字面量（含负号）建值实参：`Buf<4>` → Integral(4)；类型写法递归解析。
            if (check(TokenType::Minus) && m_pos + 1 < m_tokens.size()
                && m_tokens[m_pos + 1].is(TokenType::IntLiteral)) {
                // `Buf<-3+10>` 含完整常量表达式，不能仅消费负整数字面量。
                if (m_pos + 2 < m_tokens.size()
                    && isExprContinuation(m_tokens[m_pos + 2].type)) {
                    goto parseConstExprArg;
                }
                //  严格说 -3 不是字面量，而是「一元减 + 字面量」构成的常量表达式
                // （[expr.unary.op] + [expr.const]）。本分支只识别最常见的
                // 「负号 + 整数字面量」，覆盖 Buf<-3>；带运算符的写法
                // （Buf<-3+10>）已在上面回退到完整常量表达式分支。
                // 对应 Itanium 编码 _Z3BufILin3EE（n 表负数，见 NameMangler）。
                advance();  // '-'
                const Token& lit = advance();
                //  负号只改【值】不改【形态】：`-4L` 仍是 long（[expr.unary.op]/7
                // 的整型提升在此不改变类型）。故形态取自字面量本身。
                IntLiteralValue lv = parseIntLiteral(lit.text, lit.location);
                lv.value = -lv.value;
                logParse(
                    "  [parse:targ] ★ non-type argument (NTTP): negative integer {} (形态 {})\n",
                    lv.value, lv.type->toString());
                args.push_back(TemplateArg::ofValue(lv.value, lv.type));
            }
            else if (check(TokenType::IntLiteral)) {
                // `Buf<2+2>` 需交完整常量表达式解析；前瞻只窥探，不消费尚未归属本分支的记号。
                if (m_pos + 1 < m_tokens.size()
                    && isExprContinuation(m_tokens[m_pos + 1].type)) {
                    goto parseConstExprArg;
                }
                const Token& tok = advance();
                IntLiteralValue lv = parseIntLiteral(tok.text, tok.location);
                logParse(
                    "  [parse:targ] ★ non-type argument (NTTP): integer literal {} (形态 {})\n",
                    lv.value, lv.type->toString());
                args.push_back(TemplateArg::ofValue(lv.value, lv.type));
            }
            // [temp.arg.nontype]：true/false 是 bool 值实参，如 `enable_if<true, T>`。
            else if (check(TokenType::KwTrue) || check(TokenType::KwFalse)) {
                bool b = check(TokenType::KwTrue);
                advance();
                logParse(
                    "  [parse:targ] ★ non-type argument (NTTP): bool literal {} ({})\n",
                    b ? "true" : "false", b ? 1 : 0);
                args.push_back(TemplateArg::ofValue(b ? 1 : 0, Type::makeBool()));
            }
            //  字符字面量作非类型实参 —— `Char<'a'>` 之于 `template<char C>`，
            // 与 `Buf<4>` 之于 `template<int N>` 是同一回事，只是形态是 char。
            // 对应 Itanium 编码 _Z4CharILc97EE（c = char，97 = 'a'）。
            // 对照 clang：ParseTemplateArgument 里 tok::char_constant 走
            // clang/lib/Sema/SemaExpr.cpp:Sema::ActOnCharacterConstant（值留到 Sema 才求值）。
            else if (check(TokenType::CharLiteral)) {
                const Token& tok = advance();
                int64_t val = 0;
                for (char ch : tok.text) val = (val << 8) | static_cast<unsigned char>(ch);
                logParse(
                    "  [parse:targ] ★ non-type argument (NTTP): char literal '{}' ({})\n",
                    escapeCharText(tok.text), val);
                args.push_back(TemplateArg::ofValue(val, Type::makeChar()));
            }
            else {
                // 常量表达式实参如 `Buf<2+2>`、`Buf<(1+2)*3>` 交表达式解析；类型实参仍走 parseType。
                if (check(TokenType::LParen) || check(TokenType::Bang)) {
                parseConstExprArg:
                    size_t exprStart = m_pos;
                    // '>' 停止规则只罩住这个实参表达式本身（括号内已豁免）
                    bool savedStop = m_stopAtGreater;
                    m_stopAtGreater = true;
                    ExprPtr e = parseExpression();
                    m_stopAtGreater = savedStop;
                    std::string srcText;
                    for (size_t ti = exprStart; ti < m_pos; ti++) {
                        if (!srcText.empty()) srcText += " ";
                        srcText += m_tokens[ti].text;
                    }
                    logParse(
                        "  [parse:targ] ★ non-type argument (NTTP): constant expression '{}' "
                        "（求值推迟到语义期）\n", srcText);
                    TemplateArg a;
                    a.kind      = TemplateArgKind::Integral;
                    a.valueExpr = e;
                    args.push_back(a);
                } else {
                    args.push_back(TemplateArg::ofType(parseType()));
                }
            }
        } while (match(TokenType::Comma));
    }

    expect(TokenType::Greater, "Expected '>' after template arguments");

    logParse("  [parse:targ] ★ template argument list: <{} argument(s)> — ",
        args.size());
    // 整行拼接后输出，使试探缓冲能整体保留或丢弃一条日志。
    std::string targLine;
    for (size_t i = 0; i < args.size(); i++) {
        if (i > 0) targLine += ", ";
        targLine += (args[i].isValue() ? "(value) " : "(type) ") + args[i].toString();
    }
    logParse("{}\n", targLine);
    return args;
}

// translation-unit := declaration*；读到 Eof 为止。
TranslationUnit Parser::parseTranslationUnit() {
    TranslationUnit unit;
    while (!isAtEnd()) {
        unit.declarations.push_back(parseDeclaration());
    }
    return unit;
}

// 按声明首部选择模板、类、枚举、命名空间、别名、函数或全局变量解析入口。
DeclPtr Parser::parseDeclaration() {
    // template<typename T> ...
    if (check(TokenType::KwTemplate)) {
        return parseTemplateDecl();
    }

    if (check(TokenType::KwClass) || check(TokenType::KwStruct)) {
        return parseClassDecl();
    }

    // enum ...
    if (check(TokenType::KwEnum)) {
        return parseEnumDecl();
    }

    // namespace ...
    if (check(TokenType::KwNamespace)) {
        return parseNamespaceDecl();
    }

    // using ... / typedef ...
    if (check(TokenType::KwUsing) || check(TokenType::KwTypedef)) {
        return parseTypeAliasDecl();
    }

    // ── 非模板推导指引：`Box(int) -> Box<int>;`（[temp.deduct.guide]）──
    // 必须排在"函数 vs 全局变量"的前瞻【之前】：两者都从 parseType 开始试探，
    // 而 `Box(int)` 会让 parseType 只吃掉 `Box`，随后 check(Identifier) 失败
    // （下一个是 '('）⇒ 被误判成全局变量声明 ⇒ 报 "Expected global variable name"。
    if (looksLikeDeductionGuide()) {
        return parseDeductionGuide();
    }

    // 函数声明或全局变量声明
    bool isVirtual = false;
    if (check(TokenType::KwVirtual)) {
        isVirtual = true;
    }

    // 函数与全局变量共享类型前缀，用 TokenCheckpoint 试探并保证失败时恢复游标。
    // 参考：clang 的 TentativeParsingAction。
    bool isFunc = false;
    {
        TokenCheckpoint cp(*this);
        try {
            if (isVirtual) {
                advance();
            }
            parseType(); // 试探性解析：仅用于前进 token 流以观察后续符号，返回值不保留
            if (check(TokenType::Identifier)) {
                advance();
                if (check(TokenType::LParen)) {
                    isFunc = true;
                }
            }
        } catch (const ParseError&) {
            isFunc = false;
        }
    }   // 纯前瞻不 commit；退出时恢复游标并丢弃试探日志。

    if (isFunc) {
        return parseFunctionDecl(isVirtual);
    } else {
        return parseGlobalVarDecl();
    }
}

// ─── 全局变量声明 ─────────────────────────────────────────────────────────────
GlobalVarDeclPtr Parser::parseGlobalVarDecl() {
    auto decl = std::make_shared<GlobalVarDecl>();
    decl->location = current().location;
    decl->declaredType = parseType();
    const Token& nameTok = expect(TokenType::Identifier, "Expected global variable name");
    decl->name = nameTok.text;
    // ── static 数据成员的类外定义：`int A::count = 7;`（[class.static.data]）──
    // 名字部收 IDENT (:: IDENT)* 拼成全名 "A::count"（命名空间类则是 "N::A::count"）。
    // Sema 的 processGlobalVarDecl 见名字带 "::" ⇒ 按"静态成员定义"校验
    // （类存在/有该声明/类型一致/不重定义），合法后存储发射与全局变量同路。
    // 对照 clang：ParseDeclWithFlags 收 nested-name-specifier，Sema 侧
    //   ActOnCXXDeclarator 判定这是 static 成员定义而非新变量。
    while (check(TokenType::ColonColon)) {
        advance();
        const Token& next = expect(TokenType::Identifier,
            "Expected member name after '::' in out-of-class definition");
        decl->name += "::" + next.text;
    }
    //  全局变量名进当前作用域（值位）。
    //  限定名（`int A::count = 7;` 的静态成员类外定义）**不登记** ——
    // 它的名字住在类 A 的作用域里，不是全局作用域；把带 "::" 的整串
    // 当裸名登记，限定名查找会拿它去裸名表里找（永远查不到）。
    if (decl->name.find("::") == std::string::npos) {
        declareName(decl->name, ScopeBit::Value);
    }
    decl->declaredType = parseArrayDeclaratorSuffix(decl->declaredType);
    // 读取并清除 parseType 留下的 constexpr 前缀，避免影响下一条声明。
    decl->isConstexpr = m_pendingConstexpr;
    m_pendingConstexpr = false;
    if (match(TokenType::Assign)) {
        decl->initializer = parseExpression();
    }
    expect(TokenType::Semicolon, "Expected ';' after global variable declaration");
    return decl;
}

// ─── 枚举声明 ─────────────────────────────────────────────────────────────────
EnumDeclPtr Parser::parseEnumDecl() {
    auto decl = std::make_shared<EnumDecl>();
    decl->location = current().location;
    expect(TokenType::KwEnum, "Expected 'enum'");
    if (match(TokenType::KwClass) || match(TokenType::KwStruct)) {
        decl->isScoped = true;
    }
    if (check(TokenType::Identifier)) {
        decl->name = advance().text;
        //  枚举名进当前作用域（类型位）—— 枚举是类型名。
        // 对照 clang：clang/lib/Sema/SemaDecl.cpp:Sema::ActOnEnumBody 把枚举名挂进
        // 外层 DeclContext（clang/lib/Sema/SemaDecl.cpp:ActOnEnumDecl）。
        declareName(decl->name, ScopeBit::Type);
    }
    if (match(TokenType::Colon)) {
        decl->underlyingType = parseType();
    } else {
        decl->underlyingType = Type::makeInt();
    }
    expect(TokenType::LBrace, "Expected '{' in enum declaration");
    int64_t nextVal = 0;
    while (!check(TokenType::RBrace) && !isAtEnd()) {
        EnumItem item;
        item.location = current().location;
        const Token& nameTok = expect(TokenType::Identifier, "Expected enum item name");
        item.name = nameTok.text;
        //  枚举项登记进**外层**作用域（值位）—— 不受限枚举的枚举项是外层
        // 作用域的普通名字（[basic.scope.enum]：枚举项声明在枚举说明符
        // 出现的作用域里）。故此处不另开枚举作用域。
        declareName(item.name, ScopeBit::Value);
        if (match(TokenType::Assign)) {
            item.hasCustomValue = true;
            item.valueExpr = parseExpression();
            // 枚举项只支持整型字面量初始化式（其余形态留给 CodeGen 报错）
            if (item.valueExpr->kind == NodeKind::IntLiteral) {
                auto lit = std::static_pointer_cast<IntLiteralExpr>(item.valueExpr);
                item.value = lit->value;
                nextVal = lit->value + 1;
            }
        } else {
            item.value = nextVal++;
        }
        decl->items.push_back(std::move(item));
        if (!match(TokenType::Comma)) {
            break;
        }
    }
    expect(TokenType::RBrace, "Expected '}' after enum items");
    expect(TokenType::Semicolon, "Expected ';' after enum declaration");
    return decl;
}

// ─── 命名空间声明 ─────────────────────────────────────────────────────────────
NamespaceDeclPtr Parser::parseNamespaceDecl() {
    auto decl = std::make_shared<NamespaceDecl>();
    decl->location = current().location;
    expect(TokenType::KwNamespace, "Expected 'namespace'");
    const Token& nameTok = expect(TokenType::Identifier, "Expected namespace name");
    decl->name = nameTok.text;
    //  进命名空间作用域：名字 N 登记进**外层**作用域并带上
    // 作用域路径 "N::"，供限定名查找（`N::S`）定位。
    // 对照 clang：clang/lib/Parse/ParseDeclCXX.cpp:Parser::ParseNamespace 走
    // ParseScope(Scope::DeclScope)，
    // Sema 侧 ActOnNamespaceDeclaration 把名字挂进外层 DeclContext。
    openScope(ScopeKind::Namespace, decl->name, ScopeBit::Namespace);
    expect(TokenType::LBrace, "Expected '{' in namespace declaration");
    while (!check(TokenType::RBrace) && !isAtEnd()) {
        decl->declarations.push_back(parseDeclaration());
    }
    expect(TokenType::RBrace, "Expected '}' after namespace declaration");
    closeScope();
    return decl;
}

// [temp.deduct.guide]：`Box(int) -> Box<int>;`。
// 前瞻配对参数括号后确认 `->`，避免与普通调用混淆。
bool Parser::looksLikeDeductionGuide() const {
    if (!check(TokenType::Identifier)) return false;
    size_t i = m_pos + 1;
    if (i >= m_tokens.size() || !m_tokens[i].is(TokenType::LParen)) return false;
    int depth = 0;
    for (; i < m_tokens.size(); i++) {
        if (m_tokens[i].is(TokenType::LParen)) depth++;
        else if (m_tokens[i].is(TokenType::RParen)) {
            depth--;
            if (depth == 0) break;
        }
        else if (m_tokens[i].is(TokenType::Semicolon) ||
                 m_tokens[i].is(TokenType::LBrace)) return false;  // 跑出声明范围了
    }
    if (i + 1 >= m_tokens.size()) return false;
    return m_tokens[i + 1].is(TokenType::Arrow);
}

DeductionGuideDeclPtr Parser::parseDeductionGuide() {
    auto decl = std::make_shared<DeductionGuideDecl>();
    decl->location = current().location;

    const Token& nameTok = expect(TokenType::Identifier, "Expected class template name in deduction guide");
    decl->guideName = nameTok.text;
    //  指引名进当前作用域（模板位）—— 指引是为某个模板写的，不是新类型。
    // 对照 clang：clang/lib/Sema/SemaTemplate.cpp:Sema::isDeductionGuideName
    // 把指引挂进外层 DeclContext。
    declareName(decl->guideName, ScopeBit::Template);

    expect(TokenType::LParen, "Expected '(' in deduction guide");
    if (!check(TokenType::RParen)) {
        do {
            Parameter p;
            p.type = parseType();
            // 形参名可选：`MyPtr(T)` 与 `MyPtr(T value)` 都合法
            // （指引形参名对推导毫无影响，clang 也允许省略）
            if (check(TokenType::Identifier)) p.name = advance().text;
            decl->parameters.push_back(std::move(p));
        } while (match(TokenType::Comma));
    }
    expect(TokenType::RParen, "Expected ')' in deduction guide");

    expect(TokenType::Arrow, "Expected '->' in deduction guide");

    // `->` 右侧：目标类型 id（Name<args>）。用 parseType 解析后取名字与实参。
    TypePtr target = parseType();
    if (!target->isClass() || target->templateArgs.empty()) {
        errorAt(current(), std::format(
            "deduction guide for '{}' must map to a specialization of a class "
            "template (e.g. {}<...>)", decl->guideName, decl->guideName));
    }
    for (const auto& arg : target->templateArgs) {
        if (!arg.isType()) {
            errorAt(current(), "deduction guide target arguments must be types");
        }
        decl->targetArgs.push_back(arg.type);
    }

    expect(TokenType::Semicolon, "Expected ';' after deduction guide");

    logParse("  [parse:guide] ★ deduction guide: {}(", decl->guideName);
    std::string guideLine;
    for (size_t i = 0; i < decl->parameters.size(); i++) {
        if (i > 0) guideLine += ", ";
        guideLine += decl->parameters[i].type->toString();
    }
    guideLine += ") -> " + target->toString();
    logParse("{}\n", guideLine);
    return decl;
}

// ─── 类型别名声明 ─────────────────────────────────────────────────────────────
TypeAliasDeclPtr Parser::parseTypeAliasDecl() {
    auto decl = std::make_shared<TypeAliasDecl>();
    decl->location = current().location;
    if (match(TokenType::KwUsing)) {
        const Token& aliasTok = expect(TokenType::Identifier, "Expected alias name in using declaration");
        decl->aliasName = aliasTok.text;
        expect(TokenType::Assign, "Expected '=' in using declaration");
        decl->underlyingType = parseType();
        expect(TokenType::Semicolon, "Expected ';' after using declaration");
    } else if (match(TokenType::KwTypedef)) {
        decl->underlyingType = parseType();
        const Token& aliasTok = expect(TokenType::Identifier, "Expected alias name in typedef declaration");
        decl->aliasName = aliasTok.text;
        expect(TokenType::Semicolon, "Expected ';' after typedef declaration");
    }
    //  类型别名进当前作用域（类型位）—— 别名是类型名。
    // 对照 clang：clang/lib/Sema/SemaDecl.cpp:Sema::ActOnTypedefNameDecl 挂进当前
    // DeclContext（clang/lib/Sema/SemaDecl.cpp:ActOnTypeAliasDecl）。
    if (!decl->aliasName.empty()) {
        declareName(decl->aliasName, ScopeBit::Type);
    }
    return decl;
}

// [temp.param]：自由模板与成员模板共用形参表解析，支持类型、值及模板模板形参。
// 调用方管理形参作用域，使其覆盖整个模板体；本函数同步形参表与可见计数。
// 参考：clang/lib/Parse/ParseTemplate.cpp:Parser::ParseTemplateParameters。
void Parser::parseTemplateParameterList(TemplateDecl& decl, TemplateParamFrame& frame) {
    logParse("  [parse:template]   parsing parameter list <");

    // 解析形参表 —— 形参位扫到哪种写法 ⇒ 建出哪种 TemplateParam：
    //   'typename'|'class' IDENT      ⇒ {Type, "T"}            类型形参
    //   'typename'|'class' 后非 IDENT ⇒ {Type, "$unnamedN"}    无名类型形参
    //   类型 IDENT（如 int N）        ⇒ {NonType, "N"}         NTTP
    //   '>' —— 一个形参都不写          ⇒ 空表 = 【全特化】标记（[temp.expl.spec]），
    //       故先判 '>' 再进循环，不能用 do-while 无条件吃一个形参。

    if (!check(TokenType::Greater)) {
        int unnamedSeq = 0;        // 无名形参的合成名序号（见下方 unnamed 分支）
        int innerUnnamedSeq = 0;   // 模板模板形参【内层表】自己的合成名序号
        do {
            TemplateParam param;
            param.location = current().location;

            if (check(TokenType::KwTypename) || check(TokenType::KwClass)) {
                // 类型形参可有名或无名：`class T`、`typename`；无名形参使用内部名字。
                param.kind = TemplateParamKind::Type;
                bool isClassKw = check(TokenType::KwClass);
                advance();
                logParse("{}", isClassKw ? "class " : "typename ");

                if (check(TokenType::Identifier)) {
                    param.name = advance().text;
                    logParse("{}", param.name);
                    logParse(
                        "\n  [parse:template]   ★ type parameter registered: '{}'\n",
                        param.name);
                } else {
                    // 无名形参：合成一个用户写不出来的名字（带 '$' 前缀）。
                    // 用不可写字符是刻意的——保证不会与任何真实标识符撞车。
                    param.name = "$unnamed" + std::to_string(unnamedSeq++);
                    param.isUnnamed = true;
                    logParse(
                        "(unnamed) ⇒ 内部合成名 '{}'\n  [parse:template]   "
                        "★ type parameter registered: '{}' (nameless, [temp.param]/3)\n",
                        param.name, param.name);
                }
            }
            else if (check(TokenType::KwTemplate)) {
                // [temp.param]：`template<template<class> class C, class T>`。
                // 模板模板形参保存内层签名；匹配留给语义分析。
                param.kind = TemplateParamKind::Template;
                advance();   // 'template'
                expect(TokenType::Less,
                       "Expected '<' after 'template' in template template parameter");
                if (!check(TokenType::Greater)) {
                    do {
                        //  与【外层形参表】同一套形态分支，区别只在建出的节点挂到
                        // C 自己的子表 innerParams 上 —— 不能塞进外层那个 vector，
                        // 那会多出假的形参位（docs/learn/33 §3.1 记的坑）。
                        // 内层形参名只为诊断好看：它不进模板体（模板体里用的是实参
                        // 模板的形参，跟这里的名字无关）。
                        TemplateParam inner;
                        inner.location = current().location;
                        if (check(TokenType::KwTypename) || check(TokenType::KwClass)) {
                            inner.kind = TemplateParamKind::Type;
                            advance();
                            if (check(TokenType::Identifier)) {
                                inner.name = advance().text;   // 内层形参名可省
                            } else {
                                inner.name = "$unnamed" + std::to_string(innerUnnamedSeq++);
                                inner.isUnnamed = true;
                            }
                        }
                        else if (check(TokenType::KwTemplate)) {
                            // 内层又是模板模板参数：本项目只做一层
                            // （更深的嵌套在此报错而不是静默算错）
                            error("nested template template parameters are not implemented "
                                  "(depth > 1)");
                        }
                        else {
                            // 内层 NTTP：如 `template<template<int> class C>`
                            inner.kind = TemplateParamKind::NonType;
                            inner.nonType = parseType();
                            if (check(TokenType::Identifier)) inner.name = advance().text;
                        }
                        param.innerParams.push_back(
                            std::make_shared<TemplateParam>(std::move(inner)));
                    } while (match(TokenType::Comma));
                }
                expect(TokenType::Greater,
                       "Expected '>' to close template template parameter list");
                const size_t innerArity = param.innerParams.size();
                // 自己的 class / typename 关键字
                if (!check(TokenType::KwClass) && !check(TokenType::KwTypename)) {
                    error("Expected 'class' or 'typename' after the template parameter "
                          "list of a template template parameter");
                }
                advance();
                param.name = expect(TokenType::Identifier,
                    "Expected name of the template template parameter").text;
                logParse(
                    "template <...({})> class {}\n  [parse:template]   "
                    "★ template template parameter registered: '{}' "
                    "(accepts a template with {} parameter(s), [temp.param]/4)\n",
                    innerArity, param.name, param.name, innerArity);
            }
            else {
                // 非类型模板参数 (NTTP)：如 int N, bool Flag 等。
                param.kind = TemplateParamKind::NonType;
                param.nonType = parseType();
                const Token& paramName = expect(TokenType::Identifier, "Expected parameter name after type in template parameter");
                param.name = paramName.text;
                logParse("{} {}", param.nonType->toString(), param.name);
                logParse("\n  [parse:template]   ★ non-type parameter registered: {} '{}'\n",
                    param.nonType->toString(), param.name);
            }

            // 默认实参解析后才将本形参引入作用域，避免默认值意外引用自身。
            // `template<class T, class U = T>` 可引用前一位 T。
            // 参考：clang/lib/Parse/ParseTemplate.cpp:Parser::ParseTypeParameter。
            if (match(TokenType::Assign)) {
                if (check(TokenType::IntLiteral)) {
                    const Token& lit = advance();
                    IntLiteralValue lv = parseIntLiteral(lit.text, lit.location);
                    param.defaultArg = TemplateArg::ofValue(lv.value, lv.type);
                }
                else if (check(TokenType::Minus)
                         && m_pos + 1 < m_tokens.size()
                         && m_tokens[m_pos + 1].is(TokenType::IntLiteral)) {
                    advance();
                    const Token& lit = advance();
                    IntLiteralValue lv = parseIntLiteral(lit.text, lit.location);
                    param.defaultArg = TemplateArg::ofValue(-lv.value, lv.type);
                }
                else if (check(TokenType::KwTrue) || check(TokenType::KwFalse)) {
                    // 默认值也可以是 bool（`template <bool B = true>`）
                    bool b = check(TokenType::KwTrue);
                    advance();
                    param.defaultArg = TemplateArg::ofValue(b ? 1 : 0, Type::makeBool());
                }
                else if (check(TokenType::CharLiteral)) {
                    // 默认值也可以是字符（`template <char C = 'x'>`）
                    const Token& tok = advance();
                    int64_t val = 0;
                    for (char ch : tok.text) val = (val << 8) | static_cast<unsigned char>(ch);
                    param.defaultArg = TemplateArg::ofValue(val, Type::makeChar());
                }
                else {
                    param.defaultArg = TemplateArg::ofType(parseType());
                }
                param.hasDefault = true;
                logParse(
                    "  [parse:template]   ★ default template argument: '{}' = {}\n",
                    param.name, param.defaultArg.toString());
            }

            // 形参入表与 frame.count 必须同步，否则模板体查询不到刚声明的类型或值形参。
            // 参考：clang/lib/Sema/SemaTemplate.cpp:Sema::ActOnTypeParameter。
            decl.typeParams.push_back(param.name);
            decl.templateParams.push_back(
                std::make_shared<TemplateParam>(std::move(param)));
            frame.count = decl.templateParams.size();   // ← 可见范围同步扩张
//  形参名登记进【模板形参作用域】—— 种类位按形参形态分：
// 类型形参 ⇒ 类型位 / NTTP ⇒ 值位 / 模板模板形参 ⇒ 模板位。
//  位置与上面那两行同步（都在默认实参解析之后）—— [basic.scope.pdecl]/9：
// 形参名的作用域从其声明符之后才开始。
            {
                const TemplateParam& tp = *decl.templateParams.back();
                unsigned paramBits = ScopeBit::Type;
                if (tp.kind == TemplateParamKind::NonType) {
                    paramBits = ScopeBit::Value;
                } else if (tp.kind == TemplateParamKind::Template) {
                    paramBits = ScopeBit::Template;
                }
                declareName(tp.name, paramBits);
            }
            logParse(
                "  [parse:template]   ↗ register tparam '{}' into scope\n",
                decl.templateParams.back()->name);

        } while (match(TokenType::Comma) && (logParse("{}", ", "), true));
    }
    else {
        logParse("  [parse:template] ★ empty parameter list — explicit (full) specialization\n");
    }

    logParse(">\n");

    expect(TokenType::Greater, "Expected '>' after template parameters");

    // ── 默认实参约束（[temp.param]/12）──
    // 一旦某位形参带了默认值，其后每一位都必须带；否则出现"空洞"，
    // 调用者无法用位置实参填满。对照 clang: clang/include/clang/Basic/DiagnosticSemaKinds.td:err_template_param_default_arg_missing
    for (size_t i = 0; i < decl.templateParams.size(); i++) {
        if (decl.templateParams[i]->hasDefault) {
            for (size_t j = i + 1; j < decl.templateParams.size(); j++) {
                if (!decl.templateParams[j]->hasDefault) {
                    errorAt(current(), std::format(
                        "template parameter '{}' must have a default argument "
                        "because '{}' (declared before it) has one",
                        decl.templateParams[j]->name, decl.templateParams[i]->name));
                }
            }
            break;
        }
    }
}

// [temp]：类模板、函数模板、别名模板及推导指引保存为蓝图，实例化时再替换形参。
TemplateDeclPtr Parser::parseTemplateDecl() {
    auto decl = std::make_shared<TemplateDecl>();
    decl->location = current().location;

    logParse("  [parse:template] ▶ template declaration at {}\n",
        current().location.toString());

    // 强制消费 'template' 与 '<'（必选文法，用 expect 保证结构）
    expect(TokenType::KwTemplate, "Expected 'template'");
    expect(TokenType::Less, "Expected '<' after 'template'");

    // ── 形参作用域建帧：构造即入栈、析构即出栈，使形参表与模板体解析期间
    //    parseType 能把裸 T 识别为 TemplateParam 节点。对照 clang：
    //    ParseTemplateParameters 的 TemplateScopes.Enter(TemplateParamScope)
    //    （ParseTemplate.cpp:332），同样开在解析形参表之前、MultiParseScope 析构时 Exit。
    //    （理论见 docs/learn/30）
    TemplateParamFrame frame(this, decl.get());
    //  进模板形参作用域：形参名登记进这一层，模板体（类体/函数体）
    // 解析期间可见 —— `template<class T> struct Box { T v; };` 里的 T。
    //  该作用域对【名字登记】是透明的：模板自己的名字（Box）属于外层作用域。
    // 对照 clang：ParseTemplateParameters 的 TemplateScopes.Enter(TemplateParamScope)
    // （clang/lib/Parse/ParseTemplate.cpp:332）。
    openScope(ScopeKind::TemplateParam, "", 0u);

    //  形参表解析【复用共享实现】—— 自由模板与成员模板共用同一份文法与语义规则。
    parseTemplateParameterList(*decl, frame);

    // （形参已在上面循环里逐个注册进 frame；frame 是栈上局部对象，返回时析构自动出栈。）

    // 解析模板体：类模板 或 函数模板（S1+）
    // 分派依据：template<...> 之后的第一个 Token
    //   class / struct                 → 类模板（struct 与 class 只差默认访问级别）
    //   类型关键字 / 标识符（如 T、int） → 函数模板（返回类型可以是模板参数名 T）
    //   其他                           → 报错
    if (check(TokenType::KwClass) || check(TokenType::KwStruct)) {
        logParse("  [parse:template]   parsing class body for template...\n");
        // 类名后可能跟模板 id（Box<T*, T> / Box<int*, int>）—— 那是特化，
        // 由 parseClassDecl 把尖括号里的模式回填进 specPattern（[temp.class.spec]）。
        decl->classTemplate = parseClassDecl(&decl->specPattern);

        // [temp.class.spec]/[temp.expl.spec]：形参表与类名后的实参模式共同决定模板种类。
        // `template<class T> struct B;` 是主模板；`B<T*>` 是偏特化；`template<> B<int>` 是全特化。
        if (decl->templateParams.empty() && decl->specPattern.empty()) {
            error(std::format(
                "extraneous 'template<>' in declaration of class '{}' —— "
                "写了 `template<>` 却没有在类名后写 `<...>`：全特化必须点明模板实参"
                "（如 `template<> struct {}<int, int>`）；若本意是定义主模板，"
                "请去掉 `template<>`",
                decl->templateName(), decl->templateName()));
        }

        if (!decl->specPattern.empty()) {
            decl->specKind = decl->templateParams.empty()
                                 ? TemplateSpecKind::ExplicitSpec
                                 : TemplateSpecKind::PartialSpec;
            logParse(
                "  [parse:template] ★ {} specialization of '{}' with pattern <{}>\n",
                decl->isExplicitSpec() ? "EXPLICIT (full)" : "PARTIAL",
                decl->templateName(),
                [&] { std::string s;
                      for (size_t i = 0; i < decl->specPattern.size(); i++) {
                          if (i > 0) s += ", ";
                          s += decl->specPattern[i].toString();
                      } return s; }());
        }
    }
    else if (check(TokenType::Identifier) && looksLikeDeductionGuide()) {
        // ── 模板推导指引：template<class T> MyPtr(T) -> MyPtr<T>; ──
        // 分派依据是"配对右括号后跟 '->'"，与函数模板 `T f(T)` 区分开。
        decl->guide = parseDeductionGuide();
        decl->guide->templateParams = decl->templateParams;   // 形参表由外层 template<> 提供
    }
    else if (check(TokenType::KwUsing)) {
        // [temp.alias]：`template<class T> using Vec = MyPtr<T>;` 保存待替换的别名目标。
        logParse("  [parse:template]   parsing alias template...\n");
        decl->aliasTemplate = parseTypeAliasDecl();
    }
    else if (current().isTypeKeyword() || check(TokenType::Identifier)) {
        logParse("  [parse:template]   parsing function signature for template...\n");
        decl->funcTemplate = parseFunctionDecl();
    }
    else {
        errorAt(current(),
            "Expected 'class' or a function signature after template parameter list");
    }

    //  出模板形参作用域 + 模板名登记进【外层】作用域——
    //  位置在 closeScope() 之后：此时"当前作用域"就是模板声明所在的外层
    // 作用域（模板形参作用域对【名字登记】是透明的）。
    // 类模板的名字已由 parseClassDecl 的 openScope 登记（类型位|模板位）；
    // 别名模板 / 推导指引 / 函数模板的名字在这里补登记。
    closeScope();
    if (decl->isFunctionTemplate()) {
        // 函数模板的名字是【值】名（函数），但它同时是模板名。
        declareName(decl->templateName(), ScopeBit::Value | ScopeBit::Template);
    } else if (!decl->isClassTemplate()) {
        // 别名模板的名字是类型名；推导指引的名字指向被推导的类模板（也是类型名）。
        declareName(decl->templateName(), ScopeBit::Type | ScopeBit::Template);
    }

    logParse("  [parse:template] ◀ {} template '{}' with {} parameter(s) stored as blueprint\n",
        decl->isClassTemplate()      ? "class"
        : decl->isAliasTemplate()    ? "alias"
        : decl->isDeductionGuide()   ? "deduction-guide"
                                     : "function",
        decl->templateName(), decl->typeParams.size());

    // 蓝图摘要整行输出，保证试探日志完整回滚。
    // 形参种类读取 templateParams，裸名字表 typeParams 无法区分类型与值形参。
    std::string summary = "  [parse:template]   blueprint summary:\n    template <";
    for (size_t i = 0; i < decl->templateParams.size(); i++) {
        if (i > 0) summary += ", ";
        const TemplateParam& p = *decl->templateParams[i];
        if (p.kind == TemplateParamKind::Type) {
            summary += "typename " + p.name;
        } else if (p.kind == TemplateParamKind::NonType) {
            // NTTP：形参写成「类型 + 名字」（`int N` / `bool B`），不是 typename
            summary += (p.nonType ? p.nonType->toString() : "?") + " " + p.name;
        } else {
            // 模板模板形参：`template <...> class C`
            summary += "template <...> class " + p.name;
        }
    }
    if (decl->isClassTemplate()) {
        summary += std::format("> class {} {{ ... }}\n", decl->templateName());
    } else if (decl->isAliasTemplate()) {
        summary += std::format("> using {} = {}\n", decl->templateName(),
            decl->aliasTemplate->underlyingType
                ? decl->aliasTemplate->underlyingType->toString() : "?");
    } else if (decl->isDeductionGuide()) {
        // 指引没有类体/函数体可言，摘要只报形参表
        summary += std::format("> {}(", decl->templateName());
        for (size_t i = 0; i < decl->guide->parameters.size(); i++) {
            if (i > 0) summary += ", ";
            summary += decl->guide->parameters[i].type->toString();
        }
        summary += ") -> " + decl->templateName() + "<...>  (deduction guide)\n";
    } else {
        summary += std::format("> {} {}(",
            decl->funcTemplate->returnType ? decl->funcTemplate->returnType->toString() : "?",
            decl->templateName());
        for (size_t i = 0; i < decl->funcTemplate->parameters.size(); i++) {
            if (i > 0) summary += ", ";
            summary += decl->funcTemplate->parameters[i].type->toString() + " "
                     + decl->funcTemplate->parameters[i].name;
        }
        summary += ") { ... }\n";
    }
    logParse("{}", summary);

    // ── 帧出栈 ──
    //  这里【什么都不用写】：frame 是栈上局部对象，函数返回时析构，析构里回写
    // m_currentFrame = parent 完成出栈；异常路径由栈展开同样保证
    // （对照 clang 的 MultiParseScope）。
    logParse("  [parse:template]   ↘ tparam scope popped\n");

    return decl;
}

// class|struct Name [: public Base ("," public Base)*] { member* } ;
// 类作用域覆盖成员解析，支持字段、方法、构造/析构、别名及嵌套类。
std::vector<CtorInitializer> Parser::parseCtorInitializerList() {
    std::vector<CtorInitializer> list;
    expect(TokenType::Colon, "Expected ':' to start constructor initializer list");
    do {
        CtorInitializer init;
        init.location = current().location;
        const Token& memberTok = expect(TokenType::Identifier, "Expected member or base name in initializer");
        init.memberName = memberTok.text;
        expect(TokenType::LParen, "Expected '(' after member name in initializer");
        if (!check(TokenType::RParen)) {
            do {
                init.arguments.push_back(parseExpression());
            } while (match(TokenType::Comma));
        }
        expect(TokenType::RParen, "Expected ')' in member initializer");
        list.push_back(std::move(init));
    } while (match(TokenType::Comma));
    return list;
}

// ─── 构造函数声明 ─────────────────────────────────────────────────────────────
CtorDeclPtr Parser::parseConstructorDecl(const std::string& ownerClass) {
    auto ctor = std::make_shared<ConstructorDecl>();
    ctor->location = current().location;
    ctor->ownerClassName = ownerClass;
    const Token& nameTok = expect(TokenType::Identifier, "Expected constructor name");
    ctor->name = nameTok.text;
    expect(TokenType::LParen, "Expected '(' in constructor parameter list");
    ctor->parameters = parseParameterList();
    expect(TokenType::RParen, "Expected ')' after constructor parameters");

    if (check(TokenType::Colon)) {
        ctor->initList = parseCtorInitializerList();
    }

    if (check(TokenType::LBrace)) {
        // parseBlockStmt 恒返回 BlockStmt，直接下行转换（与 :1421 的函数体一致）
        ctor->body = std::static_pointer_cast<BlockStmt>(parseBlockStmt());
    } else {
        expect(TokenType::Semicolon, "Expected '{' or ';' after constructor declaration");
    }
    return ctor;
}

// ─── 析构函数声明 ─────────────────────────────────────────────────────────────
DtorDeclPtr Parser::parseDestructorDecl(const std::string& ownerClass, bool isVirtual) {
    auto dtor = std::make_shared<DestructorDecl>();
    dtor->location = current().location;
    dtor->ownerClassName = ownerClass;
    dtor->isVirtual = isVirtual;
    expect(TokenType::Tilde, "Expected '~' in destructor declaration");
    const Token& nameTok = expect(TokenType::Identifier, "Expected destructor name");
    dtor->name = "~" + nameTok.text;
    expect(TokenType::LParen, "Expected '(' in destructor parameter list");
    expect(TokenType::RParen, "Expected ')' in destructor parameter list");

    if (check(TokenType::LBrace)) {
        dtor->body = std::static_pointer_cast<BlockStmt>(parseBlockStmt());
    } else {
        expect(TokenType::Semicolon, "Expected '{' or ';' after destructor declaration");
    }
    return dtor;
}

// ── parseClassDecl 主入口（成员文法见上方"类声明"小节）──
// 多继承（[class.mi]）：逗号分隔的基类列表，说明符可省略（默认级别按 struct/class 定）；
// 声明顺序即子对象摆放顺序（主基类优化：第一个多态基类的虚表与派生类合并）。
ClassDeclPtr Parser::parseClassDecl(std::vector<TemplateArg>* outSpecPattern) {
    auto decl = std::make_shared<ClassDecl>();
    decl->location = current().location;

    bool isStruct = false;
    if (match(TokenType::KwStruct)) {
        isStruct = true;
        decl->currentAccess = AccessModifier::Public;
    } else {
        expect(TokenType::KwClass, "Expected 'class' or 'struct'");
        decl->currentAccess = AccessModifier::Private;
    }
    // isStruct 还决定【默认继承访问级别】（见下方继承列表，[class.derived]/2）

    const Token& nameToken = expect(TokenType::Identifier, "Expected class name");
    decl->name = nameToken.text;

    // [temp.class.spec]：类名后的实参表是特化模式，如 `Box<T*, T>`。
    if (outSpecPattern && check(TokenType::Less)) {
        // [temp.class.spec]：特化模式同时接受类型与值，如 `T*`、`int`、`true`、`N`。
        *outSpecPattern = parseTemplateArgumentList();
        logParse("  [parse:class] ★ specialization pattern for '{}'\n",
            decl->name);
    }

    // [class.derived]：继承列表用逗号分隔；struct 默认 public，class 默认 private。
    // 本实现尚未提供 private/protected 继承的访问控制语义，显式写出时诊断为未实现。
    if (match(TokenType::Colon)) {
        do {
            if (match(TokenType::KwPublic)) {
                // 显式 public：本项目唯一受支持的形态，放行
            } else if (check(TokenType::KwPrivate) || check(TokenType::KwProtected)) {
                error(std::format(
                    "'{}' inheritance is not implemented (only public inheritance is); "
                    "omit the specifier on a struct to get public by default",
                    current().text));
            } else if (!isStruct) {
                // 省略说明符 + class ⇒ 默认 private 继承，同上未实现
                error("a class default-inherits privately ([class.derived]/2), which is "
                      "not implemented; write 'public' explicitly or use 'struct'");
            }
            // 落到这里 = 省略说明符 + struct ⇒ 默认 public，继续解析基类名
            // 基类名支持命名空间限定：: public std::false_type
            // （std 垫片 is_range 的基类正是 std::true_type / std::false_type，
            //  与 parseType 的限定名处理同构）
            std::string baseName = expect(TokenType::Identifier,
                                          "Expected base class name").text;
            while (check(TokenType::ColonColon)) {
                advance();
                baseName += "::" + expect(TokenType::Identifier,
                    "Expected base class name after '::'").text;
            }
            decl->baseClassNames.push_back(baseName);
        } while (match(TokenType::Comma));
    }

    // [dcl.type.elab]：`struct A;` 只声明名字，不提供成员或布局，允许后续指针引用。
    if (match(TokenType::Semicolon)) {
        decl->isForwardDecl = true;   //  让 Sema 跳过成员生成与布局计算
        logParse(
            "  [parse:class] ★ forward declaration of '{}' (no body) [dcl.type.elab]\n",
            decl->name);
        //  前置声明没有类体 ⇒ 不开类作用域，但名字仍要在外层可见
        // （`struct A;` 之后 `A* p;` 要能认出 A 是类型）。
        declareName(decl->name,
                    ScopeBit::Type | (outSpecPattern ? ScopeBit::Template : 0u));
        return decl;
    }

    //  进类作用域：类名登记进【外层】作用域（类型位；模板再加模板位），
    // 类体里的成员（字段/方法/嵌套类/类内别名）登记进这一层。
    // 对照 clang：clang/lib/Sema/SemaDecl.cpp:Sema::ActOnTagStartDefinition 走
    // ParseScope(Scope::DeclScope)，
    // Sema 侧 ActOnTagStartDefinition/ActOnTagFinishDefinition 挂成员。
    openScope(ScopeKind::Class, decl->name,
              ScopeBit::Type | (outSpecPattern ? ScopeBit::Template : 0u));

    expect(TokenType::LBrace, "Expected '{' after class name");

    // 解析类成员
    while (!check(TokenType::RBrace) && !isAtEnd()) {
        // 访问修饰符
        AccessModifier access = decl->currentAccess;
        if (check(TokenType::KwPublic)) { // 判断成员的访问级别是 public 还是priviate
            advance();
            expect(TokenType::Colon, "Expected ':' after 'public'");
            decl->currentAccess = AccessModifier::Public;
            access = AccessModifier::Public;
            continue;
        }
        if (check(TokenType::KwPrivate)) {
            advance();
            expect(TokenType::Colon, "Expected ':' after 'private'");
            decl->currentAccess = AccessModifier::Private;
            access = AccessModifier::Private;
            continue;
        }
        if (check(TokenType::KwProtected)) {
            advance();
            expect(TokenType::Colon, "Expected ':' after 'protected'");
            decl->currentAccess = AccessModifier::Protected;
            access = AccessModifier::Protected;
            continue;
        }

        // [class.nest]：`struct A { struct B { int v; }; };`。
        // 嵌套类保存在外层成员树中，由 Sema 统一限定名字。
        if (check(TokenType::KwClass) || check(TokenType::KwStruct)) {
            auto nested = parseClassDecl();
            decl->nestedClasses.push_back(nested);
            logParse("  [parse:member] ★ '{}' 是嵌套类（名字属于类作用域 [class.nest]）\n",
                nested->name);
            continue;
        }

        // [dcl.typedef]：`using type = T;` 与 `typedef T type;` 保存相同的成员别名信息。
        if (check(TokenType::KwUsing)) {
            advance();
            const Token& aliasTok = expect(TokenType::Identifier,
                                           "Expected alias name after 'using'");
            expect(TokenType::Assign, "Expected '=' in alias declaration");
            TypePtr target = parseType();
            expect(TokenType::Semicolon, "Expected ';' after alias declaration");
            decl->typeAliases[aliasTok.text] = target;
            decl->typeAliasOrder.push_back(aliasTok.text);
            //  类内类型别名登记进【类作用域】（类型位）—— 类作用域的成员。
            declareName(aliasTok.text, ScopeBit::Type);
            logParse("  [parse:alias] using {} = {}\n",
                aliasTok.text, target->toString());
            continue;
        }
        if (check(TokenType::KwTypedef)) {
            advance();
            TypePtr target = parseType();
            const Token& aliasTok = expect(TokenType::Identifier,
                                           "Expected alias name in typedef");
            expect(TokenType::Semicolon, "Expected ';' after typedef");
            decl->typeAliases[aliasTok.text] = target;
            decl->typeAliasOrder.push_back(aliasTok.text);
            //  类内类型别名登记进【类作用域】（类型位）—— 类作用域的成员。
            declareName(aliasTok.text, ScopeBit::Type);
            logParse("  [parse:alias] typedef {} {}\n",
                target->toString(), aliasTok.text);
            continue;
        }

        // [class.static]：static 成员函数没有隐式 this；static 数据成员不占对象布局。
        bool isStatic = false;
        if (match(TokenType::KwStatic)) {
            isStatic = true;
        }

        // 虚函数标记
        bool isVirtual = false;
        if (check(TokenType::KwVirtual)) {
            isVirtual = true;
            advance();
        }

        // [temp.mem]：`struct S { template<class T> T id(T x) { return x; } };`。
        // 成员函数模板与自由模板共用形参解析和推导，实例化函数另带 ownerClassName。
        // 本入口仅支持成员函数模板，成员类模板、别名模板和变量模板仍不支持。
        // 参考：clang/lib/Parse/ParseDeclCXX.cpp:Parser::ParseCXXMemberSpecification。
        if (check(TokenType::KwTemplate)) {
            advance();   // 'template'
            expect(TokenType::Less, "Expected '<' after 'template'");

            auto memTmpl = std::make_shared<TemplateDecl>();
            // 形参作用域建帧：使下面 parseMethodDecl 里的裸 T 认得出是模板形参
            TemplateParamFrame memFrame(this, memTmpl.get());
            //  进模板形参作用域：形参名登记进这一层，方法体解析期间可见。
            openScope(ScopeKind::TemplateParam, "", 0u);

            // 成员模板调用共享 parseTemplateParameterList，形参能力及可见计数与自由模板一致。
            parseTemplateParameterList(*memTmpl, memFrame);

            // 成员说明符位于 template-head 之后：`template<class U> static U f(U);`。
            bool mtIsStatic = isStatic;          // 前缀位置已吃过 ⇒ 兼容既有写法
            if (match(TokenType::KwStatic)) mtIsStatic = true;
            if (check(TokenType::KwVirtual) || isVirtual) {
                // 成员【函数】模板不能是虚函数（[temp.mem]/2 末句：模板不能被 virtual 化
                // —— 虚表要求每个实例一张条目，而实例是按需产生的）。
                // 文案与 clang 逐字相同（'virtual' cannot be specified on
                // member function templates）。
                error("'virtual' cannot be specified on member function templates");
            }

            auto method = parseMethodDecl(decl->name, access);
            method->isStatic  = mtIsStatic;
            method->isVirtual = false;           // 上面已把 virtual 拒掉
            memTmpl->funcTemplate = method;   // templateName() 由此取到函数名
            memTmpl->isMemberTemplate = true;
            decl->memberTemplates.push_back(memTmpl);
            //  出模板形参作用域；随后给方法名补【模板位】——
            //  位置在 closeScope() 之后：此时"当前作用域"是类作用域。
            //  方法名的【值位】已经在那儿了 —— parseFunctionDecl 里的
            // openScope(Function, name, Value) 把名字登记进【最近的外层非模板形参
            // 作用域】（ParseScopeTable::nameScopeIndex 跳过模板形参作用域 ⇒ 落类作用域），
            // 这里只补一位（位并集 ⇒ 值位|模板位）。
            closeScope();
            declareName(method->name, ScopeBit::Template);

            logParse(
                "  [parse:member] ★ '{}' 是成员模板（{} 个模板形参，[temp.mem]）"
                "—— 调用点按实参推导实例化\n",
                method->name, memTmpl->templateParams.size());
            continue;
        }

        // 1. 析构函数：[virtual] ~ClassName()
        if (check(TokenType::Tilde)) {
            auto dtor = parseDestructorDecl(decl->name, isVirtual);
            decl->methods.push_back(dtor);
            continue;
        }

        // 构造函数无返回类型：当前类名后直接跟参数表，可带成员初始化列表。
        if (!isVirtual && check(TokenType::Identifier) && current().text == decl->name) {
            size_t saved = m_pos;
            advance();
            if (check(TokenType::LParen)) { // 构造方法
                m_pos = saved;
                auto ctor = parseConstructorDecl(decl->name);
                decl->methods.push_back(ctor);
                continue;
            }
            m_pos = saved;
        }

        // 普通方法与字段用 TokenCheckpoint 纯试探区分，判定后回滚游标和日志。
        bool isMethod = false;
        {
            TokenCheckpoint cp(*this);
            try {
                parseType(); // 试探性解析：仅用于前进 token 流以观察后续符号
                if (check(TokenType::Identifier)) {
                    advance();
                    if (check(TokenType::LParen)) {
                        isMethod = true;
                    }
                }
            } catch (const ParseError&) {
                isMethod = false;
            }
        }   // 未 commit ⇒ 全状态回卷 + 试探日志整体废弃

        if (isMethod) {
            auto method = parseMethodDecl(decl->name, access);
            method->isVirtual = isVirtual;
            method->isStatic  = isStatic;
            if (isStatic) {
                logParse(
                    "  [parse:member] ★ '{}' 是 static 成员函数（无隐式 this，"
                    "调用不传对象 [class.static]/2）\n", method->name);
            }
            decl->methods.push_back(method);
        } else {
            if (isStatic) {
                // [class.static.data]：`static int count;`、`static const int limit = 7;`。
                // 保存声明和初始化信息；存储与常量折叠由 Sema 决定，不作为对象字段。
                TypePtr smType = parseType();   // 收 const/constexpr 前缀
                const Token& smName = expect(TokenType::Identifier,
                                             "Expected static member name");
                ExprPtr smInit;
                if (match(TokenType::Assign)) {
                    smInit = parseExpression();   // 类内初始化式（Sema 判 const 整型族）
                }
                expect(TokenType::Semicolon, "Expected ';' after static member declaration");
                StaticDataMember sm;
                sm.name = smName.text;
                sm.type = smType;
                sm.initializer = std::move(smInit);
                sm.isConstexpr = m_pendingConstexpr;   // parseType 留下的前缀
                m_pendingConstexpr = false;
                //  static 数据成员登记进【类作用域】（值位）—— 它是类作用域的名字，
                // 但【不占对象内存】（存储在类外定义，[class.static.data]）。
                declareName(smName.text, ScopeBit::Value);
                decl->staticDataMembers.push_back(std::move(sm));
                logParse(
                    "  [parse:member] ★ '{}' 是 static 数据成员（不占对象内存，"
                    "存储在类外定义 [class.static.data]）\n", sm.name);
                continue;
            }
            // 字段声明
            TypePtr fieldType = parseType();
            const Token& fieldName = expect(TokenType::Identifier, "Expected field name");
            fieldType = parseArrayDeclaratorSuffix(fieldType);
            expect(TokenType::Semicolon, "Expected ';' after field declaration");

            FieldInfo field;
            field.name = fieldName.text;
            //  字段登记进【类作用域】（值位）。
            declareName(fieldName.text, ScopeBit::Value);
            field.type = fieldType;
            field.access = access;
            decl->fields.push_back(field);
        }
    }

    expect(TokenType::RBrace, "Expected '}' after class body");
    expect(TokenType::Semicolon, "Expected ';' after class definition");

    closeScope();
    return decl;
}

// 成员方法复用函数声明解析；virtual 标记由类成员入口传入。
FuncDeclPtr Parser::parseMethodDecl(const std::string& ownerClass,
                                     AccessModifier /*access*/) {
    bool isVirtual = false;
    if (check(TokenType::KwVirtual)) {
        advance();
        isVirtual = true;
    }

    auto func = parseFunctionDecl(false, ownerClass);
    func->isVirtual = isVirtual;

    // 检查 override
    // override 出现在函数声明的末尾（在 { 或 ; 之前）
    // 由于 parseFunctionDecl 已经解析了函数体和 ;，这里需要特殊处理
    // 简化处理：在 parseFunctionDecl 中处理 override

    return func;
}

// function-decl := type IDENT "(" parameter-list ")" [override] (body | ";")。
FuncDeclPtr Parser::parseFunctionDecl(bool isVirtual, const std::string& ownerClass) {
    auto decl = std::make_shared<FunctionDecl>();
    decl->location = current().location;
    decl->isVirtual = isVirtual;
    decl->ownerClassName = ownerClass;

    // 跳过 virtual（如果在 parseMethodDecl 中没有被消费）
    if (check(TokenType::KwVirtual)) {
        advance();
    }

    // 返回类型
    decl->returnType = parseType();

    // 函数名
    const Token& nameToken = expect(TokenType::Identifier, "Expected function name");
    decl->name = nameToken.text;

    //  进函数作用域：函数名登记进【外层】作用域（值位），形参登记进
    // 函数作用域，函数体是它里面的一层块作用域。
    // 对照 clang：ActOnStartOfFunctionDef 里 PushScope(Scope::DeclScope) 开函数
    // 作用域，形参挂在其中（clang/lib/Sema/SemaDecl.cpp:ActOnStartOfFunctionDef）。
    openScope(ScopeKind::Function, decl->name, ScopeBit::Value);

    // 参数列表
    expect(TokenType::LParen, "Expected '(' after function name");
    decl->parameters = parseParameterList();
    expect(TokenType::RParen, "Expected ')' after parameters");

    // [dcl.fct]：方法的后置 cv 位于参数表后、override/final 前，如 `int get() const`。
    if (check(TokenType::KwConst)) {
        if (ownerClass.empty()) {
            // 非成员函数没有隐式 this，后置 const 无从作用（clang 同款文案）
            error("non-member function cannot have 'const' qualifier");
        }
        advance();
        decl->isConstMethod = true;
        logParse(
            "  [parse:fct] ★ '{}' 是 const 成员函数（后置 const，"
            "this 类型 T* → const T*，[dcl.fct]/7）\n", decl->name);
    }

    // override 关键字（在 { 或 ; 之前）
    if (match(TokenType::KwOverride)) {
        decl->isOverride = true;
    }

    //函数体或分号
    if (check(TokenType::LBrace)) {
        decl->body = std::static_pointer_cast<BlockStmt>(parseBlockStmt());
    } else {
        expect(TokenType::Semicolon, "Expected '{' or ';' after function declaration");
    }

    closeScope();
    return decl;
}

// parameter-list := [ type [IDENT] ("," type [IDENT])* ]。
std::vector<Parameter> Parser::parseParameterList() {
    std::vector<Parameter> params;

    if (check(TokenType::RParen)) return params; // 空参数列表

    int unnamedSeq = 0;   // 无名形参的合成名序号（每个参数列表独立编号）
    do {
        Parameter param;
        param.type = parseType();
        // [dcl.fct]：无名形参使用内部名字，避免与用户声明冲突；调用签名仍只依赖类型。
        if (check(TokenType::Comma) || check(TokenType::RParen)) {
            param.name = "$unnamed" + std::to_string(unnamedSeq++);
            logParse(
                "  [parse:param] (unnamed) ⇒ 内部合成名 '{}' ([dcl.fct]/3)\n", param.name);
        } else {
            const Token& nameToken = expect(TokenType::Identifier, "Expected parameter name");
            param.name = nameToken.text;
            //  形参名登记进【函数作用域】（值位）—— 形参在函数体里可见。
            // 无名形参（合成名 "$unnamedN"）不登记：那个名字用户写不出来，
            // 引用不到它，登记进去只会污染查找面。
            declareName(param.name, ScopeBit::Value);
        }
        params.push_back(param);
    } while (match(TokenType::Comma));

    return params;
}

// [dcl.dcl]：simple-declaration 可以没有声明符，如 `struct S;`、`S;`。
// 可作为类型的候选留给 Sema 检查，不能仅凭缺少变量名就拒绝。
static bool isDeclaratorlessDecl(const TypePtr& type) {
    TypePtr t = type;
    while (t && t->isConst()) t = t->innerType;   // const 属 decl-specifier，不是 ptr-operator
    return t && !t->isPointer() && !t->isReference();
}

// 语句按首 Token 分派；标识符开头时需消除声明与表达式歧义。
// 参考：clang/lib/Parse/ParseStmt.cpp:Parser::ParseStatement。
StmtPtr Parser::parseStatement() {
    if (check(TokenType::LBrace))    return parseBlockStmt();
    if (check(TokenType::KwIf))      return parseIfStmt();
    if (check(TokenType::KwWhile))   return parseWhileStmt();
    if (check(TokenType::KwFor))     return parseForStmt();
    if (check(TokenType::KwBreak))   return parseBreakStmt();
    if (check(TokenType::KwContinue)) return parseContinueStmt();
    if (check(TokenType::KwSwitch))  return parseSwitchStmt();
    if (check(TokenType::KwCase))    return parseCaseStmt();
    if (check(TokenType::KwDefault)) return parseDefaultStmt();
    if (check(TokenType::KwDo))      return parseDoWhileStmt();
    if (check(TokenType::KwReturn))  return parseReturnStmt();
    if (check(TokenType::KwDelete))  return parseDeleteStmt();

    // ── 空语句 `;`（[stmt]/1 的 null statement）──
    // `while (1) ;` / `;;` 里的裸分号。只消费掉，不产生任何动作。
    // 对照 clang：ParseStatement 的 tok::semi 分支 → ActOnNullStmt。
    if (check(TokenType::Semicolon)) {
        advance();
        return std::make_shared<EmptyStmt>();
    }

    // 变量声明：以类型关键字开头
    //  `const` 必须算在 isTypeKeyword() 里：漏了它，语句层的 `const int x;`
    // 走不到本分支、会被当表达式解析（历史上正是这个 bug）。
    if (current().isTypeKeyword()) {
        TypePtr type = parseType();
        // [dcl.dcl]：`int;`、`const int;` 可无声明符；`int*;` 仍要求声明符。
        // 具体判据见 isDeclaratorlessDecl。
        if (check(TokenType::Semicolon) && isDeclaratorlessDecl(type)) {
            advance();
            return std::make_shared<EmptyStmt>();
        }
        return parseVarDeclStmt(type);
    }

    // [stmt.ambig]：`S obj;` 与 `foo();`、`a < b > c;` 共享标识符前缀。
    // 先试探声明；失败时恢复游标，按表达式解析，不能仅凭 `<` 判为模板 id。
    if (check(TokenType::KwTypename) || check(TokenType::Identifier)) {
        TypePtr type;
        bool isDecl = false;
        std::string bareDeclName;   // 保留裸名声明的名字与位置，供提交后的日志和语义诊断使用。
        SourceLocation bareDeclLoc = peek().location;
        {
            TokenCheckpoint cp(*this);
            try {
                type = parseType();
                if (check(TokenType::Identifier)) {
                    isDecl = true;
                } else if (check(TokenType::Semicolon)) {
                    // 模板 id 无声明符候选需保留类型实参语法，包括限定者链上的模板 id。
                    bool hasTemplIdSyntax = false;
                    for (TypePtr t = type; t; t = t->nestedQualifier) {
                        if (!t->templateArgs.empty()) { hasTemplIdSyntax = true; break; }
                    }
                    isDecl = hasTemplIdSyntax && isDeclaratorlessDecl(type);
                    // 裸 `S;` 若不是值名，保留为无声明符的类型候选；可见性由 Sema 裁决。
                    // 未知名字也保留到 Sema，使诊断与 lookupVisibleType 使用同一作用域规则。
                    if (!isDecl && !hasTemplIdSyntax) {
                        const unsigned bits = nameBits(type->name);
                        if ((bits & ScopeBit::Value) == 0) {
                            isDecl = true;
                            bareDeclName = type->name;   // commit 后打日志
                        }
                    }
                }
                // 模板 id 的基名经活作用域表及 C++20 假定规则判断；不满足时回滚为比较链。
                if (isDecl) {
                    for (TypePtr t = type; t; t = t->nestedQualifier) {
                        if (!t->templateArgs.empty()
                            && !isTemplateNameInTemplateId(t->name)) {
                            isDecl = false;
                            break;
                        }
                    }
                }
            } catch (const ParseError&) {
                isDecl = false;   // 类型解析失败 = 这不是声明（预期路径）
            }
            if (isDecl) cp.commit();
        }   // 未 commit ⇒ 全状态回卷 + 试探日志整体废弃
        if (isDecl) {
            if (check(TokenType::Semicolon)) {   // 无声明符声明或裸名声明候选。
                if (!bareDeclName.empty())
                    logParse("  [parse:stmt] '{}' 判为声明（type 位）\n",
                             bareDeclName);
                advance();
                // 裸名声明保留 EmptyStmt::typeName，可见性统一由 Sema 裁决。
                auto empty = std::make_shared<EmptyStmt>(bareDeclName);
                empty->location = bareDeclLoc;
                return empty;
            }
            return parseVarDeclStmt(type);
        }
    }

    return parseExprOrAssignStmt();
}

// ─── 代码块 ──────────────────────────────────────────────────────────────────
// 文法：compound-stmt := '{' stmt* '}'
// 对应 clang：ParseCompoundStatement（Parser.cpp）
// 入参 demo：Token 流 [{][int][x][;][x][=][1][;][}]
//   → 产出 BlockStmt{ statements = [VarDeclStmt x, AssignStmt x=1] }
StmtPtr Parser::parseBlockStmt() {
    expect(TokenType::LBrace, "Expected '{'");
    //  进块作用域：块内声明的名字只在这一层可见。
    // 对照 clang：ParseCompoundStatement 走 ParseScope(Scope::DeclScope)。
    openScope(ScopeKind::Block, "", 0u);
    std::vector<StmtPtr> stmts;

    while (!check(TokenType::RBrace) && !isAtEnd()) {
        stmts.push_back(parseStatement());
    }

    expect(TokenType::RBrace, "Expected '}'");
    closeScope();
    return std::make_shared<BlockStmt>(std::move(stmts));
}

// var-decl-stmt := IDENT ["=" expr | "(" args ")"] ";"；类型已由调用方解析。
TypePtr Parser::parseArrayDeclaratorSuffix(TypePtr baseType) {
    while (check(TokenType::LBracket)) {
        advance();
        ExprPtr sizeExpr = parseExpression();
        expect(TokenType::RBracket, "Expected ']' after array size");
        baseType = Type::makeArray(baseType, sizeExpr);
    }
    return baseType;
}

StmtPtr Parser::parseVarDeclStmt(TypePtr type) {
    auto loc = current().location;
    const Token& nameToken = expect(TokenType::Identifier, "Expected variable name");
    //  局部变量名登记进【当前作用域】（值位）—— 块作用域的普通名字。
    declareName(nameToken.text, ScopeBit::Value);
    type = parseArrayDeclaratorSuffix(type);

    ExprPtr init = nullptr;
    std::vector<ExprPtr> ctorArgs;
    if (match(TokenType::Assign)) {
        init = parseExpression();
    }
    else if (check(TokenType::LParen)) {
        // ── 直接初始化 `Type name(args...);`（[dcl.init]/16）──
        // CTAD（[dcl.type.class.deduct]）**只在直接初始化时触发**，推导的输入就是这串
        // 实参 —— 故此文法分支是 CTAD 的前置条件。对照 clang：ParseDeclarator 的
        //   '(' 分支（ParseFunctionDeclarator / ParseParenDeclarator）。
        advance();  // 消费 '('
        if (!check(TokenType::RParen)) {
            do {
                ctorArgs.push_back(parseExpression());
            } while (match(TokenType::Comma));
        }
        expect(TokenType::RParen, "Expected ')' after constructor arguments");
    }

    expect(TokenType::Semicolon, "Expected ';' after variable declaration");

    auto stmt = std::make_shared<VarDeclStmt>(nameToken.text, type, init);
    stmt->ctorArgs = std::move(ctorArgs);
    // 读取并清除 parseType 留下的 constexpr 前缀，避免影响下一条声明。
    stmt->isConstexpr = m_pendingConstexpr;
    m_pendingConstexpr = false;
    stmt->location = loc;
    return stmt;
}

// [stmt.switch]：`switch (expr) { case value: ...; default: ...; }`。
// case/default 归属当前 switch，break 的合法性由语义分析检查。
StmtPtr Parser::parseSwitchStmt() {
    auto loc = current().location;
    expect(TokenType::KwSwitch, "Expected 'switch'");
    expect(TokenType::LParen, "Expected '(' after 'switch'");
    ExprPtr cond = parseExpression();
    expect(TokenType::RParen, "Expected ')' after switch condition");
    StmtPtr body = parseStatement();
    logParse("  [parse:stmt] switch (...)  <body>\n");
    auto stmt = std::make_shared<SwitchStmt>(std::move(cond), std::move(body));
    stmt->location = loc;
    return stmt;
}

// ─── case 标号 ────────────────────────────────────────────────────────────────
// 文法：case-label := 'case' 常量表达式 ':' stmt
// 标号后必须是语句；`case 1: }` 这种空标号用 EmptyStmt 补齐
// （`case 1:;` 的裸分号也一样 —— parseStatement 的 Semicolon 分支）。
StmtPtr Parser::parseCaseStmt() {
    auto loc = current().location;
    expect(TokenType::KwCase, "Expected 'case'");
    ExprPtr value = parseExpression();
    expect(TokenType::Colon, "Expected ':' after case value");
    StmtPtr body = check(TokenType::RBrace)
                       ? StmtPtr(std::make_shared<EmptyStmt>())
                       : parseStatement();
    auto stmt = std::make_shared<CaseStmt>(std::move(value), std::move(body));
    stmt->location = loc;
    return stmt;
}

// ─── default 标号 ─────────────────────────────────────────────────────────────
// 文法：default-label := 'default' ':' stmt
StmtPtr Parser::parseDefaultStmt() {
    auto loc = current().location;
    expect(TokenType::KwDefault, "Expected 'default'");
    expect(TokenType::Colon, "Expected ':' after 'default'");
    StmtPtr body = check(TokenType::RBrace)
                       ? StmtPtr(std::make_shared<EmptyStmt>())
                       : parseStatement();
    auto stmt = std::make_shared<DefaultStmt>(std::move(body));
    stmt->location = loc;
    return stmt;
}

// ─── do-while 循环（[stmt.do]）───────────────────────────────────────────────
// 文法：do-stmt := 'do' stmt 'while' '(' expr ')' ';'
//  体先于条件执行 —— codegen 把条件测试放在体代码【之后】；
// continue 落条件式（同 while，与 for 的增量步不同）。
// 对照 clang：ParseDoStatement。
StmtPtr Parser::parseDoWhileStmt() {
    auto loc = current().location;
    expect(TokenType::KwDo, "Expected 'do'");
    StmtPtr body = parseStatement();
    expect(TokenType::KwWhile, "Expected 'while' after do-while body");
    expect(TokenType::LParen, "Expected '(' after 'while'");
    ExprPtr cond = parseExpression();
    expect(TokenType::RParen, "Expected ')' after do-while condition");
    expect(TokenType::Semicolon, "Expected ';' after do-while");
    logParse("  [parse:stmt] do <body> while (...)\n");
    auto stmt = std::make_shared<DoWhileStmt>(std::move(body), std::move(cond));
    stmt->location = loc;
    return stmt;
}

// if-stmt := "if" "(" expr ")" stmt ["else" stmt]；else 归最近未配对的 if。
StmtPtr Parser::parseIfStmt() {
    auto loc = current().location;
    expect(TokenType::KwIf, "Expected 'if'");
    expect(TokenType::LParen, "Expected '(' after 'if'");

    ExprPtr cond = parseExpression();

    expect(TokenType::RParen, "Expected ')' after condition");

    StmtPtr thenBranch = parseStatement();
    StmtPtr elseBranch = nullptr;

    if (match(TokenType::KwElse)) {
        elseBranch = parseStatement();
    }

    auto stmt = std::make_shared<IfStmt>(cond, thenBranch, elseBranch);
    stmt->location = loc;
    return stmt;
}

// ─── while 语句 ───────────────────────────────────────────────────────────────
// 文法：while-stmt := 'while' '(' expr ')' stmt
// 对应 clang：ParseWhileStatement（ParseStmt.cpp）
// 入参 demo：Token 流 [while][(][i][<][n][)][i][=][i][+][1][;]
//   → 产出 WhileStmt{ cond=(i<n), body=AssignStmt(i = i+1) }
StmtPtr Parser::parseWhileStmt() {
    auto loc = current().location;
    expect(TokenType::KwWhile, "Expected 'while'");
    expect(TokenType::LParen, "Expected '(' after 'while'");

    ExprPtr cond = parseExpression();

    expect(TokenType::RParen, "Expected ')' after condition");

    StmtPtr body = parseStatement();

    auto stmt = std::make_shared<WhileStmt>(cond, body);
    stmt->location = loc;
    return stmt;
}

// for-stmt := "for" "(" init ";" [cond] ";" [incr] ")" stmt。
// 省略条件表示持续循环，初始化声明的作用域覆盖条件、增量与循环体。
StmtPtr Parser::parseForStmt() {
    auto loc = current().location;
    expect(TokenType::KwFor, "Expected 'for'");
    expect(TokenType::LParen, "Expected '(' after 'for'");
    //  for 的【隐式作用域】（[stmt.for]/2：init 的声明在条件与循环体里可见，
    // 但出了 for 语句就看不见）—— 用块作用域表达。
    // 对照 clang：ParseForStatement 里 ParseScope(Scope::DeclScope)。
    openScope(ScopeKind::Block, "", 0u);

    // init：空（;) / 声明式（类型开头，自带结尾分号）/ 表达式式或赋值式
    // （赋值在本语言是语句，for 头局部放宽 —— 与 incr 同款的处置）
    StmtPtr init;
    if (check(TokenType::Semicolon)) {
        advance();
        init = std::make_shared<EmptyStmt>();
    } else if (current().isTypeKeyword()) {
        TypePtr type = parseType();
        init = parseVarDeclStmt(type);           // 吃掉自己的 ';'
    } else {
        ExprPtr target = parseExpression();
        if (check(TokenType::Assign)) {
            auto aloc = current().location;
            advance();
            auto asgn = std::make_shared<AssignStmt>(target, parseExpression());
            asgn->location = aloc;
            init = asgn;
        } else {
            init = std::make_shared<ExprStmt>(target);
        }
        expect(TokenType::Semicolon, "Expected ';' after for-init");
    }

    // cond：可空（for(;;)）
    ExprPtr cond;
    if (!check(TokenType::Semicolon)) {
        cond = parseExpression();
    }
    expect(TokenType::Semicolon, "Expected ';' after for condition");

    // incr：可空。本语言赋值是语句，但 for 头第三格允许无分号的赋值
    // （`i = i + 1`）—— 故这里产出【语句】（AssignStmt/ExprStmt），与 init 同构。
    StmtPtr incr = std::make_shared<EmptyStmt>();
    if (!check(TokenType::RParen)) {
        ExprPtr target = parseExpression();
        if (check(TokenType::Assign)) {
            auto loc = current().location;
            advance();
            ExprPtr value = parseExpression();
            auto asgn = std::make_shared<AssignStmt>(target, value);
            asgn->location = loc;
            incr = asgn;
        } else {
            incr = std::make_shared<ExprStmt>(target);
        }
    }
    expect(TokenType::RParen, "Expected ')' after for clauses");

    StmtPtr body = parseStatement();

    closeScope();   //  出 for 的隐式作用域
    auto stmt = std::make_shared<ForStmt>(init, cond, incr, body);
    stmt->location = loc;
    return stmt;
}

// ─── break / continue 语句 ────────────────────────────────────────────────────
// 文法：break-stmt := 'break' ';'    continue-stmt := 'continue' ';'
// 对照 clang：ParseBreakStatement / ParseContinueStatement。
// 合法性（在不在循环里）不在语法层判 —— 交 Sema 的循环上下文栈（像 clang 的
// ContinueJumpChecker / BreakJumpChecker 在 SemaAct 阶段报错）。
StmtPtr Parser::parseBreakStmt() {
    auto loc = current().location;
    expect(TokenType::KwBreak, "Expected 'break'");
    expect(TokenType::Semicolon, "Expected ';' after break");
    auto stmt = std::make_shared<BreakStmt>();
    stmt->location = loc;
    return stmt;
}

StmtPtr Parser::parseContinueStmt() {
    auto loc = current().location;
    expect(TokenType::KwContinue, "Expected 'continue'");
    expect(TokenType::Semicolon, "Expected ';' after continue");
    auto stmt = std::make_shared<ContinueStmt>();
    stmt->location = loc;
    return stmt;
}

// ─── return 语句 ──────────────────────────────────────────────────────────────
// 文法：return-stmt := 'return' [expr] ';'
//   返回值可选：前瞻到 ';' 即为裸 return（void 函数）。
// 对应 clang：ParseReturnStatement（ParseStmt.cpp）
// 入参 demo：Token 流 [return][a][+][b][;] → ReturnStmt{ value = BinaryExpr(a+b) }
//           Token 流 [return][;]           → ReturnStmt{ value = nullptr }
StmtPtr Parser::parseReturnStmt() {
    auto loc = current().location;
    expect(TokenType::KwReturn, "Expected 'return'");

    ExprPtr value = nullptr;
    if (!check(TokenType::Semicolon)) {
        value = parseExpression();
    }

    expect(TokenType::Semicolon, "Expected ';' after return");

    auto stmt = std::make_shared<ReturnStmt>(value);
    stmt->location = loc;
    return stmt;
}

// ─── delete 语句 ──────────────────────────────────────────────────────────────
StmtPtr Parser::parseDeleteStmt() {
    auto loc = current().location;
    expect(TokenType::KwDelete, "Expected 'delete'");
    bool isArray = false;
    if (match(TokenType::LBracket)) {
        expect(TokenType::RBracket, "Expected ']' after '[' in delete[]");
        isArray = true;
    }
    ExprPtr expr = parseExpression();
    expect(TokenType::Semicolon, "Expected ';' after delete statement");
    auto stmt = std::make_shared<DeleteStmt>(expr, isArray);
    stmt->location = loc;
    return stmt;
}

// expr-or-assign := expr ["=" expr] ";"；赋值目标的合法性留给 Sema。
StmtPtr Parser::parseExprOrAssignStmt() {
    ExprPtr expr = parseExpression();

    // 赋值语句
    if (match(TokenType::Assign)) {
        ExprPtr value = parseExpression();
        expect(TokenType::Semicolon, "Expected ';' after assignment");
        auto stmt = std::make_shared<AssignStmt>(expr, value);
        stmt->location = expr->location;
        return stmt;
    }

    expect(TokenType::Semicolon, "Expected ';' after expression");
    auto stmt = std::make_shared<ExprStmt>(expr);
    stmt->location = expr->location;
    return stmt;
}

// 表达式按优先级分层递归下降，同层二元运算按结合方向建树。
// `1 + 2 * 3` → Add(1, Mul(2, 3))；`a - b - c` → Sub(Sub(a, b), c)。
// 调用参数不消费分隔逗号，括号表达式仍可包含逗号运算。
ExprPtr Parser::parseExpression() {
    ExprPtr cond = parseOrExpr();
    if (!check(TokenType::Question)) return cond;

    auto loc = current().location;
    advance();                                     // 吃掉 '?'
    ExprPtr thenExpr = parseExpression();          // 中支是全表达式（可再含 ?:）
    expect(TokenType::Colon, "Expected ':' in conditional expression");
    ExprPtr elseExpr = parseExpression();          // 右结合挂在这一支

    auto expr = std::make_shared<ConditionalExpr>(cond, thenExpr, elseExpr);
    expr->location = loc;
    return expr;
}

// ─── || ──────────────────────────────────────────────────────────────────────
// 文法：or-expr := and-expr ('||' and-expr)*      最低优先级，左结合
// 入参 demo：Token 流 [a][||][b][&&][c]
//   left=Var(a)；见 '||'，right = parseAndExpr → And(b,c)
//   → 产出 Or(a, And(b,c))   ← && 比 || 绑定更紧，成为其右孩子
ExprPtr Parser::parseOrExpr() {
    ExprPtr left = parseAndExpr();

    while (check(TokenType::PipePipe)) {
        auto loc = current().location;
        advance();
        ExprPtr right = parseAndExpr();
        auto expr = std::make_shared<BinaryExpr>(BinaryOp::Or, left, right);
        expr->location = loc;
        left = expr;
    }

    return left;
}

// ─── && ──────────────────────────────────────────────────────────────────────
// 文法：and-expr := equality ('&&' equality)*     左结合
// 入参 demo：Token 流 [x][&&][y] → And(Var(x), Var(y))
// 注：此处只做结构解析；短路求值是语义/代码生成阶段的事。
ExprPtr Parser::parseAndExpr() {
    ExprPtr left = parseEqualityExpr();

    while (check(TokenType::AmpAmp)) {
        auto loc = current().location;
        advance();
        ExprPtr right = parseEqualityExpr();
        auto expr = std::make_shared<BinaryExpr>(BinaryOp::And, left, right);
        expr->location = loc;
        left = expr;
    }

    return left;
}

// ─── == != ───────────────────────────────────────────────────────────────────
// 文法：equality := comparison (('==' | '!=') comparison)*   左结合
// 入参 demo：Token 流 [a][==][b] → BinaryExpr(Eq, Var(a), Var(b))
// 决策：前瞻 Token 在 EqualEqual / BangEqual 二者中映射到 Eq / Neq。
ExprPtr Parser::parseEqualityExpr() {
    ExprPtr left = parseComparisonExpr();

    while (check(TokenType::EqualEqual) || check(TokenType::BangEqual)) {
        auto loc = current().location;
        BinaryOp op = (current().type == TokenType::EqualEqual)
                      ? BinaryOp::Eq : BinaryOp::Neq;
        advance();
        ExprPtr right = parseComparisonExpr();
        auto expr = std::make_shared<BinaryExpr>(op, left, right);
        expr->location = loc;
        left = expr;
    }

    return left;
}

// ─── < > <= >= ──────────────────────────────────────────────────────────────
// 文法：comparison := additive (('<'|'>'|'<='|'>=') additive)*   左结合
// 入参 demo：Token 流 [i][<][n] → BinaryExpr(Lt, Var(i), Var(n))
// 注意：比较运算符 '<' 与模板实参列表的 '<' 是同一个 Token —— 模板 id 的
//       歧义消解发生在 parsePrimaryExpr（标识符后试探 '<' 类型列表 '('），
//       本层拿到的 '<' 一律按比较运算符处理。
ExprPtr Parser::parseComparisonExpr() {
    ExprPtr left = parseAdditiveExpr();

    // m_stopAtGreater：模板实参表内裸 '>' 是收尾不是大于号（见 parser.h 注释）。
    // '>=' 不受此限 —— 收尾永远是裸 '>'（<= 亦是真比较，照常吃）。
    while (check(TokenType::Less) || check(TokenType::Greater)
           || check(TokenType::LessEqual) || check(TokenType::GreaterEqual)) {
        if (m_stopAtGreater && check(TokenType::Greater)) break;
        auto loc = current().location;
        BinaryOp op;
        switch (current().type) {
            case TokenType::Less:         op = BinaryOp::Lt;  break;
            case TokenType::Greater:      op = BinaryOp::Gt;  break;
            case TokenType::LessEqual:    op = BinaryOp::Le;  break;
            case TokenType::GreaterEqual: op = BinaryOp::Ge;  break;
            default:                      op = BinaryOp::Lt;  break;
        }
        advance();
        ExprPtr right = parseAdditiveExpr();
        auto expr = std::make_shared<BinaryExpr>(op, left, right);
        expr->location = loc;
        left = expr;
    }

    return left;
}

// ─── + - ─────────────────────────────────────────────────────────────────────
// 文法：additive := multiplicative (('+' | '-') multiplicative)*   左结合
// 入参 demo：Token 流 [a][-][b][-][c]
//   循环向左折叠：先 Sub(a,b)，再 Sub(Sub(a,b), c) → 左结合 ✓
ExprPtr Parser::parseAdditiveExpr() {
    ExprPtr left = parseMultiplicativeExpr();

    while (check(TokenType::Plus) || check(TokenType::Minus)) {
        auto loc = current().location;
        BinaryOp op = (current().type == TokenType::Plus)
                      ? BinaryOp::Add : BinaryOp::Sub;
        advance();
        ExprPtr right = parseMultiplicativeExpr();
        auto expr = std::make_shared<BinaryExpr>(op, left, right);
        expr->location = loc;
        left = expr;
    }

    return left;
}

// ─── * / % ───────────────────────────────────────────────────────────────────
// 文法：multiplicative := unary (('*' | '/' | '%') unary)*   左结合
// 入参 demo：Token 流 [2][*][3][%][4] → Mod(Mul(2,3), 4)
// 注意：'*' 在类型上下文（parseType）里是指针后缀，在表达式上下文里是乘法——
//       同一 Token 的含义由调用它的文法位置决定（上下文相关）。
ExprPtr Parser::parseMultiplicativeExpr() {
    ExprPtr left = parseUnaryExpr();

    while (check(TokenType::Star) || check(TokenType::Slash)
           || check(TokenType::Percent)) {
        auto loc = current().location;
        BinaryOp op;
        switch (current().type) {
            case TokenType::Star:    op = BinaryOp::Mul; break;
            case TokenType::Slash:   op = BinaryOp::Div; break;
            case TokenType::Percent: op = BinaryOp::Mod; break;
            default:                 op = BinaryOp::Mul; break;
        }
        advance();
        ExprPtr right = parseUnaryExpr();
        auto expr = std::make_shared<BinaryExpr>(op, left, right);
        expr->location = loc;
        left = expr;
    }

    return left;
}

// unary := ("-" | "!" | "&" | "*") unary | postfix。
ExprPtr Parser::parseUnaryExpr() {
    if (check(TokenType::Minus) || check(TokenType::Bang)
        || check(TokenType::Ampersand) || check(TokenType::Star)) {
        auto loc = current().location;
        // 一元位置的 `&x` 是取地址；类型中的 `T&` 是引用，二元 `a & b` 是位与。
        UnaryOp op = (current().type == TokenType::Minus)     ? UnaryOp::Neg
                   : (current().type == TokenType::Bang)      ? UnaryOp::Not
                   : (current().type == TokenType::Ampersand) ? UnaryOp::Addr
                                                              : UnaryOp::Deref;
        advance();
        ExprPtr operand = parseUnaryExpr();
        auto expr = std::make_shared<UnaryExpr>(op, operand);
        expr->location = loc;
        return expr;
    }

    return parsePostfixExpr();
}

// postfix := primary (call-args | ("." | "->") IDENT | "[" expr "]")*。
ExprPtr Parser::parsePostfixExpr() {
    ExprPtr expr = parsePrimaryExpr();

    while (true) {
        if (check(TokenType::LParen)) {
            // 函数调用
            auto loc = current().location;
            auto args = parseArgumentList();
            auto call = std::make_shared<CallExpr>(expr, std::move(args));
            call->location = loc;
            expr = call;
        }
        else if (check(TokenType::Dot) || check(TokenType::Arrow)) {
            // 成员访问
            // 文法：('.' | '->') ( nested-name-specifier )? IDENT
            //   ☆ 限定名形式 `obj.Q::m`（[expr.ref]/[expr.prim.id.qual]）：写限定名的
            //     唯一目的是**绕过名字隐藏**（派生类声明了同名成员时基类那一族被隐藏）。
            //     与 Parser 对 `A::B` 的既有处置一致 —— 拼成一整个名字串交给语义层
            //     （见 parsePrimaryExpr 里 `while (::) name += "::" + ident`）。
            auto loc = current().location;
            bool isArrow = (current().type == TokenType::Arrow);
            advance();
            const Token& member = expect(TokenType::Identifier, "Expected member name");
            std::string memberName = member.text;
            while (check(TokenType::ColonColon)) {
                advance();
                const Token& next = expect(TokenType::Identifier,
                                           "Expected member name after '::'");
                memberName += "::" + next.text;
            }
            auto memExpr = std::make_shared<MemberExpr>(expr, memberName, isArrow);
            memExpr->location = loc;
            expr = memExpr;
        }
        else if (check(TokenType::LBracket)) {
            // [expr.sub]：保存 `v[i]` 的对象和下标；原生数组寻址或容器 at/set 由 Sema 区分。
            auto loc = current().location;
            advance();  // consume '['
            ExprPtr indexExpr = parseExpression();
            expect(TokenType::RBracket, "Expected ']' after subscript");
            auto idxExpr = std::make_shared<IndexExpr>(expr, indexExpr);
            idxExpr->location = loc;
            expr = idxExpr;
        }
        else {
            break;
        }
    }

    return expr;
}

// 基本表达式包括字面量、名字、new、this、dynamic_cast 与括号表达式。
// `(a + b)` 保留括号结构供 decltype 判断，模板调用实参由模板 id 分支处理。
ExprPtr Parser::parsePrimaryExpr() {
    auto loc = current().location;

    // 整数字面量
    if (check(TokenType::IntLiteral)) {
        const Token& tok = advance();
        // 形态（后缀/进制决定的类型）随节点带走 —— `int x = 4L;` 里 4L 是 long
        // 而不是 int，后续 typeCompatible / 推导都按 long 走。
        IntLiteralValue lv = parseIntLiteral(tok.text, tok.location);
        auto expr = std::make_shared<IntLiteralExpr>(lv.value, lv.type);
        expr->location = loc;
        return expr;
    }

    // 字符字面量 'a'：词法已把转义翻译好，这里只把"字符"折算成"码点值"。
    //  单字符 ⇒ 该字节的值；多字符字面量（'ab'，Implementation-defined）按
    // "最后一个字符在最低字节"的常见实现折叠 —— 与 GCC/Clang 一致。
    if (check(TokenType::CharLiteral)) {
        const Token& tok = advance();
        int64_t val = 0;
        for (char ch : tok.text) val = (val << 8) | static_cast<unsigned char>(ch);
        auto expr = std::make_shared<CharLiteralExpr>(val);
        expr->location = loc;
        logParse("  [parse:literal] '{}' ⇒ 字符码点 {}\n",
                                 escapeCharText(tok.text), val);
        return expr;
    }

    // 布尔字面量
    if (check(TokenType::KwTrue)) {
        advance();
        auto expr = std::make_shared<BoolLiteralExpr>(true);
        expr->location = loc;
        return expr;
    }
    if (check(TokenType::KwFalse)) {
        advance();
        auto expr = std::make_shared<BoolLiteralExpr>(false);
        expr->location = loc;
        return expr;
    }

    // 字符串字面量
    if (check(TokenType::StringLiteral)) {
        std::string val = advance().text;
        auto expr = std::make_shared<StringLiteralExpr>(val);
        expr->location = loc;
        return expr;
    }

    // nullptr
    if (check(TokenType::KwNullptr)) {
        advance();
        auto expr = std::make_shared<NullptrLiteralExpr>();
        expr->location = loc;
        return expr;
    }

    // this
    if (check(TokenType::KwThis)) {
        advance();
        auto expr = std::make_shared<ThisExpr>();
        expr->location = loc;
        return expr;
    }

    // dynamic_cast<T*>(expr) 表达式（[expr.dynamic.cast]）
    // 文法：'dynamic_cast' '<' 类名 '*' '>' '(' expr ')'
    // 简化点：只支持"类名*"目标类型（真 C++ 还允许引用形式与静态偏移转型）。
    if (check(TokenType::KwDynamicCast)) {
        advance(); // 消费 dynamic_cast
        expect(TokenType::Less, "Expected '<' after 'dynamic_cast'");
        const Token& clsTok = expect(TokenType::Identifier,
            "Expected class name in dynamic_cast<...>");
        expect(TokenType::Star, "dynamic_cast target must be a pointer type (T*)");
        expect(TokenType::Greater, "Expected '>' after dynamic_cast<...>");
        expect(TokenType::LParen, "Expected '(' after dynamic_cast<T*>");
        ExprPtr operand = parseExpression();
        expect(TokenType::RParen, "Expected ')' to close dynamic_cast");
        auto expr = std::make_shared<DynamicCastExpr>(clsTok.text, std::move(operand));
        expr->location = loc;
        logParse("  [parse] dynamic_cast<{}*>(operand)\n", clsTok.text);
        return expr;
    }

    // new 表达式
    if (check(TokenType::KwNew) || (check(TokenType::Identifier) && current().text == "new")) {
        advance(); // 消费 new
//  限定名（[dcl.type.elab]/2：new 后面跟的是 type-id，type-id 可以是限定名）：
// `new A::B(6)` —— 嵌套类只能这样 new（裸 `new B(6)` 指全局 B）。
// 收集成 "A::B" 交给语义期按登记名查（嵌套类的登记名就是限定名）。
        std::string classNameText =
            expect(TokenType::Identifier, "Expected class name after 'new'").text;
        while (check(TokenType::ColonColon)) {
            advance();
            classNameText += "::"
                + expect(TokenType::Identifier, "Expected identifier after '::'").text;
        }
        auto expr = std::make_shared<NewExpr>(classNameText);
        expr->location = loc;

        // P3 —— 可选模板实参表：new Box<int>() / new Buf<4>()。
        // 与 parseType 的模板 id 分支同构（复用 parseTemplateArgumentList），
        // 实参暂挂在节点上，语义阶段实例化后改写 className 为实例名。
        if (check(TokenType::Less)) {
            expr->templateArgs = parseTemplateArgumentList();
            logParse(
                "  [parse:new] ★ template-id: new {}<{} argument(s)>\n",
                expr->className, expr->templateArgs.size());
        }

        // 可选的构造参数
        if (match(TokenType::LParen)) {
            if (!check(TokenType::RParen)) {
                do {
                    expr->constructorArgs.push_back(parseExpression());
                } while (match(TokenType::Comma));
            }
            expect(TokenType::RParen, "Expected ')' after constructor arguments");
        }

        return expr;
    }

    // 变量引用（或 template-id：foo<int>(...) —— S3 显式模板实参）
    if (check(TokenType::Identifier)) {
        std::string name = advance().text;
        while (check(TokenType::ColonColon)) {
            advance();
            name += "::" + expect(TokenType::Identifier, "Expected identifier after '::'").text;
        }
        auto expr = std::make_shared<VarExpr>(name);
        expr->location = loc;

        // 显式模板实参 template-id（[temp.arg.explicit]）。
        // 当前支持：foo<int>(x)（函数模板调用）、Cls<int>::value（类型限定成员）。
        // 仅当实参表后紧跟 '(' 或 '::' 才认作 template-id；否则 '<' 按比较运算解析。
        if (check(TokenType::Less) && isTemplateNameHere(name)) {
            std::vector<TemplateArg> explicitArgs;
            bool isTemplateId = false;
            {
                TokenCheckpoint cp(*this);
                try {
                    // parseTemplateArgumentList 自消费 '<' 与 '>'；
                    // 其中 parseType 对非类型实参抛异常（如 a<b 里的 b 不是类型），
                    // 异常即"这不是 template-id"的信号。
                    explicitArgs = parseTemplateArgumentList();
                    isTemplateId = check(TokenType::LParen) || check(TokenType::ColonColon); // class a= Box::<int>; 这种非法
                } catch (const ParseError&) {
                    isTemplateId = false;   // 语法错误 = 这不是 template-id（预期路径）
                }
                if (isTemplateId) cp.commit();
            }   // 未 commit ⇒ 回卷 + 试探日志整体废弃
            if (isTemplateId) {
                expr->explicitTemplateArgs = std::move(explicitArgs);
                logParse("  [parse] template-id: {}<{} explicit arg(s)>\n",
                    name, expr->explicitTemplateArgs.size());

                // 限定链：Box<int>::value、Box<int>::Inner::value、Box<int>::make()。
                ExprPtr qualified = expr;
                while (check(TokenType::ColonColon)) {
                    advance();
                    const Token& memTok = expect(TokenType::Identifier,
                        "Expected member name after '::'");
                    auto me = std::make_shared<MemberExpr>(qualified, memTok.text, /*arrow=*/false);
                    me->isTypeAccess = true;
                    me->location = loc;
                    logParse(
                        "  [parse] 类型限定访问: <template-id>::{} (static member)\n",
                        memTok.text);
                    qualified = me;
                }
                return qualified;
            }
            // 不是 template-id：checkpoint 已回卷，'< 交给比较表达式解析
        }

        return expr;
    }

    // 括号表达式
    if (match(TokenType::LParen)) {
        // 括号内是独立的表达式世界：模板实参表的 '>' 停止规则不带进来
        // （`Buf<(2>1)>` 里括号内的是真比较）。save/restore 保嵌套安全。
        bool savedStop = m_stopAtGreater;
        m_stopAtGreater = false;
        ExprPtr inner = parseExpression();
        // ── 逗号表达式（[expr.comma]，优先级全语言最低）──
        // 只在括号内开放：调用实参表 / 初始化列表里的 ',' 仍是分隔符，
        // 若在顶层开放它们全得做上下文区分（已知边界，docs/learn/41）。
        while (match(TokenType::Comma)) {
            ExprPtr right = parseExpression();
            auto comma = std::make_shared<CommaExpr>(std::move(inner), std::move(right));
            comma->location = loc;
            inner = std::move(comma);
        }
        m_stopAtGreater = savedStop;
        expect(TokenType::RParen, "Expected ')' after expression");
        return inner;
    }

    error(std::format("Unexpected token '{}' in expression", current().text));
}

// call-args := "(" [expr ("," expr)*] ")"；分隔逗号不属于单个参数表达式。
std::vector<ExprPtr> Parser::parseArgumentList() {
    expect(TokenType::LParen, "Expected '('");
    std::vector<ExprPtr> args;

    if (!check(TokenType::RParen)) {
        do {
            args.push_back(parseExpression());
        } while (match(TokenType::Comma));
    }

    expect(TokenType::RParen, "Expected ')'");
    return args;
}

// ─────────────────────────────────────────────────────────────────────────────
// 辅助：Token 类型转二元运算符
// ─────────────────────────────────────────────────────────────────────────────
// 纯查表映射（TokenType → BinaryOp），无副作用。各层优先级函数目前内联了
// 自己的 switch，此表作为集中式对照备用；default 分支兜底返回 Add 仅为
// 满足 return 语义（调用方保证只传入运算符 Token）。
BinaryOp Parser::tokenToBinaryOp(TokenType t) const {
    switch (t) {
        case TokenType::Plus:         return BinaryOp::Add;
        case TokenType::Minus:        return BinaryOp::Sub;
        case TokenType::Star:         return BinaryOp::Mul;
        case TokenType::Slash:        return BinaryOp::Div;
        case TokenType::Percent:      return BinaryOp::Mod;
        case TokenType::EqualEqual:   return BinaryOp::Eq;
        case TokenType::BangEqual:    return BinaryOp::Neq;
        case TokenType::Less:         return BinaryOp::Lt;
        case TokenType::Greater:      return BinaryOp::Gt;
        case TokenType::LessEqual:    return BinaryOp::Le;
        case TokenType::GreaterEqual: return BinaryOp::Ge;
        case TokenType::AmpAmp:       return BinaryOp::And;
        case TokenType::PipePipe:     return BinaryOp::Or;
        default:                      return BinaryOp::Add;
    }
}

} // namespace minicc
