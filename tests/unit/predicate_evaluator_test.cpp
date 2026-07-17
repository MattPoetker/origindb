#include "changefeed/predicate_evaluator.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using origindb::PredicateEvaluator;
using nlohmann::json;

namespace {

// Parses a WHERE clause, asserting success.
std::unique_ptr<PredicateEvaluator> MustParse(const std::string& clause) {
    std::string error;
    auto evaluator = PredicateEvaluator::Parse(clause, error);
    EXPECT_NE(evaluator, nullptr) << "Failed to parse '" << clause << "': " << error;
    return evaluator;
}

// Parses a WHERE clause and evaluates it against a row.
bool Eval(const std::string& clause, const json& row) {
    auto evaluator = MustParse(clause);
    if (!evaluator) return false;
    return evaluator->Evaluate(row);
}

// Asserts that a clause fails to parse.
void ExpectParseError(const std::string& clause) {
    std::string error;
    auto evaluator = PredicateEvaluator::Parse(clause, error);
    EXPECT_EQ(evaluator, nullptr) << "Expected parse failure for '" << clause << "'";
    EXPECT_FALSE(error.empty()) << "Expected error message for '" << clause << "'";
}

// ==================== Comparison operators ====================

TEST(PredicateEvaluatorTest, EqualityString) {
    json row = {{"name", "alice"}};
    EXPECT_TRUE(Eval("name = 'alice'", row));
    EXPECT_FALSE(Eval("name = 'bob'", row));
    EXPECT_TRUE(Eval("name != 'bob'", row));
    EXPECT_FALSE(Eval("name != 'alice'", row));
    EXPECT_TRUE(Eval("name <> 'bob'", row));
}

TEST(PredicateEvaluatorTest, EqualityNumber) {
    json row = {{"age", 30}};
    EXPECT_TRUE(Eval("age = 30", row));
    EXPECT_FALSE(Eval("age = 31", row));
    EXPECT_TRUE(Eval("age != 31", row));
    EXPECT_TRUE(Eval("age <> 31", row));
}

TEST(PredicateEvaluatorTest, OrderingNumbers) {
    json row = {{"age", 30}};
    EXPECT_TRUE(Eval("age > 20", row));
    EXPECT_FALSE(Eval("age > 30", row));
    EXPECT_TRUE(Eval("age >= 30", row));
    EXPECT_TRUE(Eval("age < 40", row));
    EXPECT_FALSE(Eval("age < 30", row));
    EXPECT_TRUE(Eval("age <= 30", row));
}

TEST(PredicateEvaluatorTest, IntFloatCoercion) {
    json int_row = {{"score", 10}};
    json float_row = {{"score", 10.0}};
    EXPECT_TRUE(Eval("score = 10.0", int_row));
    EXPECT_TRUE(Eval("score = 10", float_row));
    EXPECT_TRUE(Eval("score > 9.5", int_row));
    EXPECT_TRUE(Eval("score < 10.5", int_row));
}

TEST(PredicateEvaluatorTest, NegativeAndSignedNumbers) {
    json row = {{"balance", -5}};
    EXPECT_TRUE(Eval("balance = -5", row));
    EXPECT_TRUE(Eval("balance < 0", row));
    EXPECT_TRUE(Eval("balance > -10", row));
    EXPECT_TRUE(Eval("balance = -5.0", row));
    EXPECT_TRUE(Eval("balance < +1", row));
}

TEST(PredicateEvaluatorTest, StringOrderingLexicographic) {
    json row = {{"name", "bob"}};
    EXPECT_TRUE(Eval("name > 'alice'", row));
    EXPECT_TRUE(Eval("name < 'carol'", row));
    EXPECT_TRUE(Eval("name >= 'bob'", row));
    EXPECT_TRUE(Eval("name <= 'bob'", row));
    EXPECT_FALSE(Eval("name > 'bob'", row));
}

TEST(PredicateEvaluatorTest, BooleanLiterals) {
    json row = {{"active", true}, {"deleted", false}};
    EXPECT_TRUE(Eval("active = TRUE", row));
    EXPECT_TRUE(Eval("active = true", row));   // keywords case-insensitive
    EXPECT_FALSE(Eval("active = FALSE", row));
    EXPECT_TRUE(Eval("deleted = FALSE", row));
    EXPECT_TRUE(Eval("active != FALSE", row));
    // Ordering on booleans is not supported
    EXPECT_FALSE(Eval("active > FALSE", row));
}

// ==================== AND/OR precedence, parens, NOT ====================

TEST(PredicateEvaluatorTest, AndOr) {
    json row = {{"a", 1}, {"b", 2}};
    EXPECT_TRUE(Eval("a = 1 AND b = 2", row));
    EXPECT_FALSE(Eval("a = 1 AND b = 3", row));
    EXPECT_TRUE(Eval("a = 1 OR b = 3", row));
    EXPECT_FALSE(Eval("a = 0 OR b = 3", row));
}

TEST(PredicateEvaluatorTest, AndBindsTighterThanOr) {
    json row = {{"a", 1}, {"b", 0}, {"c", 0}};
    // Parsed as: a = 1 OR (b = 1 AND c = 1) -> true
    EXPECT_TRUE(Eval("a = 1 OR b = 1 AND c = 1", row));
    // Parsed as: (a = 1 AND b = 1) OR c = 0 -> true via the right side
    EXPECT_TRUE(Eval("a = 1 AND b = 1 OR c = 0", row));
    // Both AND groups false
    EXPECT_FALSE(Eval("a = 0 AND b = 0 OR c = 1 AND a = 1", row));
}

TEST(PredicateEvaluatorTest, Parentheses) {
    json row = {{"a", 1}, {"b", 0}, {"c", 0}};
    // Parens override precedence: (a = 1 OR b = 1) AND c = 1 -> false
    EXPECT_FALSE(Eval("(a = 1 OR b = 1) AND c = 1", row));
    EXPECT_TRUE(Eval("(a = 1 OR b = 1) AND c = 0", row));
    EXPECT_TRUE(Eval("((a = 1))", row));
}

TEST(PredicateEvaluatorTest, Not) {
    json row = {{"a", 1}};
    EXPECT_FALSE(Eval("NOT a = 1", row));
    EXPECT_TRUE(Eval("NOT a = 2", row));
    EXPECT_TRUE(Eval("NOT (a = 2 AND a = 1)", row));
    EXPECT_TRUE(Eval("NOT NOT a = 1", row));
}

TEST(PredicateEvaluatorTest, KeywordsCaseInsensitive) {
    json row = {{"a", 1}, {"b", 2}};
    EXPECT_TRUE(Eval("a = 1 and b = 2", row));
    EXPECT_TRUE(Eval("a = 1 Or b = 3", row));
    EXPECT_TRUE(Eval("not a = 2", row));
}

TEST(PredicateEvaluatorTest, IdentifiersCaseSensitive) {
    json row = {{"Name", "alice"}};
    EXPECT_TRUE(Eval("Name = 'alice'", row));
    EXPECT_FALSE(Eval("name = 'alice'", row));  // different column, missing -> false
}

// ==================== LIKE ====================

TEST(PredicateEvaluatorTest, LikePercentWildcard) {
    json row = {{"email", "alice@example.com"}};
    EXPECT_TRUE(Eval("email LIKE '%@example.com'", row));
    EXPECT_TRUE(Eval("email LIKE 'alice%'", row));
    EXPECT_TRUE(Eval("email LIKE '%example%'", row));
    EXPECT_FALSE(Eval("email LIKE '%@other.com'", row));
}

TEST(PredicateEvaluatorTest, LikeUnderscoreWildcard) {
    json row = {{"code", "a1c"}};
    EXPECT_TRUE(Eval("code LIKE 'a_c'", row));
    EXPECT_FALSE(Eval("code LIKE 'a_d'", row));
    EXPECT_FALSE(Eval("code LIKE 'a_'", row));  // must match the whole value
}

TEST(PredicateEvaluatorTest, LikeExactMatchWithoutWildcards) {
    json row = {{"name", "alice"}};
    EXPECT_TRUE(Eval("name LIKE 'alice'", row));
    EXPECT_FALSE(Eval("name LIKE 'alic'", row));
}

TEST(PredicateEvaluatorTest, LikeEscapesRegexSpecialCharacters) {
    json row = {{"path", "a.b*c"}};
    // '.' and '*' in the pattern must be literal, not regex metacharacters
    EXPECT_TRUE(Eval("path LIKE 'a.b*c'", row));
    EXPECT_FALSE(Eval("path LIKE 'aXb*c'", row));
    json row2 = {{"path", "axbyc"}};
    EXPECT_FALSE(Eval("path LIKE 'a.b*c'", row2));
}

TEST(PredicateEvaluatorTest, LikeMatchesStringValuesOnly) {
    json row = {{"age", 30}};
    EXPECT_FALSE(Eval("age LIKE '3%'", row));
}

// ==================== IS NULL / IS NOT NULL ====================

TEST(PredicateEvaluatorTest, IsNull) {
    json row = {{"a", nullptr}, {"b", 1}};
    EXPECT_TRUE(Eval("a IS NULL", row));
    EXPECT_FALSE(Eval("b IS NULL", row));
    EXPECT_TRUE(Eval("missing IS NULL", row));  // missing column is null
}

TEST(PredicateEvaluatorTest, IsNotNull) {
    json row = {{"a", nullptr}, {"b", 1}};
    EXPECT_FALSE(Eval("a IS NOT NULL", row));
    EXPECT_TRUE(Eval("b IS NOT NULL", row));
    EXPECT_FALSE(Eval("missing IS NOT NULL", row));
}

// ==================== Missing columns and null semantics ====================

TEST(PredicateEvaluatorTest, MissingColumnComparisonIsFalse) {
    json row = {{"a", 1}};
    EXPECT_FALSE(Eval("missing = 1", row));
    EXPECT_FALSE(Eval("missing != 1", row));  // collapsed null semantics
    EXPECT_FALSE(Eval("missing < 1", row));
    EXPECT_FALSE(Eval("missing LIKE '%'", row));
}

TEST(PredicateEvaluatorTest, NullColumnComparisonIsFalse) {
    json row = {{"a", nullptr}};
    EXPECT_FALSE(Eval("a = 1", row));
    EXPECT_FALSE(Eval("a != 1", row));
    EXPECT_FALSE(Eval("a = 'x'", row));
}

TEST(PredicateEvaluatorTest, ComparisonToNullLiteralIsFalse) {
    json row = {{"a", 1}, {"b", nullptr}};
    EXPECT_FALSE(Eval("a = NULL", row));
    EXPECT_FALSE(Eval("b = NULL", row));  // use IS NULL instead
    EXPECT_FALSE(Eval("a != NULL", row));
}

TEST(PredicateEvaluatorTest, NotOfMissingColumnComparison) {
    json row = {{"a", 1}};
    // Collapsed semantics: missing = 1 is false, so NOT of it is true
    EXPECT_TRUE(Eval("NOT missing = 1", row));
}

// ==================== Type mismatch ====================

TEST(PredicateEvaluatorTest, StringVersusNumberMismatchIsFalse) {
    json row = {{"age", 30}, {"name", "42"}};
    EXPECT_FALSE(Eval("age = '30'", row));   // number column vs string literal
    EXPECT_FALSE(Eval("name = 42", row));    // string column vs number literal
    EXPECT_FALSE(Eval("name < 100", row));
    EXPECT_FALSE(Eval("age > 'x'", row));
}

TEST(PredicateEvaluatorTest, BooleanMismatchIsFalse) {
    json row = {{"active", true}};
    EXPECT_FALSE(Eval("active = 1", row));
    EXPECT_FALSE(Eval("active = 'true'", row));
}

// ==================== Quote escaping ====================

TEST(PredicateEvaluatorTest, EscapedQuotesInStrings) {
    json row = {{"name", "o'brien"}};
    EXPECT_TRUE(Eval("name = 'o''brien'", row));
    EXPECT_FALSE(Eval("name = 'obrien'", row));
}

TEST(PredicateEvaluatorTest, EscapedQuotesInLikePattern) {
    json row = {{"name", "o'brien"}};
    EXPECT_TRUE(Eval("name LIKE 'o''%'", row));
}

TEST(PredicateEvaluatorTest, EmptyStringLiteral) {
    json row = {{"name", ""}};
    EXPECT_TRUE(Eval("name = ''", row));
    EXPECT_FALSE(Eval("name != ''", row));
}

// ==================== Parse errors ====================

TEST(PredicateEvaluatorTest, ParseErrors) {
    ExpectParseError("");                       // empty clause
    ExpectParseError("name =");                 // missing literal
    ExpectParseError("= 'x'");                  // missing column
    ExpectParseError("name = 'x' AND");         // dangling AND
    ExpectParseError("name = 'x' OR");          // dangling OR
    ExpectParseError("(name = 'x'");            // unbalanced paren
    ExpectParseError("name = 'x')");            // trailing paren
    ExpectParseError("name = 'unterminated");   // unterminated string
    ExpectParseError("name == 'x'");            // invalid operator
    ExpectParseError("name = 'x' extra");       // trailing tokens
    ExpectParseError("name LIKE 42");           // LIKE requires string pattern
    ExpectParseError("name IS");                // incomplete IS NULL
    ExpectParseError("name IS NOT");            // incomplete IS NOT NULL
    ExpectParseError("name @ 'x'");             // unexpected character
    ExpectParseError("age = 1.2.3");            // malformed number... tokenizes as 1.2 then .3 -> trailing
    ExpectParseError("NOT");                    // NOT without operand
    ExpectParseError("name = NULL NULL");       // trailing keyword
}

TEST(PredicateEvaluatorTest, ParseErrorSetsMessage) {
    std::string error;
    auto evaluator = PredicateEvaluator::Parse("name =", error);
    EXPECT_EQ(evaluator, nullptr);
    EXPECT_FALSE(error.empty());
}

// ==================== Miscellaneous ====================

TEST(PredicateEvaluatorTest, NonObjectRowIsFalse) {
    auto evaluator = MustParse("a = 1");
    ASSERT_NE(evaluator, nullptr);
    EXPECT_FALSE(evaluator->Evaluate(json::array()));
    EXPECT_FALSE(evaluator->Evaluate(json(42)));
    EXPECT_FALSE(evaluator->Evaluate(json()));
}

TEST(PredicateEvaluatorTest, ComplexExpression) {
    json row = {{"status", "active"}, {"age", 25}, {"email", "a@b.com"}, {"deleted_at", nullptr}};
    EXPECT_TRUE(Eval(
        "status = 'active' AND (age >= 18 AND age < 65) "
        "AND email LIKE '%@b.com' AND deleted_at IS NULL",
        row));
    EXPECT_FALSE(Eval(
        "status = 'inactive' OR age < 18 OR deleted_at IS NOT NULL",
        row));
}

} // namespace
