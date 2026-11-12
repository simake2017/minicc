#pragma once
// 各阶段共用的类型树、模板实参与类布局。
// 参考 clang/AST/Type.h、TemplateBase.h、RecordLayout.h。
// 本实现用独立 Const 节点展示限定层次，Sema 计算布局后供 CodeGen 使用。
// 例：const int* → Pointer(Const(Int))；int* const → Const(Pointer(Int))。

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

namespace minicc {

// 前向声明
struct Type;
using TypePtr = std::shared_ptr<Type>;
// decltype 的操作数（见 TypeKind::Decltype）。
// ★ 此处只前置声明、不 include ast.h：ast.h 反过来依赖 type.h，
//   互相 include 会成环。只存指针，不需要完整类型。
struct Expression;
using DecltypeExprPtr = std::shared_ptr<Expression>;
// 函数声明节点（ast.h 定义 FunctionDecl）—— vtable 槽要挂它的句柄。
// ★ 同样只前置声明：依赖保持单向（ast.h → type.h），shared_ptr 允许不完整类型。
struct FunctionDecl;

// 类型种类；char、signed char、unsigned char 各有独立身份。
// unsigned 与 unsigned int 等多关键字写法由 Parser 归一。
enum class TypeKind : uint8_t {
    Void,           // void
    Bool,           // bool
    Char,           // char            —— 与下面两个是不同类型
    SChar,          // signed char
    UChar,          // unsigned char
    Short,          // short / short int / signed short
    UShort,         // unsigned short
    Int,            // int
    UInt,           // unsigned / unsigned int
    Long,           // long / long int
    ULong,          // unsigned long
    LongLong,       // long long / long long int
    ULongLong,      // unsigned long long
    Double,         // double
    Pointer,        // T*
    LValueReference, // T&（左值引用）
    RValueReference, // T&&（右值引用 / 万能引用，当 T 是模板参数时）
    Const,          // const T（常量限定）
    Class,          // 类类型（含内存布局信息）
    TemplateParam,  // 模板参数占位符（如 T）
    Auto,           // auto 占位符（等待推导）
    Decltype,       // decltype(expr) —— 半成品类型，替换后才求值（见下方"两段式"说明）
    Array,          // T[N] —— 数组类型（主线 E，[dcl.array]）
};

// 类型编码见 src/sema/template_instantiation.cpp: NameMangler::encodeType。
// 遵循 Itanium ABI §5.1.2 的基本编码：int → i，unsigned int → j，T* → P + T。
// const 在此是节点，在 clang 中由 QualType 的限定位表示。

// ─────────────────────────────────────────────────────────────────────────────
// AccessModifier：访问修饰符
// ─────────────────────────────────────────────────────────────────────────────
enum class AccessModifier : uint8_t {
    Public,
    Private,
    Protected,
};

// 字段的类型与对象内偏移；对应 clang FieldDecl 和 ASTRecordLayout。
// 例：struct Point { int x; int y; }; → x@0、y@4。
struct FieldInfo {
    std::string name;            // 显示名 —— MI 下带限定前缀，仅用于打印与"全限定名"查找
    TypePtr     type;
    uint32_t    offset  = 0;     // 字段在对象内存中的字节偏移量
    uint32_t    size    = 0;     // 字段占用的字节数
    AccessModifier access = AccessModifier::Public;
    std::string sourceClass;     // 声明该字段的类名（空 = 自身字段；非空 = 继承自该类）

    // 字段身份须包含基类子对象和字段下标；显示名不能区分所有继承路径。
    // 名字用于查找，viaBase + baseFieldIndex 用于布局定位。
    std::string declaredName;    // 裸名（不含任何限定前缀）—— 名字查找的权威键
    std::string viaBase;         // 装着它的【直接基类】子对象名（空 = 本类自身字段）
    int         baseFieldIndex = -1;  // 在 viaBase 那个基类布局 fields[] 中的下标

