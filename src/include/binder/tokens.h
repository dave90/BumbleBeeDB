//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// tokens.h
//
// Identification: src/include/binder/tokens.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

namespace bumblebee {

//===--------------------------------------------------------------------===//
// Statements
//===--------------------------------------------------------------------===//
class BoundStatement;

class CreateStatement;
class DeleteStatement;
class ExplainStatement;
class InsertStatement;
class SelectStatement;
class UpdateStatement;

//===--------------------------------------------------------------------===//
// Expressions
//===--------------------------------------------------------------------===//
class AbstractExpression;

class ColumnValueExpression;
class ComparisonExpression;
class ConstantValueExpression;

//===--------------------------------------------------------------------===//
// TableRefs
//===--------------------------------------------------------------------===//
class BoundTableRef;

}  // namespace bumblebee
