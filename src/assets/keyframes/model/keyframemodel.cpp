/***************************************************************************
 *   Copyright (C) 2017 by Nicolas Carion                                  *
 *   This file is part of Kdenlive. See www.kdenlive.org.                  *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) version 3 or any later version accepted by the       *
 *   membership of KDE e.V. (or its successor approved  by the membership  *
 *   of KDE e.V.), which shall act as a proxy defined in Section 14 of     *
 *   version 3 of the license.                                             *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>. *
 ***************************************************************************/

#include "keyframemodel.hpp"
#include "core.h"
#include "doc/docundostack.hpp"
#include "macros.hpp"
#include "profiles/profilemodel.hpp"
#include "rotoscoping/bpoint.h"
#include "rotoscoping/rotohelper.hpp"

#include <QSize>
#include <QLineF>
#include <QDebug>
#include <QJsonDocument>
#include <mlt++/Mlt.h>
#include <utility>

KeyframeModel::KeyframeModel(std::weak_ptr<AssetParameterModel> model, const QModelIndex &index, std::weak_ptr<DocUndoStack> undo_stack, QObject *parent)
    : QAbstractListModel(parent)
    , m_model(std::move(model))
    , m_undoStack(std::move(undo_stack))
    , m_index(index)
    , m_lastData()
    , m_lock(QReadWriteLock::Recursive)
{
    qDebug() << "Construct keyframemodel. Checking model:" << m_model.expired();
    if (auto ptr = m_model.lock()) {
        m_paramType = ptr->data(m_index, AssetParameterModel::TypeRole).value<ParamType>();
    }
    setup();
    refresh();
}

void KeyframeModel::setup()
{
    // We connect the signals of the abstractitemmodel to a more generic one.
    connect(this, &KeyframeModel::columnsMoved, this, &KeyframeModel::modelChanged);
    connect(this, &KeyframeModel::columnsRemoved, this, &KeyframeModel::modelChanged);
    connect(this, &KeyframeModel::columnsInserted, this, &KeyframeModel::modelChanged);
    connect(this, &KeyframeModel::rowsMoved, this, &KeyframeModel::modelChanged);
    connect(this, &KeyframeModel::rowsRemoved, this, &KeyframeModel::modelChanged);
    connect(this, &KeyframeModel::rowsInserted, this, &KeyframeModel::modelChanged);
    connect(this, &KeyframeModel::modelReset, this, &KeyframeModel::modelChanged);
    connect(this, &KeyframeModel::dataChanged, this, &KeyframeModel::modelChanged);
    connect(this, &KeyframeModel::modelChanged, this, &KeyframeModel::sendModification);
}

bool KeyframeModel::addKeyframe(GenTime pos, KeyframeType type, QVariant value, bool notify, Fun &undo, Fun &redo)
{
    qDebug() << "ADD keyframe" << pos.frames(pCore->getCurrentFps()) << value << notify;
    QWriteLocker locker(&m_lock);
    Fun local_undo = []() { return true; };
    Fun local_redo = []() { return true; };
    if (m_keyframeList.count(pos) > 0) {
        qDebug() << "already there";
        if (std::pair<KeyframeType, QVariant>({type, value}) == m_keyframeList.at(pos)) {
            qDebug() << "nothing to do";
            return true; // nothing to do
        }
        // In this case we simply change the type and value
        KeyframeType oldType = m_keyframeList[pos].first;
        QVariant oldValue = m_keyframeList[pos].second;
        local_undo = updateKeyframe_lambda(pos, oldType, oldValue, notify);
        local_redo = updateKeyframe_lambda(pos, type, value, notify);
    } else {
        local_redo = addKeyframe_lambda(pos, type, value, notify);
        local_undo = deleteKeyframe_lambda(pos, notify);
    }
    if (local_redo()) {
        UPDATE_UNDO_REDO(local_redo, local_undo, undo, redo);
        return true;
    }
    return false;
}

bool KeyframeModel::addKeyframe(int frame, double normalizedValue)
{
    QVariant result = getNormalizedValue(normalizedValue);
    if (result.isValid()) {
        // TODO: Use default configurable kf type
        return addKeyframe(GenTime(frame, pCore->getCurrentFps()), KeyframeType::Linear, result);
    }
    return false;
}

bool KeyframeModel::addKeyframe(GenTime pos, KeyframeType type, QVariant value)
{
    QWriteLocker locker(&m_lock);
    Fun undo = []() { return true; };
    Fun redo = []() { return true; };

    bool update = (m_keyframeList.count(pos) > 0);
    bool res = addKeyframe(pos, type, std::move(value), true, undo, redo);
    if (res) {
        PUSH_UNDO(undo, redo, update ? i18n("Change keyframe type") : i18n("Add keyframe"));
    }
    return res;
}

bool KeyframeModel::removeKeyframe(GenTime pos, Fun &undo, Fun &redo, bool notify)
{
    qDebug() << "Going to remove keyframe at " << pos.frames(pCore->getCurrentFps()) << " NOTIFY: " << notify;
    qDebug() << "before" << getAnimProperty();
    QWriteLocker locker(&m_lock);
    Q_ASSERT(m_keyframeList.count(pos) > 0);
    KeyframeType oldType = m_keyframeList[pos].first;
    QVariant oldValue = m_keyframeList[pos].second;
    Fun local_undo = addKeyframe_lambda(pos, oldType, oldValue, notify);
    Fun local_redo = deleteKeyframe_lambda(pos, notify);
    if (local_redo()) {
        qDebug() << "after" << getAnimProperty();
        UPDATE_UNDO_REDO(local_redo, local_undo, undo, redo);
        return true;
    }
    return false;
}

bool KeyframeModel::duplicateKeyframe(GenTime srcPos, GenTime dstPos, Fun &undo, Fun &redo)
{
    QWriteLocker locker(&m_lock);
    Q_ASSERT(m_keyframeList.count(srcPos) > 0);
    KeyframeType oldType = m_keyframeList[srcPos].first;
    QVariant oldValue = m_keyframeList[srcPos].second;
    Fun local_redo = addKeyframe_lambda(dstPos, oldType, oldValue, true);
    Fun local_undo = deleteKeyframe_lambda(dstPos, true);
    if (local_redo()) {
        UPDATE_UNDO_REDO(local_redo, local_undo, undo, redo);
        return true;
    }
    return false;
}

