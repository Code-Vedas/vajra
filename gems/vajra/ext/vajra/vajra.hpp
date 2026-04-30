// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_EXTENSION_HPP
#define VAJRA_EXTENSION_HPP

#include "server.hpp"

namespace VajraNative
{
  bool shutdown_requested();
  void start();
  void stop();
}

#endif
