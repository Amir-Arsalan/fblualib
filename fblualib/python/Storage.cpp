/*
 *  Copyright (c) 2014, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include "Storage.h"

namespace fblualib {
namespace python {

int initStorage(lua_State* /*L*/) {
  PythonGuard g;
  PythonStorage<double>::define();
  PythonStorage<float>::define();
  PythonStorage<int>::define();
  PythonStorage<long>::define();
  PythonStorage<unsigned char>::define();
  return 0;
}

}}  // namespaces
