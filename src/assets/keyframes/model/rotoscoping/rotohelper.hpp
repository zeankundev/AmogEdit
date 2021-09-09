/*
Copyright (C) 2018  Jean-Baptiste Mardelle <jb@kdenlive.org>
This file is part of Kdenlive. See www.kdenlive.org.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of
the License or (at your option) version 3 or any later version
accepted by the membership of KDE e.V. (or its successor approved
by the membership of KDE e.V.), which shall act as a proxy
defined in Section 14 of version 3 of the license.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef ROTOHELPER_H
#define ROTOHELPER_H

#include "assets/keyframes/model/keyframemonitorhelper.hpp"
#include "bpoint.h"
#include <QPersistentModelIndex>
#include <QVariant>

class Monitor;

/** @class RotoHelper
    @brief \@todo Describe class RotoHelper
    @todo Describe class RotoHelper
 */
class RotoHelper : public KeyframeMonitorHelper
{
    Q_OBJECT

public:
    /** @brief Construct a keyframe list bound to the given effect
       @param init_value is the value taken by the param at time 0.
       @param model is the asset this parameter belong to
       @param index is the index of this parameter in its model
     */
    explicit RotoHelper(Monitor *monitor, std::shared_ptr<AssetParameterModel> model, QPersistentModelIndex index, QObject *parent = nullptr);
    /** @brief Send signals to the monitor to update the qml overlay.
       @param returns : true if the monitor's connection was changed to active.
    */
    static QVariant getSpline(const QVariant &value, const QSize frame);
    /** @brief Returns a list of spline control points, based on its string definition and frame size
       @param value : the spline's string definition
       @param frame: the frame size
    */
    static QList<BPoint> getPoints(const QVariant &value, const QSize frame);
    void refreshParams(int pos) override;

private slots:
    void slotUpdateFromMonitorData(const QVariantList &v) override;
};

#endif
