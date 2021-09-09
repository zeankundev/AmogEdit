/***************************************************************************
 *   Copyright (C) 2016 by Jean-Baptiste Mardelle (jb@kdenlive.org)        *
 *   This file is part of Kdenlive. See www.kdenlive.org.                  *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or         *
 *   modify it under the terms of the GNU General Public License as        *
 *   published by the Free Software Foundation; either version 2 of        *
 *   the License or (at your option) version 3 or any later version        *
 *   accepted by the membership of KDE e.V. (or its successor approved     *
 *   by the membership of KDE e.V.), which shall act as a proxy            *
 *   defined in Section 14 of version 3 of the license.                    *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>. *
 ***************************************************************************/

#include "klocalizedstring.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPainter>
#include <QSpinBox>
#include <QVBoxLayout>
#include <utility>

#include "assets/keyframes/view/keyframeview.hpp"
#include "core.h"
#include "doc/kdenlivedoc.h"
#include "kdenlivesettings.h"
#include "keyframeimport.h"
#include "profiles/profilemodel.hpp"
#include "widgets/positionwidget.h"
#include <macros.hpp>

#include "mlt++/MltAnimation.h"
#include "mlt++/MltProperties.h"


KeyframeImport::KeyframeImport(const QString &animData, std::shared_ptr<AssetParameterModel> model, QList<QPersistentModelIndex> indexes, int parentIn, int parentDuration, QWidget *parent)
    : QDialog(parent)
    , m_model(std::move(model))
    , m_indexes(indexes)
    , m_supportsAnim(false)
    , m_previewLabel(nullptr)
    , m_isReady(false)
{
    auto *lay = new QVBoxLayout(this);
    auto *l1 = new QHBoxLayout;
    QLabel *lab = new QLabel(i18n("Data to import:"), this);
    l1->addWidget(lab);

    m_dataCombo = new QComboBox(this);
    l1->addWidget(m_dataCombo);
    l1->addStretch(10);
    lay->addLayout(l1);
    int in = -1;
    int out = -1;
    // Set  up data
    auto json = QJsonDocument::fromJson(animData.toUtf8());
    if (!json.isArray()) {
        qDebug() << "Error : Json file should be an array";
        // Try to build data from a single value
        QJsonArray list;
        QJsonObject currentParam;
        currentParam.insert(QLatin1String("name"), QStringLiteral("data"));
        currentParam.insert(QLatin1String("value"), animData);
        bool ok;
        QString firstFrame = animData.section(QLatin1Char('='), 0, 0);
        in = firstFrame.toInt(&ok);
        if (!ok) {
            firstFrame.chop(1);
            in = firstFrame.toInt(&ok);
        }
        QString first = animData.section(QLatin1Char('='), 1, 1);
        if (!first.isEmpty()) {
            int spaces = first.count(QLatin1Char(' '));
            switch (spaces) {
                case 0:
                    currentParam.insert(QLatin1String("type"), QJsonValue(int(ParamType::Animated)));
                    break;
                default:
                    currentParam.insert(QLatin1String("type"), QJsonValue(int(ParamType::AnimatedRect)));
                    break;
            }
            //currentParam.insert(QLatin1String("min"), QJsonValue(min));
            //currentParam.insert(QLatin1String("max"), QJsonValue(max));
            list.push_back(currentParam);
            json = QJsonDocument(list);
        }
    }
    if (!json.isArray()) {
        qDebug() << "Error : Json file should be an array";
        return;
    }
    auto list = json.array();
    int ix = 0;
    for (const auto &entry : qAsConst(list)) {
        if (!entry.isObject()) {
            qDebug() << "Warning : Skipping invalid marker data";
            continue;
        }
        auto entryObj = entry.toObject();
        if (!entryObj.contains(QLatin1String("name"))) {
            qDebug() << "Warning : Skipping invalid marker data (does not contain name)";
            continue;
        }
        QString name = entryObj[QLatin1String("name")].toString();
        QString value = entryObj[QLatin1String("value")].toString();
        int type = entryObj[QLatin1String("type")].toInt(0);
        double min = entryObj[QLatin1String("min")].toDouble(0);
        double max = entryObj[QLatin1String("max")].toDouble(0);
        if (in == -1) {
            in = entryObj[QLatin1String("in")].toInt(0);
        }
        if (out == -1) {
            out = entryObj[QLatin1String("out")].toInt(0);
        }
        m_dataCombo->insertItem(ix, name);
        m_dataCombo->setItemData(ix, value, Qt::UserRole);
        m_dataCombo->setItemData(ix, type, Qt::UserRole + 1);
        m_dataCombo->setItemData(ix, min, Qt::UserRole + 2);
        m_dataCombo->setItemData(ix, max, Qt::UserRole + 3);
        ix++;
    }
    m_previewLabel = new QLabel(this);
    m_previewLabel->setMinimumSize(100, 150);
    m_previewLabel->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::MinimumExpanding);
    m_previewLabel->setScaledContents(true);
    lay->addWidget(m_previewLabel);
    // Zone in / out
    in = qMax(0, in);
    if (out <= 0) {
        out = in + parentDuration;
    }
    m_inPoint = new PositionWidget(i18n("In"), in, 0, out, pCore->currentDoc()->timecode(), QString(), this);
    connect(m_inPoint, &PositionWidget::valueChanged, this, &KeyframeImport::updateDisplay);
    lay->addWidget(m_inPoint);
    m_outPoint = new PositionWidget(i18n("Out"), out, in, out, pCore->currentDoc()->timecode(), QString(), this);
    connect(m_outPoint, &PositionWidget::valueChanged, this, &KeyframeImport::updateDisplay);
    lay->addWidget(m_outPoint);

    int count = 0;
    // Check what kind of parameters are in our target
    for (const QPersistentModelIndex &idx : indexes) {
        auto type = m_model->data(idx, AssetParameterModel::TypeRole).value<ParamType>();
        if (type == ParamType::KeyframeParam) {
            m_simpleTargets.insert(m_model->data(idx, Qt::DisplayRole).toString(), idx);
            QString name(m_model->data(idx, AssetParameterModel::NameRole).toString());
            if(name.contains("Position X") || name.contains("Position Y") || name.contains("Size X") || name.contains("Size Y")) {
                count++;
            }
        } else if (type == ParamType::AnimatedRect) {
            m_geometryTargets.insert(m_model->data(idx, Qt::DisplayRole).toString(), idx);
        }
    }

    l1 = new QHBoxLayout;
    m_targetCombo = new QComboBox(this);
    m_sourceCombo = new QComboBox(this);
    /*ix = 0;
    if (!m_geometryTargets.isEmpty()) {
        m_sourceCombo->insertItem(ix, i18n("Geometry"));
        m_sourceCombo->setItemData(ix, QString::number(10), Qt::UserRole);
        ix++;
        m_sourceCombo->insertItem(ix, i18n("Position"));
        m_sourceCombo->setItemData(ix, QString::number(11), Qt::UserRole);
        ix++;
    }
    if (!m_simpleTargets.isEmpty()) {
        m_sourceCombo->insertItem(ix, i18n("X"));
        m_sourceCombo->setItemData(ix, QString::number(0), Qt::UserRole);
        ix++;
        m_sourceCombo->insertItem(ix, i18n("Y"));
        m_sourceCombo->setItemData(ix, QString::number(1), Qt::UserRole);
        ix++;
        m_sourceCombo->insertItem(ix, i18n("Width"));
        m_sourceCombo->setItemData(ix, QString::number(2), Qt::UserRole);
        ix++;
        m_sourceCombo->insertItem(ix, i18n("Height"));
        m_sourceCombo->setItemData(ix, QString::number(3), Qt::UserRole);
        ix++;
    }*/
    connect(m_sourceCombo, SIGNAL(currentIndexChanged(int)), this, SLOT(updateRange()));
    m_alignSourceCombo = new QComboBox(this);
    m_alignSourceCombo->addItems(QStringList() << i18n("Top left") << i18n("Center") << i18n("Bottom right"));
    m_alignTargetCombo = new QComboBox(this);
    m_alignTargetCombo->addItems(QStringList() << i18n("Top left") << i18n("Center") << i18n("Bottom right"));
    lab = new QLabel(i18n("Map "), this);
    QLabel *lab2 = new QLabel(i18n(" to "), this);
    l1->addWidget(lab);
    l1->addWidget(m_sourceCombo);
    l1->addWidget(m_alignSourceCombo);
    l1->addWidget(lab2);
    l1->addWidget(m_targetCombo);
    l1->addWidget(m_alignTargetCombo);
    l1->addStretch(10);
    ix = 0;
    QMap<QString, QModelIndex>::const_iterator j = m_geometryTargets.constBegin();
    while (j != m_geometryTargets.constEnd()) {
        m_targetCombo->insertItem(ix, j.key());
        m_targetCombo->setItemData(ix, j.value(), Qt::UserRole);
        ++j;
        ix++;
    }
    if(count == 4) {
        // add an option to map to a a fake rectangle (pretend position params are a mlt rect)
        m_targetCombo->insertItem(ix, i18n("Rectangle"));
    }
    ix = 0;
    j = m_simpleTargets.constBegin();
    while (j != m_simpleTargets.constEnd()) {
        m_targetCombo->insertItem(ix, j.key());
        m_targetCombo->setItemData(ix, j.value(), Qt::UserRole);
        ++j;
        ix++;
    }
    if (m_simpleTargets.count() + m_geometryTargets.count() > 1) {
        // Target contains several animatable parameters, propose choice
    }
    lay->addLayout(l1);

    // Output offset
    int clipIn = parentIn;
    m_offsetPoint = new PositionWidget(i18n("Offset"), clipIn, 0, clipIn + parentDuration, pCore->currentDoc()->timecode(), "", this);
    lay->addWidget(m_offsetPoint);

    // Source range
    m_sourceRangeLabel = new QLabel(i18n("Source range %1 to %2", 0, 100), this);
    lay->addWidget(m_sourceRangeLabel);

    // update range info
    connect(m_targetCombo, SIGNAL(currentIndexChanged(int)), this, SLOT(updateDestinationRange()));

    // Destination range
    l1 = new QHBoxLayout;
    lab = new QLabel(i18n("Destination range"), this);

    l1->addWidget(lab);
    l1->addWidget(&m_destMin);
    l1->addWidget(&m_destMax);
    lay->addLayout(l1);

    l1 = new QHBoxLayout;
    m_limitRange = new QCheckBox(i18n("Actual range only"), this);
    connect(m_limitRange, &QAbstractButton::toggled, this, &KeyframeImport::updateRange);
    connect(m_limitRange, &QAbstractButton::toggled, this, &KeyframeImport::updateDisplay);
    l1->addWidget(m_limitRange);
    l1->addStretch(10);
    lay->addLayout(l1);
    l1 = new QHBoxLayout;
    m_limitKeyframes = new QCheckBox(i18n("Limit keyframe number"), this);
    m_limitKeyframes->setChecked(true);
    m_limitNumber = new QSpinBox(this);
    m_limitNumber->setMinimum(1);
    m_limitNumber->setValue(20);
    l1->addWidget(m_limitKeyframes);
    l1->addWidget(m_limitNumber);
    l1->addStretch(10);
    lay->addLayout(l1);
    connect(m_limitKeyframes, &QCheckBox::toggled, m_limitNumber, &QSpinBox::setEnabled);
    connect(m_limitKeyframes, &QAbstractButton::toggled, this, &KeyframeImport::updateDisplay);
    connect(m_limitNumber, SIGNAL(valueChanged(int)), this, SLOT(updateDisplay()));
    connect(m_dataCombo, SIGNAL(currentIndexChanged(int)), this, SLOT(updateDataDisplay()));
    QDialogButtonBox *buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(buttonBox);
    m_isReady = true;
    updateDestinationRange();
    updateDataDisplay();
}

