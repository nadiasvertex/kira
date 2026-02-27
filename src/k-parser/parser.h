#pragma once

#include <string>
#include <vector>
#include <memory>

namespace kparser {

class Parser {
 public:
  Parser();
  ~Parser();

  // Parse input string and return result
  bool Parse(const std::string& input);

  // Get the parsed tokens
  const std::vector<std::string>& GetTokens() const;

 private:
  std::vector<std::string> tokens_;
};

}  // namespace kparser
