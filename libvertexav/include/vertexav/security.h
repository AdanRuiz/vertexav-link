// Security primitives (§15): pairing keys, challenge-response, HKDF, UDP signatures.
#pragma once

#include <QByteArray>
#include <QString>

namespace vertexav::security {

inline constexpr int kNonceLen = 16;
inline constexpr int kPskLen = 32;

QByteArray randomBytes(int n); // OS cryptographic generator
QByteArray generatePsk();
QString pskToCode(const QByteArray& psk); // base32 without padding, 52 chars (§15.3)
QByteArray codeToPsk(const QString& code, bool* ok = nullptr);

QByteArray hmacSha256(const QByteArray& key, const QByteArray& message);
QByteArray hkdfSha256(const QByteArray& ikm, const QByteArray& salt, const QByteArray& info, int length);

QByteArray clientProof(const QByteArray& psk, const QByteArray& serverNonce,
        const QByteArray& clientNonce, const QString& clientId);
QByteArray serverProof(const QByteArray& psk, const QByteArray& clientNonce,
        const QByteArray& serverNonce, const QString& sessionId);
QByteArray macKey(const QByteArray& psk, const QByteArray& serverNonce, const QByteArray& clientNonce);

QByteArray sign(const QByteArray& macKey, const QByteArray& data); // 8 bytes (§15.5)
bool verify(const QByteArray& macKey, const QByteArray& data, const QByteArray& sig);
bool constantTimeEquals(const QByteArray& a, const QByteArray& b);

} // namespace vertexav::security