KeyframeImport::~KeyframeImport() = default;

void KeyframeImport::resizeEvent(QResizeEvent *ev)
{
    QWidget::resizeEvent(ev);
    updateDisplay();
}

void KeyframeImport::updateDataDisplay()
{
    QString comboData = m_dataCombo->currentData().toString();
    auto type = m_dataCombo->currentData(Qt::UserRole + 1).value<ParamType>();
    auto values = m_dataCombo->currentData(Qt::UserRole).toString().split(QLatin1Char(';'));

    // we do not need all the options if there is only one keyframe
    bool onlyOne = values.length() == 1;
    m_previewLabel->setVisible(!onlyOne);
    m_limitKeyframes->setVisible(!onlyOne);
    m_limitNumber->setVisible(!onlyOne);
    m_inPoint->setVisible(!onlyOne);
    m_outPoint->setVisible(!onlyOne);

    m_maximas = KeyframeModel::getRanges(comboData, m_model);
    m_sourceCombo->clear();
    if (type == ParamType::KeyframeParam) {
        // 1 dimensional param.
        m_sourceCombo->addItem(m_dataCombo->currentText(), ImportRoles::SimpleValue);

        // map rotation to rotation by default if possible
        if (m_dataCombo->currentText() == QStringLiteral("rotation")) {
            int idx = m_targetCombo->findText(i18n("Rotation"));
            if (idx > -1) {
                m_targetCombo->setCurrentIndex(idx);
            }
        }
        updateRange();
        return;
    }
    double wDist = m_maximas.at(2).y() - m_maximas.at(2).x();
    double hDist = m_maximas.at(3).y() - m_maximas.at(3).x();
    m_sourceCombo->addItem(i18n("Geometry"), ImportRoles::FullGeometry);
    m_sourceCombo->addItem(i18n("Position"), ImportRoles::Position);
    m_sourceCombo->addItem(i18n("Inverted Position"), ImportRoles::InvertedPosition);
    m_sourceCombo->addItem(i18n("Offset Position"), ImportRoles::OffsetPosition);
    m_sourceCombo->addItem(i18n("X"), ImportRoles::XOnly);
    m_sourceCombo->addItem(i18n("Y"), ImportRoles::YOnly);
    if (wDist > 0) {
        m_sourceCombo->addItem(i18n("Width"), ImportRoles::WidthOnly);
    }
    if (hDist > 0) {
        m_sourceCombo->addItem(i18n("Height"), ImportRoles::HeightOnly);
    }

    // if available map to Rectangle by default
    int idx = m_targetCombo->findText(i18n("Rectangle"));
    if (idx > -1) {
        m_targetCombo->setCurrentIndex(idx);
    }
    updateRange();
    /*if (!m_inPoint->isValid()) {
        m_inPoint->blockSignals(true);
        m_outPoint->blockSignals(true);
        // m_inPoint->setRange(0, m_keyframeView->duration);
        m_inPoint->setPosition(0);
        m_outPoint->setPosition(m_model->data(m_targetCombo->currentData().toModelIndex(), AssetParameterModel::ParentDurationRole).toInt());
        m_inPoint->blockSignals(false);
        m_outPoint->blockSignals(false);
    }*/
}