bool KeyframeModel::removeKeyframe(int frame)
{
    GenTime pos(frame, pCore->getCurrentFps());
    return removeKeyframe(pos);
}

bool KeyframeModel::removeKeyframe(GenTime pos)
{
    QWriteLocker locker(&m_lock);
    Fun undo = []() { return true; };
    Fun redo = []() { return true; };

    if (m_keyframeList.count(pos) > 0 && m_keyframeList.find(pos) == m_keyframeList.begin()) {
        return false; // initial point must stay
    }

    bool res = removeKeyframe(pos, undo, redo);
    if (res) {
        PUSH_UNDO(undo, redo, i18n("Delete keyframe"));
    }
    return res;
}

bool KeyframeModel::moveKeyframe(GenTime oldPos, GenTime pos, QVariant newVal, Fun &undo, Fun &redo)
{
    qDebug() << "starting to move keyframe" << oldPos.frames(pCore->getCurrentFps()) << pos.frames(pCore->getCurrentFps());
    QWriteLocker locker(&m_lock);
    Q_ASSERT(m_keyframeList.count(oldPos) > 0);
    if (oldPos == pos) {
        if (!newVal.isValid()) {
            // no change
            return true;
        }
        if (m_paramType == ParamType::AnimatedRect) {
            return updateKeyframe(pos, newVal);
        }
        // Calculate real value from normalized
        QVariant result = getNormalizedValue(newVal.toDouble());
        return updateKeyframe(pos, result);
    }
    if (oldPos != pos && hasKeyframe(pos)) {
        // Move rejected, another keyframe is here
        qDebug()<<"==== MOVE REJECTED!!";
        return false;
    }
    KeyframeType oldType = m_keyframeList[oldPos].first;
    QVariant oldValue = m_keyframeList[oldPos].second;
    Fun local_undo = []() { return true; };
    Fun local_redo = []() { return true; };
    qDebug() << getAnimProperty();
    // TODO: use the new Animation::key_set_frame to move a keyframe
    bool res = removeKeyframe(oldPos, local_undo, local_redo);
    qDebug() << "Move keyframe finished deletion:" << res;
    qDebug() << getAnimProperty();
    if (res) {
        if (m_paramType == ParamType::AnimatedRect) {
            if (!newVal.isValid()) {
                newVal = oldValue;
            }
            res = addKeyframe(pos, oldType, newVal, true, local_undo, local_redo);
        } else if (newVal.isValid()) {
            QVariant result = getNormalizedValue(newVal.toDouble());
            if (result.isValid()) {
                res = addKeyframe(pos, oldType, result, true, local_undo, local_redo);
            }
        } else {
            res = addKeyframe(pos, oldType, oldValue, true, local_undo, local_redo);
        }
        qDebug() << "Move keyframe finished insertion:" << res;
        qDebug() << getAnimProperty();
    }
    if (res) {
        UPDATE_UNDO_REDO(local_redo, local_undo, undo, redo);
    } else {
        bool undone = local_undo();
        Q_ASSERT(undone);
    }
    return res;
}

bool KeyframeModel::moveKeyframe(int oldPos, int pos, bool logUndo)
{
    GenTime oPos(oldPos, pCore->getCurrentFps());
    GenTime nPos(pos, pCore->getCurrentFps());
    return moveKeyframe(oPos, nPos, QVariant(), logUndo);
}

bool KeyframeModel::offsetKeyframes(int oldPos, int pos, bool logUndo)
{
    if (oldPos == pos) return true;
    GenTime oldFrame(oldPos, pCore->getCurrentFps());
    Q_ASSERT(m_keyframeList.count(oldFrame) > 0);
    GenTime diff(pos - oldPos, pCore->getCurrentFps());
    QWriteLocker locker(&m_lock);
    Fun undo = []() { return true; };
    Fun redo = []() { return true; };
    QList<GenTime> times;
    for (const auto &m : m_keyframeList) {
        if (m.first < oldFrame) continue;
        times << m.first;
    }
    bool res = true;
    for (const auto &t : qAsConst(times)) {
        res &= moveKeyframe(t, t + diff, QVariant(), undo, redo);
    }
    if (res && logUndo) {
        PUSH_UNDO(undo, redo, i18nc("@action", "Move keyframes"));
    }
    return res;
}

bool KeyframeModel::moveKeyframe(int oldPos, int pos, QVariant newVal)
{
    GenTime oPos(oldPos, pCore->getCurrentFps());
    GenTime nPos(pos, pCore->getCurrentFps());
    return moveKeyframe(oPos, nPos, std::move(newVal), true);
}

bool KeyframeModel::moveKeyframe(GenTime oldPos, GenTime pos, QVariant newVal, bool logUndo)
{
    QWriteLocker locker(&m_lock);
    Q_ASSERT(m_keyframeList.count(oldPos) > 0);
    if (oldPos == pos) return true;
    Fun undo = []() { return true; };
    Fun redo = []() { return true; };
    bool res = moveKeyframe(oldPos, pos, std::move(newVal), undo, redo);
    if (res && logUndo) {
        PUSH_UNDO(undo, redo, i18nc("@action", "Move keyframe"));
    }
    return res;
}

bool KeyframeModel::directUpdateKeyframe(GenTime pos, QVariant value)
{
    QWriteLocker locker(&m_lock);
    Q_ASSERT(m_keyframeList.count(pos) > 0);
    KeyframeType type = m_keyframeList[pos].first;
    auto operation = updateKeyframe_lambda(pos, type, std::move(value), true);
    return operation();
}

bool KeyframeModel::updateKeyframe(GenTime pos, const QVariant &value, Fun &undo, Fun &redo, bool update)
{
    QWriteLocker locker(&m_lock);
    Q_ASSERT(m_keyframeList.count(pos) > 0);
    KeyframeType type = m_keyframeList[pos].first;
    QVariant oldValue = m_keyframeList[pos].second;
    // Check if keyframe is different
    if (m_paramType == ParamType::KeyframeParam) {
        if (qFuzzyCompare(oldValue.toDouble(), value.toDouble())) return true;
    }
    auto operation = updateKeyframe_lambda(pos, type, value, update);
    auto reverse = updateKeyframe_lambda(pos, type, oldValue, update);
    bool res = operation();
    if (res) {
        UPDATE_UNDO_REDO(operation, reverse, undo, redo);
    }
    return res;
}

