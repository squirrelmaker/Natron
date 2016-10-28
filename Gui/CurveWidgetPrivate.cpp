/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <http://www.natron.fr/>,
 * Copyright (C) 2016 INRIA and Alexandre Gauthier-Foichat
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Natron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "CurveWidgetPrivate.h"

#include <cmath> // floor
#include <algorithm> // min, max
#include <stdexcept>

#include <QtCore/QThread>
#include <QApplication>
#include <QtCore/QDebug>

#include "Engine/Bezier.h"
#include "Engine/TimeLine.h"
#include "Engine/Settings.h"
#include "Engine/KnobTypes.h"
#include "Engine/Image.h" // Image::clamp
#include "Engine/StringAnimationManager.h"

#include "Gui/ActionShortcuts.h"
#include "Gui/CurveEditor.h"
#include "Gui/CurveWidget.h"
#include "Gui/AnimationModuleEditor.h"
#include "Gui/AnimationModule.h"
#include "Gui/AnimationModuleSelectionModel.h"
#include "Gui/Gui.h"
#include "Gui/KnobGui.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/GuiApplicationManager.h"
#include "Gui/Menu.h"
#include "Gui/ticks.h"


#define AXIS_MAX 100000.
#define AXIS_MIN -100000.

NATRON_NAMESPACE_ENTER;


CurveWidgetPrivate::CurveWidgetPrivate(Gui* gui,
                                       const AnimationModuleBasePtr& model,
                                       AnimationViewBase* publicInterface)
: AnimationModuleViewPrivateBase(gui, publicInterface, model)
, _state(eEventStateNone)
, _selectedDerivative()
, sizeH()
{
}

CurveWidgetPrivate::~CurveWidgetPrivate()
{

}



void
CurveWidgetPrivate::drawCurves()
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    assert( QGLContext::currentContext() == _widget->context() );

    //now draw each curve
    std::vector<CurveGuiPtr> curves = getSelectedCurves();
    int i = 0;
    int nCurves = (int)curves.size();
    for (std::vector<CurveGuiPtr>::const_iterator it = curves.begin(); it != curves.end(); ++it, ++i) {
        (*it)->drawCurve(i, nCurves);
    }
}