void KeyframeImport::updateRange()
{
    int pos = m_sourceCombo->currentData().toInt();
    m_alignSourceCombo->setEnabled(pos == ImportRoles::Position || pos == ImportRoles::InvertedPosition);
    m_alignTargetCombo->setEnabled(pos == ImportRoles::Position || pos == ImportRoles::InvertedPosition);
    QString rangeText;
    if (m_limitRange->isChecked()) {
        switch (pos) {
            case ImportRoles::SimpleValue:
            case ImportRoles::XOnly:
            rangeText = i18n("Source range %1 to %2", m_maximas.at(0).x(), m_maximas.at(0).y());
            break;
        case ImportRoles::YOnly:
            rangeText = i18n("Source range %1 to %2", m_maximas.at(1).x(), m_maximas.at(1).y());
            break;
        case ImportRoles::WidthOnly:
            rangeText = i18n("Source range %1 to %2", m_maximas.at(2).x(), m_maximas.at(2).y());
            break;
        case ImportRoles::HeightOnly:
            rangeText = i18n("Source range %1 to %2", m_maximas.at(3).x(), m_maximas.at(3).y());
            break;
        default:
            rangeText = i18n("Source range: (%1-%2), (%3-%4)", m_maximas.at(0).x(), m_maximas.at(0).y(), m_maximas.at(1).x(), m_maximas.at(1).y());
            break;
        }
    } else {
        int profileWidth = pCore->getCurrentProfile()->width();
        int profileHeight = pCore->getCurrentProfile()->height();
        switch (pos) {
        case ImportRoles::SimpleValue:
            rangeText = i18n("Source range %1 to %2", qMin(0, m_maximas.at(0).x()), m_maximas.at(0).y());
            break;
        case ImportRoles::XOnly:
            rangeText = i18n("Source range %1 to %2", qMin(0, m_maximas.at(0).x()), qMax(profileWidth, m_maximas.at(0).y()));
            break;
        case ImportRoles::YOnly:
            rangeText = i18n("Source range %1 to %2", qMin(0, m_maximas.at(1).x()), qMax(profileHeight, m_maximas.at(1).y()));
            break;
        case ImportRoles::WidthOnly:
            rangeText = i18n("Source range %1 to %2", qMin(0, m_maximas.at(2).x()), qMax(profileWidth, m_maximas.at(2).y()));
            break;
        case ImportRoles::HeightOnly:
            rangeText = i18n("Source range %1 to %2", qMin(0, m_maximas.at(3).x()), qMax(profileHeight, m_maximas.at(3).y()));
            break;
        default:
            rangeText = i18n("Source range: (%1-%2), (%3-%4)", qMin(0, m_maximas.at(0).x()), qMax(profileWidth, m_maximas.at(0).y()),
                             qMin(0, m_maximas.at(1).x()), qMax(profileHeight, m_maximas.at(1).y()));
            break;
        }
    }
    m_sourceRangeLabel->setText(rangeText);
    updateDisplay();
}