bool KeyframeModel::updateKeyframe(int pos, double newVal)
{
    GenTime Pos(pos, pCore->getCurrentFps());
    if (auto ptr = m_model.lock()) {
        double min = ptr->data(m_index, AssetParameterModel::VisualMinRole).toDouble();
        double max = ptr->data(m_index, AssetParameterModel::VisualMaxRole).toDouble();
        if (qFuzzyIsNull(min) && qFuzzyIsNull(max)) {
            min = ptr->data(m_index, AssetParameterModel::MinRole).toDouble();
            max = ptr->data(m_index, AssetParameterModel::MaxRole).toDouble();
        }
        double factor = ptr->data(m_index, AssetParameterModel::FactorRole).toDouble();
        double norm = ptr->data(m_index, AssetParameterModel::DefaultRole).toDouble();
        int logRole = ptr->data(m_index, AssetParameterModel::ScaleRole).toInt();
        double realValue;
        if (logRole == -1) {
            // Logarythmic scale
            if (newVal >= 0.5) {
                realValue = norm + pow(2 * (newVal - 0.5), 10.0 / 6) * (max / factor - norm);
            } else {
                realValue = norm - pow(2 * (0.5 - newVal), 10.0 / 6) * (norm - min / factor);
            }
        } else {
            realValue = (newVal * (max - min) + min) / factor;
        }
        return updateKeyframe(Pos, realValue);
    }
    return false;
}

bool KeyframeModel::updateKeyframe(GenTime pos, QVariant value)
{
    QWriteLocker locker(&m_lock);
    Q_ASSERT(m_keyframeList.count(pos) > 0);

    Fun undo = []() { return true; };
    Fun redo = []() { return true; };
    bool res = updateKeyframe(pos, std::move(value), undo, redo);
    if (res) {
        PUSH_UNDO(undo, redo, i18n("Update keyframe"));
    }
    return res;
}

KeyframeType convertFromMltType(mlt_keyframe_type type)
{
    switch (type) {
    case mlt_keyframe_linear:
        return KeyframeType::Linear;
    case mlt_keyframe_discrete:
        return KeyframeType::Discrete;
    case mlt_keyframe_smooth:
        return KeyframeType::Curve;
    }
    return KeyframeType::Linear;
}

bool KeyframeModel::updateKeyframeType(GenTime pos, int type, Fun &undo, Fun &redo)
{
    QWriteLocker locker(&m_lock);
    Q_ASSERT(m_keyframeList.count(pos) > 0);
    KeyframeType oldType = m_keyframeList[pos].first;
    KeyframeType newType = convertFromMltType(mlt_keyframe_type(type));
    QVariant value = m_keyframeList[pos].second;
    // Check if keyframe is different
    if (m_paramType == ParamType::KeyframeParam) {
        if (oldType == newType) return true;
    }
    auto operation = updateKeyframe_lambda(pos, newType, value, true);
    auto reverse = updateKeyframe_lambda(pos, oldType, value, true);
    bool res = operation();
    if (res) {
        UPDATE_UNDO_REDO(operation, reverse, undo, redo);
    }
    return res;
}

Fun KeyframeModel::updateKeyframe_lambda(GenTime pos, KeyframeType type, const QVariant &value, bool notify)
{
    QWriteLocker locker(&m_lock);
    return [this, pos, type, value, notify]() {
        qDebug() << "update lambda" << pos.frames(pCore->getCurrentFps()) << value << notify;
        Q_ASSERT(m_keyframeList.count(pos) > 0);
        int row = static_cast<int>(std::distance(m_keyframeList.begin(), m_keyframeList.find(pos)));
        m_keyframeList[pos].first = type;
        m_keyframeList[pos].second = value;
        if (notify) emit dataChanged(index(row), index(row), {ValueRole, NormalizedValueRole, TypeRole});
        return true;
    };
}

Fun KeyframeModel::addKeyframe_lambda(GenTime pos, KeyframeType type, const QVariant &value, bool notify)
{
    QWriteLocker locker(&m_lock);
    return [this, notify, pos, type, value]() {
        qDebug() << "add lambda" << pos.frames(pCore->getCurrentFps()) << value << notify;
        Q_ASSERT(m_keyframeList.count(pos) == 0);
        // We determine the row of the newly added marker
        auto insertionIt = m_keyframeList.lower_bound(pos);
        int insertionRow = static_cast<int>(m_keyframeList.size());
        if (insertionIt != m_keyframeList.end()) {
            insertionRow = static_cast<int>(std::distance(m_keyframeList.begin(), insertionIt));
        }
        if (notify) beginInsertRows(QModelIndex(), insertionRow, insertionRow);
        m_keyframeList[pos].first = type;
        m_keyframeList[pos].second = value;
        if (notify) endInsertRows();
        return true;
    };
}

Fun KeyframeModel::deleteKeyframe_lambda(GenTime pos, bool notify)
{
    QWriteLocker locker(&m_lock);
    return [this, pos, notify]() {
        qDebug() << "delete lambda" << pos.frames(pCore->getCurrentFps()) << notify;
        qDebug() << "before" << getAnimProperty();
        Q_ASSERT(m_keyframeList.count(pos) > 0);
        //Q_ASSERT(pos != GenTime()); // cannot delete initial point
        int row = static_cast<int>(std::distance(m_keyframeList.begin(), m_keyframeList.find(pos)));
        if (notify) beginRemoveRows(QModelIndex(), row, row);
        m_keyframeList.erase(pos);
        if (notify) endRemoveRows();
        qDebug() << "after" << getAnimProperty();
        return true;
    };
}

QHash<int, QByteArray> KeyframeModel::roleNames() const
{
    QHash<int, QByteArray> roles;
    roles[PosRole] = "position";
    roles[FrameRole] = "frame";
    roles[TypeRole] = "type";
    roles[ValueRole] = "value";
    roles[NormalizedValueRole] = "normalizedValue";
    return roles;
}

