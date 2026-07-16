#include "changefeed/predicate_evaluator.h"

#include <cctype>
#include <regex>
#include <vector>

namespace instantdb {

// ==================== AST ====================

namespace detail {

struct PredicateNode {
    enum class Kind { OR, AND, NOT, COMPARISON, LIKE, IS_NULL };
    enum class Op { EQ, NE, LT, LE, GT, GE };
    enum class LiteralType { STRING, NUMBER, BOOLEAN, NULL_VALUE };

    Kind kind;

    // OR/AND use left and right; NOT uses left only
    std::unique_ptr<PredicateNode> left;
    std::unique_ptr<PredicateNode> right;

    // COMPARISON / LIKE / IS_NULL
    std::string column;

    // COMPARISON
    Op op = Op::EQ;
    LiteralType literal_type = LiteralType::NULL_VALUE;
    std::string string_value;
    double number_value = 0.0;
    bool bool_value = false;

    // LIKE
    std::regex like_regex;

    // IS_NULL: true for IS NOT NULL
    bool negated = false;
};

} // namespace detail

namespace {

using detail::PredicateNode;

// ==================== Tokenizer ====================

enum class TokenType {
    IDENT, STRING, NUMBER,
    KW_AND, KW_OR, KW_NOT, KW_LIKE, KW_IS, KW_NULL, KW_TRUE, KW_FALSE,
    EQ, NE, LT, LE, GT, GE,
    LPAREN, RPAREN,
    END
};

struct Token {
    TokenType type;
    std::string text;      // identifier name or string contents
    double number = 0.0;   // NUMBER only
    size_t pos = 0;        // position in input, for error messages
};

std::string ToLowerAscii(const std::string& str) {
    std::string result = str;
    for (char& c : result) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return result;
}

TokenType KeywordOrIdent(const std::string& word) {
    std::string lower = ToLowerAscii(word);
    if (lower == "and")   return TokenType::KW_AND;
    if (lower == "or")    return TokenType::KW_OR;
    if (lower == "not")   return TokenType::KW_NOT;
    if (lower == "like")  return TokenType::KW_LIKE;
    if (lower == "is")    return TokenType::KW_IS;
    if (lower == "null")  return TokenType::KW_NULL;
    if (lower == "true")  return TokenType::KW_TRUE;
    if (lower == "false") return TokenType::KW_FALSE;
    return TokenType::IDENT;
}

bool IsDigit(char c) {
    return std::isdigit(static_cast<unsigned char>(c)) != 0;
}

bool Tokenize(const std::string& input, std::vector<Token>& tokens, std::string& error) {
    size_t i = 0;
    const size_t n = input.size();

    while (i < n) {
        char c = input[i];

        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
            continue;
        }

        const size_t start = i;

        // Identifier or keyword
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            size_t j = i;
            while (j < n && (std::isalnum(static_cast<unsigned char>(input[j])) || input[j] == '_')) {
                ++j;
            }
            std::string word = input.substr(i, j - i);
            tokens.push_back({KeywordOrIdent(word), word, 0.0, start});
            i = j;
            continue;
        }

        // Number (optional sign, digits, optional fraction)
        bool signed_number = (c == '+' || c == '-') && i + 1 < n &&
                             (IsDigit(input[i + 1]) ||
                              (input[i + 1] == '.' && i + 2 < n && IsDigit(input[i + 2])));
        if (IsDigit(c) || (c == '.' && i + 1 < n && IsDigit(input[i + 1])) || signed_number) {
            size_t j = i;
            if (input[j] == '+' || input[j] == '-') ++j;
            while (j < n && IsDigit(input[j])) ++j;
            if (j < n && input[j] == '.') {
                ++j;
                if (j >= n || !IsDigit(input[j])) {
                    error = "Malformed number at position " + std::to_string(start);
                    return false;
                }
                while (j < n && IsDigit(input[j])) ++j;
            }
            std::string num_str = input.substr(i, j - i);
            try {
                double value = std::stod(num_str);
                tokens.push_back({TokenType::NUMBER, num_str, value, start});
            } catch (const std::exception&) {
                error = "Malformed number '" + num_str + "' at position " + std::to_string(start);
                return false;
            }
            i = j;
            continue;
        }

