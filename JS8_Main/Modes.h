#ifndef MODES_HPP__
#define MODES_HPP__

#include "qt_helpers.h"

#include <QAbstractListModel>

class QString;
class QVariant;
class QModelIndex;

//
// Class Modes - Qt model that implements a list of data modes
//
//
// Responsibilities
//
// 	Provides  a  single column  list  model  that contains  the  human
// 	readable string version of the data mode in the display role. Also
// 	provided  is a  translatable  column header  string  and tool  tip
// 	string.
//
//
// Collaborations
//
// 	Implements a concrete sub-class of the QAbstractListModel class.
//
class Modes final : public QAbstractListModel {
    Q_OBJECT
    Q_ENUMS(Mode)

  public:
    //
    // This enumeration contains the supported modes, to complement this
    // an array of human readable strings in the implementation
    // (Modes.cpp) must be maintained in parallel.
    //
    enum Mode {
        ALL, // matches with all modes
        JS8,
        // [JTTY] A frequency where we behave as a JTTY station (WSJT-X
        // convention, JTTY transmit mode forced). Never written into the
        // frozen frequency-item stream: saved as ALL plus a side map, see
        // Configuration.cpp. ALL behaves as JS8.
        JTTY,
        MODES_END_SENTINAL_AND_COUNT // this must be last
    };
    Q_ENUM(Mode)

    explicit Modes(QObject *parent = nullptr);

    // translate between enumeration and human readable strings
    static char const *name(Mode);
    static Mode value(QString const &);

    // Implement the QAbstractListModel interface
    int rowCount(QModelIndex const &parent = QModelIndex{}) const override {
        if (parent.isValid()) return 0;
#ifdef JS8_ENABLE_JTTY
        return MODES_END_SENTINAL_AND_COUNT; // Number of modes in Mode
#else
        return MODES_END_SENTINAL_AND_COUNT - 1; // JTTY (last) not offered
#endif
                                                   // enumeration class
    }
    QVariant data(QModelIndex const &,
                  int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation,
                        int = Qt::DisplayRole) const override;
};

// Qt boilerplate to make the Modes::Mode enumeration a type that can
// be streamed and queued as a signal argument as well as showing the
// human readable string when output to debug streams.
#if QT_VERSION < 0x050500
// Qt 5.5 introduces the Q_ENUM macro which automatically registers
// the meta-type
Q_DECLARE_METATYPE(Modes::Mode);
#endif

#if !defined(QT_NO_DEBUG_STREAM)
ENUM_QDEBUG_OPS_DECL(Modes, Mode);
#endif

ENUM_QDATASTREAM_OPS_DECL(Modes, Mode);
ENUM_CONVERSION_OPS_DECL(Modes, Mode);

#endif