QVariant KeyframeModel::data(const QModelIndex &index, int role) const
{
    READ_LOCK();
    if (index.row() < 0 || index.row() >= static_cast<int>(m_keyframeList.size()) || !index.isValid()) {
        return QVariant();
    }
    auto it = m_keyframeList.begin();
    std::advance(it, index.row());
    switch (role) {
    case Qt::DisplayRole:
    case Qt::EditRole:
    case ValueRole:
        return it->second.second;
    case NormalizedValueRole: {
        if (m_paramType == ParamType::AnimatedRect) {
            const QString &data = it->second.second.toString();
            bool ok;
            double converted = data.section(QLatin1Char(' '), -1).toDouble(&ok);
            if (!ok) {
                qDebug() << "QLocale: Could not convert animated rect opacity" << data;
            }
            if (auto ptr = m_model.lock()) {
                if (ptr->getAssetId() != QLatin1String("qtblend")) {
                    converted /= 100.;
                }
            }
            return converted;
        }
        double val = it->second.second.toDouble();
        if (auto ptr = m_model.lock()) {
            Q_ASSERT(m_index.isValid());
            double min = ptr->data(m_index, AssetParameterModel::VisualMinRole).toDouble();
            double max = ptr->data(m_index, AssetParameterModel::VisualMaxRole).toDouble();
            if (qFuzzyIsNull(min) && qFuzzyIsNull(max)) {
                min = ptr->data(m_index, AssetParameterModel::MinRole).toDouble();
                max = ptr->data(m_index, AssetParameterModel::MaxRole).toDouble();
            }
            double factor = ptr->data(m_index, AssetParameterModel::FactorRole).toDouble();
            double norm = ptr->data(m_index, AssetParameterModel::DefaultRole).toDouble();
            int logRole = ptr->data(m_index, AssetParameterModel::ScaleRole).toInt();
            double linear = val * factor;
            if (logRole == -1) {
                // Logarythmic scale
                // transform current value to 0..1 scale
                if (linear >= norm) {
                    double scaled = (linear - norm) / (max * factor - norm);
                    return 0.5 + pow(scaled, 0.6) * 0.5;
                }
                double scaled = (linear - norm) / (min * factor - norm);
                // Log scale
                return 0.5 - pow(scaled, 0.6) * 0.5;
            }
            return (linear - min) / (max - min);
        } else {
            qDebug() << "// CANNOT LOCK effect MODEL";
        }
        return 1;
    }
    case PosRole:
        return it->first.seconds();
    case FrameRole:
    case Qt::UserRole:
        return it->first.frames(pCore->getCurrentFps());
    case TypeRole:
        return QVariant::fromValue<KeyframeType>(it->second.first);
    }
    return QVariant();
}

int KeyframeModel::rowCount(const QModelIndex &parent) const
{
    READ_LOCK();
    if (parent.isValid()) return 0;
    return static_cast<int>(m_keyframeList.size());
}

bool KeyframeModel::singleKeyframe() const
{
    READ_LOCK();
    return m_keyframeList.size() <= 1;
}

Keyframe KeyframeModel::getKeyframe(const GenTime &pos, bool *ok) const
{
    READ_LOCK();
    if (m_keyframeList.count(pos) <= 0) {
        // return empty marker
        *ok = false;
        return {GenTime(), KeyframeType::Linear};
    }
    *ok = true;
    return {pos, m_keyframeList.at(pos).first};
}

Keyframe KeyframeModel::getNextKeyframe(const GenTime &pos, bool *ok) const
{
    auto it = m_keyframeList.upper_bound(pos);
    if (it == m_keyframeList.end()) {
        // return empty marker
        *ok = false;
        return {GenTime(), KeyframeType::Linear};
    }
    *ok = true;
    return {(*it).first, (*it).second.first};
}

Keyframe KeyframeModel::getPrevKeyframe(const GenTime &pos, bool *ok) const
{
    auto it = m_keyframeList.lower_bound(pos);
    if (it == m_keyframeList.begin()) {
        // return empty marker
        *ok = false;
        return {GenTime(), KeyframeType::Linear};
    }
    --it;
    *ok = true;
    return {(*it).first, (*it).second.first};
}

Keyframe KeyframeModel::getClosestKeyframe(const GenTime &pos, bool *ok) const
{
    if (m_keyframeList.count(pos) > 0) {
        return getKeyframe(pos, ok);
    }
    bool ok1, ok2;
    auto next = getNextKeyframe(pos, &ok1);
    auto prev = getPrevKeyframe(pos, &ok2);
    *ok = ok1 || ok2;
    if (ok1 && ok2) {
        double fps = pCore->getCurrentFps();
        if (qAbs(next.first.frames(fps) - pos.frames(fps)) < qAbs(prev.first.frames(fps) - pos.frames(fps))) {
            return next;
        }
        return prev;
    } else if (ok1) {
        return next;
    } else if (ok2) {
        return prev;
    }
    // return empty marker
    return {GenTime(), KeyframeType::Linear};
}

bool KeyframeModel::hasKeyframe(int frame) const
{
    return hasKeyframe(GenTime(frame, pCore->getCurrentFps()));
}
bool KeyframeModel::hasKeyframe(const GenTime &pos) const
{
    READ_LOCK();
    return m_keyframeList.count(pos) > 0;
}

bool KeyframeModel::removeAllKeyframes(Fun &undo, Fun &redo)
{
    QWriteLocker locker(&m_lock);
    std::vector<GenTime> all_pos;
    Fun local_undo = []() { return true; };
    Fun local_redo = []() { return true; };
    int kfrCount = int(m_keyframeList.size()) - 1;
    if (kfrCount <= 0) {
        // Nothing to do
        UPDATE_UNDO_REDO(local_redo, local_undo, undo, redo);
        return true;
    }
    // we trigger only one global remove/insertrow event
    Fun update_redo_start = [this, kfrCount]() {
        beginRemoveRows(QModelIndex(), 1, kfrCount);
        return true;
    };
    Fun update_redo_end = [this]() {
        endRemoveRows();
        return true;
    };
    Fun update_undo_start = [this, kfrCount]() {
        beginInsertRows(QModelIndex(), 1, kfrCount);
        return true;
    };
    Fun update_undo_end = [this]() {
        endInsertRows();
        return true;
    };
    PUSH_LAMBDA(update_redo_start, local_redo);
    PUSH_LAMBDA(update_undo_start, local_undo);
    for (const auto &m : m_keyframeList) {
        all_pos.push_back(m.first);
    }
    update_redo_start();
    bool res = true;
    bool first = true;
    for (const auto &p : all_pos) {
        if (first) { // skip first point
            first = false;
            continue;
        }
        res = removeKeyframe(p, local_undo, local_redo, false);
        if (!res) {
            bool undone = local_undo();
            Q_ASSERT(undone);
            return false;
        }
    }
    update_redo_end();
    PUSH_LAMBDA(update_redo_end, local_redo);
    PUSH_LAMBDA(update_undo_end, local_undo);
    UPDATE_UNDO_REDO(local_redo, local_undo, undo, redo);
    return true;
}

