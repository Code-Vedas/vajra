# frozen_string_literal: true

# Copyright Codevedas Inc. 2025-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

RSpec.describe Vajra do
  let(:failing_loader) do
    proc { |_path| raise LoadError, 'cannot load such file -- vajra/vajra' }
  end

  it 'has a version number' do
    expect(Vajra::VERSION).not_to be_nil
  end

  it 'loads the native extension through the canonical require path' do
    expect(
      [described_class.respond_to?(:start), described_class.respond_to?(:stop)]
    ).to eq([true, true])
  end

  it 'builds a versioned ASCII header' do
    expect(described_class.header).to include('__      __')
    expect(described_class.header).to include("v#{described_class::VERSION}")
  end

  it 'prints the header before delegating to the native start entrypoint' do
    allow(described_class).to receive(:native_start)

    expect { described_class.start }.to output(include("v#{described_class::VERSION}")).to_stdout
    expect(described_class).to have_received(:native_start)
  end

  it 'raises an actionable error when the native extension cannot be loaded' do
    expect { described_class::NativeExtension.load!(loader: failing_loader) }.to raise_error(
      LoadError,
      /bundle exec rake compile/
    )
  end

  it 'preserves the original load error backtrace' do
    failing_loader = proc do |_path|
      raise LoadError, 'cannot load such file -- vajra/vajra'
    rescue LoadError => e
      raise e, e.message, ['/tmp/native_extension.rb:12']
    end

    begin
      described_class::NativeExtension.load!(loader: failing_loader)
    rescue LoadError => e
      expect(e.backtrace.first).to eq('/tmp/native_extension.rb:12')
    end
  end
end