void KeyframeImport::updateDestinationRange()
{
    if (m_simpleTargets.contains(m_targetCombo->currentText())) {
        // 1 dimension target
        m_destMin.setEnabled(true);
        m_destMax.setEnabled(true);
        m_limitRange->setEnabled(true);
        double min = m_model->data(m_targetCombo->currentData().toModelIndex(), AssetParameterModel::MinRole).toDouble();
        double max = m_model->data(m_targetCombo->currentData().toModelIndex(), AssetParameterModel::MaxRole).toDouble();
        m_destMin.setRange(min, max);
        m_destMax.setRange(min, max);
        m_destMin.setValue(min);
        m_destMax.setValue(max);
    } else {
        int profileWidth = pCore->getCurrentProfile()->width();
        m_destMin.setRange(-2 * profileWidth, 2 * profileWidth);
        m_destMax.setRange(-2 * profileWidth, 2 * profileWidth);
        m_destMin.setEnabled(false);
        m_destMax.setEnabled(false);
        m_limitRange->setEnabled(false);
    }
}

void KeyframeImport::updateDisplay()
{
    if (!m_isReady) {
        // Not ready
        return;
    }
    QPixmap pix(m_previewLabel->width(), m_previewLabel->height());
    pix.fill(Qt::transparent);
    QList<QPoint> maximas;
    int selectedtarget = m_sourceCombo->currentData().toInt();
    int profileWidth = pCore->getCurrentProfile()->width();
    int profileHeight = pCore->getCurrentProfile()->height();
    if (!m_maximas.isEmpty()) {
        if (m_maximas.at(0).x() == m_maximas.at(0).y() || (selectedtarget == ImportRoles::YOnly || selectedtarget == ImportRoles::WidthOnly || selectedtarget == ImportRoles::HeightOnly)) {
            maximas << QPoint();
        } else {
            if (m_limitRange->isChecked()) {
                maximas << m_maximas.at(0);
            } else {
                QPoint p1;
                if (selectedtarget == ImportRoles::SimpleValue) {  
                    p1 = QPoint(qMin(0, m_maximas.at(0).x()), m_maximas.at(0).y());
                } else {
                    p1 = QPoint(qMin(0, m_maximas.at(0).x()), qMax(profileWidth, m_maximas.at(0).y()));
                }
                maximas << p1;
            }
        }
    }
    if (m_maximas.count() > 1) {
        if (m_maximas.at(1).x() == m_maximas.at(1).y() || (selectedtarget == ImportRoles::XOnly || selectedtarget == ImportRoles::WidthOnly || selectedtarget == ImportRoles::HeightOnly)) {
            maximas << QPoint();
        } else {
            if (m_limitRange->isChecked()) {
                maximas << m_maximas.at(1);
            } else {
                QPoint p2(qMin(0, m_maximas.at(1).x()), qMax(profileHeight, m_maximas.at(1).y()));
                maximas << p2;
            }
        }
    }
    if (m_maximas.count() > 2) {
        if (m_maximas.at(2).x() == m_maximas.at(2).y() || (selectedtarget == ImportRoles::XOnly || selectedtarget == ImportRoles::YOnly || selectedtarget == ImportRoles::HeightOnly)) {
            maximas << QPoint();
        } else {
            if (m_limitRange->isChecked()) {
                maximas << m_maximas.at(2);
            } else {
                QPoint p3(qMin(0, m_maximas.at(2).x()), qMax(profileWidth, m_maximas.at(2).y()));
                maximas << p3;
            }
        }
    }
    if (m_maximas.count() > 3) {
        if (m_maximas.at(3).x() == m_maximas.at(3).y() || (selectedtarget == ImportRoles::XOnly || selectedtarget == ImportRoles::WidthOnly || selectedtarget == ImportRoles::YOnly)) {
            maximas << QPoint();
        } else {
            if (m_limitRange->isChecked()) {
                maximas << m_maximas.at(3);
            } else {
                QPoint p4(qMin(0, m_maximas.at(3).x()), qMax(profileHeight, m_maximas.at(3).y()));
                maximas << p4;
            }
        }
    }
    drawKeyFrameChannels(pix, m_inPoint->getPosition(), m_outPoint->getPosition(), m_limitKeyframes->isChecked() ? m_limitNumber->value() : 0,
                         palette().text().color());
    m_previewLabel->setPixmap(pix);
}