    // 权威裸名：declaredName 优先，缺失时退回"显示名剥掉最外层前缀"
    // demo：bareName() 对 name="B.x" / declaredName="x" ⇒ "x"；对 name="x" ⇒ "x"
    std::string bareName() const {
        if (!declaredName.empty()) return declaredName;
        auto dot = name.find('.');
        return dot == std::string::npos ? name : name.substr(dot + 1);
    }
};

// 虚表槽持有方法声明，符号统一从声明的 mangledName 取得。
// 参考 clang/AST/VTableBuilder.h: VTableComponent。
// 避免在形参类型尚未解析时另算符号；覆写只替换句柄。
// 例：Base::foo 占槽 0，Derived::foo 覆写后该槽指向派生声明。
struct VTableEntry {
    // 仅前置声明 FunctionDecl，保持 ast.h → type.h 单向依赖。
    std::shared_ptr<FunctionDecl> method;
    uint32_t    index   = 0;     // 在 vtable 中的索引（0-based）
    bool        isOverridden = false;
    std::string baseFunctionName; // 原始方法名（override 检测用；析构统一记作 "dtor"）
    // 槽位身份由方法名和形参类型链共同确定，与成员查找共用 paramTypeChain。
    // 例：f() 与 f(int) 不占同一槽；零参签名为空串。
    std::string signature;
    // 多继承 thunk 调整量（[class.mi] + Itanium ABI 2.4）：
    // 非 0 时，本槽位不能直接填函数地址，而要填一个跳板（thunk）——
    // 跳板先把 this 加上 vptr[-2]（offset-to-top）归顶，再跳真实函数。
    // 仅出现在【次表的覆写槽】：基类自己的函数期待基类 this（调用方
    // 本来就传基类指针），无需调整；覆写函数期待最派生类 this，才要调整。
    int         thunkAdjust = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// BaseSubobject：多继承下的基类子对象信息（[class.mi]）
// ─────────────────────────────────────────────────────────────────────────────
// class D : A, B 的对象 = A 子对象 + B 子对象 + D 自身字段，按声明顺序摆放。
// 每个多态基类子对象自带一个 _vptr；第一个多态基类（主基类）与派生类共享
// 主虚表（Itanium 主基类优化），其余基类各占一张次表。
struct BaseSubobject {
    std::string baseClassName;        // 基类名
    uint32_t    offset = 0;           // 子对象在完整对象中的起始偏移
    bool        hasVTable = false;    // 该子对象是否带 _vptr（基类是否多态）
    bool        isPrimary = false;    // 是否主基类（与派生类共享主表）
    uint32_t    vtableSegmentOffset = 0;  // 本表段在 _ZTV 符号内的字节偏移
                                          //（vptr = &_ZTV + 段偏移 + 16）
    std::vector<VTableEntry> entries;     // 次表槽位（仅非主多态基类有意义）
};

// 模板实参按种类保存，对应 clang/AST/TemplateBase.h: TemplateArgument。
// 例：Box<int> 为 Type；Buf<4> 为 Integral；Wrap<Box,int> 中 Box 为 Template。
// 类型与值不能都装进 TypePtr；参数包尚未实现。
enum class TemplateArgKind : uint8_t {
    Type,     // 类型实参：Box<int>            →  payload 在 type 字段
    Integral, // 非类型实参：Buf<4>（NTTP）    →  payload 在 value 字段
    Template, // 模板模板实参：Wrap<Box, int>  →  payload 在 templateName 字段
};

struct TemplateArg {
    TemplateArgKind kind = TemplateArgKind::Type;
    TypePtr         type  = nullptr; // kind == Type     时有效
    int64_t         value = 0;       // kind == Integral 时有效
    // 常量表达式实参在 Sema 校验时归一为 value/valueType。
    // 例：Buf<2+2> 先存表达式；求值后与 Buf<4> 使用相同值形态。
    DecltypeExprPtr valueExpr = nullptr;

    // 模板模板实参保存模板名，逐位形参签名匹配由 Sema 负责。
    // 例：Wrap<Box,int> 的第一位是模板 Box，不是类型 Box。
    std::string     templateName;

    // ── 非类型实参的【形态】（[temp.arg.nontype]）──────────────────────────
    // 值统一用 int64_t 存（bool 存 0/1），但 Itanium 编码必须区分形态：
    //   Buf<1>    （int 形参）  ⇒ _Z3BufILi1EE
    //   Flag<true>（bool 形参） ⇒ _Z4FlagILb1EE   ← 是 'b' 不是 'i'
    // 故在此记住"这个值是按哪种类型写的"；为空时 mangler 按 int 兜底。
    // 对照 clang：TemplateArgument 的值形态带 QualType（来自 IntegerLiteral 的 AST 类型）。
    TypePtr         valueType = nullptr;

    TemplateArg() = default;

    // 类型实参允许 TypePtr 隐式构造；值实参必须显式使用 ofValue。
    TemplateArg(TypePtr t)  // NOLINT(*-explicit-constructor)
        : kind(TemplateArgKind::Type), type(std::move(t)) {}

