//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// vector_data_mngr.cpp
//
// Identification: src/type/vector/vector_data_mngr.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "type/vector/vector_data_mngr.h"

namespace bumblebee {

// Every member of the VectorDataMngr hierarchy is inlined in the header: the accessors run
// once per Vector operation and an out-of-line call per access showed up in profiles. This
// translation unit exists so the class has a home and its vtable is emitted here.

}  // namespace bumblebee