bool KeyframeModel::removeAllKeyframes()
{
    QWriteLocker locker(&m_lock);
    Fun undo = []() { return true; };
    Fun redo = []() { return true; };
    bool res = removeAllKeyframes(undo, redo);
    if (res) {
        PUSH_UNDO(undo, redo, i18n("Delete all keyframes"));
    }
    return res;
}

mlt_keyframe_type convertToMltType(KeyframeType type)
{
    switch (type) {
    case KeyframeType::Linear:
        return mlt_keyframe_linear;
    case KeyframeType::Discrete:
        return mlt_keyframe_discrete;
    case KeyframeType::Curve:
        return mlt_keyframe_smooth;
    }
    return mlt_keyframe_linear;
}

QString KeyframeModel::getAnimProperty() const
{
    if (m_paramType == ParamType::Roto_spline) {
        return getRotoProperty();
    }
    Mlt::Properties mlt_prop;
    if (auto ptr = m_model.lock()) {
        ptr->passProperties(mlt_prop);
    }
    int ix = 0;
    bool first = true;
    std::shared_ptr<Mlt::Animation> anim(nullptr);
    for (const auto &keyframe : m_keyframeList) {
        if (first) {
            switch (m_paramType) {
            case ParamType::AnimatedRect:
                mlt_prop.anim_set("key", keyframe.second.second.toString().toUtf8().constData(), keyframe.first.frames(pCore->getCurrentFps()));
                break;
            default:
                mlt_prop.anim_set("key", keyframe.second.second.toDouble(), keyframe.first.frames(pCore->getCurrentFps()));
                break;
            }
            anim.reset(mlt_prop.get_anim("key"));
            anim->key_set_type(ix, convertToMltType(keyframe.second.first));
            first = false;
            ix++;
            continue;
        }
        switch (m_paramType) {
        case ParamType::AnimatedRect:
            mlt_prop.anim_set("key", keyframe.second.second.toString().toUtf8().constData(), keyframe.first.frames(pCore->getCurrentFps()));
            break;
        default:
            mlt_prop.anim_set("key", keyframe.second.second.toDouble(), keyframe.first.frames(pCore->getCurrentFps()));
            break;
        }
        anim->key_set_type(ix, convertToMltType(keyframe.second.first));
        ix++;
    }
    QString ret;
    if (anim) {
        char *cut = anim->serialize_cut();
        ret = QString(cut);
        free(cut);
    }
    return ret;
}

QString KeyframeModel::getRotoProperty() const
{
    QJsonDocument doc;
    if (auto ptr = m_model.lock()) {
        int in = ptr->data(m_index, AssetParameterModel::ParentInRole).toInt();
        int out = in + ptr->data(m_index, AssetParameterModel::ParentDurationRole).toInt();
        QVariantMap map;
        for (const auto &keyframe : m_keyframeList) {
            map.insert(QString::number(keyframe.first.frames(pCore->getCurrentFps())).rightJustified(int(log10(double(out))) + 1, '0'), keyframe.second.second);
        }
        doc = QJsonDocument::fromVariant(map);
    }
    return doc.toJson();
}

void KeyframeModel::parseAnimProperty(const QString &prop)
{
    Fun undo = []() { return true; };
    Fun redo = []() { return true; };
    disconnect(this, &KeyframeModel::modelChanged, this, &KeyframeModel::sendModification);
    removeAllKeyframes(undo, redo);
    int in = 0;
    int out = 0;
    bool useOpacity = true;
    Mlt::Properties mlt_prop;
    if (auto ptr = m_model.lock()) {
        in = ptr->data(m_index, AssetParameterModel::ParentInRole).toInt();
        out = ptr->data(m_index, AssetParameterModel::ParentDurationRole).toInt();
        ptr->passProperties(mlt_prop);
        useOpacity = ptr->data(m_index, AssetParameterModel::OpacityRole).toBool();
    } else  {
        qDebug()<<"###################\n\n/// ERROR LOCKING MODEL!!! ";
    }
    mlt_prop.set("key", prop.toUtf8().constData());
    // This is a fake query to force the animation to be parsed
    (void)mlt_prop.anim_get_double("key", 0, out);

    Mlt::Animation anim = mlt_prop.get_animation("key");

    qDebug() << "Found" << anim.key_count() << ", OUT: " << out << ", animation properties: " << prop;
    bool useDefaultType = !prop.contains(QLatin1Char('='));
    for (int i = 0; i < anim.key_count(); ++i) {
        int frame;
        mlt_keyframe_type type;
        anim.key_get(i, frame, type);
        if (useDefaultType) {
            // TODO: use a default user defined type
            type = mlt_keyframe_linear;
        }
        QVariant value;
        switch (m_paramType) {
        case ParamType::AnimatedRect: {
            mlt_rect rect = mlt_prop.anim_get_rect("key", frame);
            if (useOpacity) {
                value = QVariant(QStringLiteral("%1 %2 %3 %4 %5").arg(rect.x).arg(rect.y).arg(rect.w).arg(rect.h).arg(rect.o, 0, 'f'));
            } else {
                value = QVariant(QStringLiteral("%1 %2 %3 %4").arg(rect.x).arg(rect.y).arg(rect.w).arg(rect.h));
            }
            break;
        }
        default:
            value = QVariant(mlt_prop.anim_get_double("key", frame));
            break;
        }
        if (i == 0 && frame > in) {
            // Always add a keyframe at start pos
            addKeyframe(GenTime(in, pCore->getCurrentFps()), convertFromMltType(type), value, true, undo, redo);
        } else if (frame == in && hasKeyframe(GenTime(in))) {
            // First keyframe already exists, adjust its value
            updateKeyframe(GenTime(frame, pCore->getCurrentFps()), value, undo, redo, true);
            continue;
        }
        addKeyframe(GenTime(frame, pCore->getCurrentFps()), convertFromMltType(type), value, true, undo, redo);
    }
    connect(this, &KeyframeModel::modelChanged, this, &KeyframeModel::sendModification);
}

