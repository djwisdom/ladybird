/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022, networkException <networkexception@serenityos.org>
 * Copyright (c) 2023, Kenneth Myhra <kennethmyhra@serenityos.org>
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>

namespace Web::Infra {

String normalize_newlines(String const&);
Utf16String normalize_newlines(Utf16String const&);
ErrorOr<String> strip_and_collapse_whitespace(StringView string);
Utf16String strip_and_collapse_whitespace(Utf16String const& string);
bool is_code_unit_prefix(StringView potential_prefix, StringView input);
ErrorOr<String> convert_to_scalar_value_string(StringView string);
ByteBuffer isomorphic_encode(StringView input);
String isomorphic_decode(ReadonlyBytes input);
bool code_unit_less_than(StringView a, StringView b);

}