        // Single-quoted string with '' escape
        if (c == '\'') {
            std::string value;
            size_t j = i + 1;
            bool terminated = false;
            while (j < n) {
                if (input[j] == '\'') {
                    if (j + 1 < n && input[j + 1] == '\'') {
                        value += '\'';
                        j += 2;
                    } else {
                        terminated = true;
                        ++j;
                        break;
                    }
                } else {
                    value += input[j];
                    ++j;
                }
            }
            if (!terminated) {
                error = "Unterminated string literal at position " + std::to_string(start);
                return false;
            }
            tokens.push_back({TokenType::STRING, value, 0.0, start});
            i = j;
            continue;
        }

        // Operators and parentheses
        switch (c) {
            case '=':
                tokens.push_back({TokenType::EQ, "=", 0.0, start});
                ++i;
                continue;
            case '!':
                if (i + 1 < n && input[i + 1] == '=') {
                    tokens.push_back({TokenType::NE, "!=", 0.0, start});
                    i += 2;
                    continue;
                }
                error = "Unexpected character '!' at position " + std::to_string(start);
                return false;
            case '<':
                if (i + 1 < n && input[i + 1] == '=') {
                    tokens.push_back({TokenType::LE, "<=", 0.0, start});
                    i += 2;
                } else if (i + 1 < n && input[i + 1] == '>') {
                    tokens.push_back({TokenType::NE, "<>", 0.0, start});
                    i += 2;
                } else {
                    tokens.push_back({TokenType::LT, "<", 0.0, start});
                    ++i;
                }
                continue;
            case '>':
                if (i + 1 < n && input[i + 1] == '=') {
                    tokens.push_back({TokenType::GE, ">=", 0.0, start});
                    i += 2;
                } else {
                    tokens.push_back({TokenType::GT, ">", 0.0, start});
                    ++i;
                }
                continue;
            case '(':
                tokens.push_back({TokenType::LPAREN, "(", 0.0, start});
                ++i;
                continue;
            case ')':
                tokens.push_back({TokenType::RPAREN, ")", 0.0, start});
                ++i;
                continue;
            default:
                error = std::string("Unexpected character '") + c + "' at position " +
                        std::to_string(start);
                return false;
        }
    }

    tokens.push_back({TokenType::END, "", 0.0, n});
    return true;
}

// ==================== LIKE pattern compilation ====================

// Converts a SQL LIKE pattern to an anchored regex: % -> .*, _ -> .,
// everything else is escaped literally.
std::string LikePatternToRegex(const std::string& pattern) {
    static const std::string kRegexSpecial = "\\^$.|?*+()[]{}";
    std::string result;
    result.reserve(pattern.size() * 2);
    for (char c : pattern) {
        if (c == '%') {
            result += ".*";
        } else if (c == '_') {
            result += '.';
        } else if (kRegexSpecial.find(c) != std::string::npos) {
            result += '\\';
            result += c;
        } else {
            result += c;
        }
    }
    return result;
}

// ==================== Recursive-descent parser ====================

class Parser {
public:
    explicit Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

    std::unique_ptr<PredicateNode> Parse(std::string& error) {
        auto expr = ParseExpr();
        if (!expr) {
            error = error_;
            return nullptr;
        }
        if (Peek().type != TokenType::END) {
            error = "Unexpected trailing input at position " + std::to_string(Peek().pos);
            return nullptr;
        }
        return expr;
    }

private:
    const Token& Peek() const { return tokens_[index_]; }

    const Token& Advance() { return tokens_[index_++]; }

    bool Match(TokenType type) {
        if (Peek().type == type) {
            ++index_;
            return true;
        }
        return false;
    }

    std::unique_ptr<PredicateNode> Fail(const std::string& message) {
        if (error_.empty()) {
            error_ = message + " at position " + std::to_string(Peek().pos);
        }
        return nullptr;
    }

