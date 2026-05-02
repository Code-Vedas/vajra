# frozen_string_literal: true

# Copyright Codevedas Inc. 2025-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

RSpec.describe Vajra, '.start' do
  it 'keeps the native start entrypoint public' do
    expect(described_class).to respond_to(:start)
    expect(described_class.singleton_methods).not_to include(:native_start)
  end
end
