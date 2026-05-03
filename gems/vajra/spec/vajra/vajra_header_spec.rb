# frozen_string_literal: true

# Copyright Codevedas Inc. 2025-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

RSpec.describe Vajra, '.header' do
  it 'builds a versioned ASCII header' do
    expect(described_class.header).to include('__      __')
    expect(described_class.header).to include("v#{described_class::VERSION}")
  end
end