void KeyframeModel::resetAnimProperty(const QString &prop)
{
    Fun undo = []() { return true; };
    Fun redo = []() { return true; };

    // Delete all existing keyframes
    disconnect(this, &KeyframeModel::modelChanged, this, &KeyframeModel::sendModification);
    removeAllKeyframes(undo, redo);

    Mlt::Properties mlt_prop;
    int in = 0;
    bool useOpacity = true;
    if (auto ptr = m_model.lock()) {
        in = ptr->data(m_index, AssetParameterModel::ParentInRole).toInt();
        ptr->passProperties(mlt_prop);
        if (m_paramType == ParamType::AnimatedRect) {
            useOpacity = ptr->data(m_index, AssetParameterModel::OpacityRole).toBool();
        }
    }
    mlt_prop.set("key", prop.toUtf8().constData());
    // This is a fake query to force the animation to be parsed
    (void)mlt_prop.anim_get_int("key", 0, 0);

    Mlt::Animation anim = mlt_prop.get_animation("key");

    qDebug() << "Found" << anim.key_count() << "animation properties";
    for (int i = 0; i < anim.key_count(); ++i) {
        int frame;
        mlt_keyframe_type type;
        anim.key_get(i, frame, type);
        if (!prop.contains(QLatin1Char('='))) {
            // TODO: use a default user defined type
            type = mlt_keyframe_linear;
        }
        QVariant value;
        switch (m_paramType) {
        case ParamType::AnimatedRect: {
            mlt_rect rect = mlt_prop.anim_get_rect("key", frame);
            if (useOpacity) {
                value = QVariant(QStringLiteral("%1 %2 %3 %4 %5").arg(rect.x).arg(rect.y).arg(rect.w).arg(rect.h).arg(QString::number(rect.o, 'f')));
            } else {
                value = QVariant(QStringLiteral("%1 %2 %3 %4").arg(rect.x).arg(rect.y).arg(rect.w).arg(rect.h));
            }
            break;
        }
        default:
            value = QVariant(mlt_prop.anim_get_double("key", frame));
            break;
        }
        if (i == 0 && frame > in) {
            // Always add a keyframe at start pos
            addKeyframe(GenTime(in, pCore->getCurrentFps()), convertFromMltType(type), value, false, undo, redo);
        } else if (frame == in && hasKeyframe(GenTime(in))) {
            // First keyframe already exists, adjust its value
            updateKeyframe(GenTime(frame, pCore->getCurrentFps()), value, undo, redo, false);
            continue;
        }
        addKeyframe(GenTime(frame, pCore->getCurrentFps()), convertFromMltType(type), value, false, undo, redo);
    }
    QString effectName;
    if (auto ptr = m_model.lock()) {
        effectName = ptr->data(m_index, Qt::DisplayRole).toString();
    } else {
        effectName = i18n("effect");
    }
    Fun update_local = [this]() {
        emit dataChanged(index(0), index(int(m_keyframeList.size())), {});
        return true;
    };
    update_local();
    PUSH_LAMBDA(update_local, undo);
    PUSH_LAMBDA(update_local, redo);
    PUSH_UNDO(undo, redo, i18n("Reset %1", effectName));
    connect(this, &KeyframeModel::modelChanged, this, &KeyframeModel::sendModification);
}

void KeyframeModel::parseRotoProperty(const QString &prop)
{
    Fun undo = []() { return true; };
    Fun redo = []() { return true; };

    QJsonParseError jsonError;
    QJsonDocument doc = QJsonDocument::fromJson(prop.toLatin1(), &jsonError);
    QVariant data = doc.toVariant();
    if (data.canConvert(QVariant::Map)) {
        QMap<QString, QVariant> map = data.toMap();
        QMap<QString, QVariant>::const_iterator i = map.constBegin();
        while (i != map.constEnd()) {
            addKeyframe(GenTime(i.key().toInt(), pCore->getCurrentFps()), KeyframeType::Linear, i.value(), false, undo, redo);
            ++i;
        }
    }
}

QVariant KeyframeModel::getInterpolatedValue(int p) const
{
    auto pos = GenTime(p, pCore->getCurrentFps());
    return getInterpolatedValue(pos);
}

QVariant KeyframeModel::updateInterpolated(const QVariant &interpValue, double val)
{
    QStringList vals = interpValue.toString().split(QLatin1Char(' '));
    if (!vals.isEmpty()) {
        vals[vals.size() - 1] = QString::number(val, 'f');
    }
    return vals.join(QLatin1Char(' '));
}

QVariant KeyframeModel::getNormalizedValue(double newVal) const
{
    if (auto ptr = m_model.lock()) {
        double min = ptr->data(m_index, AssetParameterModel::VisualMinRole).toDouble();
        double max = ptr->data(m_index, AssetParameterModel::VisualMaxRole).toDouble();
        if (qFuzzyIsNull(min) && qFuzzyIsNull(max)) {
            min = ptr->data(m_index, AssetParameterModel::MinRole).toDouble();
            max = ptr->data(m_index, AssetParameterModel::MaxRole).toDouble();
        }
        if (qFuzzyIsNull(min) && qFuzzyIsNull(max)) {
            min = 0.;
            max = 1.;
        }
        double factor = ptr->data(m_index, AssetParameterModel::FactorRole).toDouble();
        double norm = ptr->data(m_index, AssetParameterModel::DefaultRole).toDouble();
        int logRole = ptr->data(m_index, AssetParameterModel::ScaleRole).toInt();
        double realValue;
        if (logRole == -1) {
            // Logarythmic scale
            if (newVal >= 0.5) {
                realValue = norm + pow(2 * (newVal - 0.5), 10.0 / 6) * (max / factor - norm);
            } else {
                realValue = norm - pow(2 * (0.5 - newVal), 10.0 / 6) * (norm - min / factor);
            }
        } else {
            realValue = (newVal * (max - min) + min) / factor;
        }
        return QVariant(realValue);
    }
    return QVariant();
}