    static TemplateArg ofType(TypePtr t) {
        TemplateArg a; a.kind = TemplateArgKind::Type; a.type = std::move(t); return a;
    }
    // vt = 该值的【形态】（int / bool …），决定 mangling 编码是 Li…E 还是 Lb…E。
    static TemplateArg ofValue(int64_t v, TypePtr vt = nullptr) {
        TemplateArg a; a.kind = TemplateArgKind::Integral; a.value = v;
        a.valueType = std::move(vt); return a;
    }
    // 模板模板实参：只带模板名。demo: Wrap<Box, int> ⇒ 实参 0 = ofTemplate("Box")
    static TemplateArg ofTemplate(std::string n) {
        TemplateArg a; a.kind = TemplateArgKind::Template;
        a.templateName = std::move(n); return a;
    }

    bool isType()     const { return kind == TemplateArgKind::Type; }
    bool isValue()    const { return kind == TemplateArgKind::Integral; }
    bool isTemplate() const { return kind == TemplateArgKind::Template; }

    // 全特化匹配同时比较值和类型：ofValue(1,int) != ofValue(1,bool)。
    // 定义置于 src/ast/type.cpp，因为这里 Type 尚不完整。
    bool equals(const TemplateArg& o) const;

    // 可读形式：ofType(makeInt()) → "int"；ofValue(4) → "4"。
    std::string toString() const;
};

// Sema 计算的完整对象布局，CodeGen 直接消费，避免重复计算偏移。
// 例：struct Point { int x; int y; }; → x@0、y@4、totalSize=8。
// 多态布局另含 vptr、主表、次表和基类子对象。
struct ClassLayout {
    std::string              className;
    uint32_t                 totalSize   = 0;    // 整个对象的字节大小
    bool                     hasVTable   = false; // 是否有虚函数表
    std::vector<FieldInfo>   fields;             // 所有字段（含继承的，名字已限定）
    std::vector<VTableEntry> vtableEntries;      // 主虚表条目（主基类槽位合并结果）

    // 多继承扩展（[class.mi]）：全部基类子对象（含单继承的 0/1 个）。
    // 单继承时 = {一个元素, offset=0, isPrimary=true}，行为退化为原实现。
    // CodeGen 依据它：① 摆放对象内各 _vptr；② 发射次表与 thunk；
    // ③ 上/下转型的指针调整量；④ RTTI 的基类数组（typeinfo + 偏移）。
    std::vector<BaseSubobject> bases;

    // vtable 槽位 -1：RTTI type_info 指针（位于 vtable 起始地址的前一个指针位置）
    // 在汇编层面：vtable[-1] = type_info_address
    std::string              rttiMangledName;    // RTTI 符号名

    // 查找字段偏移量（[class.member.lookup] 的字段版）
    // 三级顺序，缺一不可：
    //   ① 全限定名精确匹配 —— 调用方自己写了 "A.a" 时照单全收
    //   ② 【本类自身字段】优先 —— [class.member.lookup]/3：派生类的声明【隐藏】基类同名
    //      声明。自身字段与基类字段撞名时会被重命名成 "D.x"，靠 declaredName 才认得出
    //   ③ 继承字段 —— 名字对各来源一视同仁，先到先得（真 C++ 在此应报歧义，
    //      [class.member.lookup]/8；minicc 只做"能查到"，歧义诊断见 docs/已知边界）
    const FieldInfo* findField(const std::string& name) const {
        auto all = findFields(name);
        return all.empty() ? nullptr : all.front();
    }

