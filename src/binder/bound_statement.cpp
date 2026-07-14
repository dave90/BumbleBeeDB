//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// bound_statement.cpp
//
// Identification: src/binder/bound_statement.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "binder/bound_statement.h"

namespace bumblebee {

BoundStatement::BoundStatement(StatementType type) : type_(type) {}

}  // namespace bumblebee
