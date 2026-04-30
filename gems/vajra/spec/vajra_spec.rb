# frozen_string_literal: true

# Copyright Codevedas Inc. 2025-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

RSpec.describe Vajra do
  let(:failing_loader) do
    proc { |_path| raise LoadError, "cannot load such file -- vajra/vajra" }
  end

  it "has a version number" do
    expect(Vajra::VERSION).not_to be_nil
  end

  it "loads the native extension through the canonical require path" do
    expect(
      [described_class.respond_to?(:start), described_class.respond_to?(:stop)]
    ).to eq([true, true])
  end

  it "raises an actionable error when the native extension cannot be loaded" do
    expect { described_class::NativeExtension.load!(loader: failing_loader) }.to raise_error(
      LoadError,
      /bundle exec rake compile/
    )
  end
end