QVariant KeyframeModel::getInterpolatedValue(const GenTime &pos) const
{
    if (m_keyframeList.count(pos) > 0) {
        return m_keyframeList.at(pos).second;
    }
    if (m_keyframeList.size() == 0) {
        return QVariant();
    }
    Mlt::Properties mlt_prop;
    QString animData;
    int out = 0;
    bool useOpacity = false;
    if (auto ptr = m_model.lock()) {
        ptr->passProperties(mlt_prop);
        ptr->data(m_index, AssetParameterModel::ParentInRole).toInt();
        out = ptr->data(m_index, AssetParameterModel::ParentDurationRole).toInt();
        useOpacity = ptr->data(m_index, AssetParameterModel::OpacityRole).toBool();
        animData = ptr->data(m_index, AssetParameterModel::ValueRole).toString();
    }
    if (m_paramType == ParamType::KeyframeParam) {
        if (!animData.isEmpty()) {
            mlt_prop.set("key", animData.toUtf8().constData());
            // This is a fake query to force the animation to be parsed
            (void)mlt_prop.anim_get_double("key", 0, out);
            return QVariant(mlt_prop.anim_get_double("key", pos.frames(pCore->getCurrentFps())));
        }
        return QVariant();
    } else if (m_paramType == ParamType::AnimatedRect) {
        if (!animData.isEmpty()) {
            mlt_prop.set("key", animData.toUtf8().constData());
            // This is a fake query to force the animation to be parsed
            (void)mlt_prop.anim_get_double("key", 0, out);
            mlt_rect rect = mlt_prop.anim_get_rect("key", pos.frames(pCore->getCurrentFps()));
            QString res = QStringLiteral("%1 %2 %3 %4").arg(int(rect.x)).arg(int(rect.y)).arg(int(rect.w)).arg(int(rect.h));
            if (useOpacity) {
                res.append(QStringLiteral(" %1").arg(QString::number(rect.o, 'f')));
            }
            return QVariant(res);
        }
        return QVariant();
    } else if (m_paramType == ParamType::Roto_spline) {
        // interpolate
        auto next = m_keyframeList.upper_bound(pos);
        if (next == m_keyframeList.cbegin()) {
            return (m_keyframeList.cbegin())->second.second;
        } else if (next == m_keyframeList.cend()) {
            auto it = m_keyframeList.cend();
            --it;
            return it->second.second;
        }
        auto prev = next;
        --prev;

        QSize frame = pCore->getCurrentFrameSize();
        QList<BPoint> p1 = RotoHelper::getPoints(prev->second.second, frame);
        QList<BPoint> p2 = RotoHelper::getPoints(next->second.second, frame);
        // relPos should be in [0,1]:
        // - equal to 0 on prev keyframe
        // - equal to 1 on next keyframe
        qreal relPos = 0;
        if (next->first != prev->first) {
            relPos = (pos.frames(pCore->getCurrentFps()) - prev->first.frames(pCore->getCurrentFps())) / qreal
                    (((next->first - prev->first).frames(pCore->getCurrentFps())));
        }
        int count = qMin(p1.count(), p2.count());
        QList<QVariant> vlist;
        for (int i = 0; i < count; ++i) {
            BPoint bp;
            QList<QVariant> pl;
            for (int j = 0; j < 3; ++j) {
                if (p1.at(i)[j] != p2.at(i)[j]) {
                    bp[j] = QLineF(p1.at(i)[j], p2.at(i)[j]).pointAt(relPos);
                } else {
                    bp[j] = p1.at(i)[j];
                }
                pl << QVariant(QList<QVariant>() << QVariant(bp[j].x() / frame.width()) << QVariant(bp[j].y() / frame.height()));
            }
            vlist << QVariant(pl);
        }
        return vlist;
    }
    return QVariant();
}

void KeyframeModel::sendModification()
{
    if (auto ptr = m_model.lock()) {
        Q_ASSERT(m_index.isValid());
        QString name = ptr->data(m_index, AssetParameterModel::NameRole).toString();
        if (m_paramType == ParamType::KeyframeParam || m_paramType == ParamType::AnimatedRect || m_paramType == ParamType::Roto_spline) {
            m_lastData = getAnimProperty();
            ptr->setParameter(name, m_lastData, false, m_index);
        } else {
            Q_ASSERT(false); // Not implemented, TODO
        }
    }
}

QString KeyframeModel::realValue(double normalizedValue) const
{
    double value = getNormalizedValue(normalizedValue).toDouble();
    if (auto ptr = m_model.lock()) {
        int decimals = ptr->data(m_index, AssetParameterModel::DecimalsRole).toInt();
        value *= ptr->data(m_index, AssetParameterModel::FactorRole).toDouble();
        QString result;
        if (decimals == 0) {
            if (ptr->getAssetId() == QLatin1String("qtblend")) {
                value = qRound(value * 100.);
            }
            // Fix rounding erros in double > int conversion
            if (value > 0.) {
                value += 0.001;
            } else {
                value -= 0.001;
            }
            result = QString::number(int(value));
        } else {
            result = QString::number(value, 'f', decimals);
        }
        result.append(ptr->data(m_index, AssetParameterModel::SuffixRole).toString());
        return result;
    }
    return QString::number(value);
}

void KeyframeModel::refresh()
{
    Q_ASSERT(m_index.isValid());
    QString animData;
    if (auto ptr = m_model.lock()) {
        animData = ptr->data(m_index, AssetParameterModel::ValueRole).toString();
    } else {
        qDebug() << "WARNING : unable to access keyframe's model";
        return;
    }
    if (animData == m_lastData) {
        // nothing to do
        qDebug() << "// DATA WAS ALREADY PARSED, ABORTING REFRESH\n";
        return;
    }
    if (m_paramType == ParamType::KeyframeParam || m_paramType == ParamType::AnimatedRect) {
        parseAnimProperty(animData);
    } else if (m_paramType == ParamType::Roto_spline) {
        parseRotoProperty(animData);
    } else {
        // first, try to convert to double
        bool ok = false;
        double value = animData.toDouble(&ok);
        if (ok) {
            Fun undo = []() { return true; };
            Fun redo = []() { return true; };
            addKeyframe(GenTime(), KeyframeType::Linear, QVariant(value), false, undo, redo);
        } else {
            Q_ASSERT(false); // Not implemented, TODO
        }
    }
    m_lastData = animData;
}

