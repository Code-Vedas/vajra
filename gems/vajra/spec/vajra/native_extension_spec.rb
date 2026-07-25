# frozen_string_literal: true

# Copyright Codevedas Inc. 2025-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

RSpec.describe Vajra::NativeExtension do
  let(:failing_loader) do
    proc { |_path| raise LoadError, 'cannot load such file -- vajra/vajra' }
  end

  it 'raises an actionable error when the native extension cannot be loaded' do
    expect { described_class.load!(loader: failing_loader) }.to raise_error(
      LoadError,
      /bundle exec rake compile/
    )
  end

  it 'preserves the original load error backtrace' do
    backtrace_loader = proc do |_path|
      raise LoadError, 'cannot load such file -- vajra/vajra'
    rescue LoadError => e
      raise e, e.message, ['/tmp/native_extension.rb:12']
    end

    expect { described_class.load!(loader: backtrace_loader) }.to raise_error(LoadError) do |e|
      expect(e.backtrace.first).to eq('/tmp/native_extension.rb:12')
    end
  end

  it 'loads only the native extension for the current platform' do
    loader = instance_double(Method, call: true)

    described_class.load!(loader: loader)

    expect(loader).to have_received(:call).with(
      File.expand_path("../../lib/vajra/vajra.#{RbConfig::CONFIG.fetch('DLEXT')}", __dir__)
    )
  end

  it 'normalizes a callable loader result to a boolean' do
    loader = instance_double(Method, call: Object.new)

    expect(described_class.load!(loader: loader, extension_path: '/tmp/vajra.bundle')).to be(true)
  end
end
