#pragma once

#include <QString>

namespace mtg::ai {

// API keys live in the macOS Keychain (generic passwords under the app's
// service name), never in settings files or project files.
namespace Keychain {
bool store(const QString &account, const QString &secret);
QString load(const QString &account);
bool remove(const QString &account);
bool has(const QString &account);
}  // namespace Keychain

}  // namespace mtg::ai