    // 同上，但返回该层级的【全部】命中 —— 长度 > 1 即 [class.member.lookup]/8 的歧义
    // （`struct A{int x;}; struct B{int x;}; struct D:A,B{}; d.x` 在真 C++ 里 ill-formed）。
    // 分出这个接口的理由：findField 只能回答"取哪一条"，回答不了"是不是只有一条"；
    // 而"静默取第一条"正是 已知边界 的观感（布局打印出两条同名、偏移还错）。
    // 对照 clang：clang/include/clang/Sema/Lookup.h:LookupResult::isAmbiguous + 承载多个 NamedDecl。
    std::vector<const FieldInfo*> findFields(const std::string& name) const {
        // 按 declaredName 查裸名，避免显示前缀影响继承字段查找。
        std::vector<const FieldInfo*> hit;
        for (auto& f : fields) {                                  // ① 全限定名
            if (f.name == name) hit.push_back(&f);
        }
        if (!hit.empty()) return hit;
        for (auto& f : fields) {                                  // ② 自身字段（隐藏）
            if ((f.sourceClass.empty() || f.sourceClass == className) &&
                f.bareName() == name) hit.push_back(&f);
        }
        if (!hit.empty()) return hit;                             // 自身命中即隐藏基类同名
        for (auto& f : fields) {                                  // ③ 继承字段（可能多条）
            if (!f.sourceClass.empty() && f.sourceClass != className &&
                f.bareName() == name) hit.push_back(&f);
        }
        return hit;
    }
};

// 类型采用嵌套节点表示修饰符，外层节点决定修饰对象。
// 参考 clang/AST/Type.h；clang 的 const 由 QualType 承载。
// 例：const int* → Pointer(Const(Int))；int* const → Const(Pointer(Int))。
// 引用构造时即折叠：T& && → T&；T&& && → T&&。
struct Type {
    TypeKind kind;
    std::string name;           // 类型名称（如 "int", "MyClass", "T"）

    // ── 数组类型特有（TypeKind::Array，[dcl.array]）──
    TypePtr         elementType;     // 元素类型 T
    int64_t         arraySize = 0;   // 元素个数 N（编译期已知正整数）
    DecltypeExprPtr arraySizeExpr;   // 待求值的表达式（语义期折叠成 arraySize）

    // ── 指针类型特有 ──
    TypePtr pointeeType;        // 指向的类型（仅 Pointer 类型使用）

    // ── 引用类型特有（T& 和 T&&）──
    TypePtr referencedType;     // 被引用的类型（仅 LValueReference / RValueReference 使用）

    // ── const 类型特有 ──
    TypePtr innerType;          // const 限定的内部类型（仅 Const 使用）

    // ── 类类型特有 ──
    ClassLayout classLayout;    // 类的内存布局（仅 Class 类型使用）

    // ── 类模板实参（P3 / NTTP）──
    // Box<int> 解析为 Class(name="Box", templateArgs=[Type:int])；
    // Buf<4>   解析为 Class(name="Buf", templateArgs=[Integral:4])。
    // ★ 用 TemplateArg（tagged 值）而非 TypePtr —— 见上方 TemplateArg 注释：
    //    类型实参与非类型实参（NTTP 的值）必须共存于同一槽位，
    //    否则 template<int N> 的 4 无处安放。
    // 语义阶段 resolveType 见到非空实参 → 触发按需实例化（见
    // SemanticAnalyzer::getOrInstantiateClass），产出具体实例类型
    // （如 Box_int）后整体替换本节点——即"模板 id 是半成品类型，
    // 实例化后才成为完整类型"（[temp.inst] 的落地形态）。
    std::vector<TemplateArg> templateArgs;

    // ── 嵌套/依赖类型名（`S<int>::type`、`T::type`）──
    // 【写法 ⇒ 两个字段 ⇒ 谁来解】
    //     S<int>::type  ⇒ nestedQualifier=S<int>（半成品）、name="type"
    //                     ⇒ resolveType 先按需实例化限定者，再查实例类的 typeAliases
    //     T::type       ⇒ nestedQualifier=TemplateParam(T)、name="type"
    //                     ⇒ 依赖情形：等 substituteType 在【直接上下文】当场解，查不到即
    //                       Sfinae::fail（[temp.deduct]/8；见 docs/learn/24/25）
    // 【为什么单列一组字段】它既不是"类名"（不能拿去查 m_classTypes），也不是"模板 id"
    //   （没有实参可实例化）—— 而是一条【待解析的路径】，故必须与两者分开存。
    // 对照 clang：DependentNameType / ElaboratedType（qualifier + NamedDecl）。
    TypePtr     nestedQualifier;    // 限定部分（如 S<int> 或 T）
    bool isNestedName() const { return nestedQualifier != nullptr; }

