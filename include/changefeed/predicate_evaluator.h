#pragma once

#include <memory>
#include <string>

#include <nlohmann/json.hpp>

namespace origindb {

namespace detail {
struct PredicateNode;
}

// Evaluates a SQL WHERE clause against a flat JSON row object (column -> value).
//
// Supported grammar (keywords case-insensitive, identifiers case-sensitive):
//   expr       := term (OR term)*
//   term       := factor (AND factor)*
//   factor     := '(' expr ')' | NOT factor | comparison
//   comparison := ident (= | != | <> | < | <= | > | >=) literal
//               | ident LIKE 'pattern'
//               | ident IS [NOT] NULL
//   literal    := 'string' | number | TRUE | FALSE | NULL
//
// Semantics: numeric comparisons coerce int/float; string comparisons are
// lexicographic. Comparing a string to a number, a missing column, or a null
// operand evaluates to false (SQL NULL semantics collapsed to false), except
// IS NULL which is true for missing or null columns. LIKE uses SQL wildcards
// (% and _) and matches string values only.
class PredicateEvaluator {
public:
    ~PredicateEvaluator();

    // Parses a WHERE clause expression.
    // Returns nullptr and sets `error` on parse failure.
    static std::unique_ptr<PredicateEvaluator> Parse(const std::string& where_clause,
                                                     std::string& error);

    // Evaluates the predicate against a flat JSON object mapping column
    // names to values. Returns false for non-object input.
    bool Evaluate(const nlohmann::json& row) const;

private:
    explicit PredicateEvaluator(std::unique_ptr<detail::PredicateNode> root);

    std::unique_ptr<detail::PredicateNode> root_;
};

} // namespace origindb