void
CurveWidgetPrivate::drawScale()
{
    glCheckError(GL_GPU);
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    assert( QGLContext::currentContext() == _widget->context() );

    QPointF btmLeft = zoomCtx.toZoomCoordinates(0, _widget->height() - 1);
    QPointF topRight = zoomCtx.toZoomCoordinates(_widget->width() - 1, 0);

    ///don't attempt to draw a scale on a widget with an invalid height/width
    if ( (_widget->height() <= 1) || (_widget->width() <= 1) ) {
        return;
    }

    QFontMetrics fontM = _widget->fontMetrics();
    const double smallestTickSizePixel = 10.; // tick size (in pixels) for alpha = 0.
    const double largestTickSizePixel = 500.; // tick size (in pixels) for alpha = 1.
    double gridR, gridG, gridB;
    SettingsPtr sett = appPTR->getCurrentSettings();
    sett->getCurveEditorGridColor(&gridR, &gridG, &gridB);


    double scaleR, scaleG, scaleB;
    sett->getCurveEditorScaleColor(&scaleR, &scaleG, &scaleB);

    QColor scaleColor;
    scaleColor.setRgbF( Image::clamp(scaleR, 0., 1.),
                        Image::clamp(scaleG, 0., 1.),
                        Image::clamp(scaleB, 0., 1.) );


    {
        GLProtectAttrib<GL_GPU> a(GL_CURRENT_BIT | GL_COLOR_BUFFER_BIT | GL_ENABLE_BIT);

        GL_GPU::glEnable(GL_BLEND);
        GL_GPU::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        for (int axis = 0; axis < 2; ++axis) {
            const double rangePixel = (axis == 0) ? _widget->width() : _widget->height(); // AXIS-SPECIFIC
            const double range_min = (axis == 0) ? btmLeft.x() : btmLeft.y(); // AXIS-SPECIFIC
            const double range_max = (axis == 0) ? topRight.x() : topRight.y(); // AXIS-SPECIFIC
            const double range = range_max - range_min;
            double smallTickSize;
            bool half_tick;
            ticks_size(range_min, range_max, rangePixel, smallestTickSizePixel, &smallTickSize, &half_tick);
            int m1, m2;
            const int ticks_max = 1000;
            double offset;
            ticks_bounds(range_min, range_max, smallTickSize, half_tick, ticks_max, &offset, &m1, &m2);
            std::vector<int> ticks;
            ticks_fill(half_tick, ticks_max, m1, m2, &ticks);
            const double smallestTickSize = range * smallestTickSizePixel / rangePixel;
            const double largestTickSize = range * largestTickSizePixel / rangePixel;
            const double minTickSizeTextPixel = (axis == 0) ? fontM.width( QLatin1String("00") ) : fontM.height(); // AXIS-SPECIFIC
            const double minTickSizeText = range * minTickSizeTextPixel / rangePixel;
            for (int i = m1; i <= m2; ++i) {
                double value = i * smallTickSize + offset;
                const double tickSize = ticks[i - m1] * smallTickSize;
                const double alpha = ticks_alpha(smallestTickSize, largestTickSize, tickSize);

                glCheckError(GL_GPU);
                GL_GPU::glColor4f(gridR, gridG, gridB, alpha);

                GL_GPU::glBegin(GL_LINES);
                if (axis == 0) {
                    GL_GPU::glVertex2f( value, btmLeft.y() ); // AXIS-SPECIFIC
                    GL_GPU::glVertex2f( value, topRight.y() ); // AXIS-SPECIFIC
                } else {
                    GL_GPU::glVertex2f(btmLeft.x(), value); // AXIS-SPECIFIC
                    GL_GPU::glVertex2f(topRight.x(), value); // AXIS-SPECIFIC
                }
                GL_GPU::glEnd();
                glCheckErrorIgnoreOSXBug(GL_GPU);

                if (tickSize > minTickSizeText) {
                    const int tickSizePixel = rangePixel * tickSize / range;
                    const QString s = QString::number(value);
                    const int sSizePixel = (axis == 0) ? fontM.width(s) : fontM.height(); // AXIS-SPECIFIC
                    if (tickSizePixel > sSizePixel) {
                        const int sSizeFullPixel = sSizePixel + minTickSizeTextPixel;
                        double alphaText = 1.0; //alpha;
                        if (tickSizePixel < sSizeFullPixel) {
                            // when the text size is between sSizePixel and sSizeFullPixel,
                            // draw it with a lower alpha
                            alphaText *= (tickSizePixel - sSizePixel) / (double)minTickSizeTextPixel;
                        }
                        alphaText = std::min(alphaText, alpha); // don't draw more opaque than tcks
                        QColor c = scaleColor;
                        c.setAlpha(255 * alphaText);
                        if (axis == 0) {
                            _widget->renderText(value, btmLeft.y(), s.toStdString(), c.redF(), c.greenF(), c.blueF(), Qt::AlignHCenter);
                        } else {
                            _widget->renderText(btmLeft.x(), value, s.toStdString(), c.redF(), c.greenF(), c.blueF(), Qt::AlignVCenter);
                        }
                    }
                }
            }
        }
    } // GLProtectAttrib a(GL_CURRENT_BIT | GL_COLOR_BUFFER_BIT | GL_ENABLE_BIT);


    glCheckError(GL_GPU);
    GL_GPU::glColor4f(gridR, gridG, gridB, 1.);
    GL_GPU::glBegin(GL_LINES);
    GL_GPU::glVertex2f(AXIS_MIN, 0);
    GL_GPU::glVertex2f(AXIS_MAX, 0);
    GL_GPU::glVertex2f(0, AXIS_MIN);
    GL_GPU::glVertex2f(0, AXIS_MAX);
    GL_GPU::glEnd();


    glCheckErrorIgnoreOSXBug(GL_GPU);
} // drawScale