    // ── 实例"出身"（模板实例类型专有）──
    // 【要解决什么】实例化后类型名变成 `MyPtr_int`，模板 id 的信息（哪个模板、哪些实参）
    //   在改名那刻就丢了；推导却必须回答"`MyPtr_int` 是 `MyPtr<T>` 对 T 的一次成功绑定吗？"
    // demo: MyPtr<int> ⇒ name="MyPtr_int"（参与布局与 mangling）、
    //                   templateOriginName="MyPtr"、templateOriginArgs=[Type:int]
    //   推导 P=MyPtr<T> vs A=MyPtr_int：出身同名 + 实参个数相等 ⇒ 逐位合一 ⇒ T := int。
    // ★ 必须与 templateArgs 分开存：那个字段的语义是"待实例化的半成品"（resolveType 见到
    //   非空实参就触发实例化），出身是【只读记录】，不参与任何解析决策。名字不可反推 ——
    //   它是清洗过的可读串、不是单射（踩坑史 T1/T3）。
    // 对照 clang：ClassTemplateSpecializationDecl 自带 TemplateArgumentList，无此问题。
    std::string              templateOriginName;
    std::vector<TemplateArg> templateOriginArgs;
    bool isTemplateInstance() const { return !templateOriginName.empty(); }

    // ── 模板参数类型特有 ──
    std::string templateParamName; // 模板参数名（如 "T"）

    // ── decltype 类型特有（TypeKind::Decltype）──
    // ★ 两段式：decltype 出现时不立刻求值，先原样留存表达式，等【替换】（substituteType）
    //   阶段再求。原因：is_range<T, void_t<decltype(declval<T>().begin())>> 里 decltype 位于
    //   【模板模式】中，此刻 T 未知 —— 立即求值无从下手。
    //   对照 clang：DecltypeType 在依赖上下文中就是依赖类型，直到 clang/lib/Sema/SemaTemplateInstantiate.cpp:Sema::SubstType 才求值。
    DecltypeExprPtr decltypeExpr;              // 操作数表达式（半成品，替换后求值）
    bool            decltypeParen = false;     // 是否多套了一层括号，见 [dcl.type.decltype]

    // ── 工厂方法 ──
    // 每个 make* 返回一个新构造的 Type 节点；组合规则见上方"洋葱式"说明。
    // 各工厂函数的入参含义、产物形状与 encodeType 结果见 src/ast/type.cpp 对应实现。
    static TypePtr makeVoid();
    static TypePtr makeBool();
    // ── 整型家族（[basic.fundamental]/2）──
    // makeChar 造的是裸 `char`，不是 signed char —— 两者是不同类型，各有工厂。
    static TypePtr makeChar();
    static TypePtr makeSChar();
    static TypePtr makeUChar();
    static TypePtr makeShort();
    static TypePtr makeUShort();
    static TypePtr makeInt();
    static TypePtr makeUInt();          // `unsigned` / `unsigned int`
    static TypePtr makeLong();
    static TypePtr makeULong();
    static TypePtr makeLongLong();
    static TypePtr makeULongLong();
    static TypePtr makeDouble();
    static TypePtr makePointer(TypePtr pointee);
    static TypePtr makeLValueReference(TypePtr referenced);   // T&
    static TypePtr makeRValueReference(TypePtr referenced);   // T&&
    static TypePtr makeConst(TypePtr inner);                  // const T
    static TypePtr makeClass(const std::string& name);
    // 数组类型工厂
    static TypePtr makeArray(TypePtr elem, int64_t size);
    static TypePtr makeArray(TypePtr elem, DecltypeExprPtr sizeExpr);
    static TypePtr makeTemplateParam(const std::string& paramName);
    static TypePtr makeAuto();
    // decltype(expr)：paren 表示原文是否写成 decltype((e))，见 [dcl.type.decltype]
    static TypePtr makeDecltype(DecltypeExprPtr expr, bool paren);

