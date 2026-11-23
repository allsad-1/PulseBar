#pragma once
#include "model.h"
#include <string_view>

namespace pulse {
std::optional<Balance> parse_balance(std::string_view json);
std::optional<CodexQuota> parse_codex(std::string_view line);
std::optional<CodexQuota> read_codex();

struct BalanceResult {
    std::optional<Balance> balance;
    std::wstring status;
    bool retry = false;
    unsigned http_status = 0;
};
BalanceResult read_deepseek();
bool deepseek_credential_exists();
bool save_deepseek_key(std::wstring_view key);
bool remove_deepseek_key();
}