CurveGuiPtr
CurveWidgetPrivate::isNearbyCurve(const QPoint &pt,
                                  double* x,
                                  double *y) const
{
    QPointF curvePos = zoomCtx.toZoomCoordinates( pt.x(), pt.y() );

    
   const AnimItemDimViewKeyFramesMap& keys = _model.lock()->getSelectionModel()->getCurrentKeyFramesSelection();
    
    for (AnimItemDimViewKeyFramesMap::const_iterator it = keys.begin(); it != keys.end(); ++it) {
        CurveGuiPtr guiCurve = it->first.item->getCurveGui(it->first.dim, it->first.view);
        if (!guiCurve) {
            continue;
        }

        
        double yCurve = 0;
        
        try {
            yCurve = it->first.item->evaluateCurve( true /*useExpression*/, curvePos.x(), guiCurve->getDimension(), guiCurve->getView());
        } catch (...) {
        }
        
        double yWidget = zoomCtx.toWidgetCoordinates(0, yCurve).y();
        if ( (pt.y() < yWidget + TO_DPIX(CLICK_DISTANCE_TOLERANCE)) && (pt.y() > yWidget - TO_DPIX(CLICK_DISTANCE_TOLERANCE)) ) {
            if (x != NULL) {
                *x = curvePos.x();
            }
            if (y != NULL) {
                *y = yCurve;
            }
            
            return guiCurve;
        }
    }

    return CurveGuiPtr();
} // isNearbyCurve

AnimItemDimViewKeyFrame
CurveWidgetPrivate::isNearbyKeyFrame(const QPoint & pt) const
{
    AnimItemDimViewKeyFrame ret;
    
    const AnimItemDimViewKeyFramesMap& keys = _model.lock()->getSelectionModel()->getCurrentKeyFramesSelection();
    for (AnimItemDimViewKeyFramesMap::const_iterator it = keys.begin(); it != keys.end(); ++it) {
        CurveGuiPtr guiCurve = it->first.item->getCurveGui(it->first.dim, it->first.view);
        if (!guiCurve) {
            continue;
        }
        KeyFrameSet set = guiCurve->getKeyFrames();

        for (KeyFrameSet::const_iterator it2 = set.begin(); it2 != set.end(); ++it2) {
            QPointF keyFramewidgetPos = zoomCtx.toWidgetCoordinates( it2->getTime(), it2->getValue() );
            if ( (std::abs( pt.y() - keyFramewidgetPos.y() ) < TO_DPIX(CLICK_DISTANCE_TOLERANCE)) &&
                (std::abs( pt.x() - keyFramewidgetPos.x() ) < TO_DPIX(CLICK_DISTANCE_TOLERANCE)) ) {
                
                ret.key.key = *it2;
                StringAnimationManagerPtr stringAnim = it->first.item->getInternalAnimItem()->getStringAnimation();
                if (stringAnim) {
                    stringAnim->stringFromInterpolatedIndex(it2->getValue(), it->first.view, &ret.key.string);
                }
                ret.id = it->first;
                
                return ret;
            }
           
        }

    } // for all curves
    return ret;
} // CurveWidgetPrivate::isNearbyKeyFrame