QString KeyframeImport::selectedData() const
{
    // return serialized keyframes
    if (m_simpleTargets.contains(m_targetCombo->currentText())) {
        // Exporting a 1 dimension animation
        int ix = m_sourceCombo->currentData().toInt();
        QPoint maximas;
        if (m_limitRange->isChecked()) {
            maximas = m_maximas.at(ix);
        } else if (ix == ImportRoles::WidthOnly) {
            // Width maximas
            maximas = QPoint(qMin(m_maximas.at(ix).x(), 0), qMax(m_maximas.at(ix).y(), pCore->getCurrentProfile()->width()));
        } else {
            // Height maximas
            maximas = QPoint(qMin(m_maximas.at(ix).x(), 0), qMax(m_maximas.at(ix).y(), pCore->getCurrentProfile()->height()));
        }
        std::shared_ptr<Mlt::Properties> animData = KeyframeModel::getAnimation(m_model, m_dataCombo->currentData().toString());
        std::shared_ptr<Mlt::Animation> anim(new Mlt::Animation(animData->get_animation("key")));
        animData->anim_get_double("key", m_inPoint->getPosition(), m_outPoint->getPosition());
        int existingKeys = anim->key_count();
        if (m_limitKeyframes->isChecked() && m_limitNumber->value() < existingKeys) {
            // We need to limit keyframes, create new animation
            int in = m_inPoint->getPosition();
            int out = m_outPoint->getPosition();
            std::shared_ptr<Mlt::Properties> animData2 = KeyframeModel::getAnimation(m_model, m_dataCombo->currentData().toString());
            std::shared_ptr<Mlt::Animation> anim2(new Mlt::Animation(animData2->get_animation("key")));
            anim2->interpolate();
            
            // Remove existing kfrs
            int firstKeyframe = -1;
            int lastKeyframe = -1;
            if (anim2->is_key(0)) {
                if (in == 0) {
                    firstKeyframe = 0;
                }
                anim2->remove(0);
            }
            int keyPos = anim2->next_key(0);
            while (anim2->is_key(keyPos)) {
                if (firstKeyframe == -1) {
                    firstKeyframe = keyPos;
                }
                if (keyPos < out) {
                    lastKeyframe = keyPos;
                } else {
                    lastKeyframe = out;
                }
                anim2->remove(keyPos);
                keyPos = anim2->next_key(keyPos);
            }
            anim2->interpolate();
            int length = lastKeyframe;
            double interval = double(length) / (m_limitNumber->value() - 1);
            int pos = 0;
            for (int i = 0; i < m_limitNumber->value(); i++) {
                pos = firstKeyframe + in + i * interval;
                pos = qMin(pos, length - 1);
                double dval = animData->anim_get_double("key", pos);
                animData2->anim_set("key", dval, pos);
                
            }
            anim2->interpolate();
            return anim2->serialize_cut();
        }
        return anim->serialize_cut();
    }
    std::shared_ptr<Mlt::Properties> animData = KeyframeModel::getAnimation(m_model, m_dataCombo->currentData().toString());
    std::shared_ptr<Mlt::Animation> anim(new Mlt::Animation(animData->get_animation("key")));
    animData->anim_get_rect("key", m_inPoint->getPosition(), m_outPoint->getPosition());
    int existingKeys = anim->key_count();
    if (m_limitKeyframes->isChecked() && m_limitNumber->value() < existingKeys) {
            // We need to limit keyframes, create new animation
            int in = m_inPoint->getPosition();
            int out = m_outPoint->getPosition();
            std::shared_ptr<Mlt::Properties> animData2 = KeyframeModel::getAnimation(m_model, m_dataCombo->currentData().toString());
            std::shared_ptr<Mlt::Animation> anim2(new Mlt::Animation(animData2->get_animation("key")));
            anim2->interpolate();
            
            // Remove existing kfrs
            int firstKeyframe = -1;
            int lastKeyframe = -1;
            if (anim2->is_key(0)) {
                if (in == 0) {
                    firstKeyframe = 0;
                }
                anim2->remove(0);
            }
            int keyPos = anim2->next_key(0);
            while (anim2->is_key(keyPos)) {
                if (firstKeyframe == -1) {
                    firstKeyframe = keyPos;
                }
                if (keyPos < out) {
                    lastKeyframe = keyPos;
                } else {
                    lastKeyframe = out;
                }
                anim2->remove(keyPos);
                keyPos = anim2->next_key(keyPos);
            }
            anim2->interpolate();
            int length = lastKeyframe;
            double interval = double(length) / (m_limitNumber->value() - 1);
            int pos = 0;
            for (int i = 0; i < m_limitNumber->value(); i++) {
                pos = firstKeyframe + in + i * interval;
                pos = qMin(pos, length - 1);
                mlt_rect rect = animData->anim_get_rect("key", pos);
                animData2->anim_set("key", rect, pos);
                
            }
            anim2->interpolate();
            return anim2->serialize_cut();
        }
    
    return anim->serialize_cut();
}

