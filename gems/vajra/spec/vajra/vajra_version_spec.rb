# frozen_string_literal: true

# Copyright Codevedas Inc. 2025-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

RSpec.describe 'Vajra::VERSION' do
  it 'has a version number' do
    expect(Vajra::VERSION).not_to be_nil
  end
end
