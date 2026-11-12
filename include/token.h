#pragma once
// Lexer 与 Parser 共用的 Token 格式。
// 参考 clang/Basic/TokenKinds.def、Basic/Token.h；标准 [lex.token]。
// 例：int x = 42; → KwInt、Identifier、Assign、IntLiteral、Semicolon、Eof。

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <format>

namespace minicc {

// 文法终结符；uint8_t 可容纳当前种类。
enum class TokenType : uint8_t {
    // ── 文件结束 ──
    Eof,

    // ── 字面量 ──
    IntLiteral,       // 42 / 0x2A / 4L / 4u
    CharLiteral,      // 'a'   —— text 存【翻译后】的字符（同 StringLiteral 的做法）
    StringLiteral,    // "hello"

    // ── 标识符 ──
    Identifier,       // myVar, foo_bar

    // ── 关键字 ──
    KwAuto,           // auto
    KwBool,           // bool
    KwBreak,          // break      —— 跳出最近一层循环（主线 C）
    KwChar,           // char        —— 整型家族（[basic.fundamental]/2）
    KwClass,          // class
    KwConst,          // const
    KwConstexpr,      // constexpr  —— 编译期常量（主线 D 二批：[dcl.constexpr]）
    KwContinue,       // continue   —— 跳转到最近一层循环的增量步（主线 C）
    KwSwitch,         // switch     —— 多路分支（主线 C 二批：[stmt.switch]）
    KwCase,           // case
    KwDefault,        // default
    KwDo,             // do         —— do-while 循环（体先于条件执行）
    KwDecltype,       // decltype
    KwDelete,         // delete
    KwDouble,         // double
    KwDynamicCast,    // dynamic_cast
    KwElse,           // else
    KwEnum,           // enum
    KwFalse,          // false
    KwFor,            // for
    KwIf,             // if
    KwInt,            // int
    KwLong,           // long        —— long / long long
    KwNamespace,      // namespace
    KwNew,            // new
    KwNullptr,        // nullptr
    KwOverride,       // override
    KwPublic,         // public
    KwPrivate,        // private
    KwProtected,      // protected
    KwReturn,         // return
    KwShort,          // short
    KwSigned,         // signed
    KwStatic,         // static —— 类内 static 成员函数（[class.static]/2）
    KwStruct,         // struct
    KwTemplate,       // template
    KwThis,           // this
    KwTrue,           // true
    KwTypedef,        // typedef
    KwTypename,       // typename
    KwUnsigned,       // unsigned
    KwUsing,          // using
    KwVirtual,        // virtual
    KwVoid,           // void
    KwWhile,          // while

    // ── 运算符 ──
    Plus,             // +
    Minus,            // -
    Star,             // *
    Slash,            // /
    Percent,          // %
    Ampersand,        // &
    Pipe,             // |
    Caret,            // ^
    Tilde,            // ~
    Bang,             // !
    Assign,           // =
    Less,             // <
    Greater,          // >
    Dot,              // .
    Comma,            // ,
    Colon,            // :
    Question,         // ?   —— 三元运算符 a ? b : c 的前半（主线 C）
    Semicolon,        // ;
    Arrow,            // ->
    ColonColon,       // ::
    EqualEqual,       // ==
    BangEqual,        // !=
    LessEqual,        // <=
    GreaterEqual,     // >=
    AmpAmp,           // &&
    PipePipe,         // ||
    PlusAssign,       // +=
    MinusAssign,      // -=
    StarAssign,       // *=
    SlashAssign,      // /=

