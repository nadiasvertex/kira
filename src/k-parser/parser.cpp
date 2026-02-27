#include "parser.h"

#include <sstream>
#include <algorithm>

namespace kparser {

Parser::Parser() = default;

Parser::~Parser() = default;

bool Parser::Parse(const std::string& input) {
  tokens_.clear();
  
  // Simple tokenization by whitespace
  std::istringstream iss(input);
  std::string token;
  
  while (iss >> token) {
    tokens_.push_back(token);
  }
  
  return !tokens_.empty();
}

const std::vector<std::string>& Parser::GetTokens() const {
  return tokens_;
}

}  // namespace kparser