void KeyframeModel::reset()
{
    Q_ASSERT(m_index.isValid());
    QString animData;
    if (auto ptr = m_model.lock()) {
        animData = ptr->data(m_index, AssetParameterModel::ValueRole).toString();
    } else {
        qDebug() << "WARNING : unable to access keyframe's model";
        return;
    }
    if (animData == m_lastData) {
        // nothing to do
        qDebug() << "// DATA WAS ALREADY PARSED, ABORTING\n_________________";
        return;
    }
    if (m_paramType == ParamType::KeyframeParam || m_paramType == ParamType::AnimatedRect) {
        qDebug() << "parsing keyframe" << animData;
        resetAnimProperty(animData);
    } else if (m_paramType == ParamType::Roto_spline) {
        // TODO: resetRotoProperty(animData);
    } else {
        // first, try to convert to double
        bool ok = false;
        double value = animData.toDouble(&ok);
        if (ok) {
            Fun undo = []() { return true; };
            Fun redo = []() { return true; };
            addKeyframe(GenTime(), KeyframeType::Linear, QVariant(value), false, undo, redo);
            PUSH_UNDO(undo, redo, i18n("Reset effect"));
            qDebug() << "KEYFRAME ADDED" << value;
        } else {
            Q_ASSERT(false); // Not implemented, TODO
        }
    }
    m_lastData = animData;
}

QList<QPoint> KeyframeModel::getRanges(const QString &animData, const std::shared_ptr<AssetParameterModel> &model)
{
    Mlt::Properties mlt_prop;
    model->passProperties(mlt_prop);
    mlt_prop.set("key", animData.toUtf8().constData());
    // This is a fake query to force the animation to be parsed
    (void)mlt_prop.anim_get_int("key", 0, 0);

    Mlt::Animation anim = mlt_prop.get_animation("key");
    int frame;
    mlt_keyframe_type type;
    anim.key_get(0, frame, type);
    mlt_rect rect = mlt_prop.anim_get_rect("key", frame);
    QPoint pX(int(rect.x), int(rect.x));
    QPoint pY(int(rect.y), int(rect.y));
    QPoint pW(int(rect.w), int(rect.w));
    QPoint pH(int(rect.h), int(rect.h));
    QPoint pO(int(rect.o), int(rect.o));
    for (int i = 1; i < anim.key_count(); ++i) {
        anim.key_get(i, frame, type);
        if (!animData.contains(QLatin1Char('='))) {
            // TODO: use a default user defined type
            type = mlt_keyframe_linear;
        }
        rect = mlt_prop.anim_get_rect("key", frame);
        pX.setX(qMin(int(rect.x), pX.x()));
        pX.setY(qMax(int(rect.x), pX.y()));
        pY.setX(qMin(int(rect.y), pY.x()));
        pY.setY(qMax(int(rect.y), pY.y()));
        pW.setX(qMin(int(rect.w), pW.x()));
        pW.setY(qMax(int(rect.w), pW.y()));
        pH.setX(qMin(int(rect.h), pH.x()));
        pH.setY(qMax(int(rect.h), pH.y()));
        pO.setX(qMin(int(rect.o), pO.x()));
        pO.setY(qMax(int(rect.o), pO.y()));
        // value = QVariant(QStringLiteral("%1 %2 %3 %4 %5").arg(rect.x).arg(rect.y).arg(rect.w).arg(rect.h).arg(locale.toString(rect.o)));
    }
    QList<QPoint> result{pX, pY, pW, pH, pO};
    return result;
}

std::shared_ptr<Mlt::Properties> KeyframeModel::getAnimation(std::shared_ptr<AssetParameterModel> model, const QString &animData, int duration)
{
    std::shared_ptr<Mlt::Properties> mlt_prop(new Mlt::Properties());
    model->passProperties(*mlt_prop.get());
    mlt_prop->set("key", animData.toUtf8().constData());
    // This is a fake query to force the animation to be parsed
    (void)mlt_prop->anim_get_rect("key", 0, duration);
    return mlt_prop;
}

const QString KeyframeModel::getAnimationStringWithOffset(std::shared_ptr<AssetParameterModel> model, const QString &animData, int offset)
{
    Mlt::Properties mlt_prop;
    model->passProperties(mlt_prop);
    mlt_prop.set("key", animData.toUtf8().constData());
    // This is a fake query to force the animation to be parsed
    (void)mlt_prop.anim_get_rect("key", 0);
    Mlt::Animation anim = mlt_prop.get_animation("key");
    if (offset > 0) {
        for (int i = anim.key_count() - 1; i >= 0; --i) {
            int pos = anim.key_get_frame(i) + offset;
            anim.key_set_frame(i, pos);
        }
    } else {
        for (int i = 0; i < anim.key_count(); ++i) {
            int pos = anim.key_get_frame(i) + offset;
            if (pos > 0) {
                anim.key_set_frame(i, pos);
            }
        }
    }
    return qstrdup(anim.serialize_cut());
}

QList<GenTime> KeyframeModel::getKeyframePos() const
{
    QList<GenTime> all_pos;
    for (const auto &m : m_keyframeList) {
        all_pos.push_back(m.first);
    }
    return all_pos;
}

bool KeyframeModel::removeNextKeyframes(GenTime pos, Fun &undo, Fun &redo)
{
    QWriteLocker locker(&m_lock);
    std::vector<GenTime> all_pos;
    Fun local_undo = []() { return true; };
    Fun local_redo = []() { return true; };
    int firstPos = 0;
    for (const auto &m : m_keyframeList) {
        if (m.first <= pos) {
            firstPos++;
            continue;
        }
        all_pos.push_back(m.first);
    }
    int kfrCount = int(all_pos.size());
    // we trigger only one global remove/insertrow event
    Fun update_redo_start = [this, firstPos, kfrCount]() {
        beginRemoveRows(QModelIndex(), firstPos, kfrCount);
        return true;
    };
    Fun update_redo_end = [this]() {
        endRemoveRows();
        return true;
    };
    Fun update_undo_start = [this, firstPos, kfrCount]() {
        beginInsertRows(QModelIndex(), firstPos, kfrCount);
        return true;
    };
    Fun update_undo_end = [this]() {
        endInsertRows();
        return true;
    };
    PUSH_LAMBDA(update_redo_start, local_redo);
    PUSH_LAMBDA(update_undo_start, local_undo);
    update_redo_start();
    bool res = true;
    for (const auto &p : all_pos) {
        res = removeKeyframe(p, local_undo, local_redo, false);
        if (!res) {
            bool undone = local_undo();
            Q_ASSERT(undone);
            return false;
        }
    }
    update_redo_end();
    PUSH_LAMBDA(update_redo_end, local_redo);
    PUSH_LAMBDA(update_undo_end, local_undo);
    UPDATE_UNDO_REDO(local_redo, local_undo, undo, redo);
    return true;
}