AnimItemDimViewKeyFrame
CurveWidgetPrivate::isNearbyKeyFrameText(const QPoint& pt) const
{
    AnimItemDimViewKeyFrame ret;
    
    QFontMetrics fm( _widget->font() );
    int yOffset = 4;
    const AnimItemDimViewKeyFramesMap& keys = _model.lock()->getSelectionModel()->getCurrentKeyFramesSelection();

    for (AnimItemDimViewKeyFramesMap::const_iterator it = keys.begin(); it != keys.end(); ++it) {
        CurveGuiPtr guiCurve = it->first.item->getCurveGui(it->first.dim, it->first.view);
        if (!guiCurve) {
            continue;
        }
        KeyFrameSet set = guiCurve->getKeyFrames();
        // Bail if multiple selection because text is not visible
        if (set.size() > 1) {
            return ret;
        }

        for (KeyFrameSet::const_iterator it2 = set.begin(); it2 != set.end(); ++it2) {
            
            QPointF topLeftWidget = zoomCtx.toWidgetCoordinates( it2->getTime(), it2->getValue() );
            topLeftWidget.ry() += yOffset;
            
            QString coordStr =  QString::fromUtf8("x: %1, y: %2").arg( it2->getTime() ).arg( it2->getValue() );
            QPointF btmRightWidget( topLeftWidget.x() + fm.width(coordStr), topLeftWidget.y() + fm.height() );
            
            if ( (pt.x() >= topLeftWidget.x() - TO_DPIX(CLICK_DISTANCE_TOLERANCE)) && (pt.x() <= btmRightWidget.x() + TO_DPIX(CLICK_DISTANCE_TOLERANCE)) &&
                ( pt.y() >= topLeftWidget.y() - TO_DPIX(CLICK_DISTANCE_TOLERANCE)) && ( pt.y() <= btmRightWidget.y() + TO_DPIX(CLICK_DISTANCE_TOLERANCE)) ) {
                ret.key.key = *it2;
                StringAnimationManagerPtr stringAnim = it->first.item->getInternalAnimItem()->getStringAnimation();
                if (stringAnim) {
                    stringAnim->stringFromInterpolatedIndex(it2->getValue(), it->first.view, &ret.key.string);
                }
                ret.id = it->first;
                
                return ret;
            }

            
        }
        
    } // for all curves
    return ret;
}

std::pair<MoveTangentCommand::SelectedTangentEnum, AnimItemDimViewKeyFrame >
CurveWidgetPrivate::isNearbyTangent(const QPoint & pt) const
{
    
    std::pair<MoveTangentCommand::SelectedTangentEnum, AnimItemDimViewKeyFrame > ret;

    const AnimItemDimViewKeyFramesMap& keys = _model.lock()->getSelectionModel()->getCurrentKeyFramesSelection();
    for (AnimItemDimViewKeyFramesMap::const_iterator it = keys.begin(); it != keys.end(); ++it) {
        CurveGuiPtr guiCurve = it->first.item->getCurveGui(it->first.dim, it->first.view);
        if (!guiCurve) {
            continue;
        }
        KeyFrameSet set = guiCurve->getKeyFrames();
        
        for (KeyFrameSet::const_iterator it2 = set.begin(); it2 != set.end(); ++it2) {
            
            QPointF leftTanPos, rightTanPos;
            _widget->getKeyTangentPoints(it2, set, &leftTanPos, &rightTanPos);
            QPointF leftTanPt = zoomCtx.toWidgetCoordinates(leftTanPos.x(), leftTanPos.y());
            QPointF rightTanPt = zoomCtx.toWidgetCoordinates( rightTanPos.x(), rightTanPos.y());
            if ( ( pt.x() >= (leftTanPt.x() - TO_DPIX(CLICK_DISTANCE_TOLERANCE)) ) &&
                ( pt.x() <= (leftTanPt.x() + TO_DPIX(CLICK_DISTANCE_TOLERANCE)) ) &&
                ( pt.y() <= (leftTanPt.y() + TO_DPIX(CLICK_DISTANCE_TOLERANCE)) ) &&
                ( pt.y() >= (leftTanPt.y() - TO_DPIX(CLICK_DISTANCE_TOLERANCE)) ) ) {
                
                ret.second.key.key = *it2;
                StringAnimationManagerPtr stringAnim = it->first.item->getInternalAnimItem()->getStringAnimation();
                if (stringAnim) {
                    stringAnim->stringFromInterpolatedIndex(it2->getValue(), it->first.view, &ret.second.key.string);
                }
                ret.second.id = it->first;
                ret.first = MoveTangentCommand::eSelectedTangentLeft;
                return ret;
            } else if ( ( pt.x() >= (rightTanPt.x() - TO_DPIX(CLICK_DISTANCE_TOLERANCE)) ) &&
                       ( pt.x() <= (rightTanPt.x() + TO_DPIX(CLICK_DISTANCE_TOLERANCE)) ) &&
                       ( pt.y() <= (rightTanPt.y() + TO_DPIX(CLICK_DISTANCE_TOLERANCE)) ) &&
                       ( pt.y() >= (rightTanPt.y() - TO_DPIX(CLICK_DISTANCE_TOLERANCE)) ) ) {
                ret.second.key.key = *it2;
                StringAnimationManagerPtr stringAnim = it->first.item->getInternalAnimItem()->getStringAnimation();
                if (stringAnim) {
                    stringAnim->stringFromInterpolatedIndex(it2->getValue(), it->first.view, &ret.second.key.string);
                }
                ret.second.id = it->first;
                ret.first = MoveTangentCommand::eSelectedTangentRight;
                return ret;
            }
        }
        
    } // for all curves
    return ret;
    
}

