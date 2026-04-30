# frozen_string_literal: true

# Copyright Codevedas Inc. 2025-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

require_relative 'vajra/version'

# Ruby entrypoint for booting the native Vajra HTTP listener.
module Vajra
  # Base error type for Ruby-side Vajra failures.
  class Error < StandardError; end

  # Loads the compiled native entrypoint through the canonical package path.
  module NativeExtension
    module_function

    def load!(loader: method(:require), extension_path: File.expand_path('vajra/vajra', __dir__))
      loader.call(extension_path)
    rescue LoadError => e
      raise LoadError, <<~MESSAGE, e.backtrace
        Unable to load the Vajra native extension.
        Run `bundle exec rake compile` from the `gems/vajra/` package directory and retry.
        Original error: #{e.message}
      MESSAGE
    end
  end

  NativeExtension.load!

  class << self
    alias native_start start

    def start
      puts header
      native_start
    end

    def header
      art = <<~'TEXT'
        __      __  _         _   ____       _
        \ \    / / / \       | | |  _ \     / \
         \ \  / / / _ \   _  | | | |_) |   / _ \
          \ \/ / / ___ \ | |_| | |  _ <   / ___ \
           \__/ /_/   \_\ \___/  |_| \_\ /_/   \_\
      TEXT

      "#{art}\nv#{Vajra::VERSION}\n"
    end
  end
end
