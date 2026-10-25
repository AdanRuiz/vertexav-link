/*
  Q Light Controller Plus
  vertexavchannels.h

  Input channel map of the VertexAV Link plugin. These numbers are part of the plugin's
  public behaviour: users map them in input profiles and the virtual console, so they must
  not change between versions.

  Licensed under the Apache License, Version 2.0
*/

#ifndef VERTEXAVCHANNELS_H
#define VERTEXAVCHANNELS_H

#include <QString>
#include <QStringList>

namespace VertexAVChannels
{
    // Tempo
    const quint32 Beat = 1;            // pulse on every beat; also feeds the QLC+ beat generator
    const quint32 Downbeat = 2;        // pulse on the "1" of each bar
    const quint32 Bpm = 3;             // BPM - 50, clamped to 0..255
    const quint32 Playing = 4;         // 255 while music with tempo is playing

    // Intent and mix
    const quint32 Energy = 5;          // 0..255
    const quint32 TransitionProgress = 6;
    const quint32 TransitionActive = 7;
    const quint32 MixCut = 8;          // pulse

    // Sections of the deck on air (exclusive: one channel at 255, the rest at 0)
    const quint32 SectionBase = 10;
    inline const QStringList &sectionRoles()
    {
        static const QStringList roles = {
            "intro", "verse", "chorus", "build", "drop", "breakdown", "bridge", "outro", "silence", "unknown"
        };
        return roles;
    }

    // Countdowns (pulse): 16, 8, 4, 2, 1 beats before a drop or a big energy jump
    const quint32 CountdownBase = 20;
    inline const QList<int> &countdownBeats()
    {
        static const QList<int> beats = { 16, 8, 4, 2, 1 };
        return beats;
    }

    const quint32 StrobeOk = 30;
    const quint32 IntensityBase = 31;  // low, medium, high, peak (exclusive)
    inline const QStringList &intensities()
    {
        static const QStringList list = { "low", "medium", "high", "peak" };
        return list;
    }

    const quint32 MoodBase = 40;       // exclusive
    inline const QStringList &moods()
    {
        static const QStringList list = {
            "dark", "euphoric", "happy", "melancholic", "aggressive", "chill", "dreamy", "groovy", "epic", "neutral"
        };
        return list;
    }

    // Palette: up to 6 colours, 3 channels each (R, G, B)
    const quint32 PaletteBase = 50;
    const int PaletteColors = 6;

    // Decks 1..4
    const quint32 DeckAudibleBase = 70;
    const quint32 DeckPlayingBase = 74;
    const quint32 OnAirBase = 78;      // exclusive
    const int MaxDecks = 4;

    // Standard triggers (Annex B.3)
    const quint32 TriggerBase = 100;
    inline const QStringList &standardTriggers()
    {
        static const QStringList list = {
            "blackout", "fullon", "strobe", "freeze", "fog", "mode.chill", "mode.build", "mode.peak"
        };
        return list;
    }
    const quint32 ColorBase = 110;
    inline const QStringList &triggerColors()
    {
        static const QStringList list = { "red", "green", "blue", "white", "amber", "purple", "cyan", "pink" };
        return list;
    }
    const quint32 FxBase = 120;        // fx.1 .. fx.16
    const int FxCount = 16;
    const quint32 CustomBase = 200;    // other triggers: 200 + CRC16(name) % 800
    const quint32 CustomRange = 800;

    // Pulse length for momentary channels (ms)
    const int PulseMs = 60;
}

#endif
