# frozen_string_literal: true

# Copyright Codevedas Inc. 2025-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

require_relative "vajra/version"

module Vajra
  # Base error type for Ruby-side Vajra failures.
  class Error < StandardError; end

  # Loads the compiled native entrypoint through the canonical package path.
  module NativeExtension
    module_function

    def load!(loader: method(:require_relative))
      loader.call("vajra/vajra")
    rescue LoadError => e
      raise LoadError, <<~MESSAGE
        Unable to load the Vajra native extension.
        Run `bundle exec rake compile` from the `gems/vajra/` package directory and retry.
        Original error: #{e.message}
      MESSAGE
    end
  end
end

Vajra::NativeExtension.load!