    // ── 类型查询 ──
    bool isVoid()             const { return kind == TypeKind::Void; }
    bool isBool()             const { return kind == TypeKind::Bool; }
    bool isInt()              const { return kind == TypeKind::Int; }
    bool isDouble()           const { return kind == TypeKind::Double; }
    // isInteger 包含 bool，供算术与整型常量检查统一使用。
    // bool 的可表示集合仍单独按 {0,1} 处理。
    bool isInteger()          const {
        switch (kind) {
            case TypeKind::Bool:
            case TypeKind::Char:   case TypeKind::SChar:  case TypeKind::UChar:
            case TypeKind::Short:  case TypeKind::UShort:
            case TypeKind::Int:    case TypeKind::UInt:
            case TypeKind::Long:   case TypeKind::ULong:
            case TypeKind::LongLong: case TypeKind::ULongLong:
                return true;
            default: return false;
        }
    }
    // 三个字符类型：char / signed char / unsigned char 是【三个不同类型】([basic.fundamental]/7)
    bool isChar()             const {
        return kind == TypeKind::Char || kind == TypeKind::SChar
            || kind == TypeKind::UChar;
    }
    // 无符号整型（不含 bool，bool 的"可表示值"只有 0/1，是另一条规则，见 checkTemplateArguments）
    bool isUnsignedInteger()  const {
        switch (kind) {
            case TypeKind::UChar: case TypeKind::UShort: case TypeKind::UInt:
            case TypeKind::ULong: case TypeKind::ULongLong:
                return true;
            default: return false;
        }
    }
    // 整型值 v 能否被本类型【无损表示】（[dcl.init]/7 的窄化 + [expr.const]/10 的
    // converted constant expression 例外：常量表达式只要"值装得下"就不算窄化）。
    // 定义放 src/ast/type.cpp（要用到 integerBitWidth 的移位，且这是"类型自己答得出"的问题）。
    // demo: makeBool().canRepresentValue(1)   ⇒ true   （bool 的可表示集合是 {0,1}）
    //       makeBool().canRepresentValue(2)   ⇒ false
    //       makeUInt().canRepresentValue(-1)  ⇒ false  （无符号装不下负值）
    //       makeShort().canRepresentValue(70000) ⇒ false
    bool canRepresentValue(int64_t v) const;

    // 整型宽度（位）：bool 按 1 位处理 —— 它的可表示集合是 {0,1}，正是 [dcl.init]/7
    // 对 bool 窄化判定的口径。
    uint32_t integerBitWidth() const {
        switch (kind) {
            case TypeKind::Bool:    return 1;
            case TypeKind::Char:    case TypeKind::SChar: case TypeKind::UChar: return 8;
            case TypeKind::Short:   case TypeKind::UShort:                      return 16;
            case TypeKind::Int:     case TypeKind::UInt:                        return 32;
            case TypeKind::Long:    case TypeKind::ULong:
            case TypeKind::LongLong: case TypeKind::ULongLong:                  return 64;
            default: return 0;
        }
    }
    bool isArray()            const { return kind == TypeKind::Array; }
    bool isPointer()          const { return kind == TypeKind::Pointer; }
    bool isLValueReference()  const { return kind == TypeKind::LValueReference; }
    bool isRValueReference()  const { return kind == TypeKind::RValueReference; }
    bool isReference()        const { return isLValueReference() || isRValueReference(); }
    bool isConst()            const { return kind == TypeKind::Const; }
    bool isClass()            const { return kind == TypeKind::Class; }
    bool isTemplateParam()    const { return kind == TypeKind::TemplateParam; }
    bool isAuto()             const { return kind == TypeKind::Auto; }
    bool isDecltype()         const { return kind == TypeKind::Decltype; }
    // 可参与算术的类型：整型（含 bool）与 double。
    bool isNumeric()          const { return isInteger() || isDouble(); }

    // 去除引用和 const 的"裸类型"（用于类型比较和推导）
    TypePtr stripReferences() const;
    TypePtr stripConst() const;

    // 获取类型的大小（字节）
    uint32_t sizeInBytes() const;

    // 类型比较
    bool equals(const TypePtr& other) const;

    // 获取可读的类型名
    std::string toString() const;
};

// ── 引用绑定判据（[dcl.init.ref]，全项目【单点】）─────────────────────────────
// 问：形参类型是 paramType、实参的值类别是 argIsLValue 时，能不能绑上去？
//   T&        → 只收左值（[dcl.init.ref]/5.4.1）
//   const T&  → 左右值皆可（/5.4.2："const 左值引用可绑右值"）
//   T&&       → 只收右值（非模板；模板的 T&& 是万能引用，走 [temp.deduct.call]/3，
//               由推导引擎先折叠成 T := A& 再替换，不经过本判据）
//   非引用形参 → 恒可（值类别不影响按值传参）
// ★ 为什么必须单点：这条判据此前**只写在模板推导那一条路上**（deducePair 的 P=T&
//   分支），于是两个方向各错一次 —— `const T&` 收右值被误拒（拒收合法程序）、
//   非模板 `int&&` 收左值被放行（接受非法程序）。承项目铁律「同一判据不许写两份」。
// why（可空）：绑不上时填一句人话，供日志与报错文案使用。
bool referenceBindsValueCategory(const TypePtr& paramType, bool argIsLValue,
                                 std::string* why = nullptr);

} // namespace minicc