QString KeyframeImport::selectedTarget() const
{
    return m_targetCombo->currentData().toString();
}

void KeyframeImport::drawKeyFrameChannels(QPixmap &pix, int in, int out, int limitKeyframes, const QColor &textColor)
{
    qDebug()<<"============= DRAWING KFR CHANNS: "<<m_dataCombo->currentData().toString();
    std::shared_ptr<Mlt::Properties> animData = KeyframeModel::getAnimation(m_model, m_dataCombo->currentData().toString());
    QRect br(0, 0, pix.width(), pix.height());
    double frameFactor = double(out - in) / br.width();
    int offset = 1;
    if (limitKeyframes > 0) {
        offset = int((out - in) / limitKeyframes / frameFactor);
    }
    double min = m_dataCombo->currentData(Qt::UserRole + 2).toDouble();
    double max = m_dataCombo->currentData(Qt::UserRole + 3).toDouble();
    double xDist;
    if (max > min) {
        xDist = max - min;
    } else {
        xDist = m_maximas.at(0).y() - m_maximas.at(0).x();
    }
    double yDist = m_maximas.at(1).y() - m_maximas.at(1).x();
    double wDist = m_maximas.at(2).y() - m_maximas.at(2).x();
    double hDist = m_maximas.at(3).y() - m_maximas.at(3).x();
    double xOffset = m_maximas.at(0).x();
    double yOffset = m_maximas.at(1).x();
    double wOffset = m_maximas.at(2).x();
    double hOffset = m_maximas.at(3).x();
    QColor cX(255, 0, 0, 100);
    QColor cY(0, 255, 0, 100);
    QColor cW(0, 0, 255, 100);
    QColor cH(255, 255, 0, 100);
    // Draw curves labels
    QPainter painter;
    painter.begin(&pix);
    QRectF txtRect = painter.boundingRect(br, QStringLiteral("t"));
    txtRect.setX(2);
    txtRect.setWidth(br.width() - 4);
    txtRect.moveTop(br.height() - txtRect.height());
    QRectF drawnText;
    int maxHeight = int(br.height() - txtRect.height() - 2);
    painter.setPen(textColor);
    int rectSize = int(txtRect.height() / 2);
    if (xDist > 0) {
        painter.fillRect(int(txtRect.x()), int(txtRect.top() + rectSize / 2), rectSize, rectSize, cX);
        txtRect.setX(txtRect.x() + rectSize * 2);
        painter.drawText(txtRect, 0, i18nc("X as in x coordinate", "X") + QStringLiteral(" (%1-%2)").arg(m_maximas.at(0).x()).arg(m_maximas.at(0).y()),
                         &drawnText);
    }
    if (yDist > 0) {
        if (drawnText.isValid()) {
            txtRect.setX(drawnText.right() + rectSize);
        }
        painter.fillRect(int(txtRect.x()), int(txtRect.top() + rectSize / 2), rectSize, rectSize, cY);
        txtRect.setX(txtRect.x() + rectSize * 2);
        painter.drawText(txtRect, 0, i18nc("Y as in y coordinate", "Y") + QStringLiteral(" (%1-%2)").arg(m_maximas.at(1).x()).arg(m_maximas.at(1).y()),
                         &drawnText);
    }
    if (wDist > 0) {
        if (drawnText.isValid()) {
            txtRect.setX(drawnText.right() + rectSize);
        }
        painter.fillRect(int(txtRect.x()), int(txtRect.top() + rectSize / 2), rectSize, rectSize, cW);
        txtRect.setX(txtRect.x() + rectSize * 2);
        painter.drawText(txtRect, 0, i18n("Width") + QStringLiteral(" (%1-%2)").arg(m_maximas.at(2).x()).arg(m_maximas.at(2).y()), &drawnText);
    }
    if (hDist > 0) {
        if (drawnText.isValid()) {
            txtRect.setX(drawnText.right() + rectSize);
        }
        painter.fillRect(int(txtRect.x()), int(txtRect.top() + rectSize / 2), rectSize, rectSize, cH);
        txtRect.setX(txtRect.x() + rectSize * 2);
        painter.drawText(txtRect, 0, i18n("Height") + QStringLiteral(" (%1-%2)").arg(m_maximas.at(3).x()).arg(m_maximas.at(3).y()), &drawnText);
    }

    // Draw curves
    for (int i = 0; i < br.width(); i++) {
        mlt_rect rect = animData->anim_get_rect("key", int(i * frameFactor) + in);
        if (xDist > 0) {
            painter.setPen(cX);
            int val = int((rect.x - xOffset) * maxHeight / xDist);
            qDebug() << "// DRAWINC CURVE : " << rect.x <<", POS: "<<(int(i * frameFactor) + in)<< ", RESULT: " << val;
            painter.drawLine(i, maxHeight - val, i, maxHeight);
        }
        if (yDist > 0) {
            painter.setPen(cY);
            int val = int((rect.y - yOffset) * maxHeight / yDist);
            painter.drawLine(i, maxHeight - val, i, maxHeight);
        }
        if (wDist > 0) {
            painter.setPen(cW);
            int val = int((rect.w - wOffset) * maxHeight / wDist);
            qDebug() << "// OFFSET: " << wOffset << ", maxH: " << maxHeight << ", wDIst:" << wDist << " = " << val;
            painter.drawLine(i, maxHeight - val, i, maxHeight);
        }
        if (hDist > 0) {
            painter.setPen(cH);
            int val = int((rect.h - hOffset) * maxHeight / hDist);
            painter.drawLine(i, maxHeight - val, i, maxHeight);
        }
    }
    if (offset > 1) {
        // Overlay limited keyframes curve
        cX.setAlpha(255);
        cY.setAlpha(255);
        cW.setAlpha(255);
        cH.setAlpha(255);
        mlt_rect rect1 = animData->anim_get_rect("key", in);
        int prevPos = 0;
        for (int i = offset; i < br.width(); i += offset) {
            mlt_rect rect2 = animData->anim_get_rect("key", int(i * frameFactor) + in);
            if (xDist > 0) {
                painter.setPen(cX);
                int val1 = int((rect1.x - xOffset) * maxHeight / xDist);
                int val2 = int((rect2.x - xOffset) * maxHeight / xDist);
                painter.drawLine(prevPos, maxHeight - val1, i, maxHeight - val2);
            }
            if (yDist > 0) {
                painter.setPen(cY);
                int val1 = int((rect1.y - yOffset) * maxHeight / yDist);
                int val2 = int((rect2.y - yOffset) * maxHeight / yDist);
                painter.drawLine(prevPos, maxHeight - val1, i, maxHeight - val2);
            }
            if (wDist > 0) {
                painter.setPen(cW);
                int val1 = int((rect1.w - wOffset) * maxHeight / wDist);
                int val2 = int((rect2.w - wOffset) * maxHeight / wDist);
                painter.drawLine(prevPos, maxHeight - val1, i, maxHeight - val2);
            }
            if (hDist > 0) {
                painter.setPen(cH);
                int val1 = int((rect1.h - hOffset) * maxHeight / hDist);
                int val2 = int((rect2.h - hOffset) * maxHeight / hDist);
                painter.drawLine(prevPos, maxHeight - val1, i, maxHeight - val2);
            }
            rect1 = rect2;
            prevPos = i;
        }
    }
}

