#include "Keychain.h"

#include <QByteArray>

#ifdef Q_OS_MACOS
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#endif

namespace mtg::ai::Keychain {

#ifdef Q_OS_MACOS

namespace {

constexpr const char *kService = "dev.montage.editor";

struct CFRef {
    CFTypeRef ref = nullptr;
    ~CFRef()
    {
        if (ref) CFRelease(ref);
    }
};

CFStringRef cfString(const QString &s)
{
    const QByteArray utf8 = s.toUtf8();
    return CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8 *>(utf8.constData()), utf8.size(),
                                   kCFStringEncodingUTF8, false);
}

// Base query for this app's item for `account`; caller releases.
CFMutableDictionaryRef baseQuery(const QString &account)
{
    CFMutableDictionaryRef q = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                                                         &kCFTypeDictionaryValueCallBacks);
    CFRef service{cfString(QString::fromLatin1(kService))};
    CFRef acct{cfString(account)};
    CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(q, kSecAttrService, service.ref);
    CFDictionarySetValue(q, kSecAttrAccount, acct.ref);
    return q;
}

}  // namespace

bool store(const QString &account, const QString &secret)
{
    if (secret.isEmpty()) return remove(account);
    const QByteArray bytes = secret.toUtf8();
    CFRef data{CFDataCreate(nullptr, reinterpret_cast<const UInt8 *>(bytes.constData()), bytes.size())};

    CFRef query{baseQuery(account)};
    CFRef update{CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks)};
    CFDictionarySetValue(static_cast<CFMutableDictionaryRef>(const_cast<void *>(update.ref)), kSecValueData, data.ref);
    OSStatus st = SecItemUpdate(static_cast<CFDictionaryRef>(query.ref), static_cast<CFDictionaryRef>(update.ref));
    if (st == errSecItemNotFound) {
        auto *add = static_cast<CFMutableDictionaryRef>(const_cast<void *>(query.ref));
        CFDictionarySetValue(add, kSecValueData, data.ref);
        CFDictionarySetValue(add, kSecAttrAccessible, kSecAttrAccessibleAfterFirstUnlock);
        st = SecItemAdd(add, nullptr);
    }
    return st == errSecSuccess;
}

QString load(const QString &account)
{
    CFRef query{baseQuery(account)};
    auto *q = static_cast<CFMutableDictionaryRef>(const_cast<void *>(query.ref));
    CFDictionarySetValue(q, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(q, kSecMatchLimit, kSecMatchLimitOne);
    CFRef result;
    if (SecItemCopyMatching(q, &result.ref) != errSecSuccess || !result.ref) return {};
    auto *data = static_cast<CFDataRef>(result.ref);
    return QString::fromUtf8(reinterpret_cast<const char *>(CFDataGetBytePtr(data)), CFDataGetLength(data));
}

bool remove(const QString &account)
{
    CFRef query{baseQuery(account)};
    const OSStatus st = SecItemDelete(static_cast<CFDictionaryRef>(query.ref));
    return st == errSecSuccess || st == errSecItemNotFound;
}

bool has(const QString &account)
{
    return !load(account).isEmpty();
}

#else

bool store(const QString &, const QString &) { return false; }
QString load(const QString &) { return {}; }
bool remove(const QString &) { return true; }
bool has(const QString &) { return false; }

#endif

}  // namespace mtg::ai::Keychain