    // expr := term (OR term)*
    std::unique_ptr<PredicateNode> ParseExpr() {
        auto left = ParseTerm();
        if (!left) return nullptr;

        while (Match(TokenType::KW_OR)) {
            auto right = ParseTerm();
            if (!right) return nullptr;

            auto node = std::make_unique<PredicateNode>();
            node->kind = PredicateNode::Kind::OR;
            node->left = std::move(left);
            node->right = std::move(right);
            left = std::move(node);
        }
        return left;
    }

    // term := factor (AND factor)*
    std::unique_ptr<PredicateNode> ParseTerm() {
        auto left = ParseFactor();
        if (!left) return nullptr;

        while (Match(TokenType::KW_AND)) {
            auto right = ParseFactor();
            if (!right) return nullptr;

            auto node = std::make_unique<PredicateNode>();
            node->kind = PredicateNode::Kind::AND;
            node->left = std::move(left);
            node->right = std::move(right);
            left = std::move(node);
        }
        return left;
    }

    // factor := '(' expr ')' | NOT factor | comparison
    std::unique_ptr<PredicateNode> ParseFactor() {
        if (Match(TokenType::LPAREN)) {
            auto expr = ParseExpr();
            if (!expr) return nullptr;
            if (!Match(TokenType::RPAREN)) {
                return Fail("Expected ')'");
            }
            return expr;
        }

        if (Match(TokenType::KW_NOT)) {
            auto operand = ParseFactor();
            if (!operand) return nullptr;

            auto node = std::make_unique<PredicateNode>();
            node->kind = PredicateNode::Kind::NOT;
            node->left = std::move(operand);
            return node;
        }

        return ParseComparison();
    }

    // comparison := ident op literal | ident LIKE 'pattern' | ident IS [NOT] NULL
    std::unique_ptr<PredicateNode> ParseComparison() {
        if (Peek().type != TokenType::IDENT) {
            return Fail("Expected column name");
        }
        std::string column = Advance().text;

        // ident IS [NOT] NULL
        if (Match(TokenType::KW_IS)) {
            bool negated = Match(TokenType::KW_NOT);
            if (!Match(TokenType::KW_NULL)) {
                return Fail("Expected NULL after IS");
            }
            auto node = std::make_unique<PredicateNode>();
            node->kind = PredicateNode::Kind::IS_NULL;
            node->column = std::move(column);
            node->negated = negated;
            return node;
        }

        // ident LIKE 'pattern'
        if (Match(TokenType::KW_LIKE)) {
            if (Peek().type != TokenType::STRING) {
                return Fail("Expected string pattern after LIKE");
            }
            std::string pattern = Advance().text;

            auto node = std::make_unique<PredicateNode>();
            node->kind = PredicateNode::Kind::LIKE;
            node->column = std::move(column);
            try {
                node->like_regex = std::regex(LikePatternToRegex(pattern));
            } catch (const std::regex_error&) {
                return Fail("Invalid LIKE pattern '" + pattern + "'");
            }
            return node;
        }

        // ident op literal
        PredicateNode::Op op;
        switch (Peek().type) {
            case TokenType::EQ: op = PredicateNode::Op::EQ; break;
            case TokenType::NE: op = PredicateNode::Op::NE; break;
            case TokenType::LT: op = PredicateNode::Op::LT; break;
            case TokenType::LE: op = PredicateNode::Op::LE; break;
            case TokenType::GT: op = PredicateNode::Op::GT; break;
            case TokenType::GE: op = PredicateNode::Op::GE; break;
            default:
                return Fail("Expected comparison operator");
        }
        Advance();

        auto node = std::make_unique<PredicateNode>();
        node->kind = PredicateNode::Kind::COMPARISON;
        node->column = std::move(column);
        node->op = op;

        const Token& literal = Peek();
        switch (literal.type) {
            case TokenType::STRING:
                node->literal_type = PredicateNode::LiteralType::STRING;
                node->string_value = literal.text;
                break;
            case TokenType::NUMBER:
                node->literal_type = PredicateNode::LiteralType::NUMBER;
                node->number_value = literal.number;
                break;
            case TokenType::KW_TRUE:
                node->literal_type = PredicateNode::LiteralType::BOOLEAN;
                node->bool_value = true;
                break;
            case TokenType::KW_FALSE:
                node->literal_type = PredicateNode::LiteralType::BOOLEAN;
                node->bool_value = false;
                break;
            case TokenType::KW_NULL:
                node->literal_type = PredicateNode::LiteralType::NULL_VALUE;
                break;
            default:
                return Fail("Expected literal after comparison operator");
        }
        Advance();

        return node;
    }

