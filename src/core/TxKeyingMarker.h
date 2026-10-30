#pragma once

#include <QRegularExpression>
#include <QStringList>
#include <QWidget>

namespace AetherSDR {

// Authoritative marker for controls that *key the transmitter* when activated —
// MOX/PTT, TUNE, ATU tune, CWX CW send, AX.25 packet send, and any future
// keying control. The #3646 automation bridge refuses invoke() on any widget
// carrying this marker unless AETHER_AUTOMATION_ALLOW_TX is set, so an agent can
// never key a live radio by accident during hardware-in-the-loop tests.
//
// This is a *positive* signal set at the keying control's creation site, which
// is far more robust than matching control names: a TX-capable control is
// guarded because it was explicitly declared keying, not because its label
// happened to contain a magic word. Name matching remains in the bridge only as
// a logged belt-and-suspenders fallback for controls that predate or forget the
// marker.
//
// Usage at the call site, right after creating the button:
//     m_moxBtn = new QPushButton("MOX");
//     markTxKeying(m_moxBtn);
inline constexpr char kTxKeyingProperty[] = "aetherTxKeying";

inline void markTxKeying(QWidget* w)
{
    if (w)
        w->setProperty(kTxKeyingProperty, true);
}

// A TX-keying control whose switch-OFF only lowers TX risk — the VOX arm. The
// bridge accepts `setChecked false` on it (or 0 / off / no / unchecked) without
// TX permission, so an agent can always disarm VOX; every other action on it
// still needs permission (#6053, upstream f42d432b). Mark the control with
// markTxKeying() too: this only relaxes the guard for the disarm.
inline constexpr char kTxDisarmAllowedProperty[] = "aetherTxDisarmAllowed";

inline void markTxDisarmAllowed(QWidget* w)
{
    if (w)
        w->setProperty(kTxDisarmAllowedProperty, true);
}

// True only for an explicit switch-off of a markTxDisarmAllowed() control. Each
// accepted spelling is one the bridge's boolean parser reads as false.
inline bool isTxDisarmRequest(const QObject* object, const QString& action,
                              const QString& value)
{
    if (!object || !object->property(kTxDisarmAllowedProperty).toBool()
        || action != QLatin1String("setChecked"))
        return false;
    const QString v = value.trimmed().toLower();
    return v == QLatin1String("false") || v == QLatin1String("0")
        || v == QLatin1String("off") || v == QLatin1String("no")
        || v == QLatin1String("unchecked");
}

// Lowercased words of an identifier or label, split on separators and camelCase
// humps (aprsSvcWXBOT -> [aprs, svc, wxbot]). The TX-guard fallback matches a
// deny-word against a WHOLE token, so "svc"+"wxbot" never reads as "cwx" (#3646).
inline QStringList identifierTokens(const QString& s)
{
    QString spaced;
    spaced.reserve(s.size() * 2);
    for (int i = 0; i < s.size(); ++i) {
        const QChar c = s.at(i);
        // Break at a lower/digit -> Upper hump (tuneButton -> "tune Button") and
        // at an acronym -> word hump (WXBot -> "WX Bot"); runs of caps stay whole
        // (WXBOT -> "wxbot").
        if (i > 0 && c.isUpper()
            && (s.at(i - 1).isLower() || s.at(i - 1).isDigit()
                || (i + 1 < s.size() && s.at(i + 1).isLower())))
            spaced.append(QLatin1Char(' '));
        spaced.append(c);
    }
    static const QRegularExpression kSeparators(QStringLiteral("[^a-z0-9]+"));
    return spaced.toLower().split(kSeparators, Qt::SkipEmptyParts);
}

// True if any haystack contributes a whole token equal to a deny-word — the
// anchored TX-guard fallback match.
inline bool matchesTxDenyToken(const QStringList& haystacks, const QStringList& deny)
{
    for (const QString& h : haystacks) {
        const QStringList tokens = identifierTokens(h);
        for (const QString& d : deny)
            if (tokens.contains(d))
                return true;
    }
    return false;
}

// The fallback deny-list: only words that unambiguously mean "keys TX".
// "tune"/"atu"/"vox" also name RX-only controls ("Tune Now", a VOX arm toggle);
// the keying TUNE/ATU buttons carry markTxKeying() instead (#3918).
inline const QStringList& txDenyTokens()
{
    static const QStringList kDeny = {
        QStringLiteral("mox"), QStringLiteral("ptt"),
        QStringLiteral("transmit"), QStringLiteral("cwx"),
    };
    return kDeny;
}

enum class TransmitControlMatch {
    None,          // not a transmit control
    Marker,        // carries the authoritative markTxKeying() marker
    NameFallback,  // unmarked button whose name/label reads as a TX keyer
};

// Is this widget a transmit control? One answer for the automation bridge and
// the keyboard activation guard. The marker is authoritative; the name match is
// a button-only fallback, and a NameFallback control should get markTxKeying().
inline TransmitControlMatch transmitControlMatch(const QWidget* w)
{
    if (!w)
        return TransmitControlMatch::None;
    if (w->property(kTxKeyingProperty).toBool())
        return TransmitControlMatch::Marker;

    // By meta-object name and Q_PROPERTY, not qobject_cast: this header sits in
    // src/core and must not include a QtWidgets class header (engine boundary).
    if (!w->inherits("QAbstractButton"))
        return TransmitControlMatch::None;  // sliders / combos / spinboxes can't trigger TX

    if (w->objectName().startsWith(QStringLiteral("panOverlayMessageClose_")))
        return TransmitControlMatch::None;  // closes an overlay notification, never keys TX.

    const QStringList hay{w->objectName(), w->accessibleName(),
                          w->property("text").toString()};
    return matchesTxDenyToken(hay, txDenyTokens()) ? TransmitControlMatch::NameFallback
                                                   : TransmitControlMatch::None;
}

} // namespace AetherSDR
