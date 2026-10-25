#include "vertexav/security.h"

#include <QCryptographicHash>
#include <QMessageAuthenticationCode>
#include <QRandomGenerator>
#include <cstring>

#include "vertexav/protocol.h"

namespace vertexav::security {

namespace {
const char kBase32[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
}

QByteArray randomBytes(int n) {
    QByteArray out(n, Qt::Uninitialized);
    // QRandomGenerator::system() uses the operating system's cryptographic generator.
    for (int i = 0; i < n; i += 4) {
        const quint32 v = QRandomGenerator::system()->generate();
        for (int j = 0; j < 4 && i + j < n; ++j) {
            out[i + j] = static_cast<char>((v >> (8 * j)) & 0xFF);
        }
    }
    return out;
}

QByteArray generatePsk() {
    return randomBytes(kPskLen);
}

QString pskToCode(const QByteArray& psk) {
    QString out;
    int buffer = 0;
    int bits = 0;
    for (const char c : psk) {
        buffer = (buffer << 8) | static_cast<quint8>(c);
        bits += 8;
        while (bits >= 5) {
            out.append(QLatin1Char(kBase32[(buffer >> (bits - 5)) & 31]));
            bits -= 5;
        }
    }
    if (bits > 0) {
        out.append(QLatin1Char(kBase32[(buffer << (5 - bits)) & 31]));
    }
    return out;
}

QByteArray codeToPsk(const QString& code, bool* ok) {
    QByteArray out;
    int buffer = 0;
    int bits = 0;
    for (const QChar qc : code) {
        const char c = qc.toUpper().toLatin1();
        if (c == ' ' || c == '-' || c == '=') {
            continue;
        }
        const char* p = std::strchr(kBase32, c);
        if (!p || c == 0) {
            if (ok) {
                *ok = false;
            }
            return {};
        }
        buffer = (buffer << 5) | static_cast<int>(p - kBase32);
        bits += 5;
        if (bits >= 8) {
            out.append(static_cast<char>((buffer >> (bits - 8)) & 0xFF));
            bits -= 8;
        }
    }
    if (ok) {
        *ok = out.size() == kPskLen;
    }
    return out.size() == kPskLen ? out : QByteArray();
}

QByteArray hmacSha256(const QByteArray& key, const QByteArray& message) {
    return QMessageAuthenticationCode::hash(message, key, QCryptographicHash::Sha256);
}

QByteArray hkdfSha256(const QByteArray& ikm, const QByteArray& salt, const QByteArray& info, int length) {
    const QByteArray prk = hmacSha256(salt, ikm);
    QByteArray out;
    QByteArray block;
    quint8 counter = 1;
    while (out.size() < length) {
        block = hmacSha256(prk, block + info + QByteArray(1, static_cast<char>(counter)));
        out.append(block);
        ++counter;
    }
    return out.left(length);
}

QByteArray clientProof(const QByteArray& psk, const QByteArray& serverNonce,
        const QByteArray& clientNonce, const QString& clientId) {
    return hmacSha256(psk, QByteArray("vertexav-auth-client") + serverNonce + clientNonce + clientId.toUtf8());
}

QByteArray serverProof(const QByteArray& psk, const QByteArray& clientNonce,
        const QByteArray& serverNonce, const QString& sessionId) {
    return hmacSha256(psk, QByteArray("vertexav-auth-server") + clientNonce + serverNonce + sessionId.toUtf8());
}

QByteArray macKey(const QByteArray& psk, const QByteArray& serverNonce, const QByteArray& clientNonce) {
    return hkdfSha256(psk, serverNonce + clientNonce, QByteArray("vertexav-udp-mac-v1"), 32);
}

QByteArray sign(const QByteArray& key, const QByteArray& data) {
    return hmacSha256(key, data).left(kSigLen);
}

bool constantTimeEquals(const QByteArray& a, const QByteArray& b) {
    if (a.size() != b.size()) {
        return false;
    }
    unsigned char diff = 0;
    for (int i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    }
    return diff == 0;
}

bool verify(const QByteArray& key, const QByteArray& data, const QByteArray& sig) {
    return constantTimeEquals(sign(key, data), sig);
}

} // namespace vertexav::security