void KeyframeImport::importSelectedData()
{
    // Simple double value
    std::shared_ptr<Mlt::Properties> animData = KeyframeModel::getAnimation(m_model, selectedData());
    std::shared_ptr<Mlt::Animation> anim(new Mlt::Animation(animData->get_animation("key")));
    std::shared_ptr<KeyframeModelList> kfrModel = m_model->getKeyframeModel();
    Fun undo = []() { return true; };
    Fun redo = []() { return true; };
    // Geometry target
    int sourceAlign = m_alignSourceCombo->currentIndex();
    int targetAlign = m_alignTargetCombo->currentIndex();
    QLocale locale; // Import from clipboard – OK to use locale here?
    locale.setNumberOptions(QLocale::OmitGroupSeparator);
    for (const auto &ix : qAsConst(m_indexes)) {
        // update keyframes in other indexes
        KeyframeModel *km = kfrModel->getKeyModel(ix);
        qDebug()<<"== "<<ix<<" = "<<m_targetCombo->currentData().toModelIndex();

        // wether we are mapping to a fake rectangle
        bool fakeRect = m_targetCombo->currentData().isNull() && m_targetCombo->currentText() == i18n("Rectangle");

        if (ix == m_targetCombo->currentData().toModelIndex() || fakeRect) {
            // Import our keyframes
            int frame = 0;
            KeyframeImport::ImportRoles convertMode = static_cast<KeyframeImport::ImportRoles> (m_sourceCombo->currentData().toInt());
            mlt_keyframe_type type;
            mlt_rect firstRect = animData->anim_get_rect("key", anim->key_get_frame(0));
            for (int i = 0; i < anim->key_count(); i++) {
                int error = anim->key_get(i, frame, type);
                if (error) {
                    continue;
                }
                QVariant current = km->getInterpolatedValue(frame);
                if (convertMode == ImportRoles::SimpleValue) {
                    double dval = animData->anim_get_double("key", frame);
                    km->addKeyframe(GenTime(frame - m_inPoint->getPosition() + m_offsetPoint->getPosition(), pCore->getCurrentFps()), KeyframeType(type), dval, true, undo, redo);
                    continue;
                }
                QStringList kfrData = current.toString().split(QLatin1Char(' '));
                // Safety check
                if (fakeRect) {
                    while (kfrData.size() < 4) {
                        kfrData.append("0");
                    }
                }
                int size = kfrData.size();
                switch (convertMode) {
                    case ImportRoles::FullGeometry:
                    case ImportRoles::HeightOnly:
                    case ImportRoles::WidthOnly:
                        if (size < 4) {
                            continue;
                        }
                        break;
                    case ImportRoles::Position:
                    case ImportRoles::InvertedPosition:
                    case ImportRoles::OffsetPosition:
                    case ImportRoles::YOnly:
                        if (size < 2) {
                            continue;
                        }
                        break;
                    default:
                        if (size == 0) {
                            continue;
                        }
                        break;
                }
                mlt_rect rect = animData->anim_get_rect("key", frame);
                if (convertMode == ImportRoles::Position || convertMode == ImportRoles::InvertedPosition) {
                    switch (sourceAlign) {
                    case 1:
                        // Align center
                        rect.x += rect.w / 2;
                        rect.y += rect.h / 2;
                        break;
                    case 2:
                        // Align bottom right
                        rect.x += rect.w;
                        rect.y += rect.h;
                        break;
                    default:
                        break;
                    }
                    switch (targetAlign) {
                    case 1:
                        // Align center
                        rect.x -= kfrData[2].toInt() / 2;
                        rect.y -= kfrData[3].toInt() / 2;
                        break;
                    case 2:
                        // Align bottom right
                        rect.x -= kfrData[2].toInt();
                        rect.y -= kfrData[3].toInt();
                        break;
                    default:
                        break;
                    }
                }
                switch (convertMode) {
                    case ImportRoles::FullGeometry:
                        kfrData[0] = locale.toString(int(rect.x));
                        kfrData[1] = locale.toString(int(rect.y));
                        kfrData[2] = locale.toString(int(rect.w));
                        kfrData[3] = locale.toString(int(rect.h));
                        break;
                    case ImportRoles::Position:
                        kfrData[0] = locale.toString(int(rect.x));
                        kfrData[1] = locale.toString(int(rect.y));
                        break;
                    case ImportRoles::InvertedPosition:
                        kfrData[0] = locale.toString(int(-rect.x));
                        kfrData[1] = locale.toString(int(-rect.y));
                        break;
                    case ImportRoles::OffsetPosition:
                        kfrData[0] = locale.toString(int(firstRect.x - rect.x));
                        kfrData[1] = locale.toString(int(firstRect.y - rect.y));
                        break;
                    case ImportRoles::SimpleValue:
                    case ImportRoles::XOnly:
                        kfrData[0] = locale.toString(int(rect.x));
                        break;
                    case ImportRoles::YOnly:
                        kfrData[1] = locale.toString(int(rect.y));
                        break;
                    case ImportRoles::WidthOnly:
                        kfrData[2] = locale.toString(int(rect.w));
                        break;
                    case ImportRoles::HeightOnly:
                        kfrData[3] = locale.toString(int(rect.h));
                        break;
                }
                // map the fake rectangle internaly to the right params
                QString name = ix.data(AssetParameterModel::NameRole).toString();
                QSize frameSize = pCore->getCurrentFrameSize();
                if (name.contains("Position X")
                        && !(convertMode == ImportRoles::WidthOnly || convertMode == ImportRoles::HeightOnly || convertMode == ImportRoles::YOnly) ) {
                    current = kfrData[0].toDouble() / frameSize.width();
                    if (convertMode == ImportRoles::FullGeometry) {
                        current = current.toDouble() + rect.w / frameSize.width() / 2;
                    }
                } else if (name.contains("Position Y")
                          && !(convertMode == ImportRoles::WidthOnly || convertMode == ImportRoles::HeightOnly || convertMode == ImportRoles::XOnly)) {
                    current = kfrData[1].toDouble() / frameSize.height();
                    if (convertMode == ImportRoles::FullGeometry) {
                        current = current.toDouble() + rect.h / frameSize.height() / 2;
                    }
                } else if (name.contains("Size X")
                          && (convertMode == ImportRoles::FullGeometry || convertMode == ImportRoles::InvertedPosition || convertMode == ImportRoles::OffsetPosition || convertMode == ImportRoles::WidthOnly)) {
                    current = kfrData[2].toDouble() / frameSize.width() / 2;
                } else if (name.contains("Size Y")
                          && (convertMode == ImportRoles::FullGeometry || convertMode == ImportRoles::InvertedPosition || convertMode == ImportRoles::OffsetPosition || convertMode == ImportRoles::HeightOnly)) {
                    current = kfrData[3].toDouble() / frameSize.height() / 2;
                } else if(fakeRect){
                    current = km->getInterpolatedValue(frame).toDouble();
                } else {
                    current = kfrData.join(QLatin1Char(' '));
                }
                km->addKeyframe(GenTime(frame - m_inPoint->getPosition() + m_offsetPoint->getPosition(), pCore->getCurrentFps()), KeyframeType(type), current, true, undo, redo);
            }
        } else {
            int frame = 0;
            mlt_keyframe_type type;
            for (int i = 0; i < anim->key_count(); i++) {
                int error = anim->key_get(i, frame, type);
                if (error) {
                    continue;
                }
                //frame += (m_inPoint->getPosition() - m_offsetPoint->getPosition());
                QVariant current = km->getInterpolatedValue(frame);
                km->addKeyframe(GenTime(frame - m_inPoint->getPosition() + m_offsetPoint->getPosition(), pCore->getCurrentFps()), KeyframeType(type), current, true, undo, redo);
            }
        }
    }
    pCore->pushUndo(undo, redo, i18n("Import keyframes from clipboard"));
}

int KeyframeImport::getImportType() const
{
    if (m_simpleTargets.contains(m_targetCombo->currentText())) {
        return -1;
    }
    return m_sourceCombo->currentData().toInt();
}