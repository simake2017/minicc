// 手写词法扫描器实现。
// 参考 clang/Lex/Lexer.cpp: Lexer::LexTokenInternal。
// 最长匹配决定边界：a->b 中 -> 是一个 Token；a - b 中 - 独立。

#include "lexer.h"
#include <cctype>
#include <stdexcept>

namespace minicc {

// Token 的诊断名称；关键字与运算符使用源码拼写。
const char* tokenTypeName(TokenType t) {
    switch (t) {
        case TokenType::Eof:           return "EOF";
        case TokenType::IntLiteral:    return "IntLiteral";
        case TokenType::CharLiteral:   return "CharLiteral";
        case TokenType::StringLiteral: return "StringLiteral";
        case TokenType::Identifier:    return "Identifier";
        case TokenType::KwAuto:        return "auto";
        case TokenType::KwBool:        return "bool";
        case TokenType::KwChar:        return "char";
        case TokenType::KwClass:       return "class";
        case TokenType::KwBreak:       return "break";
        case TokenType::KwConst:       return "const";
        case TokenType::KwConstexpr:   return "constexpr";
        case TokenType::KwContinue:    return "continue";
        case TokenType::KwSwitch:      return "switch";
        case TokenType::KwCase:        return "case";
        case TokenType::KwDefault:     return "default";
        case TokenType::KwDo:          return "do";
        case TokenType::KwDecltype:    return "decltype";
        case TokenType::KwDelete:      return "delete";
        case TokenType::KwDouble:      return "double";
        case TokenType::KwDynamicCast: return "dynamic_cast";
        case TokenType::KwElse:        return "else";
        case TokenType::KwEnum:        return "enum";
        case TokenType::KwFalse:       return "false";
        case TokenType::KwFor:         return "for";
        case TokenType::KwIf:          return "if";
        case TokenType::KwInt:         return "int";
        case TokenType::KwLong:        return "long";
        case TokenType::KwNamespace:   return "namespace";
        case TokenType::KwNew:         return "new";
        case TokenType::KwNullptr:     return "nullptr";
        case TokenType::KwOverride:    return "override";
        case TokenType::KwPublic:      return "public";
        case TokenType::KwPrivate:     return "private";
        case TokenType::KwProtected:   return "protected";
        case TokenType::KwReturn:      return "return";
        case TokenType::KwShort:       return "short";
        case TokenType::KwSigned:      return "signed";
        case TokenType::KwStatic:      return "static";
        case TokenType::KwStruct:      return "struct";
        case TokenType::KwTemplate:    return "template";
        case TokenType::KwThis:        return "this";
        case TokenType::KwTrue:        return "true";
        case TokenType::KwTypedef:     return "typedef";
        case TokenType::KwTypename:    return "typename";
        case TokenType::KwUnsigned:    return "unsigned";
        case TokenType::KwUsing:       return "using";
        case TokenType::KwVirtual:     return "virtual";
        case TokenType::KwVoid:        return "void";
        case TokenType::KwWhile:       return "while";
        case TokenType::Plus:          return "+";
        case TokenType::Minus:         return "-";
        case TokenType::Star:          return "*";
        case TokenType::Slash:         return "/";
        case TokenType::Percent:       return "%";
        case TokenType::Ampersand:     return "&";
        case TokenType::Pipe:          return "|";
        case TokenType::Caret:         return "^";
        case TokenType::Tilde:         return "~";
        case TokenType::Bang:          return "!";
        case TokenType::Assign:        return "=";
        case TokenType::Less:          return "<";
        case TokenType::Greater:       return ">";
        case TokenType::Dot:           return ".";
        case TokenType::Comma:         return ",";
        case TokenType::Colon:         return ":";
        case TokenType::Semicolon:     return ";";
        case TokenType::Arrow:         return "->";
        case TokenType::ColonColon:    return "::";
        case TokenType::Question:      return "?";
        case TokenType::EqualEqual:    return "==";
        case TokenType::BangEqual:     return "!=";
        case TokenType::LessEqual:     return "<=";
        case TokenType::GreaterEqual:  return ">=";
        case TokenType::AmpAmp:        return "&&";
        case TokenType::PipePipe:      return "||";
        case TokenType::PlusAssign:    return "+=";
        case TokenType::MinusAssign:   return "-=";
        case TokenType::StarAssign:    return "*=";
        case TokenType::SlashAssign:   return "/=";
        case TokenType::LParen:        return "(";
        case TokenType::RParen:        return ")";
        case TokenType::LBrace:        return "{";
        case TokenType::RBrace:        return "}";
        case TokenType::LBracket:      return "[";
        case TokenType::RBracket:      return "]";
    }
    return "Unknown";
}

Lexer::Lexer(std::string source)
    : m_source(std::move(source)) {}


// 查看当前字符但不消费（状态不变，可反复调用）。
// demo: "x+1" ⇒ peek()=='x'，再 peek() 仍是 'x'
char Lexer::peek() const {
    if (isAtEnd()) return '\0';
    return m_source[m_pos];
}

// 前瞻一个字符：用于判断双字符结构（"==", "->", "//", "/*"）。
// demo: "->" ⇒ peek()=='-'，peekNext()=='>'；只剩 1 个字符时返回 '\0'
char Lexer::peekNext() const {
    if (m_pos + 1 >= m_source.size()) return '\0';
    return m_source[m_pos + 1];
}

// 全词法器唯一的"消费"动作：返回当前字符，光标 +1，并维护行/列计数
//（遇 '\n' → 行号 +1、列号归 1；否则列号 +1）。所有 SourceLocation 都由这里积累而来。
// demo: "a\nb" ⇒ advance()=='a'；再 advance()=='\n'（行 1→2、列归 1）
char Lexer::advance() {
    char c = m_source[m_pos++];
    if (c == '\n') { // 在这里面会标识行号和  和 列号
        m_line++;
        m_col = 1;
    } else {
        m_col++;
    }
    return c;
}

// 光标是否已越过最后一个字符（用 >=：消费完最后一个字符后再调一次也安全）。
// demo: 空源码 ⇒ 构造后立即 isAtEnd()==true，nextToken() 直接返回 Eof
bool Lexer::isAtEnd() const {
    return m_pos >= m_source.size(); // 也就是说 是字符文件
}

// 快照当前 (行, 列)。每个扫描函数都在开头先拍快照 ——
// 若等扫完再取，位置会指向 token 之后的字符，报错定位就错了。
SourceLocation Lexer::currentLocation() const {
    return {m_line, m_col};
}

// 统一的 Token 装配入口：聚合初始化 + text 移动，各扫描函数共用。
// demo: makeToken(KwInt, "int", {1,1}) ⇒ Token{KwInt,"int",{1,1}}
Token Lexer::makeToken(TokenType type, std::string text, SourceLocation loc) const {
    return Token{type, std::move(text), loc};
}

// 跳过空白与注释，但在字面量中保留相同字符。
// 例：x/*注释*/+1 与 x + 1 产生相同 Token 序列。
void Lexer::skipWhitespaceAndComments() {
    while (!isAtEnd()) {
        char c = peek();

        // 空白字符
        // （先转 unsigned char 再传给 isspace：char 直接传负值是未定义行为）
        if (std::isspace(static_cast<unsigned char>(c))) {
            advance();
            continue;
        }

        // 注释
        if (c == '/') {
            if (peekNext() == '/') {
                // 单行注释：跳到行尾
                advance(); advance(); // 消费 //
                while (!isAtEnd() && peek() != '\n') advance();
                continue;
            }
            if (peekNext() == '*') {
                // 多行注释：找到 */
                advance(); advance(); // 消费 /*
                // 注：直到文件尾都没找到 */ 时，简化处理为"注释延续到文件末尾"，
                //     不报错（clang 会报 unterminated comment，属于可接受的简化）。
                while (!isAtEnd()) {
                    if (peek() == '*' && peekNext() == '/') {
                        advance(); advance(); // 消费 */
                        break;
                    }
                    advance();
                }
                continue;
            }
        }

        break; // 非空白非注释，停止
    }
}

// 整数字面量保留进制前缀与后缀，数值和类型由 Parser 解释。
// 例：0x2A、0b1010、077、4ULL 都产出 IntLiteral。
Token Lexer::scanNumber() {
    // 先拍位置快照：token 位置指向第一个数字（若扫完再取就指向后面的字符了）
    auto loc = currentLocation();
    std::string text;

    // ── 进制分支：可能是前缀，也可能只是数字 0（如 `0` 与 `07` 与 `0x1F`）──
    auto isHex = [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; };

    if (peek() == '0') {
        text += advance();
        if (!isAtEnd() && (peek() == 'x' || peek() == 'X')) {
            text += advance();
            while (!isAtEnd() && (isHex(peek()) || peek() == '\'')) text += advance();
        } else if (!isAtEnd() && (peek() == 'b' || peek() == 'B')) {
            text += advance();
            while (!isAtEnd() && (peek() == '0' || peek() == '1' || peek() == '\'')) text += advance();
        } else {
            // 八进制：以 0 开头的十进制数字串（`0` 自己也算）
            while (!isAtEnd() && ((peek() >= '0' && peek() <= '7') || peek() == '\'')) text += advance();
        }
    } else {
        // 十进制：贪心消费连续数字——数字分支内部的"最长匹配"：
        // 见数字就吃，直到第一个非数字字符才停，保证 42 不会被切成 4 和 2。
        while (!isAtEnd() && (std::isdigit(static_cast<unsigned char>(peek())) || peek() == '\'')) {
            text += advance();
        }
    }

    // 收集整数后缀，合法组合由 Parser 校验。
    int nLong = 0;
    while (!isAtEnd()) {
        const char c = peek();
        if (c == 'u' || c == 'U' || c == 'z' || c == 'Z') { text += advance(); }
        else if (c == 'l' || c == 'L') {
            if (nLong >= 2) break;          // 至多 long long
            nLong++; text += advance();
        }
        else break;
    }

    return makeToken(TokenType::IntLiteral, text, loc);
}

// 先扫描完整单词再查关键字表。
// 例：int → KwInt；integer → Identifier，不能拆成 int + eger。
Token Lexer::scanIdentifierOrKeyword() {
    auto loc = currentLocation();
    std::string text;

    while (!isAtEnd() && (std::isalnum(static_cast<unsigned char>(peek())) // 字母数字
                          || peek() == '_')) { // 下划线
        text += advance();
    }

    // 查关键字表
    auto it = kKeywordMap.find(text); // 判断是不是关键词
    if (it != kKeywordMap.end()) {
        return makeToken(it->second, text, loc);
    }

    return makeToken(TokenType::Identifier, text, loc);
}

// 字符串扫描时翻译转义，Token.text 不含外围引号。
// 例："hi\n" 的 text 含换行；"a\"b" 的 text 含双引号。
Token Lexer::scanString() {
    auto loc = currentLocation();
    advance(); // 消费开头的 " 只是往前推进，但是不添加

    std::string text;
    while (!isAtEnd() && peek() != '"') {
        if (peek() == '\\') {
            advance(); // 消费反斜杠：只推进游标，不并入 text
            char escaped = advance();
            // 转义翻译表：n→换行  t→制表符  \\→反斜杠  \"→双引号；
            // default 原样保留（真实 C++ 对未知转义报错，本项目宽容处理）
            switch (escaped) {
                case 'n':  text += '\n'; break;
                case 't':  text += '\t'; break;
                case '\\': text += '\\'; break;
                case '"':  text += '"';  break;
                default:   text += escaped; break;
            }
        } else {
            text += advance();
        }
    }

    // 走到文件尾仍未见闭引号 → 报错（[lex.string] 要求字符串在同一行内终结）
    // 示例：源码 `"abc`（缺闭引号）→ 抛 "Unterminated string at 1:1"
    if (isAtEnd()) {
        throw std::runtime_error(
            std::format("Unterminated string at {}", loc.toString()));
    }
    advance(); // 消费结尾的 "

    return makeToken(TokenType::StringLiteral, text, loc);
}

// 字符字面量识别常用转义，并拒绝空内容与未闭合引号。
// 例：'a'、'\n'；此处保留扫描到的字符文本。
Token Lexer::scanChar() {
    auto loc = currentLocation();
    advance(); // 消费开头的 '

    std::string text;
    while (!isAtEnd() && peek() != '\'') {
        if (peek() == '\\') {
            advance(); // 消费反斜杠
            if (isAtEnd()) break;
            char escaped = advance();
            // 转义翻译表与 scanString 共用同一套规则（[lex.ccon] 的 simple-escape-sequence）
            switch (escaped) {
                case 'n':  text += '\n'; break;
                case 't':  text += '\t'; break;
                case 'r':  text += '\r'; break;
                case '0':  text += '\0'; break;
                case '\\': text += '\\'; break;
                case '\'': text += '\''; break;
                case '"':  text += '"';  break;
                default:   text += escaped; break;
            }
        } else {
            text += advance();
        }
    }

    // 走到文件尾仍未见闭引号 → 报错（[lex.ccon] 要求字符字面量在同一行内终结）
    if (isAtEnd()) {
        throw std::runtime_error(
            std::format("Unterminated character literal at {}", loc.toString()));
    }
    advance(); // 消费结尾的 '

    if (text.empty()) {
        throw std::runtime_error(std::format(
            "Empty character literal at {} (C++ 不允许 ''，见 [lex.ccon])",
            loc.toString()));
    }

    return makeToken(TokenType::CharLiteral, text, loc);
}

// 优先匹配双字符运算符（[lex.pptoken] 最长匹配）。
// 例：<= 与 -> 各为一个 Token；< = 保留为两个 Token。
Token Lexer::scanOperator() {
    auto loc = currentLocation();
    char c = advance(); // 会消费
    char n = peek(); //只是取出来 ,中间不能有空格，比如>= 不能是 > =

    // c：已消费的首字符；n：前瞻一字符（只读，匹配成功才 advance() 消费它）
    // 双字符运算符
    switch (c) {
        case '-':
            if (n == '>') { advance(); return makeToken(TokenType::Arrow, "->", loc); }
            if (n == '=') { advance(); return makeToken(TokenType::MinusAssign, "-=", loc); }
            return makeToken(TokenType::Minus, "-", loc);
        case '+':
            if (n == '=') { advance(); return makeToken(TokenType::PlusAssign, "+=", loc); }
            return makeToken(TokenType::Plus, "+", loc);
        case '*':
            if (n == '=') { advance(); return makeToken(TokenType::StarAssign, "*=", loc); }
            return makeToken(TokenType::Star, "*", loc);
        case '/':
            if (n == '=') { advance(); return makeToken(TokenType::SlashAssign, "/=", loc); }
            return makeToken(TokenType::Slash, "/", loc);
        case '=':
            if (n == '=') { advance(); return makeToken(TokenType::EqualEqual, "==", loc); }
            return makeToken(TokenType::Assign, "=", loc);
        case '!':
            if (n == '=') { advance(); return makeToken(TokenType::BangEqual, "!=", loc); }
            return makeToken(TokenType::Bang, "!", loc);
        case '<':
            if (n == '=') { advance(); return makeToken(TokenType::LessEqual, "<=", loc); }
            return makeToken(TokenType::Less, "<", loc);
        case '>':
            if (n == '=') { advance(); return makeToken(TokenType::GreaterEqual, ">=", loc); }
            return makeToken(TokenType::Greater, ">", loc);
        case '&':
            if (n == '&') { advance(); return makeToken(TokenType::AmpAmp, "&&", loc); }
            return makeToken(TokenType::Ampersand, "&", loc);
        case '|':
            if (n == '|') { advance(); return makeToken(TokenType::PipePipe, "||", loc); }
            return makeToken(TokenType::Pipe, "|", loc);
        case ':':
            if (n == ':') { advance(); return makeToken(TokenType::ColonColon, "::", loc); }
            return makeToken(TokenType::Colon, ":", loc);
        case '?': return makeToken(TokenType::Question, "?", loc);
        default:
            break;
    }

    // 单字符运算符/分隔符
    // 走到这里说明 c 无法组成双字符运算符，退回匹配单字符——
    // 这正是最长匹配中"延长不了就取短"的那一面。
    switch (c) {
        case '%': return makeToken(TokenType::Percent, "%", loc);
        case '^': return makeToken(TokenType::Caret, "^", loc);
        case '~': return makeToken(TokenType::Tilde, "~", loc);
        case '.': return makeToken(TokenType::Dot, ".", loc);
        case ',': return makeToken(TokenType::Comma, ",", loc);
        case ';': return makeToken(TokenType::Semicolon, ";", loc);
        case '(': return makeToken(TokenType::LParen, "(", loc);
        case ')': return makeToken(TokenType::RParen, ")", loc);
        case '{': return makeToken(TokenType::LBrace, "{", loc);
        case '}': return makeToken(TokenType::RBrace, "}", loc);
        case '[': return makeToken(TokenType::LBracket, "[", loc);
        case ']': return makeToken(TokenType::RBracket, "]", loc);
        default: break;
    }

    // 既非已知运算符也非分隔符 → 非法字符错误（词法阶段就能发现的错误之一）
    // 示例：源码 "int x @ 1;" → 抛 "Unexpected character '@' at 1:7"
    throw std::runtime_error(
        std::format("Unexpected character '{}' at {}", c, loc.toString()));
}

// Token 读取入口；空白与注释不进入 Token 流。
Token Lexer::nextToken() {
    skipWhitespaceAndComments(); //跳过空白符和注释

    // 源码耗尽：返回 Eof token（而不是报错）——Parser 用它作为文法的统一终止信号；
    // 且重复调用会一直返回 Eof（幂等），调用方循环条件无需特殊处理。
    if (isAtEnd()) {
        return makeToken(TokenType::Eof, "", currentLocation());
    }

    char c = peek();

    // 数字开头 → 数字字面量
    if (std::isdigit(static_cast<unsigned char>(c))) {
        return scanNumber();
    }

    // 字母或下划线开头 → 标识符或关键字
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') { // 如果是 字母或者_开头
        return scanIdentifierOrKeyword();
    }

    // 双引号开头 → 字符串字面量
    if (c == '"') {
        return scanString();
    }

    // 单引号开头 → 字符字面量
    if (c == '\'') {
        return scanChar();
    }

    // 其他 → 运算符或分隔符
    return scanOperator();
}

// 收集完整 Token 流，包含末尾 Eof。
std::vector<Token> Lexer::tokenizeAll() {
    std::vector<Token> tokens;
    while (true) {
        Token tok = nextToken();
        tokens.push_back(tok);
        if (tok.is(TokenType::Eof)) break;
    }
    return tokens;
}

} // namespace minicc