std::pair<MoveTangentCommand::SelectedTangentEnum, AnimItemDimViewKeyFrame >
CurveWidgetPrivate::isNearbySelectedTangentText(const QPoint & pt) const
{
    QFontMetrics fm( _widget->font() );
    int yOffset = 4;
    std::pair<MoveTangentCommand::SelectedTangentEnum, AnimItemDimViewKeyFrame > ret;
    
    const AnimItemDimViewKeyFramesMap& keys = _model.lock()->getSelectionModel()->getCurrentKeyFramesSelection();
    
    for (AnimItemDimViewKeyFramesMap::const_iterator it = keys.begin(); it != keys.end(); ++it) {
        CurveGuiPtr guiCurve = it->first.item->getCurveGui(it->first.dim, it->first.view);
        if (!guiCurve) {
            continue;
        }
        KeyFrameSet set = guiCurve->getKeyFrames();
        // Bail if multiple selection because text is not visible
        if (set.size() > 1) {
            return ret;
        }
        for (KeyFrameSet::const_iterator it2 = set.begin(); it2 != set.end(); ++it2) {
            
            QPointF leftTanPos, rightTanPos;
            _widget->getKeyTangentPoints(it2, set, &leftTanPos, &rightTanPos);
            
            double rounding = std::pow(10., CURVEWIDGET_DERIVATIVE_ROUND_PRECISION);
            QPointF topLeft_LeftTanWidget = zoomCtx.toWidgetCoordinates( leftTanPos.x(), leftTanPos.y() );
            QPointF topLeft_RightTanWidget = zoomCtx.toWidgetCoordinates( rightTanPos.x(), rightTanPos.y() );
            topLeft_LeftTanWidget.ry() += yOffset;
            topLeft_RightTanWidget.ry() += yOffset;
            
            QString leftCoordStr =  QString( tr("l: %1") ).arg(std::floor( ( it2->getLeftDerivative() * rounding ) + 0.5 ) / rounding);
            QString rightCoordStr =  QString( tr("r: %1") ).arg(std::floor( ( it2->getRightDerivative() * rounding ) + 0.5 ) / rounding);
            QPointF btmRight_LeftTanWidget( topLeft_LeftTanWidget.x() + fm.width(leftCoordStr), topLeft_LeftTanWidget.y() + fm.height() );
            QPointF btmRight_RightTanWidget( topLeft_RightTanWidget.x() + fm.width(rightCoordStr), topLeft_RightTanWidget.y() + fm.height() );
            
            if ( (pt.x() >= topLeft_LeftTanWidget.x() - TO_DPIX(CLICK_DISTANCE_TOLERANCE)) && (pt.x() <= btmRight_LeftTanWidget.x() + TO_DPIX(CLICK_DISTANCE_TOLERANCE)) &&
                ( pt.y() >= topLeft_LeftTanWidget.y() - TO_DPIX(CLICK_DISTANCE_TOLERANCE)) && ( pt.y() <= btmRight_LeftTanWidget.y() + TO_DPIX(CLICK_DISTANCE_TOLERANCE)) ) {
                ret.second.key.key = *it2;
                StringAnimationManagerPtr stringAnim = it->first.item->getInternalAnimItem()->getStringAnimation();
                if (stringAnim) {
                    stringAnim->stringFromInterpolatedIndex(it2->getValue(), it->first.view, &ret.second.key.string);
                }
                ret.second.id = it->first;
                ret.first = MoveTangentCommand::eSelectedTangentLeft;
            } else if ( (pt.x() >= topLeft_RightTanWidget.x() - TO_DPIX(CLICK_DISTANCE_TOLERANCE)) && (pt.x() <= btmRight_RightTanWidget.x() + TO_DPIX(CLICK_DISTANCE_TOLERANCE)) &&
                       ( pt.y() >= topLeft_RightTanWidget.y() - TO_DPIX(CLICK_DISTANCE_TOLERANCE)) && ( pt.y() <= btmRight_RightTanWidget.y() + TO_DPIX(CLICK_DISTANCE_TOLERANCE)) ) {
                ret.second.key.key = *it2;
                StringAnimationManagerPtr stringAnim = it->first.item->getInternalAnimItem()->getStringAnimation();
                if (stringAnim) {
                    stringAnim->stringFromInterpolatedIndex(it2->getValue(), it->first.view, &ret.second.key.string);
                }
                ret.second.id = it->first;
                ret.first = MoveTangentCommand::eSelectedTangentRight;
            }
            
        }
        
    } // for all curves
    return ret;

} // isNearbySelectedTangentText