    std::vector<Token> tokens_;
    size_t index_ = 0;
    std::string error_;
};

// ==================== Evaluation ====================

template <typename T>
bool CompareOrdered(const T& lhs, const T& rhs, PredicateNode::Op op) {
    switch (op) {
        case PredicateNode::Op::EQ: return lhs == rhs;
        case PredicateNode::Op::NE: return lhs != rhs;
        case PredicateNode::Op::LT: return lhs < rhs;
        case PredicateNode::Op::LE: return lhs <= rhs;
        case PredicateNode::Op::GT: return lhs > rhs;
        case PredicateNode::Op::GE: return lhs >= rhs;
    }
    return false;
}

bool EvaluateComparison(const PredicateNode& node, const nlohmann::json& row) {
    auto it = row.find(node.column);

    // Missing column or null value: comparison is false (collapsed SQL NULL semantics)
    if (it == row.end() || it->is_null()) {
        return false;
    }

    // Comparison against NULL literal is always false
    if (node.literal_type == PredicateNode::LiteralType::NULL_VALUE) {
        return false;
    }

    const nlohmann::json& value = *it;

    switch (node.literal_type) {
        case PredicateNode::LiteralType::NUMBER:
            if (!value.is_number()) return false;  // type mismatch
            return CompareOrdered(value.get<double>(), node.number_value, node.op);

        case PredicateNode::LiteralType::STRING:
            if (!value.is_string()) return false;  // type mismatch
            return CompareOrdered(value.get<std::string>(), node.string_value, node.op);

        case PredicateNode::LiteralType::BOOLEAN:
            if (!value.is_boolean()) return false;  // type mismatch
            if (node.op == PredicateNode::Op::EQ) return value.get<bool>() == node.bool_value;
            if (node.op == PredicateNode::Op::NE) return value.get<bool>() != node.bool_value;
            return false;  // ordering on booleans is not supported

        default:
            return false;
    }
}

bool EvaluateNode(const PredicateNode& node, const nlohmann::json& row) {
    switch (node.kind) {
        case PredicateNode::Kind::OR:
            return EvaluateNode(*node.left, row) || EvaluateNode(*node.right, row);

        case PredicateNode::Kind::AND:
            return EvaluateNode(*node.left, row) && EvaluateNode(*node.right, row);

        case PredicateNode::Kind::NOT:
            return !EvaluateNode(*node.left, row);

        case PredicateNode::Kind::IS_NULL: {
            auto it = row.find(node.column);
            bool is_null = (it == row.end()) || it->is_null();
            return node.negated ? !is_null : is_null;
        }

        case PredicateNode::Kind::LIKE: {
            auto it = row.find(node.column);
            if (it == row.end() || !it->is_string()) {
                return false;  // LIKE matches string values only
            }
            return std::regex_match(it->get<std::string>(), node.like_regex);
        }

        case PredicateNode::Kind::COMPARISON:
            return EvaluateComparison(node, row);
    }
    return false;
}

} // namespace

// ==================== PredicateEvaluator ====================

PredicateEvaluator::PredicateEvaluator(std::unique_ptr<detail::PredicateNode> root)
    : root_(std::move(root)) {
}

PredicateEvaluator::~PredicateEvaluator() = default;

std::unique_ptr<PredicateEvaluator> PredicateEvaluator::Parse(
    const std::string& where_clause, std::string& error) {

    std::vector<Token> tokens;
    if (!Tokenize(where_clause, tokens, error)) {
        return nullptr;
    }

    Parser parser(std::move(tokens));
    auto root = parser.Parse(error);
    if (!root) {
        return nullptr;
    }

    return std::unique_ptr<PredicateEvaluator>(new PredicateEvaluator(std::move(root)));
}

bool PredicateEvaluator::Evaluate(const nlohmann::json& row) const {
    if (!row.is_object()) {
        return false;
    }
    return EvaluateNode(*root_, row);
}

} // namespace instantdb