    // ── 分隔符 ──
    LParen,           // (
    RParen,           // )
    LBrace,           // {
    RBrace,           // }
    LBracket,         // [
    RBracket,         // ]
};

// 完整标识符扫描后查表，避免将关键字前缀误认成关键字。
// 例：template → KwTemplate；templata → Identifier。
inline const std::unordered_map<std::string_view, TokenType> kKeywordMap = {
    {"auto",      TokenType::KwAuto},
    {"bool",      TokenType::KwBool},
    {"break",     TokenType::KwBreak},
    {"char",      TokenType::KwChar},
    {"class",     TokenType::KwClass},
    {"const",     TokenType::KwConst},
    {"constexpr", TokenType::KwConstexpr},
    {"continue",  TokenType::KwContinue},
    {"switch",    TokenType::KwSwitch},
    {"case",      TokenType::KwCase},
    {"default",   TokenType::KwDefault},
    {"do",        TokenType::KwDo},
    {"decltype",  TokenType::KwDecltype},
    {"delete",    TokenType::KwDelete},
    {"double",    TokenType::KwDouble},
    {"dynamic_cast", TokenType::KwDynamicCast},
    {"else",      TokenType::KwElse},
    {"enum",      TokenType::KwEnum},
    {"false",     TokenType::KwFalse},
    {"for",       TokenType::KwFor},
    {"if",        TokenType::KwIf},
    {"int",       TokenType::KwInt},
    {"long",      TokenType::KwLong},
    {"namespace", TokenType::KwNamespace},
    {"new",       TokenType::KwNew},
    {"nullptr",   TokenType::KwNullptr},
    {"override",  TokenType::KwOverride},
    {"public",    TokenType::KwPublic},
    {"private",   TokenType::KwPrivate},
    {"protected", TokenType::KwProtected},
    {"return",    TokenType::KwReturn},
    {"short",     TokenType::KwShort},
    {"signed",    TokenType::KwSigned},
    {"static",    TokenType::KwStatic},
    {"struct",    TokenType::KwStruct},
    {"template",  TokenType::KwTemplate},
    {"this",      TokenType::KwThis},
    {"true",      TokenType::KwTrue},
    {"typedef",   TokenType::KwTypedef},
    {"typename",  TokenType::KwTypename},
    {"unsigned",  TokenType::KwUnsigned},
    {"using",     TokenType::KwUsing},
    {"virtual",   TokenType::KwVirtual},
    {"void",      TokenType::KwVoid},
    {"while",     TokenType::KwWhile},
};

// 源码行列从 1 开始，随 Token 保留供后续诊断使用。
// 例：首行 int x 中 x 位于 {1,5}；{12,5} 格式化为 "12:5"。
struct SourceLocation {
    uint32_t line   = 1;
    uint32_t column = 1;

    // 格式化为 "行:列"，直接拼进诊断消息。demo: {12,5}.toString() ⇒ "12:5"
    std::string toString() const {
        return std::format("{}:{}", line, column);
    }
};

// Token 保存类别、文本和位置；字符串按值持有，避免依赖源码寿命。
// 数字仍保留文本，由 Parser 解释数值和后缀；字符、字符串已翻译转义。
struct Token {
    TokenType     type     = TokenType::Eof;
    std::string   text;            // 原始文本（如标识符名、数字文本）
    SourceLocation location;

    bool is(TokenType t) const { return type == t; }
    bool isNot(TokenType t) const { return type != t; }

    // 可作为类型说明符序列开头的关键字，不包含用户定义类型名。
    // 例：const int x、unsigned x、decltype(x) y；class 由类声明分支处理。
    bool isTypeKeyword() const {
        return type == TokenType::KwInt
            || type == TokenType::KwChar
            || type == TokenType::KwShort
            || type == TokenType::KwLong
            || type == TokenType::KwSigned
            || type == TokenType::KwUnsigned
            || type == TokenType::KwDouble
            || type == TokenType::KwBool
            || type == TokenType::KwVoid
            || type == TokenType::KwAuto
            || type == TokenType::KwConst      // const int x; —— [dcl.type] 类型说明符
            || type == TokenType::KwConstexpr  // constexpr int k —— 同为说明符序列开头 [dcl.type]
            || type == TokenType::KwDecltype;   // decltype(e) 也是类型说明符 [dcl.type.decltype]
    }

    // 判断是否为访问修饰符（Parser 解析类体中 "public/private/protected :" 成员区段时用）
    // demo: Token{KwPublic}.isAccessSpecifier() ⇒ true；Token{KwClass} ⇒ false
    bool isAccessSpecifier() const {
        return type == TokenType::KwPublic
            || type == TokenType::KwPrivate
            || type == TokenType::KwProtected;
    }

    // 调试格式：[种类枚举值] '文本' @ 行:列；枚举值不属于稳定接口。
    std::string toString() const {
        return std::format("[{}] '{}' @ {}",
            static_cast<int>(type), text, location.toString());
    }
};

// 供诊断使用的名称，映射见 src/lex/lexer.cpp: tokenTypeName。
// 例：KwInt → "int"；Arrow → "->"；Identifier → "Identifier"。
const char* tokenTypeName(TokenType t);

} // namespace minicc