void
CurveWidgetPrivate::keyFramesWithinRect(const RectD& canonicalRect,
                                        AnimItemDimViewKeyFramesMap* keys) const
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );

    
    const AnimItemDimViewKeyFramesMap& selectedKeys = _model.lock()->getSelectionModel()->getCurrentKeyFramesSelection();
    
    for (AnimItemDimViewKeyFramesMap::const_iterator it = selectedKeys.begin(); it != selectedKeys.end(); ++it) {
        CurveGuiPtr guiCurve = it->first.item->getCurveGui(it->first.dim, it->first.view);
        if (!guiCurve) {
            continue;
        }
        
        KeyFrameSet set = guiCurve->getKeyFrames();
        if ( set.empty() ) {
            continue;
        }
        
        StringAnimationManagerPtr stringAnim = it->first.item->getInternalAnimItem()->getStringAnimation();
        
        
        KeyFrameWithStringSet& outKeys = (*keys)[it->first];
        
        for ( KeyFrameSet::const_iterator it2 = set.begin(); it2 != set.end(); ++it2) {
            double y = it2->getValue();
            double x = it2->getTime();
            if ( (x <= canonicalRect.x2) && (x >= canonicalRect.x1) && (y <= canonicalRect.y2) && (y >= canonicalRect.y1) ) {
                //KeyPtr newSelectedKey( new SelectedKey(*it, *it2, hasPrev, prevKey, hasNext, nextKey) );
                KeyFrameWithString k;
                k.key = *it2;
                if (stringAnim) {
                    stringAnim->stringFromInterpolatedIndex(it2->getValue(), it->first.view, &k.string);
                }
                outKeys.insert(k);
            }
        }
        
    }
} // CurveWidgetPrivate::keyFramesWithinRect



void
CurveWidgetPrivate::moveSelectedTangent(const QPointF & pos)
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );


    AnimItemDimViewKeyFrame& key = _selectedDerivative.second;
    double dy = key.key.key.getValue() - pos.y();
    double dx = key.key.key.getTime() - pos.x();


    AnimationModuleBasePtr model = _model.lock();
    model->pushUndoCommand(new MoveTangentCommand(model, _selectedDerivative.first, key.id.item, key.id.dim, key.id.view, key.key.key, dx, dy));
} // moveSelectedTangent



