// Public tokenizer declaration
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace kretrieve {

// Lowercase ASCII, split on non-alphanumeric, drop English stopwords.
std::vector<std::string> tokenize(std::string_view text);

}  // namespace kretrieve
