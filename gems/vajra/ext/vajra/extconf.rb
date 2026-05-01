# frozen_string_literal: true

# Copyright Codevedas Inc. 2025-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

require 'mkmf'

append_cppflags('-std=c++17')
append_cflags('-fvisibility=hidden')

source_files = Dir.chdir(__dir__) { Dir.glob('**/*.cpp') }
source_directories = source_files.map { |path| File.dirname(path) }.uniq.sort

TOPLEVEL_BINDING.eval('$VPATH').concat(
  source_directories
    .reject { |directory| directory == '.' }
    .map { |directory| "$(srcdir)/#{directory}" }
)

srcs = source_files.map { |path| File.expand_path(path, __dir__) }
# mkmf expects the $srcs global; bridge the local array into that API surface.
# rubocop:disable Style/DocumentDynamicEvalDefinition
TOPLEVEL_BINDING.eval("$srcs = ObjectSpace._id2ref(#{srcs.object_id})", __FILE__, __LINE__)
# rubocop:enable Style/DocumentDynamicEvalDefinition

create_makefile('vajra/vajra')