void
CurveWidgetPrivate::refreshSelectionRectangle(double x,
                                              double y)
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );

    QPointF dragStartPointCanonical = _publicInterface->toWidgetCoordinates(x,y);

    selectionRect.x1 = std::min(dragStartPointCanonical.x(), x);
    selectionRect.x2 = std::max(dragStartPointCanonical.x(), x);
    selectionRect.y1 = std::min(dragStartPointCanonical.y(), y);
    selectionRect.y2 = std::max(dragStartPointCanonical.y(), y);

    AnimationModuleBasePtr model = _model.lock();
    AnimationModuleSelectionModelPtr selectModel = model->getSelectionModel();

    AnimItemDimViewKeyFramesMap selectedKeys;
    keyFramesWithinRect(selectionRect, &selectedKeys);

    selectModel->makeSelection( selectedKeys, std::vector<TableItemAnimPtr>(), std::vector<NodeAnimPtr>(), (AnimationModuleSelectionModel::SelectionTypeAdd |
                                                                                                            AnimationModuleSelectionModel::SelectionTypeClear) );
    _widget->refreshSelectionBoundingBox();
}


void
CurveWidgetPrivate::updateDopeSheetViewFrameRange()
{
    AnimationModulePtr animModule = toAnimationModule(_model.lock());
    if (!animModule) {
        return;
    }
#pragma message WARN("fix this")
    animModule->getEditor()->centerOn( zoomCtx.left(), zoomCtx.right() );
}

void
CurveWidgetPrivate::addMenuOptions()
{
    Menu* fileMenu = new Menu(_rightClickMenu);
    //fileMenu->setFont( QFont(appFont,appFontSize) );
    fileMenu->setTitle( tr("File") );
    _rightClickMenu->addAction( fileMenu->menuAction() );

    AnimationModulePtr isAnimModule = toAnimationModule(_model.lock());

    Menu* predefMenu  = 0;
    if (isAnimModule) {
        predefMenu = new Menu(_rightClickMenu);
        predefMenu->setTitle( tr("Predefined") );
        _rightClickMenu->addAction( predefMenu->menuAction() );
    }

    


    QAction* exportCurveToAsciiAction = new QAction(tr("Export curve to ASCII file"), fileMenu);
    QObject::connect( exportCurveToAsciiAction, SIGNAL(triggered()), _widget, SLOT(onExportCurveToAsciiActionTriggered()) );
    fileMenu->addAction(exportCurveToAsciiAction);

    QAction* importCurveFromAsciiAction = new QAction(tr("Import curve from ASCII file"), fileMenu);
    QObject::connect( importCurveFromAsciiAction, SIGNAL(triggered()), _widget, SLOT(onImportCurveFromAsciiActionTriggered()) );
    fileMenu->addAction(importCurveFromAsciiAction);


    if (predefMenu) {
        QAction* loop = new QAction(tr("Loop"), _rightClickMenu);
        QObject::connect( loop, SIGNAL(triggered()), _widget, SLOT(onApplyLoopExpressionOnSelectedCurveActionTriggered()) );
        predefMenu->addAction(loop);

        QAction* reverse = new QAction(tr("Reverse"), _rightClickMenu);
        QObject::connect( reverse, SIGNAL(triggered()), _widget, SLOT(onApplyReverseExpressionOnSelectedCurveActionTriggered()) );
        predefMenu->addAction(reverse);


        QAction* negate = new QAction(tr("Negate"), _rightClickMenu);
        QObject::connect( negate, SIGNAL(triggered()), _widget, SLOT(onApplyNegateExpressionOnSelectedCurveActionTriggered()) );
        predefMenu->addAction(negate);
    }
}


NATRON_NAMESPACE_EXIT;

