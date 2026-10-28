/* -*- C++ -*-
 *
 *  This file is part of RawTherapee.
 *
 *  RawTherapee is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  RawTherapee is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with RawTherapee.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "masking.h"

#include <cstdlib>
#include <functional>

#include "../editorpanel.h"

#include <cmath>

#include "eventmapper.h"
#include "options.h"
#include "paramsedited.h"
#include "rtimage.h"
#include "widgets/curves/curveeditor.h"
#include "widgets/curves/curveeditorgroup.h"

#include "rtengine/aisegmentation.h"
#include "rtengine/color.h"
#include "rtengine/masktransform.h"
#include "rtengine/procparams.h"
#include "rtengine/refreshmap.h"
#include "rtengine/rt_math.h"
#include "rtengine/utils.h"

using namespace rtengine;
using namespace rtengine::procparams;

const Glib::ustring Masking::TOOL_NAME = "masking";

namespace
{

constexpr int ELLIPSE_POINTS = 64;
constexpr int HANDLE_RADIUS = 7;      // visible handle radius, screen pixels
constexpr int MO_HANDLE_RADIUS = 12;  // mouse-over handle radius, screen pixels

const MaskComponentType COMPONENT_TYPES[] = {
    MaskComponentType::BRUSH,
    MaskComponentType::RECTANGLE,
    MaskComponentType::POLYGON,
    MaskComponentType::LINEAR_GRADIENT,
    MaskComponentType::RADIAL_GRADIENT,
    MaskComponentType::MAGIC_WAND,
    MaskComponentType::QUICK_SELECT,
    MaskComponentType::COLOR_RANGE,
    MaskComponentType::LUMINANCE_RANGE,
    MaskComponentType::AI_SUBJECT,
    MaskComponentType::AI_SKY,
    MaskComponentType::AI_BACKGROUND,
    MaskComponentType::AI_OBJECT
};

constexpr int COMPONENT_TYPE_COUNT = sizeof(COMPONENT_TYPES) / sizeof(COMPONENT_TYPES[0]);
constexpr int SEED_MARKER_COUNT = 32;

}

Masking::Masking() :
    FoldableToolPanel(this, TOOL_NAME, M("TP_MASKING_LABEL"), false, true),
    EditSubscriber(ET_OBJECTS),
    selectedMask(-1),
    selectedComponent(-1),
    listEdited(false),
    maskingProvider(nullptr),
    dragMode(DragMode::NONE),
    lastObject(-1),
    lastModifier(0),
    strokeInProgress(false),
    overlayAutoEnabled(false),
    shapeDragged(false),
    hasPickPos(false),
    polyOpen(false),
    polyInProgress(false),
    rectAnchorX(0.0),
    rectAnchorY(0.0),
    editedCheckBox(nullptr)
{
    auto m = ProcEventMapper::getInstance();
    EvMaskingEnabled = m->newEvent(MASKING, "HISTORY_MSG_MASKING_ENABLED");
    EvMaskingList = m->newEvent(MASKING, "HISTORY_MSG_MASKING_LIST");
    EvMaskingLayer = m->newEvent(MASKING, "HISTORY_MSG_MASKING_LAYER");
    EvMaskingComponentList = m->newEvent(MASKING, "HISTORY_MSG_MASKING_COMPONENT_LIST");
    EvMaskingComponent = m->newEvent(MASKING, "HISTORY_MSG_MASKING_COMPONENT");
    EvMaskingStroke = m->newEvent(MASKING, "HISTORY_MSG_MASKING_STROKE");
    EvMaskingShape = m->newEvent(MASKING, "HISTORY_MSG_MASKING_SHAPE");
    EvMaskingSample = m->newEvent(MASKING, "HISTORY_MSG_MASKING_SAMPLE");
    EvMaskingShowMask = m->newEvent(MASKING, "HISTORY_MSG_MASKING_SHOWMASK");
    EvMaskingAmount = m->newEvent(MASKING, "HISTORY_MSG_MASKING_AMOUNT");
    EvMaskingFeather = m->newEvent(MASKING, "HISTORY_MSG_MASKING_FEATHER");
    EvMaskingExposure = m->newEvent(MASKING, "HISTORY_MSG_MASKING_EXPOSURE");
    EvMaskingContrast = m->newEvent(MASKING, "HISTORY_MSG_MASKING_CONTRAST");
    EvMaskingHighlights = m->newEvent(MASKING, "HISTORY_MSG_MASKING_HIGHLIGHTS");
    EvMaskingMidtones = m->newEvent(MASKING, "HISTORY_MSG_MASKING_MIDTONES");
    EvMaskingShadows = m->newEvent(MASKING, "HISTORY_MSG_MASKING_SHADOWS");
    EvMaskingWhites = m->newEvent(MASKING, "HISTORY_MSG_MASKING_WHITES");
    EvMaskingBlacks = m->newEvent(MASKING, "HISTORY_MSG_MASKING_BLACKS");
    EvMaskingTemperature = m->newEvent(MASKING, "HISTORY_MSG_MASKING_TEMPERATURE");
    EvMaskingTint = m->newEvent(MASKING, "HISTORY_MSG_MASKING_TINT");
    EvMaskingHue = m->newEvent(MASKING, "HISTORY_MSG_MASKING_HUE");
    EvMaskingSaturation = m->newEvent(MASKING, "HISTORY_MSG_MASKING_SATURATION");
    EvMaskingClarity = m->newEvent(MASKING, "HISTORY_MSG_MASKING_CLARITY");
    EvMaskingExpand = m->newEvent(MASKING, "HISTORY_MSG_MASKING_EXPAND");
    EvMaskingSmooth = m->newEvent(MASKING, "HISTORY_MSG_MASKING_SMOOTH");
    EvMaskingPolygon = m->newEvent(MASKING, "HISTORY_MSG_MASKING_POLYGON");
    EvMaskingSeeds = m->newEvent(MASKING, "HISTORY_MSG_MASKING_SEEDS");
    EvMaskingCurveL = m->newEvent(MASKING, "HISTORY_MSG_MASKING_CURVE_L");
    EvMaskingCurveA = m->newEvent(MASKING, "HISTORY_MSG_MASKING_CURVE_A");
    EvMaskingCurveB = m->newEvent(MASKING, "HISTORY_MSG_MASKING_CURVE_B");
    EvMaskingCurveCC = m->newEvent(MASKING, "HISTORY_MSG_MASKING_CURVE_CC");
    EvMaskingCurveCH = m->newEvent(MASKING, "HISTORY_MSG_MASKING_CURVE_CH");
    EvMaskingCurveLH = m->newEvent(MASKING, "HISTORY_MSG_MASKING_CURVE_LH");
    EvMaskingCurveHH = m->newEvent(MASKING, "HISTORY_MSG_MASKING_CURVE_HH");
    EvMaskingCurveLC = m->newEvent(MASKING, "HISTORY_MSG_MASKING_CURVE_LC");
    EvMaskingCurveCL = m->newEvent(MASKING, "HISTORY_MSG_MASKING_CURVE_CL");
    CurveListener::setMulti(true);

    const auto add_button =
        [&](Gtk::Button* btn, Gtk::Box* box) -> void {
            setExpandAlignProperties(btn, false, false, Gtk::ALIGN_CENTER, Gtk::ALIGN_START);
            btn->set_relief(Gtk::RELIEF_NONE);
            btn->get_style_context()->add_class(GTK_STYLE_CLASS_FLAT);
            btn->set_can_focus(false);
            btn->set_size_request(-1, 20);
            box->pack_start(*btn, false, false);
        };

    // ------------------------------------------------------------------
    // Layers: one tree, each mask with the tools that build it nested beneath
    {
        treeStore = Gtk::TreeStore::create(treeColumns);
        tree = Gtk::manage(new Gtk::TreeView(treeStore));
        tree->set_headers_visible(false);
        tree->set_can_focus(false);
        tree->set_activate_on_single_click(true);
        tree->set_level_indentation(6);

        Gtk::CellRendererToggle* enabledRenderer = Gtk::manage(new Gtk::CellRendererToggle());
        enabledRenderer->signal_toggled().connect(sigc::mem_fun(*this, &Masking::onTreeEnabledToggled));
        Gtk::TreeViewColumn* colEnabled = Gtk::manage(new Gtk::TreeViewColumn());
        colEnabled->pack_start(*enabledRenderer, false);
        colEnabled->add_attribute(enabledRenderer->property_active(), treeColumns.enabled);
        tree->append_column(*colEnabled);

        Gtk::TreeViewColumn* colMain = Gtk::manage(new Gtk::TreeViewColumn());
        Gtk::CellRendererPixbuf* iconRenderer = Gtk::manage(new Gtk::CellRendererPixbuf());
        iconRenderer->property_stock_size() = Gtk::ICON_SIZE_SMALL_TOOLBAR;
        Gtk::CellRendererText* labelRenderer = Gtk::manage(new Gtk::CellRendererText());
        labelRenderer->property_ellipsize() = Pango::ELLIPSIZE_END;
        colMain->pack_start(*iconRenderer, false);
        colMain->pack_start(*labelRenderer, true);
        colMain->add_attribute(*iconRenderer, "icon-name", treeColumns.icon);
        colMain->add_attribute(*labelRenderer, "markup", treeColumns.label);
        colMain->set_expand(true);
        tree->append_column(*colMain);

        Gtk::TreeViewColumn* colMode = Gtk::manage(new Gtk::TreeViewColumn());
        Gtk::CellRendererText* modeRenderer = Gtk::manage(new Gtk::CellRendererText());
        modeRenderer->property_xalign() = 1.f;
        colMode->pack_start(*modeRenderer, true);
        colMode->add_attribute(*modeRenderer, "markup", treeColumns.mode);
        tree->append_column(*colMode);

        treeSelectionConn = tree->get_selection()->signal_changed().connect(sigc::mem_fun(*this, &Masking::onTreeSelectionChanged));

        Gtk::ScrolledWindow* sw = Gtk::manage(new Gtk::ScrolledWindow());
        sw->set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
        sw->set_size_request(-1, 160);
        sw->add(*tree);

        Gtk::Box* hb = Gtk::manage(new Gtk::Box());
        hb->pack_start(*sw, Gtk::PACK_EXPAND_WIDGET);

        Gtk::Box* vb = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL));
        maskRemove = Gtk::manage(new Gtk::Button());
        maskRemove->add(*Gtk::manage(new RTImage("trash-remove-small", Gtk::ICON_SIZE_BUTTON)));
        maskRemove->set_tooltip_text(M("TP_MASKING_REMOVE_MASK_TOOLTIP"));
        maskRemove->signal_clicked().connect(sigc::mem_fun(*this, &Masking::onRemovePressed));
        add_button(maskRemove, vb);
        maskDuplicate = Gtk::manage(new Gtk::Button());
        maskDuplicate->add(*Gtk::manage(new RTImage("arrow-right-small", Gtk::ICON_SIZE_BUTTON)));
        maskDuplicate->set_tooltip_text(M("TP_MASKING_DUPLICATE_MASK_TOOLTIP"));
        maskDuplicate->signal_clicked().connect(sigc::mem_fun(*this, &Masking::onMaskDuplicatePressed));
        add_button(maskDuplicate, vb);
        maskUndoTool = Gtk::manage(new Gtk::Button());
        maskUndoTool->add(*Gtk::manage(new RTImage("undo-small", Gtk::ICON_SIZE_BUTTON)));
        maskUndoTool->set_tooltip_text(M("TP_MASKING_UNDO_TOOL_TOOLTIP"));
        maskUndoTool->signal_clicked().connect(sigc::mem_fun(*this, &Masking::onUndoToolPressed));
        add_button(maskUndoTool, vb);
        maskClear = Gtk::manage(new Gtk::Button());
        maskClear->add(*Gtk::manage(new RTImage("remove-small", Gtk::ICON_SIZE_BUTTON)));
        maskClear->set_tooltip_text(M("TP_MASKING_CLEAR_MASK_TOOLTIP"));
        maskClear->signal_clicked().connect(sigc::mem_fun(*this, &Masking::onClearMaskPressed));
        add_button(maskClear, vb);
        hb->pack_start(*vb, Gtk::PACK_SHRINK);
        pack_start(*hb, Gtk::PACK_SHRINK, 2);

        Gtk::Box* nameBox = Gtk::manage(new Gtk::Box());
        nameBox->pack_start(*Gtk::manage(new Gtk::Label(M("TP_MASKING_MASK_NAME") + ":")), Gtk::PACK_SHRINK, 2);
        maskName = Gtk::manage(new Gtk::Entry());
        maskName->set_placeholder_text(M("TP_MASKING_MASK_NAME"));
        maskNameConn = maskName->signal_changed().connect(sigc::mem_fun(*this, &Masking::onMaskNameChanged));
        nameBox->pack_start(*maskName, Gtk::PACK_EXPAND_WIDGET, 2);
        pack_start(*nameBox, Gtk::PACK_SHRINK, 2);

        // Visible feedback while an AI model computes a mask (the first use
        // also loads the model, which takes seconds).
        aiBusyBox = Gtk::manage(new Gtk::Box());
        aiSpinner = Gtk::manage(new Gtk::Spinner());
        aiBusyBox->pack_start(*aiSpinner, Gtk::PACK_SHRINK, 4);
        aiBusyLabel = Gtk::manage(new Gtk::Label(M("TP_MASKING_AI_BUSY")));
        aiBusyLabel->set_line_wrap(true);
        aiBusyLabel->set_xalign(0.f);
        aiBusyBox->pack_start(*aiBusyLabel, Gtk::PACK_EXPAND_WIDGET, 2);
        pack_start(*aiBusyBox, Gtk::PACK_SHRINK, 2);
    }

    // ------------------------------------------------------------------
    // Intent bar and tool palette: pick what a tool click does, then the tool.
    {
        Gtk::Frame* frame = Gtk::manage(new Gtk::Frame(M("TP_MASKING_TOOLS")));
        Gtk::Box* fbox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL));

        const char* intentIcons[INTENT_COUNT] = {"mask-new", "mask-op-add", "mask-op-subtract", "mask-op-intersect"};
        const char* intentKeys[INTENT_COUNT] = {"TP_MASKING_INTENT_NEW", "TP_MASKING_INTENT_ADD", "TP_MASKING_INTENT_SUBTRACT", "TP_MASKING_INTENT_INTERSECT"};
        const char* intentTips[INTENT_COUNT] = {"TP_MASKING_INTENT_NEW_TOOLTIP", "TP_MASKING_INTENT_ADD_TOOLTIP", "TP_MASKING_INTENT_SUBTRACT_TOOLTIP", "TP_MASKING_INTENT_INTERSECT_TOOLTIP"};
        Gtk::Box* intentBox = Gtk::manage(new Gtk::Box());
        intentBox->set_homogeneous(true);

        for (int i = 0; i < INTENT_COUNT; ++i) {
            Gtk::ToggleButton* b = Gtk::manage(new Gtk::ToggleButton());
            Gtk::Box* content = Gtk::manage(new Gtk::Box());
            content->pack_start(*Gtk::manage(new RTImage(intentIcons[i], Gtk::ICON_SIZE_BUTTON)), Gtk::PACK_SHRINK, 2);
            content->pack_start(*Gtk::manage(new Gtk::Label(M(intentKeys[i]))), Gtk::PACK_SHRINK, 2);
            b->add(*content);
            b->set_tooltip_text(M(intentTips[i]));
            b->set_can_focus(false);
            intentConn[i] = b->signal_toggled().connect(sigc::bind(sigc::mem_fun(*this, &Masking::onIntentToggled), i));
            intentBox->pack_start(*b, Gtk::PACK_EXPAND_WIDGET, 1);
            intentButtons[i] = b;
        }

        fbox->pack_start(*intentBox, Gtk::PACK_SHRINK, 2);

        intentLabel = Gtk::manage(new Gtk::Label());
        intentLabel->set_line_wrap(true);
        intentLabel->set_xalign(0.f);
        intentLabel->get_style_context()->add_class("dim-label");
        fbox->pack_start(*intentLabel, Gtk::PACK_SHRINK, 2);

        {
            // initial state, without running the handlers on a half-built panel
            ConnectionBlocker b(intentConn[INTENT_NEW]);
            intentButtons[INTENT_NEW]->set_active(true);
        }

        palette = Gtk::manage(new Gtk::Grid());
        palette->set_row_homogeneous(true);
        palette->set_column_homogeneous(true);
        palette->set_row_spacing(2);
        palette->set_column_spacing(2);

        int cell = 0;

        for (int i = 0; i < COMPONENT_TYPE_COUNT; ++i) {
            Gtk::Button* b = makeToolButton(COMPONENT_TYPES[i]);
            palette->attach(*b, cell % 4, cell / 4, 1, 1);
            paletteButtons.push_back(b);
            ++cell;

            if (COMPONENT_TYPES[i] == MaskComponentType::BRUSH) {
                // the eraser sits next to the brush: same tool, removing instead of adding
                Gtk::Button* e = Gtk::manage(new Gtk::Button());
                Gtk::Box* content = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL));
                content->pack_start(*Gtk::manage(new RTImage("eraser-small", Gtk::ICON_SIZE_LARGE_TOOLBAR)), Gtk::PACK_SHRINK, 1);
                Gtk::Label* label = Gtk::manage(new Gtk::Label());
                label->set_markup("<small>" + Glib::Markup::escape_text(M("TP_MASKING_TOOL_ERASER")) + "</small>");
                content->pack_start(*label, Gtk::PACK_SHRINK, 0);
                e->add(*content);
                e->set_tooltip_text(M("TP_MASKING_BRUSH_ERASE_TOOLTIP"));
                e->set_relief(Gtk::RELIEF_NONE);
                e->set_can_focus(false);
                e->signal_clicked().connect(sigc::mem_fun(*this, &Masking::onEraserClicked));
                palette->attach(*e, cell % 4, cell / 4, 1, 1);
                paletteButtons.push_back(e);
                ++cell;
            }
        }

        fbox->pack_start(*palette, Gtk::PACK_SHRINK, 2);
        frame->add(*fbox);
        pack_start(*frame, Gtk::PACK_SHRINK, 4);
    }

    // ------------------------------------------------------------------
    // Selected tool: mode, invert, on-canvas editing and its settings
    {
        Gtk::Frame* frame = Gtk::manage(new Gtk::Frame(M("TP_MASKING_SELECTED_TOOL")));
        Gtk::Box* fbox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL));

        toolHeader = Gtk::manage(new Gtk::Label());
        toolHeader->set_xalign(0.f);
        toolHeader->set_line_wrap(true);
        fbox->pack_start(*toolHeader, Gtk::PACK_SHRINK, 2);

        Gtk::Box* opBox = Gtk::manage(new Gtk::Box());
        opBox->pack_start(*Gtk::manage(new Gtk::Label(M("TP_MASKING_LIST_OP") + ":")), Gtk::PACK_SHRINK, 2);

        const char* opIcons[3] = {"mask-op-add", "mask-op-subtract", "mask-op-intersect"};
        const char* opKeys[3] = {"TP_MASKING_OP_ADD", "TP_MASKING_OP_SUBTRACT", "TP_MASKING_OP_INTERSECT"};

        for (int i = 0; i < 3; ++i) {
            Gtk::ToggleButton* b = Gtk::manage(new Gtk::ToggleButton());
            b->add(*Gtk::manage(new RTImage(opIcons[i], Gtk::ICON_SIZE_BUTTON)));
            b->set_tooltip_text(M(opKeys[i]) + "\n" + M("TP_MASKING_OP_TOOLTIP"));
            b->set_can_focus(false);
            opConn[i] = b->signal_toggled().connect(sigc::bind(sigc::mem_fun(*this, &Masking::onOpToggled), i));
            opBox->pack_start(*b, Gtk::PACK_SHRINK, 1);
            opButtons[i] = b;
        }

        {
            ConnectionBlocker b(opConn[0]);
            opButtons[0]->set_active(true);
        }

        componentInvert = Gtk::manage(new Gtk::CheckButton(M("TP_MASKING_INVERT")));
        componentInvertConn = componentInvert->signal_toggled().connect(sigc::mem_fun(*this, &Masking::onComponentInvertToggled));
        opBox->pack_start(*componentInvert, Gtk::PACK_SHRINK, 2);
        fbox->pack_start(*opBox, Gtk::PACK_SHRINK, 2);

        // on-canvas editing
        editBox = Gtk::manage(new Gtk::Box());
        edit = Gtk::manage(new Gtk::ToggleButton());
        edit->get_style_context()->add_class("independent");
        edit->add(*Gtk::manage(new RTImage("crosshair-adjust", Gtk::ICON_SIZE_BUTTON)));
        edit->set_tooltip_text(M("TP_MASKING_EDIT_TOOLTIP"));
        editConn = edit->signal_toggled().connect(sigc::mem_fun(*this, &Masking::editToggled));
        editBox->pack_start(*edit, Gtk::PACK_SHRINK, 0);
        editHint = Gtk::manage(new Gtk::Label());
        editHint->set_line_wrap(true);
        editHint->set_xalign(0.f);
        editBox->pack_start(*editHint, Gtk::PACK_EXPAND_WIDGET, 4);
        fbox->pack_start(*editBox, Gtk::PACK_SHRINK, 2);

        // brush
        brushBox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL));
        brushSize = Gtk::manage(new Adjuster(M("TP_MASKING_BRUSH_SIZE"), 0.02, 25.0, 0.01, 3.0));
        brushSize->setLogScale(1000, 0.02);   // fine control at small sizes
        brushSize->set_tooltip_text(M("TP_MASKING_BRUSH_SIZE_TOOLTIP"));
        brushFeather = Gtk::manage(new Adjuster(M("TP_MASKING_BRUSH_FEATHER"), 0, 100, 1, 50));
        brushOpacity = Gtk::manage(new Adjuster(M("TP_MASKING_BRUSH_OPACITY"), 1, 100, 1, 100));
        brushOpacity->set_tooltip_text(M("TP_MASKING_BRUSH_OPACITY_TOOLTIP"));
        brushSize->setAdjusterListener(this);
        brushFeather->setAdjusterListener(this);
        brushOpacity->setAdjusterListener(this);
        brushBox->pack_start(*brushSize);
        brushBox->pack_start(*brushFeather);
        brushBox->pack_start(*brushOpacity);
        Gtk::Box* brushBtnBox = Gtk::manage(new Gtk::Box());
        brushPaint = Gtk::manage(new Gtk::ToggleButton());
        brushPaint->add(*Gtk::manage(new RTImage("paintbrush-small", Gtk::ICON_SIZE_BUTTON)));
        brushPaint->set_tooltip_text(M("TP_MASKING_BRUSH_PAINT_TOOLTIP"));
        brushPaint->set_active(true);
        brushPaintConn = brushPaint->signal_toggled().connect(sigc::bind(sigc::mem_fun(*this, &Masking::onBrushModeToggled), false));
        brushBtnBox->pack_start(*brushPaint, Gtk::PACK_SHRINK, 2);
        brushErase = Gtk::manage(new Gtk::ToggleButton());
        brushErase->add(*Gtk::manage(new RTImage("eraser-small", Gtk::ICON_SIZE_BUTTON)));
        brushErase->set_tooltip_text(M("TP_MASKING_BRUSH_ERASE_TOOLTIP"));
        brushEraseConn = brushErase->signal_toggled().connect(sigc::bind(sigc::mem_fun(*this, &Masking::onBrushModeToggled), true));
        brushBtnBox->pack_start(*brushErase, Gtk::PACK_SHRINK, 2);
        // The paint / erase toggles are state only: the palette's Brush and
        // Eraser buttons (and Alt while painting) choose the mode.
        brushBox->pack_start(*brushBtnBox, Gtk::PACK_SHRINK, 2);
        brushBtnBox->hide();
        brushBtnBox->set_no_show_all(true);
        fbox->pack_start(*brushBox, Gtk::PACK_SHRINK);

        // radial gradient
        radialBox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL));
        radialFeather = Gtk::manage(new Adjuster(M("TP_MASKING_RADIAL_FEATHER"), 0, 100, 1, 50));
        radialAngle = Gtk::manage(new Adjuster(M("TP_MASKING_RADIAL_ANGLE"), -180, 180, 1, 0));
        radialFeather->setAdjusterListener(this);
        radialAngle->setAdjusterListener(this);
        radialBox->pack_start(*radialFeather);
        radialBox->pack_start(*radialAngle);
        fbox->pack_start(*radialBox, Gtk::PACK_SHRINK);

        // rectangle marquee
        rectBox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL));
        rectFeather = Gtk::manage(new Adjuster(M("TP_MASKING_RECT_FEATHER"), 0, 100, 1, 0));
        rectCorner = Gtk::manage(new Adjuster(M("TP_MASKING_RECT_CORNER"), 0, 100, 1, 0));
        rectAngle = Gtk::manage(new Adjuster(M("TP_MASKING_RECT_ANGLE"), -180, 180, 1, 0));
        rectFeather->setAdjusterListener(this);
        rectCorner->setAdjusterListener(this);
        rectAngle->setAdjusterListener(this);
        rectBox->pack_start(*rectFeather);
        rectBox->pack_start(*rectCorner);
        rectBox->pack_start(*rectAngle);
        fbox->pack_start(*rectBox, Gtk::PACK_SHRINK);

        // lasso / polygon
        polyBox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL));
        polyFeather = Gtk::manage(new Adjuster(M("TP_MASKING_POLY_FEATHER"), 0, 100, 1, 0));
        polyFeather->setAdjusterListener(this);
        polyBox->pack_start(*polyFeather);
        Gtk::Box* polyBtnBox = Gtk::manage(new Gtk::Box());
        polyFreehand = Gtk::manage(new Gtk::ToggleButton(M("TP_MASKING_POLY_FREEHAND")));
        polyFreehand->set_tooltip_text(M("TP_MASKING_POLY_FREEHAND_TOOLTIP"));
        polyFreehandConn = polyFreehand->signal_toggled().connect(sigc::bind(sigc::mem_fun(*this, &Masking::onPolyModeToggled), false));
        polyBtnBox->pack_start(*polyFreehand, Gtk::PACK_SHRINK, 2);
        polyPolygonal = Gtk::manage(new Gtk::ToggleButton(M("TP_MASKING_POLY_POLYGONAL")));
        polyPolygonal->set_tooltip_text(M("TP_MASKING_POLY_POLYGONAL_TOOLTIP"));
        polyPolygonalConn = polyPolygonal->signal_toggled().connect(sigc::bind(sigc::mem_fun(*this, &Masking::onPolyModeToggled), true));
        polyBtnBox->pack_start(*polyPolygonal, Gtk::PACK_SHRINK, 2);
        polyClose = Gtk::manage(new Gtk::Button(M("TP_MASKING_POLY_CLOSE")));
        polyClose->signal_clicked().connect(sigc::mem_fun(*this, &Masking::onPolyClosePressed));
        polyBtnBox->pack_end(*polyClose, Gtk::PACK_SHRINK, 2);
        polyBox->pack_start(*polyBtnBox, Gtk::PACK_SHRINK, 2);
        fbox->pack_start(*polyBox, Gtk::PACK_SHRINK);

        // magic wand
        wandBox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL));
        wandTolerance = Gtk::manage(new Adjuster(M("TP_MASKING_WAND_TOLERANCE"), 0, 100, 1, 30));
        wandSmoothness = Gtk::manage(new Adjuster(M("TP_MASKING_WAND_SMOOTHNESS"), 0, 100, 1, 30));
        wandTolerance->setAdjusterListener(this);
        wandSmoothness->setAdjusterListener(this);
        wandBox->pack_start(*wandTolerance);
        wandBox->pack_start(*wandSmoothness);
        Gtk::Box* wandBtnBox = Gtk::manage(new Gtk::Box());
        wandContiguous = Gtk::manage(new Gtk::CheckButton(M("TP_MASKING_WAND_CONTIGUOUS")));
        wandContiguous->set_tooltip_text(M("TP_MASKING_WAND_CONTIGUOUS_TOOLTIP"));
        wandContiguousConn = wandContiguous->signal_toggled().connect(sigc::mem_fun(*this, &Masking::onWandContiguousToggled));
        wandBtnBox->pack_start(*wandContiguous, Gtk::PACK_SHRINK, 2);
        wandBox->pack_start(*wandBtnBox, Gtk::PACK_SHRINK, 2);
        fbox->pack_start(*wandBox, Gtk::PACK_SHRINK);

        // quick selection
        quickBox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL));
        quickSize = Gtk::manage(new Adjuster(M("TP_MASKING_QUICKSELECT_SIZE"), 0.2, 25.0, 0.1, 2.0));
        quickSize->set_tooltip_text(M("TP_MASKING_QUICKSELECT_SIZE_TOOLTIP"));
        quickTolerance = Gtk::manage(new Adjuster(M("TP_MASKING_QUICKSELECT_TOLERANCE"), 0, 100, 1, 40));
        quickTolerance->set_tooltip_text(M("TP_MASKING_QUICKSELECT_TOLERANCE_TOOLTIP"));
        quickEdge = Gtk::manage(new Adjuster(M("TP_MASKING_QUICKSELECT_EDGE"), 0, 100, 1, 50));
        quickEdge->set_tooltip_text(M("TP_MASKING_QUICKSELECT_EDGE_TOOLTIP"));
        quickSmoothness = Gtk::manage(new Adjuster(M("TP_MASKING_QUICKSELECT_SMOOTHNESS"), 0, 100, 1, 30));

        for (Adjuster* a : {quickSize, quickTolerance, quickEdge, quickSmoothness}) {
            a->setAdjusterListener(this);
            quickBox->pack_start(*a);
        }

        Gtk::Box* quickBtnBox = Gtk::manage(new Gtk::Box());
        quickSubtract = Gtk::manage(new Gtk::ToggleButton());
        Gtk::Box* quickSubContent = Gtk::manage(new Gtk::Box());
        quickSubContent->pack_start(*Gtk::manage(new RTImage("mask-op-subtract", Gtk::ICON_SIZE_BUTTON)), Gtk::PACK_SHRINK, 2);
        quickSubContent->pack_start(*Gtk::manage(new Gtk::Label(M("TP_MASKING_QUICKSELECT_SUBTRACT"))), Gtk::PACK_SHRINK, 2);
        quickSubtract->add(*quickSubContent);
        quickSubtract->set_tooltip_text(M("TP_MASKING_QUICKSELECT_SUBTRACT_TOOLTIP"));
        quickSubtract->set_can_focus(false);
        quickSubtract->signal_toggled().connect([this]() { updateBrushCursorStyle(lastModifier); });
        quickBtnBox->pack_start(*quickSubtract, Gtk::PACK_SHRINK, 2);
        quickBox->pack_start(*quickBtnBox, Gtk::PACK_SHRINK, 2);
        fbox->pack_start(*quickBox, Gtk::PACK_SHRINK);

        // color range
        colorBox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL));
        colorSampleLabel = Gtk::manage(new Gtk::Label());
        colorSampleLabel->set_xalign(0.f);
        colorBox->pack_start(*colorSampleLabel, Gtk::PACK_SHRINK, 2);
        colorTolerance = Gtk::manage(new Adjuster(M("TP_MASKING_COLOR_TOLERANCE"), 0, 100, 1, 50));
        colorSmoothness = Gtk::manage(new Adjuster(M("TP_MASKING_COLOR_SMOOTHNESS"), 0, 100, 1, 50));
        colorTolerance->setAdjusterListener(this);
        colorSmoothness->setAdjusterListener(this);
        colorBox->pack_start(*colorTolerance);
        colorBox->pack_start(*colorSmoothness);
        fbox->pack_start(*colorBox, Gtk::PACK_SHRINK);

        // luminance range
        lumBox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL));
        lumLow = Gtk::manage(new Adjuster(M("TP_MASKING_LUM_LOW"), 0, 100, 1, 50));
        lumHigh = Gtk::manage(new Adjuster(M("TP_MASKING_LUM_HIGH"), 0, 100, 1, 100));
        lumFeatherLow = Gtk::manage(new Adjuster(M("TP_MASKING_LUM_FEATHER_LOW"), 0, 50, 1, 10));
        lumFeatherHigh = Gtk::manage(new Adjuster(M("TP_MASKING_LUM_FEATHER_HIGH"), 0, 50, 1, 0));
        lumLow->setAdjusterListener(this);
        lumHigh->setAdjusterListener(this);
        lumFeatherLow->setAdjusterListener(this);
        lumFeatherHigh->setAdjusterListener(this);
        lumBox->pack_start(*lumLow);
        lumBox->pack_start(*lumHigh);
        lumBox->pack_start(*lumFeatherLow);
        lumBox->pack_start(*lumFeatherHigh);
        fbox->pack_start(*lumBox, Gtk::PACK_SHRINK);

        // AI
        aiBox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL));
        Gtk::Box* provBox = Gtk::manage(new Gtk::Box());
        provBox->pack_start(*Gtk::manage(new Gtk::Label(M("TP_MASKING_AI_PROVIDER") + ":")), Gtk::PACK_SHRINK, 2);
        aiProvider = Gtk::manage(new MyComboBoxText());

        // Every known model is listed; the ones that cannot run here (gated
        // weights not granted, package missing) are greyed out, with the
        // reason in the tooltip (GTK combo popups have no per-row tooltips).
        Glib::ustring providerTip = M("TP_MASKING_AI_PROVIDER_TOOLTIP");
        const std::string defaultProvider = AISegmentationRegistry::getInstance().getDefaultProviderId();
        int defaultIdx = 0;

        for (const auto& p : AISegmentationRegistry::getInstance().listProviderInfos()) {
            if (p.id == defaultProvider) {
                defaultIdx = int(aiProviderIds.size());
            }

            aiProviderIds.push_back(p.id);
            aiProvider->append(p.name);

            if (!p.available) {
                aiProviderUnavailable.insert(p.id);
                providerTip += "\n" + Glib::ustring::compose(M("TP_MASKING_AI_PROVIDER_UNAVAILABLE"), p.name, p.reason);
            }
        }

        for (Gtk::CellRenderer* renderer : aiProvider->get_cells()) {
            aiProvider->set_cell_data_func(*renderer, [this, renderer](const Gtk::TreeModel::const_iterator& iter) {
                const Gtk::TreeModel::Path path = aiProvider->get_model()->get_path(iter);
                const int row = path.empty() ? -1 : path[0];
                renderer->property_sensitive() = row < 0 || row >= int(aiProviderIds.size()) || !aiProviderUnavailable.count(aiProviderIds[row]);
            });
        }

        aiProvider->set_tooltip_text(providerTip);

        if (!aiProviderIds.empty()) {
            aiProvider->set_active(defaultIdx);
        }

        aiProviderConn = aiProvider->signal_changed().connect(sigc::mem_fun(*this, &Masking::onAiProviderChanged));
        provBox->pack_start(*aiProvider, Gtk::PACK_EXPAND_WIDGET, 2);
        aiBox->pack_start(*provBox, Gtk::PACK_SHRINK, 2);
        Gtk::Box* promptBox = Gtk::manage(new Gtk::Box());
        promptBox->pack_start(*Gtk::manage(new Gtk::Label(M("TP_MASKING_AI_PROMPT") + ":")), Gtk::PACK_SHRINK, 2);
        aiPrompt = Gtk::manage(new Gtk::Entry());
        aiPrompt->set_placeholder_text(M("TP_MASKING_AI_PROMPT_PLACEHOLDER"));
        aiPrompt->set_tooltip_text(M("TP_MASKING_AI_PROMPT_TOOLTIP"));
        aiPromptConn = aiPrompt->signal_activate().connect(sigc::mem_fun(*this, &Masking::onAiPromptChanged));
        aiPrompt->signal_focus_out_event().connect(sigc::mem_fun(*this, &Masking::onAiPromptFocusOut));
        promptBox->pack_start(*aiPrompt, Gtk::PACK_EXPAND_WIDGET, 2);
        aiBox->pack_start(*promptBox, Gtk::PACK_SHRINK, 2);
        aiThreshold = Gtk::manage(new Adjuster(M("TP_MASKING_AI_THRESHOLD"), 0, 100, 1, 50));
        aiThreshold->setAdjusterListener(this);
        aiBox->pack_start(*aiThreshold);
        aiRefine = Gtk::manage(new Gtk::CheckButton(M("TP_MASKING_AI_REFINE")));
        aiRefine->set_tooltip_text(M("TP_MASKING_AI_REFINE_TOOLTIP"));
        aiRefineConn = aiRefine->signal_toggled().connect(sigc::mem_fun(*this, &Masking::onAiRefineToggled));
        aiBox->pack_start(*aiRefine, Gtk::PACK_SHRINK, 2);
        aiHint = Gtk::manage(new Gtk::Label(M("TP_MASKING_AI_HINT")));
        aiHint->set_line_wrap(true);
        aiHint->set_xalign(0.f);
        aiBox->pack_start(*aiHint, Gtk::PACK_SHRINK, 2);
        aiStatus = Gtk::manage(new Gtk::Label());
        aiStatus->set_line_wrap(true);
        aiStatus->set_xalign(0.f);
        aiBox->pack_start(*aiStatus, Gtk::PACK_SHRINK, 2);
        fbox->pack_start(*aiBox, Gtk::PACK_SHRINK);

        // Asynchronous AI masks: refresh the preview when a result arrives (from a worker thread).
        aiListenerId = AISegmentationRegistry::getInstance().addResultListener([this](bool finished) {
            idle_register.add([this, finished]() {
                onSegmentationResult(finished);
                return false;
            });
        });

        frame->add(*fbox);
        pack_start(*frame, Gtk::PACK_SHRINK, 4);
    }

    // ------------------------------------------------------------------
    // Mask: overlay, invert and refinements of the whole mask
    {
        Gtk::Frame* frame = Gtk::manage(new Gtk::Frame(M("TP_MASKING_MASK_FRAME")));
        Gtk::Box* fbox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL));

        Gtk::Box* showBox = Gtk::manage(new Gtk::Box());
        showMask = Gtk::manage(new Gtk::CheckButton(M("TP_MASKING_SHOW_MASK")));
        showMaskConn = showMask->signal_toggled().connect(sigc::mem_fun(*this, &Masking::onShowMaskToggled));
        showBox->pack_start(*showMask, Gtk::PACK_SHRINK, 2);
        showComponentOnly = Gtk::manage(new Gtk::CheckButton(M("TP_MASKING_SHOW_COMPONENT")));
        showComponentOnlyConn = showComponentOnly->signal_toggled().connect(sigc::mem_fun(*this, &Masking::onShowMaskToggled));
        showBox->pack_start(*showComponentOnly, Gtk::PACK_SHRINK, 2);
        layerInvert = Gtk::manage(new Gtk::CheckButton(M("TP_MASKING_INVERT_MASK")));
        layerInvertConn = layerInvert->signal_toggled().connect(sigc::mem_fun(*this, &Masking::onLayerInvertToggled));
        showBox->pack_end(*layerInvert, Gtk::PACK_SHRINK, 2);
        fbox->pack_start(*showBox, Gtk::PACK_SHRINK, 2);

        amount = Gtk::manage(new Adjuster(M("TP_MASKING_AMOUNT"), 0, 100, 1, 100));
        feather = Gtk::manage(new Adjuster(M("TP_MASKING_FEATHER"), 0, 100, 1, 0));
        expand = Gtk::manage(new Adjuster(M("TP_MASKING_EXPAND"), -100, 100, 1, 0));
        expand->set_tooltip_text(M("TP_MASKING_EXPAND_TOOLTIP"));
        smooth = Gtk::manage(new Adjuster(M("TP_MASKING_SMOOTH"), 0, 100, 1, 0));
        smooth->set_tooltip_text(M("TP_MASKING_SMOOTH_TOOLTIP"));
        amount->setAdjusterListener(this);
        feather->setAdjusterListener(this);
        expand->setAdjusterListener(this);
        smooth->setAdjusterListener(this);
        fbox->pack_start(*amount);
        fbox->pack_start(*feather);
        fbox->pack_start(*expand);
        fbox->pack_start(*smooth);
        frame->add(*fbox);
        pack_start(*frame, Gtk::PACK_SHRINK, 4);
    }

    // ------------------------------------------------------------------
    // Adjustments
    {
        Gtk::Frame* frame = Gtk::manage(new Gtk::Frame(M("TP_MASKING_ADJUSTMENTS")));
        Gtk::Box* fbox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL));

        exposure = Gtk::manage(new Adjuster(M("TP_MASKING_EXPOSURE"), -5, 5, 0.01, 0));
        contrast = Gtk::manage(new Adjuster(M("TP_MASKING_CONTRAST"), -100, 100, 1, 0));
        highlights = Gtk::manage(new Adjuster(M("TP_MASKING_HIGHLIGHTS"), -100, 100, 1, 0));
        midtones = Gtk::manage(new Adjuster(M("TP_MASKING_MIDTONES"), -100, 100, 1, 0));
        shadows = Gtk::manage(new Adjuster(M("TP_MASKING_SHADOWS"), -100, 100, 1, 0));
        whites = Gtk::manage(new Adjuster(M("TP_MASKING_WHITES"), -100, 100, 1, 0));
        blacks = Gtk::manage(new Adjuster(M("TP_MASKING_BLACKS"), -100, 100, 1, 0));
        temperature = Gtk::manage(new Adjuster(M("TP_MASKING_TEMPERATURE"), -100, 100, 1, 0));
        tint = Gtk::manage(new Adjuster(M("TP_MASKING_TINT"), -100, 100, 1, 0));
        hue = Gtk::manage(new Adjuster(M("TP_MASKING_HUE"), -180, 180, 1, 0));
        saturation = Gtk::manage(new Adjuster(M("TP_MASKING_SATURATION"), -100, 100, 1, 0));
        clarity = Gtk::manage(new Adjuster(M("TP_MASKING_CLARITY"), -100, 100, 1, 0));

        for (Adjuster* a : {exposure, contrast, highlights, midtones, shadows, whites, blacks, temperature, tint, hue, saturation, clarity}) {
            a->setAdjusterListener(this);
            fbox->pack_start(*a);
        }

        // Curves of the L*a*b* Adjustments tool, folded away until needed. No
        // pipettes: the canvas belongs to the mask drawing tools.
        curvesExpander = Gtk::manage(new MyExpander(false, M("TP_MASKING_CURVES")));
        setExpandAlignProperties(curvesExpander, true, false, Gtk::ALIGN_FILL, Gtk::ALIGN_START);
        Gtk::Box* curvesBox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL));

        curveEditorG = new CurveEditorGroup(App::get().mut_options().lastLabCurvesDir);
        curveEditorG->setCurveListener(this);

        lshape = static_cast<DiagonalCurveEditor*>(curveEditorG->addCurve(CT_Diagonal, "L*"));
        lshape->setTooltip(M("TP_LABCURVE_CURVEEDITOR_LL_TOOLTIP"));

        ashape = static_cast<DiagonalCurveEditor*>(curveEditorG->addCurve(CT_Diagonal, "a*"));
        ashape->setRangeLabels(
            M("TP_LABCURVE_CURVEEDITOR_A_RANGE1"), M("TP_LABCURVE_CURVEEDITOR_A_RANGE2"),
            M("TP_LABCURVE_CURVEEDITOR_A_RANGE3"), M("TP_LABCURVE_CURVEEDITOR_A_RANGE4")
        );
        // from green to magenta
        std::vector<GradientMilestone> milestones = {
            GradientMilestone(0., 0., 1., 0.),
            GradientMilestone(1., 1., 0., 1.)
        };
        ashape->setBottomBarBgGradient(milestones);
        ashape->setLeftBarBgGradient(milestones);

        bshape = static_cast<DiagonalCurveEditor*>(curveEditorG->addCurve(CT_Diagonal, "b*"));
        bshape->setRangeLabels(
            M("TP_LABCURVE_CURVEEDITOR_B_RANGE1"), M("TP_LABCURVE_CURVEEDITOR_B_RANGE2"),
            M("TP_LABCURVE_CURVEEDITOR_B_RANGE3"), M("TP_LABCURVE_CURVEEDITOR_B_RANGE4")
        );
        // from blue to yellow
        milestones = {
            GradientMilestone(0., 0., 0., 1.),
            GradientMilestone(1., 1., 1., 0.)
        };
        bshape->setBottomBarBgGradient(milestones);
        bshape->setLeftBarBgGradient(milestones);

        curveEditorG->newLine();

        lhshape = static_cast<FlatCurveEditor*>(curveEditorG->addCurve(CT_Flat, M("TP_LABCURVE_CURVEEDITOR_LH")));
        lhshape->setTooltip(M("TP_LABCURVE_CURVEEDITOR_LH_TOOLTIP"));
        lhshape->setCurveColorProvider(this, 4);
        chshape = static_cast<FlatCurveEditor*>(curveEditorG->addCurve(CT_Flat, M("TP_LABCURVE_CURVEEDITOR_CH")));
        chshape->setTooltip(M("TP_LABCURVE_CURVEEDITOR_CH_TOOLTIP"));
        chshape->setCurveColorProvider(this, 1);
        hhshape = static_cast<FlatCurveEditor*>(curveEditorG->addCurve(CT_Flat, M("TP_LABCURVE_CURVEEDITOR_HH")));
        hhshape->setTooltip(M("TP_LABCURVE_CURVEEDITOR_HH_TOOLTIP"));
        hhshape->setCurveColorProvider(this, 5);

        curveEditorG->newLine();

        ccshape = static_cast<DiagonalCurveEditor*>(curveEditorG->addCurve(CT_Diagonal, M("TP_LABCURVE_CURVEEDITOR_CC")));
        ccshape->setTooltip(M("TP_LABCURVE_CURVEEDITOR_CC_TOOLTIP"));
        ccshape->setRangeLabels(
            M("TP_LABCURVE_CURVEEDITOR_CC_RANGE1"), M("TP_LABCURVE_CURVEEDITOR_CC_RANGE2"),
            M("TP_LABCURVE_CURVEEDITOR_CC_RANGE3"), M("TP_LABCURVE_CURVEEDITOR_CC_RANGE4")
        );
        ccshape->setBottomBarColorProvider(this, 2);
        ccshape->setLeftBarColorProvider(this, 7);
        ccshape->setRangeDefaultMilestones(0.05, 0.2, 0.58);

        lcshape = static_cast<DiagonalCurveEditor*>(curveEditorG->addCurve(CT_Diagonal, M("TP_LABCURVE_CURVEEDITOR_LC")));
        lcshape->setTooltip(M("TP_MASKING_CURVE_LC_TOOLTIP"));
        lcshape->setBottomBarColorProvider(this, 2);
        lcshape->setRangeLabels(
            M("TP_LABCURVE_CURVEEDITOR_CC_RANGE1"), M("TP_LABCURVE_CURVEEDITOR_CC_RANGE2"),
            M("TP_LABCURVE_CURVEEDITOR_CC_RANGE3"), M("TP_LABCURVE_CURVEEDITOR_CC_RANGE4")
        );
        lcshape->setRangeDefaultMilestones(0.05, 0.2, 0.58);

        clshape = static_cast<DiagonalCurveEditor*>(curveEditorG->addCurve(CT_Diagonal, M("TP_LABCURVE_CURVEEDITOR_CL")));
        clshape->setTooltip(M("TP_LABCURVE_CURVEEDITOR_CL_TOOLTIP"));
        clshape->setLeftBarColorProvider(this, 7);
        clshape->setRangeDefaultMilestones(0.25, 0.5, 0.75);

        // from black to white
        milestones = {
            GradientMilestone(0., 0., 0., 0.),
            GradientMilestone(1., 1., 1., 1.)
        };
        lshape->setBottomBarBgGradient(milestones);
        lshape->setLeftBarBgGradient(milestones);
        clshape->setBottomBarBgGradient(milestones);
        lcshape->setLeftBarBgGradient({GradientMilestone(0., 0.1, 0.1, 0.1), GradientMilestone(1., 0.8, 0.8, 0.8)});

        // whole hue range
        milestones.clear();

        for (int i = 0; i < 7; ++i) {
            float R, G, B;
            const float x = i / 6.f;
            Color::hsv2rgb01(x, 0.5f, 0.5f, R, G, B);
            milestones.emplace_back(x, R, G, B);
        }

        chshape->setBottomBarBgGradient(milestones);
        lhshape->setBottomBarBgGradient(milestones);
        hhshape->setBottomBarBgGradient(milestones);

        curveEditorG->curveListComplete();
        curvesBox->pack_start(*curveEditorG, Gtk::PACK_SHRINK, 2);
        curvesExpander->add(*curvesBox, false);
        fbox->pack_start(*curvesExpander, Gtk::PACK_SHRINK, 2);

        frame->add(*fbox);
        pack_start(*frame, Gtk::PACK_SHRINK, 4);
    }

    // ------------------------------------------------------------------
    // On-canvas geometry
    imgRect = new EditRectangle();
    imgRect->filled = true;
    imgRect->datum = Geometry::IMAGE;

    moHandleA = new Circle();
    moHandleB = new Circle();
    moCenter = new Circle();

    for (Circle* c : {moHandleA, moHandleB, moCenter}) {
        c->datum = Geometry::IMAGE;
        c->radiusInImageSpace = false;
        c->radius = MO_HANDLE_RADIUS;
        c->filled = true;
        c->setActive(false);
    }

    // mouse-over list: the background rectangle first so that the handles win
    EditSubscriber::mouseOverGeometry.push_back(imgRect);     // MO_IMAGE
    EditSubscriber::mouseOverGeometry.push_back(moHandleA);   // MO_HANDLE_A
    EditSubscriber::mouseOverGeometry.push_back(moHandleB);   // MO_HANDLE_B
    EditSubscriber::mouseOverGeometry.push_back(moCenter);    // MO_CENTER
    moPolyFirst = new Circle();
    moPolyFirst->datum = Geometry::IMAGE;
    moPolyFirst->radiusInImageSpace = false;
    moPolyFirst->radius = MO_HANDLE_RADIUS;
    moPolyFirst->filled = true;
    moPolyFirst->setActive(false);
    EditSubscriber::mouseOverGeometry.push_back(moPolyFirst); // MO_POLY_FIRST

    brushCursor = new Circle();
    brushCursor->datum = Geometry::IMAGE;
    brushCursor->radiusInImageSpace = true;
    brushCursor->setDashed(true);

    brushCore = new Circle();
    brushCore->datum = Geometry::IMAGE;
    brushCore->radiusInImageSpace = true;

    gradientLine = new Line();
    gradientLine->datum = Geometry::IMAGE;
    gradientLine->innerLineWidth = 2;
    gradientStart = new Circle();
    gradientEnd = new Circle();
    ellipseOutline = new Polyline();
    ellipseOutline->datum = Geometry::IMAGE;
    ellipseOutline->innerLineWidth = 2;
    ellipseCenter = new Circle();
    handleX = new Circle();
    handleY = new Circle();
    pickMarker = new Circle();
    pickMarker->setDashed(true);

    rectOutline = new Polyline();
    rectOutline->datum = Geometry::IMAGE;
    rectOutline->innerLineWidth = 2;
    rectCornerA = new Circle();
    rectCornerB = new Circle();
    rectCenter = new Circle();
    polyOutline = new Polyline();
    polyOutline->datum = Geometry::IMAGE;
    polyOutline->innerLineWidth = 2;
    polyRubber = new Line();
    polyRubber->datum = Geometry::IMAGE;
    polyRubber->setDashed(true);
    polyFirst = new Circle();

    for (int i = 0; i < SEED_MARKER_COUNT; ++i) {
        Circle* s = new Circle();
        s->datum = Geometry::IMAGE;
        s->radiusInImageSpace = false;
        s->radius = 5;
        s->filled = true;
        s->setActive(false);
        seedMarkers.push_back(s);
        EditSubscriber::visibleGeometry.push_back(s);
    }

    for (Circle* c : {gradientStart, gradientEnd, ellipseCenter, handleX, handleY, pickMarker, rectCornerA, rectCornerB, rectCenter, polyFirst}) {
        c->datum = Geometry::IMAGE;
        c->radiusInImageSpace = false;
        c->radius = HANDLE_RADIUS;
        c->filled = (c != pickMarker);
    }

    for (Geometry* g : std::vector<Geometry*> {brushCursor, brushCore, gradientLine, gradientStart, gradientEnd, ellipseOutline, ellipseCenter, handleX, handleY, pickMarker, rectOutline, rectCornerA, rectCornerB, rectCenter, polyOutline, polyRubber, polyFirst}) {
        g->setActive(false);
        EditSubscriber::visibleGeometry.push_back(g);
    }

    // The per-type boxes are shown / hidden by updateTypeBoxes(); keep the
    // parent's show_all() from re-showing them.
    for (Gtk::Widget* w : std::vector<Gtk::Widget*> {brushBox, radialBox, rectBox, polyBox, wandBox, quickBox, colorBox, lumBox, aiBox}) {
        w->show_all();
        w->set_no_show_all(true);
    }

    aiHint->set_no_show_all(true);

    show_all();
    aiBusyBox->hide();
    aiBusyBox->set_no_show_all(true);
    // The edit button is hidden for brush components (the paint / erase
    // toggles switch brushing on and off); keep show_all() from re-showing it.
    edit->set_no_show_all(true);
    updateTypeBoxes();
    updateIntent();
}

Masking::~Masking()
{
    AISegmentationRegistry::getInstance().removeResultListener(aiListenerId);
    idle_register.destroy();
    delete curveEditorG;

    for (Geometry* g : EditSubscriber::visibleGeometry) {
        delete g;
    }

    for (Geometry* g : EditSubscriber::mouseOverGeometry) {
        delete g;
    }

    EditSubscriber::visibleGeometry.clear();
    EditSubscriber::mouseOverGeometry.clear();
}

// ---------------------------------------------------------------------------
// ToolPanel interface

void Masking::read(const ProcParams* pp, const ParamsEdited* pedited)
{
    disableListener();

    setGeometry(*pp);

    masks = pp->masking.masks;
    listEdited = false;

    if (pedited) {
        set_inconsistent(multiImage && !pedited->masking.enabled);
        showMask->set_inconsistent(multiImage && !pedited->masking.showMask);

        if (editedCheckBox) {
            editedConn.block(true);
            editedCheckBox->set_active(pedited->masking.masks);
            editedConn.block(false);
            listEdited = pedited->masking.masks;
        }
    }

    setEnabled(pp->masking.enabled);

    selectedMask = masks.empty() ? -1 : LIM(pp->masking.showMask >= 0 ? pp->masking.showMask : (selectedMask < 0 ? 0 : selectedMask), 0, int(masks.size()) - 1);
    const MaskLayer* layer = currentLayer();
    selectedComponent = (layer && !layer->components.empty())
                        ? (pp->masking.showComponent >= 0 ? LIM(pp->masking.showComponent, 0, int(layer->components.size()) - 1) : int(layer->components.size()) - 1)
                        : -1;

    {
        ConnectionBlocker b1(showMaskConn);
        ConnectionBlocker b2(showComponentOnlyConn);
        showMask->set_active(pp->masking.showMask >= 0);
        showComponentOnly->set_active(pp->masking.showComponent >= 0);
    }

    populateMaskList();
    populateComponentList();
    showLayer();
    showComponent();
    updateGeometry();

    enableListener();

    if (!batchMode && !selfTestStarted && std::getenv("RT_MASK_SELFTEST")) {
        // Scripted session (sanitizer runs): drive the panel like a user would.
        selfTestStarted = true;
        Glib::signal_timeout().connect([this]() { selfTestStep(); return true; }, 700);
    }
}

void Masking::write(ProcParams* pp, ParamsEdited* pedited)
{
    componentGet();
    layerGet();

    pp->masking.enabled = getEnabled();
    pp->masking.masks = masks;
    pp->masking.showMask = showMask->get_active() ? selectedMask : -1;
    pp->masking.showComponent = (showMask->get_active() && showComponentOnly->get_active()) ? selectedComponent : -1;

    if (std::getenv("RT_MASK_SELFTEST")) {
        printf("[write] enabled=%d show=%d/%d masks=%zu:", int(pp->masking.enabled), pp->masking.showMask, pp->masking.showComponent, masks.size());

        for (const auto& m : masks) {
            printf(" {en=%d exp=%.2f amt=%.0f comps=%zu", int(m.enabled), m.exposure, m.amount, m.components.size());

            for (const auto& c : m.components) {
                printf(" [t=%d en=%d strokes=%zu pts=%zu]", int(c.type), int(c.enabled), c.strokes.size(), c.strokes.empty() ? size_t(0) : c.strokes.back().points.size());
            }

            printf("}");
        }

        printf("\n");
        fflush(stdout);
    }

    if (pedited) {
        pedited->masking.enabled = !get_inconsistent();
        pedited->masking.masks = editedCheckBox ? editedCheckBox->get_active() : true;
        pedited->masking.showMask = !showMask->get_inconsistent();
    }
}

void Masking::setDefaults(const ProcParams* defParams, const ParamsEdited* pedited)
{
    // All adjusters are per mask; their defaults are the neutral values set in the constructor.
    (void)defParams;
    (void)pedited;
}

void Masking::trimValues(ProcParams* pp)
{
    (void)pp;
}

void Masking::setBatchMode(bool batchMode)
{
    ToolPanel::setBatchMode(batchMode);

    if (batchMode) {
        // No on-canvas editing when editing several images at once. The
        // button is only hidden (not destroyed): other code paths still
        // query its state.
        editBox->hide();
        editBox->set_no_show_all(true);
        brushPaint->hide();
        brushErase->hide();
        brushPaint->set_no_show_all(true);
        brushErase->set_no_show_all(true);
        polyFreehand->hide();
        polyPolygonal->hide();
        polyFreehand->set_no_show_all(true);
        polyPolygonal->set_no_show_all(true);

        if (!editedCheckBox) {
            editedCheckBox = Gtk::manage(new Gtk::CheckButton(M("TP_MASKING_EDITED")));
            editedConn = editedCheckBox->signal_toggled().connect(sigc::mem_fun(*this, &Masking::onEditedToggled));
            pack_start(*editedCheckBox, Gtk::PACK_SHRINK, 2);
            reorder_child(*editedCheckBox, 0);
            editedCheckBox->show();
        }
    }
}

void Masking::setEditProvider(EditDataProvider* provider)
{
    EditSubscriber::setEditProvider(provider);
}

void Masking::setMaskingProvider(MaskingProvider* provider)
{
    maskingProvider = provider;
}

// ---------------------------------------------------------------------------
// data <-> widgets

Masking::MaskLayer* Masking::currentLayer()
{
    return (selectedMask >= 0 && selectedMask < int(masks.size())) ? &masks[selectedMask] : nullptr;
}

const Masking::MaskLayer* Masking::currentLayer() const
{
    return (selectedMask >= 0 && selectedMask < int(masks.size())) ? &masks[selectedMask] : nullptr;
}

Masking::MaskComponent* Masking::currentComponent()
{
    MaskLayer* l = currentLayer();
    return (l && selectedComponent >= 0 && selectedComponent < int(l->components.size())) ? &l->components[selectedComponent] : nullptr;
}

const Masking::MaskComponent* Masking::currentComponent() const
{
    const MaskLayer* l = currentLayer();
    return (l && selectedComponent >= 0 && selectedComponent < int(l->components.size())) ? &l->components[selectedComponent] : nullptr;
}

Glib::ustring Masking::componentTypeName(MaskComponentType t) const
{
    switch (t) {
        case MaskComponentType::BRUSH:
            return M("TP_MASKING_TYPE_BRUSH");

        case MaskComponentType::LINEAR_GRADIENT:
            return M("TP_MASKING_TYPE_LINEAR");

        case MaskComponentType::RADIAL_GRADIENT:
            return M("TP_MASKING_TYPE_RADIAL");

        case MaskComponentType::COLOR_RANGE:
            return M("TP_MASKING_TYPE_COLOR");

        case MaskComponentType::LUMINANCE_RANGE:
            return M("TP_MASKING_TYPE_LUMINANCE");

        case MaskComponentType::AI_SUBJECT:
            return M("TP_MASKING_TYPE_AI_SUBJECT");

        case MaskComponentType::AI_SKY:
            return M("TP_MASKING_TYPE_AI_SKY");

        case MaskComponentType::AI_BACKGROUND:
            return M("TP_MASKING_TYPE_AI_BACKGROUND");

        case MaskComponentType::AI_OBJECT:
            return M("TP_MASKING_TYPE_AI_OBJECT");

        case MaskComponentType::RECTANGLE:
            return M("TP_MASKING_TYPE_RECTANGLE");

        case MaskComponentType::POLYGON:
            return M("TP_MASKING_TYPE_POLYGON");

        case MaskComponentType::MAGIC_WAND:
            return M("TP_MASKING_TYPE_WAND");

        case MaskComponentType::QUICK_SELECT:
            return M("TP_MASKING_TYPE_QUICKSELECT");
    }

    return "";
}

Glib::ustring Masking::componentShortName(MaskComponentType t) const
{
    switch (t) {
        case MaskComponentType::BRUSH:
            return M("TP_MASKING_TOOL_BRUSH");

        case MaskComponentType::RECTANGLE:
            return M("TP_MASKING_TOOL_RECT");

        case MaskComponentType::POLYGON:
            return M("TP_MASKING_TOOL_LASSO");

        case MaskComponentType::LINEAR_GRADIENT:
            return M("TP_MASKING_TOOL_LINEAR");

        case MaskComponentType::RADIAL_GRADIENT:
            return M("TP_MASKING_TOOL_RADIAL");

        case MaskComponentType::MAGIC_WAND:
            return M("TP_MASKING_TOOL_WAND");

        case MaskComponentType::QUICK_SELECT:
            return M("TP_MASKING_TOOL_QUICKSELECT");

        case MaskComponentType::COLOR_RANGE:
            return M("TP_MASKING_TOOL_COLOR");

        case MaskComponentType::LUMINANCE_RANGE:
            return M("TP_MASKING_TOOL_LUMINANCE");

        case MaskComponentType::AI_SUBJECT:
            return M("TP_MASKING_TOOL_SUBJECT");

        case MaskComponentType::AI_SKY:
            return M("TP_MASKING_TOOL_SKY");

        case MaskComponentType::AI_BACKGROUND:
            return M("TP_MASKING_TOOL_BACKGROUND");

        case MaskComponentType::AI_OBJECT:
            return M("TP_MASKING_TOOL_OBJECT");
    }

    return "";
}

Glib::ustring Masking::componentIconName(MaskComponentType t) const
{
    switch (t) {
        case MaskComponentType::BRUSH:
            return "paintbrush-small";

        case MaskComponentType::RECTANGLE:
            return "mask-tool-rect";

        case MaskComponentType::POLYGON:
            return "mask-tool-lasso";

        case MaskComponentType::LINEAR_GRADIENT:
            return "mask-tool-linear";

        case MaskComponentType::RADIAL_GRADIENT:
            return "mask-tool-radial";

        case MaskComponentType::MAGIC_WAND:
            return "mask-tool-wand";

        case MaskComponentType::QUICK_SELECT:
            return "mask-tool-quickselect";

        case MaskComponentType::COLOR_RANGE:
            return "color-picker-small";

        case MaskComponentType::LUMINANCE_RANGE:
            return "mask-tool-luminance";

        case MaskComponentType::AI_SUBJECT:
            return "mask-tool-subject";

        case MaskComponentType::AI_SKY:
            return "mask-tool-sky";

        case MaskComponentType::AI_BACKGROUND:
            return "mask-tool-background";

        case MaskComponentType::AI_OBJECT:
            return "mask-tool-object";
    }

    return "";
}

Gtk::Button* Masking::makeToolButton(MaskComponentType t)
{
    Gtk::Button* b = Gtk::manage(new Gtk::Button());
    Gtk::Box* content = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL));
    content->pack_start(*Gtk::manage(new RTImage(componentIconName(t), Gtk::ICON_SIZE_LARGE_TOOLBAR)), Gtk::PACK_SHRINK, 1);
    Gtk::Label* label = Gtk::manage(new Gtk::Label());
    label->set_markup("<small>" + Glib::Markup::escape_text(componentShortName(t)) + "</small>");
    content->pack_start(*label, Gtk::PACK_SHRINK, 0);
    b->add(*content);
    b->set_tooltip_text(componentTypeName(t));
    b->set_relief(Gtk::RELIEF_NONE);
    b->set_can_focus(false);
    b->signal_clicked().connect(sigc::bind(sigc::mem_fun(*this, &Masking::onPaletteClicked), t));
    return b;
}

void Masking::setOpToggles(MaskBooleanOp op)
{
    const int which = LIM(rtengine::toUnderlying(op), 0, 2);

    for (int i = 0; i < 3; ++i) {
        ConnectionBlocker b(opConn[i]);
        opButtons[i]->set_active(i == which);
    }
}

Masking::MaskBooleanOp Masking::getOpToggles() const
{
    for (int i = 0; i < 3; ++i) {
        if (opButtons[i]->get_active()) {
            return static_cast<MaskBooleanOp>(i);
        }
    }

    return MaskBooleanOp::ADD;
}

void Masking::updateIntent()
{
    const MaskLayer* layer = currentLayer();
    const bool hasLayer = layer != nullptr;

    for (int i = INTENT_ADD; i < INTENT_COUNT; ++i) {
        intentButtons[i]->set_sensitive(hasLayer);
    }

    int active = INTENT_NEW;

    for (int i = 0; i < INTENT_COUNT; ++i) {
        if (intentButtons[i]->get_active()) {
            active = i;
        }
    }

    if (!hasLayer && active != INTENT_NEW) {
        for (int i = 0; i < INTENT_COUNT; ++i) {
            ConnectionBlocker b(intentConn[i]);
            intentButtons[i]->set_active(i == INTENT_NEW);
        }

        active = INTENT_NEW;
    }

    const Glib::ustring name = hasLayer
                               ? (layer->name.empty() ? Glib::ustring::compose(M("TP_MASKING_DEFAULT_NAME"), selectedMask + 1) : Glib::ustring(layer->name))
                               : Glib::ustring();

    switch (active) {
        case INTENT_ADD:
            intentLabel->set_text(Glib::ustring::compose(M("TP_MASKING_INTENT_HINT_ADD"), name));
            break;

        case INTENT_SUBTRACT:
            intentLabel->set_text(Glib::ustring::compose(M("TP_MASKING_INTENT_HINT_SUBTRACT"), name));
            break;

        case INTENT_INTERSECT:
            intentLabel->set_text(Glib::ustring::compose(M("TP_MASKING_INTENT_HINT_INTERSECT"), name));
            break;

        default:
            intentLabel->set_text(M("TP_MASKING_INTENT_HINT_NEW"));
            break;
    }
}

Glib::ustring Masking::componentSummary(const MaskComponent& c) const
{
    Glib::ustring s = componentTypeName(c.type);

    if (c.type == MaskComponentType::BRUSH) {
        s += Glib::ustring::compose(" (%1)", c.strokes.size());
    } else if (c.type == MaskComponentType::POLYGON) {
        s += Glib::ustring::compose(" (%1)", c.pointCount());
    } else if (c.type == MaskComponentType::MAGIC_WAND) {
        s += Glib::ustring::compose(" (%1)", c.seedCount());
    } else if (c.type == MaskComponentType::QUICK_SELECT) {
        s += Glib::ustring::compose(" (%1)", c.strokes.size());
    }

    if (!c.enabled) {
        s += " [off]";
    }

    return s;
}

Glib::ustring Masking::layerSummary(const MaskLayer& l) const
{
    Glib::ustring s;

    for (const auto& c : l.components) {
        if (!s.empty()) {
            s += ", ";
        }

        s += componentTypeName(c.type);
    }

    if (s.empty()) {
        s = M("TP_MASKING_EMPTY");
    }

    if (!l.enabled) {
        s += " [off]";
    }

    return s;
}

void Masking::fillRow(const Gtk::TreeModel::Row& row, int maskIdx, int compIdx)
{
    if (maskIdx < 0 || maskIdx >= int(masks.size())) {
        return;
    }

    const MaskLayer& m = masks[maskIdx];
    row[treeColumns.maskIdx] = maskIdx;
    row[treeColumns.compIdx] = compIdx;

    if (compIdx < 0) {
        const Glib::ustring name = m.name.empty() ? Glib::ustring::compose(M("TP_MASKING_DEFAULT_NAME"), maskIdx + 1) : Glib::ustring(m.name);
        row[treeColumns.enabled] = m.enabled;
        row[treeColumns.icon] = "mask-layer";
        row[treeColumns.label] = "<b>" + Glib::Markup::escape_text(name) + "</b>";
        row[treeColumns.mode] = "<small>" + Glib::Markup::escape_text(Glib::ustring::compose(M("TP_MASKING_TOOL_COUNT"), m.components.size())) + "</small>";
        return;
    }

    if (compIdx >= int(m.components.size())) {
        return;
    }

    const MaskComponent& c = m.components[compIdx];
    Glib::ustring op = compIdx == 0 ? "" : (c.op == MaskBooleanOp::ADD ? M("TP_MASKING_OP_ADD") : (c.op == MaskBooleanOp::SUBTRACT ? M("TP_MASKING_OP_SUBTRACT") : M("TP_MASKING_OP_INTERSECT")));

    if (c.invert) {
        op += op.empty() ? M("TP_MASKING_INVERT") : (" " + M("TP_MASKING_INVERT"));
    }

    row[treeColumns.enabled] = c.enabled;
    row[treeColumns.icon] = componentIconName(c.type);
    row[treeColumns.label] = Glib::Markup::escape_text(componentSummary(c));
    row[treeColumns.mode] = "<small>" + Glib::Markup::escape_text(op) + "</small>";
}

Gtk::TreeModel::iterator Masking::findRow(int maskIdx, int compIdx)
{
    for (auto it = treeStore->children().begin(); it != treeStore->children().end(); ++it) {
        if ((*it)[treeColumns.maskIdx] != maskIdx) {
            continue;
        }

        if (compIdx < 0) {
            return it;
        }

        for (auto cit = it->children().begin(); cit != it->children().end(); ++cit) {
            if ((*cit)[treeColumns.compIdx] == compIdx) {
                return cit;
            }
        }

        break;
    }

    return Gtk::TreeModel::iterator();
}

void Masking::populateTree()
{
    ConnectionBlocker b(treeSelectionConn);
    treeStore->clear();

    // Layers only: the tools used on a mask are not listed, the mask is the canvas.
    for (size_t i = 0; i < masks.size(); ++i) {
        Gtk::TreeModel::Row row = *treeStore->append();
        fillRow(row, int(i), -1);
    }

    const auto it = findRow(selectedMask, -1);

    if (it) {
        tree->get_selection()->select(it);
    }
}

void Masking::populateMaskList()
{
    populateTree();
}

void Masking::populateComponentList()
{
    populateTree();
}

void Masking::updateMaskRow(int idx)
{
    const auto it = findRow(idx, -1);

    if (it) {
        fillRow(*it, idx, -1);
    }
}

void Masking::updateComponentRow(int idx)
{
    const auto it = findRow(selectedMask, idx);

    if (it) {
        fillRow(*it, selectedMask, idx);
    }

    updateMaskRow(selectedMask);
}

void Masking::showLayer()
{
    const bool disable = listener;

    if (disable) {
        disableListener();
    }

    const MaskLayer* layer = currentLayer();
    const bool has = layer != nullptr;

    ConnectionBlocker b2(maskNameConn);
    ConnectionBlocker b3(layerInvertConn);

    maskName->set_text(has ? Glib::ustring(layer->name) : Glib::ustring());
    layerInvert->set_active(has && layer->invert);
    amount->setValue(has ? layer->amount : 100.0);
    feather->setValue(has ? layer->feather : 0.0);
    expand->setValue(has ? layer->expand : 0.0);
    smooth->setValue(has ? layer->smooth : 0.0);
    exposure->setValue(has ? layer->exposure : 0.0);
    contrast->setValue(has ? layer->contrast : 0.0);
    highlights->setValue(has ? layer->highlights : 0.0);
    midtones->setValue(has ? layer->midtones : 0.0);
    shadows->setValue(has ? layer->shadows : 0.0);
    whites->setValue(has ? layer->whites : 0.0);
    blacks->setValue(has ? layer->blacks : 0.0);
    temperature->setValue(has ? layer->temperature : 0.0);
    tint->setValue(has ? layer->tint : 0.0);
    hue->setValue(has ? layer->hue : 0.0);
    saturation->setValue(has ? layer->saturation : 0.0);
    clarity->setValue(has ? layer->clarity : 0.0);

    const MaskLayer neutral;
    const MaskLayer& cl = has ? *layer : neutral;
    lshape->setCurve(cl.lcurve);
    ashape->setCurve(cl.acurve);
    bshape->setCurve(cl.bcurve);
    ccshape->setCurve(cl.cccurve);
    chshape->setCurve(cl.chcurve);
    lhshape->setCurve(cl.lhcurve);
    hhshape->setCurve(cl.hhcurve);
    lcshape->setCurve(cl.lccurve);
    clshape->setCurve(cl.clcurve);

    for (Gtk::Widget* w : std::vector<Gtk::Widget*> {maskName, layerInvert, amount, feather, expand, smooth, exposure, contrast, highlights, midtones, shadows, whites, blacks, temperature, tint, hue, saturation, clarity, curvesExpander, maskRemove, maskDuplicate, maskUndoTool, maskClear, showMask}) {
        w->set_sensitive(has);
    }

    updateIntent();

    if (disable) {
        enableListener();
    }
}

void Masking::layerGet()
{
    MaskLayer* layer = currentLayer();

    if (!layer) {
        return;
    }

    layer->name = maskName->get_text();
    layer->invert = layerInvert->get_active();
    layer->amount = amount->getValue();
    layer->feather = feather->getValue();
    layer->expand = expand->getValue();
    layer->smooth = smooth->getValue();
    layer->exposure = exposure->getValue();
    layer->contrast = contrast->getValue();
    layer->highlights = highlights->getValue();
    layer->midtones = midtones->getValue();
    layer->shadows = shadows->getValue();
    layer->whites = whites->getValue();
    layer->blacks = blacks->getValue();
    layer->temperature = temperature->getValue();
    layer->tint = tint->getValue();
    layer->hue = hue->getValue();
    layer->saturation = saturation->getValue();
    layer->clarity = clarity->getValue();
    layer->lcurve = lshape->getCurve();
    layer->acurve = ashape->getCurve();
    layer->bcurve = bshape->getCurve();
    layer->cccurve = ccshape->getCurve();
    layer->chcurve = chshape->getCurve();
    layer->lhcurve = lhshape->getCurve();
    layer->hhcurve = hhshape->getCurve();
    layer->lccurve = lcshape->getCurve();
    layer->clcurve = clshape->getCurve();
}

void Masking::showComponent()
{
    const bool disable = listener;

    if (disable) {
        disableListener();
    }

    const MaskComponent* c = currentComponent();
    const bool has = c != nullptr;

    ConnectionBlocker b2(componentInvertConn);
    ConnectionBlocker b4(aiProviderConn);

    setOpToggles(has ? c->op : MaskBooleanOp::ADD);
    componentInvert->set_active(has && c->invert);
    toolHeader->set_markup(has ? Glib::Markup::escape_text(M("TP_MASKING_ACTIVE_TOOL")) + " <b>" + Glib::Markup::escape_text(componentTypeName(c->type)) + "</b>"
                           : (currentLayer() ? Glib::Markup::escape_text(M("TP_MASKING_NO_TOOL_SELECTED")) : Glib::ustring()));

    if (has) {
        // brush defaults for the next stroke come from the last stroke
        if (!c->strokes.empty()) {
            const auto& s = c->strokes.back();
            brushSize->setValue(s.size * 100.0);
            brushFeather->setValue(s.feather * 100.0);
            brushOpacity->setValue(s.flow * 100.0);
            ConnectionBlocker bp(brushPaintConn);
            ConnectionBlocker be(brushEraseConn);
            brushErase->set_active(s.erase);
            brushPaint->set_active(!s.erase);
        }

        // angles as displayed (the image may be rotated / flipped)
        const double viewAngle = toView(*c).angle;
        radialFeather->setValue(c->feather);
        radialAngle->setValue(viewAngle);
        rectFeather->setValue(c->feather);
        rectCorner->setValue(c->cornerRadius);
        rectAngle->setValue(viewAngle);
        polyFeather->setValue(c->feather);
        wandTolerance->setValue(c->tolerance);
        wandSmoothness->setValue(c->smoothness);
        {
            ConnectionBlocker bwc(wandContiguousConn);
            wandContiguous->set_active(c->contiguous);
        }
        colorTolerance->setValue(c->tolerance);
        colorSmoothness->setValue(c->smoothness);

        if (c->type == MaskComponentType::QUICK_SELECT) {
            quickTolerance->setValue(c->tolerance);
            quickEdge->setValue(c->edgeSensitivity);
            quickSmoothness->setValue(c->smoothness);

            if (!c->strokes.empty()) {
                quickSize->setValue(c->strokes.back().size * 100.0);
            }
        }

        lumLow->setValue(c->lumLow);
        lumHigh->setValue(c->lumHigh);
        lumFeatherLow->setValue(c->lumFeatherLow);
        lumFeatherHigh->setValue(c->lumFeatherHigh);
        aiThreshold->setValue(c->aiThreshold);
        {
            ConnectionBlocker bpc(aiPromptConn);
            ConnectionBlocker brc(aiRefineConn);
            aiPrompt->set_text(c->aiPrompt);
            aiRefine->set_active(c->aiRefineEdges);
        }

        // "" or an unknown id means the registry default (row 0 may be a greyed-out model)
        const std::string wanted = c->aiProvider.empty() ? AISegmentationRegistry::getInstance().getDefaultProviderId() : c->aiProvider;
        int provIdx = -1;

        for (size_t i = 0; i < aiProviderIds.size(); ++i) {
            if (aiProviderIds[i] == wanted) {
                provIdx = int(i);
                break;
            }
        }

        if (provIdx < 0) {
            const std::string def = AISegmentationRegistry::getInstance().getDefaultProviderId();

            for (size_t i = 0; i < aiProviderIds.size(); ++i) {
                if (aiProviderIds[i] == def) {
                    provIdx = int(i);
                    break;
                }
            }
        }

        if (provIdx < 0) {
            provIdx = 0;
        }

        if (!aiProviderIds.empty()) {
            aiProvider->set_active(provIdx);
        }
    }

    for (Gtk::Widget* w : std::vector<Gtk::Widget*> {componentInvert, editBox, showComponentOnly}) {
        w->set_sensitive(has);
    }

    for (Gtk::ToggleButton* b : opButtons) {
        b->set_sensitive(has && selectedComponent > 0);
    }

    updateColorSampleLabel();
    updateTypeBoxes();
    updateAiStatus();
    syncCanvasControls();
    setCanvasVisibility();
    updateGeometry();

    if (disable) {
        enableListener();
    }
}

void Masking::componentGet()
{
    MaskComponent* c = currentComponent();

    if (!c) {
        return;
    }

    c->op = getOpToggles();
    c->invert = componentInvert->get_active();

    // Only the widgets of the active type are written back, so that values
    // of other types (and hand-edited pp3 values) are not altered.
    switch (c->type) {
        case MaskComponentType::RADIAL_GRADIENT:
        case MaskComponentType::RECTANGLE: {
            Adjuster* const angle = c->type == MaskComponentType::RADIAL_GRADIENT ? radialAngle : rectAngle;

            if (c->type == MaskComponentType::RADIAL_GRADIENT) {
                c->feather = radialFeather->getValue();
            } else {
                c->feather = rectFeather->getValue();
                c->cornerRadius = rectCorner->getValue();
            }

            // The slider shows the displayed angle, rounded: only write it
            // back when it was moved, so that the stored angle keeps its
            // precision.
            MaskComponent v = toView(*c);

            if (std::fabs(std::remainder(angle->getValue() - v.angle, 360.0)) >= 0.5) {
                v.angle = angle->getValue();
                fromView(*c, v);
            }

            break;
        }

        case MaskComponentType::POLYGON:
            c->feather = polyFeather->getValue();
            break;

        case MaskComponentType::MAGIC_WAND:
            c->tolerance = wandTolerance->getValue();
            c->smoothness = wandSmoothness->getValue();
            c->contiguous = wandContiguous->get_active();
            break;

        case MaskComponentType::COLOR_RANGE:
            c->tolerance = colorTolerance->getValue();
            c->smoothness = colorSmoothness->getValue();
            break;

        case MaskComponentType::QUICK_SELECT:
            c->tolerance = quickTolerance->getValue();
            c->edgeSensitivity = quickEdge->getValue();
            c->smoothness = quickSmoothness->getValue();
            break;

        case MaskComponentType::LUMINANCE_RANGE:
            c->lumLow = lumLow->getValue();
            c->lumHigh = lumHigh->getValue();
            c->lumFeatherLow = lumFeatherLow->getValue();
            c->lumFeatherHigh = lumFeatherHigh->getValue();
            break;

        case MaskComponentType::AI_SUBJECT:
        case MaskComponentType::AI_SKY:
        case MaskComponentType::AI_BACKGROUND:
        case MaskComponentType::AI_OBJECT: {
            c->aiThreshold = aiThreshold->getValue();
            c->aiPrompt = aiPrompt->get_text();
            c->aiRefineEdges = aiRefine->get_active();
            const int provIdx = aiProvider->get_active_row_number();

            if (provIdx >= 0 && provIdx < int(aiProviderIds.size())) {
                c->aiProvider = aiProviderIds[provIdx];
            }

            break;
        }

        default:
            break;
    }
}

void Masking::updateTypeBoxes()
{
    const MaskComponent* c = currentComponent();
    const MaskComponentType t = c ? c->type : MaskComponentType::BRUSH;

    brushBox->set_visible(c && t == MaskComponentType::BRUSH);
    radialBox->set_visible(c && t == MaskComponentType::RADIAL_GRADIENT);
    rectBox->set_visible(c && t == MaskComponentType::RECTANGLE);
    polyBox->set_visible(c && t == MaskComponentType::POLYGON);
    wandBox->set_visible(c && t == MaskComponentType::MAGIC_WAND);
    quickBox->set_visible(c && t == MaskComponentType::QUICK_SELECT);
    colorBox->set_visible(c && t == MaskComponentType::COLOR_RANGE);
    lumBox->set_visible(c && t == MaskComponentType::LUMINANCE_RANGE);
    aiBox->set_visible(c && maskComponentIsAI(t));
    aiHint->set_visible(c && t == MaskComponentType::AI_OBJECT);

    Glib::ustring hint;

    if (c) {
        switch (t) {
            case MaskComponentType::BRUSH:
                hint = M("TP_MASKING_HINT_BRUSH");
                break;

            case MaskComponentType::LINEAR_GRADIENT:
                hint = M("TP_MASKING_HINT_LINEAR");
                break;

            case MaskComponentType::RADIAL_GRADIENT:
                hint = M("TP_MASKING_HINT_RADIAL");
                break;

            case MaskComponentType::COLOR_RANGE:
                hint = M("TP_MASKING_HINT_COLOR");
                break;

            case MaskComponentType::AI_OBJECT:
                hint = M("TP_MASKING_HINT_OBJECT");
                break;

            case MaskComponentType::RECTANGLE:
                hint = M("TP_MASKING_HINT_RECT");
                break;

            case MaskComponentType::POLYGON:
                hint = M("TP_MASKING_HINT_LASSO");
                break;

            case MaskComponentType::MAGIC_WAND:
                hint = M("TP_MASKING_HINT_WAND");
                break;

            case MaskComponentType::QUICK_SELECT:
                hint = M("TP_MASKING_HINT_QUICKSELECT");
                break;

            default:
                hint = M("TP_MASKING_HINT_NONE");
                break;
        }
    }

    editHint->set_text(hint);
    const bool canvas = c && (t == MaskComponentType::LINEAR_GRADIENT || t == MaskComponentType::RADIAL_GRADIENT
                              || t == MaskComponentType::COLOR_RANGE || t == MaskComponentType::AI_OBJECT
                              || t == MaskComponentType::RECTANGLE || t == MaskComponentType::MAGIC_WAND
                              || t == MaskComponentType::QUICK_SELECT);
    edit->set_visible(!(c && typeUsesToggles(t)));
    edit->set_sensitive(canvas);
}

void Masking::updateColorSampleLabel()
{
    const MaskComponent* c = currentComponent();

    if (c && c->type == MaskComponentType::COLOR_RANGE) {
        colorSampleLabel->set_text(Glib::ustring::compose(M("TP_MASKING_COLOR_SAMPLE"), int(std::lround(c->sampleL)), int(std::lround(c->sampleA)), int(std::lround(c->sampleB))));
    } else {
        colorSampleLabel->set_text("");
    }
}

void Masking::notify(const rtengine::ProcEvent& ev, const Glib::ustring& descr)
{
    listEdited = true;

    if (editedCheckBox) {
        editedConn.block(true);
        editedCheckBox->set_active(true);
        editedConn.block(false);
    }

    if (listener && (getEnabled() || ev == EvMaskingEnabled)) {
        listener->panelChanged(ev, descr);
    }
}

// ---------------------------------------------------------------------------
// Widget callbacks

void Masking::adjusterChanged(Adjuster* a, double newval)
{
    (void)newval;

    if (!listener) {
        return;
    }

    MaskLayer* layer = currentLayer();

    if (!layer) {
        return;
    }

    struct Entry {
        Adjuster* adj;
        rtengine::ProcEvent* ev;
    };

    const Entry layerEntries[] = {
        {amount, &EvMaskingAmount}, {feather, &EvMaskingFeather},
        {exposure, &EvMaskingExposure}, {contrast, &EvMaskingContrast}, {highlights, &EvMaskingHighlights},
        {midtones, &EvMaskingMidtones}, {shadows, &EvMaskingShadows}, {whites, &EvMaskingWhites}, {blacks, &EvMaskingBlacks},
        {temperature, &EvMaskingTemperature}, {tint, &EvMaskingTint}, {hue, &EvMaskingHue},
        {saturation, &EvMaskingSaturation}, {clarity, &EvMaskingClarity},
        {expand, &EvMaskingExpand}, {smooth, &EvMaskingSmooth}
    };

    for (const Entry& e : layerEntries) {
        if (a == e.adj) {
            // Starting the adjustments means the mask is done: hide the overlay
            // so the result is visible. Shape controls (feather, expand,
            // smooth) keep it, they are still about the mask itself.
            const bool shapeControl = a == feather || a == expand || a == smooth;

            if (!shapeControl && showMask->get_active() && !batchMode) {
                ConnectionBlocker b(showMaskConn);
                showMask->set_active(false);
                showComponentOnly->set_sensitive(false);
                overlayAutoEnabled = false;
            }

            layerGet();
            notify(*e.ev, a->getTextValue());
            return;
        }
    }

    // brush settings only affect the next stroke
    if (a == brushSize || a == brushFeather || a == brushOpacity || a == quickSize) {
        updateGeometry();
        return;
    }

    MaskComponent* c = currentComponent();

    if (!c) {
        return;
    }

    componentGet();

    if (a == radialAngle || a == radialFeather || a == rectAngle || a == rectFeather || a == rectCorner || a == polyFeather) {
        updateGeometry();
        notify(EvMaskingShape, a->getTextValue());
    } else {
        notify(EvMaskingComponent, a->getTextValue());
    }
}

void Masking::curveChanged(CurveEditor* ce)
{
    if (!listener || !currentLayer()) {
        return;
    }

    struct Entry {
        CurveEditor* curve;
        rtengine::ProcEvent* ev;
    };

    const Entry entries[] = {
        {lshape, &EvMaskingCurveL}, {ashape, &EvMaskingCurveA}, {bshape, &EvMaskingCurveB},
        {ccshape, &EvMaskingCurveCC}, {chshape, &EvMaskingCurveCH}, {lhshape, &EvMaskingCurveLH},
        {hhshape, &EvMaskingCurveHH}, {lcshape, &EvMaskingCurveLC}, {clshape, &EvMaskingCurveCL}
    };

    for (const Entry& e : entries) {
        if (ce == e.curve) {
            // like the adjustment sliders: the result should be visible, not the overlay
            if (showMask->get_active() && !batchMode) {
                ConnectionBlocker b(showMaskConn);
                showMask->set_active(false);
                showComponentOnly->set_sensitive(false);
                overlayAutoEnabled = false;
            }

            layerGet();
            notify(*e.ev, M("HISTORY_CUSTOMCURVE"));
            return;
        }
    }
}

void Masking::colorForValue(double valX, double valY, enum ColorCaller::ElemType elemType, int callerId, ColorCaller* caller)
{
    // same color bars as the L*a*b* Adjustments tool (L = f(C) shows the whole hue range)
    float R = 0.f, G = 0.f, B = 0.f;

    if (elemType == ColorCaller::CCET_VERTICAL_BAR) {
        valY = 0.5;
    }

    if (callerId == 1) {         // C = f(H): main curve
        Color::hsv2rgb01(float(valX), float(valY), 0.5f, R, G, B);
    } else if (callerId == 2) {  // C = f(C), L = f(C): bottom bar
        const float value = (1.f - 0.7f) * float(valX) + 0.7f;
        Color::hsv2rgb01(float(valY * 0.8), float(valX), value, R, G, B);
    } else if (callerId == 4) {  // L = f(H): main curve
        Color::hsv2rgb01(float(valX), 0.5f, float(valY), R, G, B);
    } else if (callerId == 5) {  // H = f(H): main curve
        float h = float((valY - 0.5) * 0.3 + valX);

        if (h > 1.0f) {
            h -= 1.0f;
        } else if (h < 0.0f) {
            h += 1.0f;
        }

        Color::hsv2rgb01(h, 0.5f, 0.5f, R, G, B);
    } else if (callerId == 7) {  // C = f(C), C = f(L): left bar
        const float value = (1.f - 0.7f) * float(valX) + 0.7f;
        Color::hsv2rgb01(float(valY * 0.8), 1.f - float(valX), value, R, G, B);
    }

    caller->ccRed = double(R);
    caller->ccGreen = double(G);
    caller->ccBlue = double(B);
}

void Masking::enabledChanged()
{
    if (listener) {
        if (get_inconsistent()) {
            listener->panelChanged(EvMaskingEnabled, M("GENERAL_UNCHANGED"));
        } else if (getEnabled()) {
            listener->panelChanged(EvMaskingEnabled, M("GENERAL_ENABLED"));
        } else {
            listener->panelChanged(EvMaskingEnabled, M("GENERAL_DISABLED"));
        }
    }
}

void Masking::onTreeSelectionChanged()
{
    const auto it = tree->get_selection()->get_selected();

    if (!it) {
        return;
    }

    const int m = (*it)[treeColumns.maskIdx];

    if (m == selectedMask) {
        return;
    }

    componentGet();
    layerGet();
    const bool maskChanged = true;
    selectedMask = m;
    // the active tool of a mask is the one used last
    selectedComponent = (m >= 0 && m < int(masks.size()) && !masks[m].components.empty()) ? int(masks[m].components.size()) - 1 : -1;

    if (maskChanged) {
        showLayer();
    }

    showComponent();

    if (showMask->get_active() && (maskChanged || showComponentOnly->get_active())) {
        notify(EvMaskingShowMask, M("HISTORY_CHANGED"));
    }
}

void Masking::onTreeEnabledToggled(const Glib::ustring& path)
{
    const auto it = treeStore->get_iter(path);

    if (!it) {
        return;
    }

    const int m = (*it)[treeColumns.maskIdx];
    const int c = (*it)[treeColumns.compIdx];

    if (m < 0 || m >= int(masks.size())) {
        return;
    }

    if (c < 0) {
        masks[m].enabled = !masks[m].enabled;
        fillRow(*it, m, c);
        notify(EvMaskingLayer, masks[m].enabled ? M("GENERAL_ENABLED") : M("GENERAL_DISABLED"));
    } else if (c < int(masks[m].components.size())) {
        masks[m].components[c].enabled = !masks[m].components[c].enabled;
        fillRow(*it, m, c);
        updateMaskRow(m);
        notify(EvMaskingComponent, masks[m].components[c].enabled ? M("GENERAL_ENABLED") : M("GENERAL_DISABLED"));
    }
}

void Masking::onIntentToggled(int which)
{
    // radio behaviour: exactly one intent is active
    for (int i = 0; i < INTENT_COUNT; ++i) {
        ConnectionBlocker b(intentConn[i]);
        intentButtons[i]->set_active(i == which);
    }

    updateIntent();
}

void Masking::onPaletteClicked(MaskComponentType t)
{
    int active = INTENT_NEW;

    for (int i = 0; i < INTENT_COUNT; ++i) {
        if (intentButtons[i]->get_active()) {
            active = i;
        }
    }

    // The engine only evaluates the masks of a switched-on tool and notify()
    // drops events while it is off. Picking a tool means "use it now", so
    // switch the tool on first, for every tool: the ones with nothing to do
    // on the canvas (Subject, Sky, Background, Luminance) never reach
    // setCanvasActive(), which used to be the only place doing this.
    if (!getEnabled() && !batchMode) {
        setEnabled(true);
        enabledChanged();
    }

    if (active == INTENT_NEW || !currentLayer()) {
        createMaskWith(t);
    } else {
        addComponent(t, active == INTENT_SUBTRACT ? MaskBooleanOp::SUBTRACT : (active == INTENT_INTERSECT ? MaskBooleanOp::INTERSECT : MaskBooleanOp::ADD));
    }

    // Picking a tool means "use it now": the preview is ready for it.
    activateToolOnCanvas();
}

void Masking::activateToolOnCanvas()
{
    const MaskComponent* c = currentComponent();

    if (!c || batchMode || !getEditProvider()) {
        return;
    }

    switch (c->type) {
        case MaskComponentType::LUMINANCE_RANGE:
        case MaskComponentType::AI_SUBJECT:
        case MaskComponentType::AI_SKY:
        case MaskComponentType::AI_BACKGROUND:
            // nothing to do on the preview for these
            return;

        default:
            break;
    }

    if (c->type == MaskComponentType::BRUSH) {
        // the palette's Brush always paints; the Eraser button switches to erasing
        ConnectionBlocker bp(brushPaintConn);
        ConnectionBlocker be(brushEraseConn);
        brushPaint->set_active(true);
        brushErase->set_active(false);
        updateBrushCursorStyle(lastModifier);
    }

    if (c->type == MaskComponentType::QUICK_SELECT) {
        // picked from the palette: strokes add (Alt or the Subtract toggle take away)
        quickSubtract->set_active(false);
    }

    // Lasso defaults to freehand through syncCanvasControls(); the shape and
    // picking tools use the edit (crosshair) state.
    setCanvasActive(true);
}

void Masking::onOpToggled(int which)
{
    MaskComponent* c = currentComponent();

    if (!c) {
        setOpToggles(MaskBooleanOp::ADD);
        return;
    }

    if (!opButtons[which]->get_active()) {
        // clicking the active mode keeps it active
        ConnectionBlocker b(opConn[which]);
        opButtons[which]->set_active(true);
        return;
    }

    for (int i = 0; i < 3; ++i) {
        if (i != which) {
            ConnectionBlocker b(opConn[i]);
            opButtons[i]->set_active(false);
        }
    }

    c->op = static_cast<MaskBooleanOp>(which);
    updateComponentRow(selectedComponent);
    notify(EvMaskingComponent, M(which == 0 ? "TP_MASKING_OP_ADD" : (which == 1 ? "TP_MASKING_OP_SUBTRACT" : "TP_MASKING_OP_INTERSECT")));
}

void Masking::createMaskWith(MaskComponentType t)
{
    componentGet();
    layerGet();

    MaskLayer layer;
    layer.name = Glib::ustring::compose(M("TP_MASKING_DEFAULT_NAME"), masks.size() + 1);
    layer.components.emplace_back(t);

    masks.push_back(layer);
    selectedMask = int(masks.size()) - 1;
    selectedComponent = 0;
    populateMaskList();
    populateComponentList();
    showLayer();
    showComponent();
    onIntentToggled(INTENT_ADD);

    // A new mask starts with its overlay shown, so the user sees what the
    // tool selects; the overlay goes away again when the adjustments start
    // (adjusterChanged). Written to the engine with the list change below.
    if (!batchMode && !showMask->get_active()) {
        ConnectionBlocker b(showMaskConn);
        showMask->set_active(true);
        showComponentOnly->set_sensitive(true);
        overlayAutoEnabled = true;
    }

    notify(EvMaskingList, M("HISTORY_CHANGED"));
}

void Masking::onUndoToolPressed()
{
    MaskLayer* layer = currentLayer();

    if (!layer || layer->components.empty()) {
        return;
    }

    releaseEdit();
    layer->components.pop_back();
    selectedComponent = int(layer->components.size()) - 1;
    updateMaskRow(selectedMask);
    showComponent();
    notify(EvMaskingComponentList, M("TP_MASKING_UNDO_TOOL_TOOLTIP"));
}

void Masking::onClearMaskPressed()
{
    MaskLayer* layer = currentLayer();

    if (!layer || layer->components.empty()) {
        return;
    }

    releaseEdit();
    layer->components.clear();
    selectedComponent = -1;
    updateMaskRow(selectedMask);
    showComponent();
    notify(EvMaskingComponentList, M("TP_MASKING_CLEAR_MASK_TOOLTIP"));
}

void Masking::onEraserClicked()
{
    if (!currentLayer()) {
        createMaskWith(MaskComponentType::BRUSH);
    }

    if (!ensureBrushComponent()) {
        return;
    }

    {
        ConnectionBlocker bp(brushPaintConn);
        ConnectionBlocker be(brushEraseConn);
        brushErase->set_active(true);
        brushPaint->set_active(false);
    }

    updateBrushCursorStyle(lastModifier);
    setCanvasActive(true);
}

void Masking::onRemovePressed()
{
    MaskLayer* layer = currentLayer();

    if (!layer) {
        return;
    }

    releaseEdit();
    masks.erase(masks.begin() + selectedMask);
    selectedMask = masks.empty() ? -1 : LIM(selectedMask, 0, int(masks.size()) - 1);
    selectedComponent = -1;
    populateTree();
    showLayer();
    showComponent();
    notify(EvMaskingList, M("HISTORY_CHANGED"));
}

void Masking::onMaskDuplicatePressed()
{
    const MaskLayer* layer = currentLayer();

    if (!layer) {
        return;
    }

    componentGet();
    layerGet();
    // the copy gets the same mask (components and refinements), but starts
    // with neutral adjustments
    MaskLayer copy = *layer;
    copy.resetAdjustments();
    copy.name = Glib::ustring::compose(M("TP_MASKING_COPY_NAME"), copy.name.empty() ? Glib::ustring::compose(M("TP_MASKING_DEFAULT_NAME"), selectedMask + 1) : Glib::ustring(copy.name));
    masks.insert(masks.begin() + selectedMask + 1, copy);
    ++selectedMask;
    populateMaskList();
    populateComponentList();
    showLayer();
    showComponent();
    notify(EvMaskingList, M("HISTORY_CHANGED"));
}

void Masking::onMaskNameChanged()
{
    MaskLayer* layer = currentLayer();

    if (!layer) {
        return;
    }

    layer->name = maskName->get_text();
    updateMaskRow(selectedMask);
    listEdited = true;
    // renaming does not need a reprocessing; the name is written with the next change
}

void Masking::onEditedToggled()
{
    listEdited = editedCheckBox && editedCheckBox->get_active();

    if (listener) {
        listener->panelChanged(EvMaskingList, listEdited ? M("HISTORY_CHANGED") : M("GENERAL_UNCHANGED"));
    }
}

void Masking::addComponent(MaskComponentType t, MaskBooleanOp op)
{
    MaskLayer* layer = currentLayer();

    if (!layer) {
        return;
    }

    componentGet();
    layer->components.emplace_back(t);
    layer->components.back().op = op;
    selectedComponent = int(layer->components.size()) - 1;
    populateComponentList();
    updateMaskRow(selectedMask);
    showComponent();
    notify(EvMaskingComponentList, componentTypeName(t));
}

void Masking::onComponentInvertToggled()
{
    MaskComponent* c = currentComponent();

    if (!c) {
        return;
    }

    componentGet();
    updateComponentRow(selectedComponent);
    notify(EvMaskingComponent, M("TP_MASKING_INVERT"));
}

void Masking::onBrushModeToggled(bool erase)
{
    // Paint and erase are the brush's on/off switches: at most one is active.
    // Clicking the active one switches brushing off (the preview can be panned again).
    Gtk::ToggleButton* clicked = erase ? brushErase : brushPaint;
    Gtk::ToggleButton* other = erase ? brushPaint : brushErase;

    {
        ConnectionBlocker bp(brushPaintConn);
        ConnectionBlocker be(brushEraseConn);
        other->set_active(false);
    }

    const bool on = clicked->get_active();

    if (on && !ensureBrushComponent()) {
        ConnectionBlocker bc(erase ? brushEraseConn : brushPaintConn);
        clicked->set_active(false);
        return;
    }

    // ensureBrushComponent() may have refreshed the widgets; restore the click.
    {
        ConnectionBlocker bp(brushPaintConn);
        ConnectionBlocker be(brushEraseConn);
        clicked->set_active(on);
        other->set_active(false);
    }

    updateBrushCursorStyle(lastModifier);
    setCanvasActive(on);
}

bool Masking::ensureBrushComponent()
{
    MaskLayer* layer = currentLayer();

    if (!layer) {
        return false;
    }

    const MaskComponent* c = currentComponent();

    if (c && c->type == MaskComponentType::BRUSH) {
        return true;
    }

    componentGet();

    if (!layer->components.empty() && layer->components.back().type == MaskComponentType::BRUSH) {
        // reuse the mask's editing brush (always the last component so it edits everything before it)
        selectedComponent = int(layer->components.size()) - 1;
    } else {
        layer->components.emplace_back(MaskComponentType::BRUSH);
        selectedComponent = int(layer->components.size()) - 1;
        notify(EvMaskingComponentList, componentTypeName(MaskComponentType::BRUSH));
    }

    populateComponentList();
    updateMaskRow(selectedMask);
    showComponent();
    return true;
}

bool Masking::eraseActive(int modifierKey) const
{
    // The eraser toggle, or holding Alt while painting (Lightroom convention).
    return brushErase->get_active() || (modifierKey & GDK_MOD1_MASK);
}

bool Masking::quickEraseActive(int modifierKey) const
{
    return quickSubtract->get_active() || (modifierKey & GDK_MOD1_MASK);
}

void Masking::updateBrushCursorStyle(int modifierKey)
{
    const MaskComponent* qc = currentComponent();
    const bool erasing = (qc && qc->type == MaskComponentType::QUICK_SELECT) ? quickEraseActive(modifierKey) : eraseActive(modifierKey);

    for (Circle* c : {brushCursor, brushCore}) {
        if (erasing) {
            c->setInnerLineColor(0.9, 0.25, 0.25);
            c->innerLineWidth = 2.f;
        } else {
            c->setAutoColor(true);
            c->innerLineWidth = 1.5f;
        }
    }
}

bool Masking::typeUsesToggles(MaskComponentType t) const
{
    return t == MaskComponentType::BRUSH || t == MaskComponentType::POLYGON;
}

void Masking::onPolyModeToggled(bool polygonal)
{
    Gtk::ToggleButton* clicked = polygonal ? polyPolygonal : polyFreehand;
    Gtk::ToggleButton* other = polygonal ? polyFreehand : polyPolygonal;

    {
        ConnectionBlocker bf(polyFreehandConn);
        ConnectionBlocker bp(polyPolygonalConn);
        other->set_active(false);
    }

    if (polyOpen) {
        closePolygon();
    }

    setCanvasActive(clicked->get_active());
}

void Masking::onPolyClosePressed()
{
    if (polyOpen) {
        closePolygon();
    }
}

void Masking::closePolygon()
{
    MaskComponent* c = currentComponent();
    polyOpen = false;
    polyInProgress = false;

    if (!c || c->type != MaskComponentType::POLYGON) {
        return;
    }

    if (c->pointCount() < 3) {
        c->points.clear();
    }

    updateComponentRow(selectedComponent);
    setCanvasVisibility();
    updateGeometry();
    notify(EvMaskingPolygon, Glib::ustring::compose(M("TP_MASKING_POLY_POINT_COUNT"), c->pointCount()));
}

void Masking::onWandContiguousToggled()
{
    MaskComponent* c = currentComponent();

    if (!c || c->type != MaskComponentType::MAGIC_WAND) {
        return;
    }

    c->contiguous = wandContiguous->get_active();
    notify(EvMaskingComponent, M("TP_MASKING_WAND_CONTIGUOUS"));
}

void Masking::addWandSeed(const rtengine::Coord& pos, bool replace)
{
    MaskComponent* c = currentComponent();

    if (!c || c->type != MaskComponentType::MAGIC_WAND || !maskingProvider) {
        return;
    }

    float L, a, b;

    if (!maskingProvider->getMaskingLabSample(pos, 4, L, a, b)) {
        return;
    }

    double nx, ny;
    normalizedFromImage(pos, nx, ny);

    if (replace) {
        c->seeds.clear();
    }

    c->seeds.insert(c->seeds.end(), {nx, ny, double(L), double(a), double(b)});
    lastSeedPos = pos;
    updateGeometry();
}

void Masking::applyModifierOp(int modifierKey)
{
    // Photoshop style: Shift = add, Alt = subtract, Shift+Alt = intersect, latched
    // when a new shape is started. The first component always defines the selection.
    MaskComponent* c = currentComponent();

    if (!c || selectedComponent <= 0) {
        return;
    }

    const bool shift = modifierKey & GDK_SHIFT_MASK;
    const bool alt = modifierKey & GDK_MOD1_MASK;

    if (!shift && !alt) {
        return;
    }

    c->op = (shift && alt) ? MaskBooleanOp::INTERSECT : (alt ? MaskBooleanOp::SUBTRACT : MaskBooleanOp::ADD);
    setOpToggles(c->op);
    updateComponentRow(selectedComponent);
}

void Masking::onAiProviderChanged()
{
    MaskComponent* c = currentComponent();

    if (!c) {
        return;
    }

    componentGet();
    notify(EvMaskingComponent, aiProvider->get_active_text());
}

void Masking::onAiPromptChanged()
{
    MaskComponent* c = currentComponent();

    if (!c || !maskComponentIsAI(c->type)) {
        return;
    }

    const std::string text = aiPrompt->get_text();

    if (text == c->aiPrompt) {
        return;
    }

    c->aiPrompt = text;
    notify(EvMaskingComponent, text.empty() ? M("TP_MASKING_AI_PROMPT") : Glib::ustring(text));
}

void Masking::onAiRefineToggled()
{
    MaskComponent* c = currentComponent();

    if (!c || !maskComponentIsAI(c->type)) {
        return;
    }

    componentGet();
    notify(EvMaskingComponent, aiRefine->get_active() ? M("GENERAL_ENABLED") : M("GENERAL_DISABLED"));
}

bool Masking::onAiPromptFocusOut(GdkEventFocus* event)
{
    (void)event;
    onAiPromptChanged();
    return false;
}

void Masking::onSegmentationResult(bool finished)
{
    updateAiStatus();

    if (finished && listener && !batchMode) {
        listener->refreshPreview(EvMaskingComponent);
    }
}

void Masking::updateAiStatus()
{
    auto& registry = AISegmentationRegistry::getInstance();
    const bool busy = registry.getPending() > 0 && !batchMode;

    // The spinner is independent of the selected tool: the user must see
    // that something is happening whatever is selected.
    if (busy) {
        aiBusyBox->show();
        aiSpinner->show();
        aiBusyLabel->show();
        aiSpinner->start();
    } else {
        aiSpinner->stop();
        aiBusyBox->hide();
    }

    const MaskComponent* c = currentComponent();

    if (!c || !maskComponentIsAI(c->type)) {
        aiStatus->set_text("");
        return;
    }

    const std::string error = registry.getLastError();

    if (registry.getPending() > 0) {
        aiStatus->set_text(M("TP_MASKING_AI_COMPUTING"));
    } else if (!error.empty()) {
        aiStatus->set_text(Glib::ustring::compose(M("TP_MASKING_AI_ERROR"), error));
    } else {
        aiStatus->set_text("");
    }
}

void Masking::onShowMaskToggled()
{
    showComponentOnly->set_sensitive(showMask->get_active());
    notify(EvMaskingShowMask, showMask->get_active() ? M("GENERAL_ENABLED") : M("GENERAL_DISABLED"));
}

void Masking::onLayerInvertToggled()
{
    MaskLayer* layer = currentLayer();

    if (!layer) {
        return;
    }

    layer->invert = layerInvert->get_active();
    notify(EvMaskingLayer, M("TP_MASKING_INVERT_MASK"));
}

// ---------------------------------------------------------------------------
// On-canvas editing

void Masking::editToggled()
{
    setCanvasActive(edit->get_active());
}

void Masking::setCanvasActive(bool active)
{
    if (active) {
        if (!isCurrentSubscriber()) {
            subscribe();
        }

        // Painting on a disabled tool would show nothing: switch it on.
        if (!getEnabled() && !batchMode) {
            setEnabled(true);
            enabledChanged();
        }

        const MaskComponent* c = currentComponent();

        if (c && (c->type == MaskComponentType::BRUSH || c->type == MaskComponentType::RECTANGLE || c->type == MaskComponentType::POLYGON
                  || c->type == MaskComponentType::MAGIC_WAND || c->type == MaskComponentType::QUICK_SELECT) && !showMask->get_active()) {
            ConnectionBlocker b(showMaskConn);
            showMask->set_active(true);
            showComponentOnly->set_sensitive(true);
            overlayAutoEnabled = true;
            notify(EvMaskingShowMask, M("GENERAL_ENABLED"));
        }
    } else {
        releaseEdit();

        if (isCurrentSubscriber()) {
            unsubscribe();
        }

        deactivateCanvasControls();
    }

    syncCanvasControls();
    setCanvasVisibility();
    updateGeometry();
}

void Masking::deactivateCanvasControls()
{
    if (overlayAutoEnabled) {
        overlayAutoEnabled = false;
        ConnectionBlocker b(showMaskConn);
        showMask->set_active(false);
        showComponentOnly->set_sensitive(false);
        notify(EvMaskingShowMask, M("GENERAL_DISABLED"));
    }
}

void Masking::syncCanvasControls()
{
    const bool active = isCurrentSubscriber();
    ConnectionBlocker be(editConn);
    ConnectionBlocker bp(brushPaintConn);
    ConnectionBlocker br(brushEraseConn);

    ConnectionBlocker bf(polyFreehandConn);
    ConnectionBlocker bpg(polyPolygonalConn);

    edit->set_active(active);

    if (!active) {
        brushPaint->set_active(false);
        brushErase->set_active(false);
        polyFreehand->set_active(false);
        polyPolygonal->set_active(false);
    } else {
        if (!brushPaint->get_active() && !brushErase->get_active()) {
            brushPaint->set_active(true);
        }

        if (!polyFreehand->get_active() && !polyPolygonal->get_active()) {
            polyFreehand->set_active(true);
        }
    }

    updateBrushCursorStyle(lastModifier);
}

void Masking::liveRefresh()
{
    if (listener && !batchMode) {
        listener->refreshPreview(EvMaskingStroke);
    }
}

void Masking::releaseEdit()
{
    if (strokeInProgress) {
        commitStroke();
    }

    if (polyOpen || polyInProgress) {
        closePolygon();
    }

    if (shapeDragged) {
        shapeDragged = false;
        notify(EvMaskingShape, M("TP_MASKING_SHAPE_CHANGED"));
    }

    dragMode = DragMode::NONE;
    EditSubscriber::action = EditSubscriber::Action::NONE;
}

void Masking::switchOffEditMode()
{
    // Another tool took over the preview (or the user left edit mode).
    releaseEdit();
    EditSubscriber::switchOffEditMode();
    deactivateCanvasControls();
    syncCanvasControls();
    setCanvasVisibility();
}

bool Masking::imageSize(int& w, int& h) const
{
    EditDataProvider* provider = getEditProvider();

    if (!provider) {
        return false;
    }

    provider->getImageSize(w, h);
    return w > 0 && h > 0;
}

bool Masking::setGeometry(const ProcParams& pp)
{
    if (geometryParams && MaskTransform::sameGeometry(*geometryParams, pp)) {
        return false;
    }

    if (!geometryParams) {
        geometryParams.reset(new ProcParams());
    }

    MaskTransform::copyGeometry(pp, *geometryParams);
    transform.reset();
    return true;
}

const MaskTransform* Masking::maskTransform() const
{
    int w, h;

    if (!geometryParams || !imageSize(w, h)) {
        return nullptr;
    }

    if (!transform || transformW != w || transformH != h) {
        transform.reset(new MaskTransform(*geometryParams, w, h));
        transformW = w;
        transformH = h;
    }

    return transform->isIdentity() ? nullptr : transform.get();
}

void Masking::viewFromImage(const rtengine::Coord& p, double& nx, double& ny) const
{
    int w, h;

    if (!imageSize(w, h)) {
        nx = ny = 0.5;
        return;
    }

    nx = LIM(double(p.x) / w, 0.0, 1.0);
    ny = LIM(double(p.y) / h, 0.0, 1.0);
}

rtengine::Coord Masking::imageFromView(double nx, double ny) const
{
    int w, h;

    if (!imageSize(w, h)) {
        return rtengine::Coord(0, 0);
    }

    return rtengine::Coord(int(std::lround(nx * w)), int(std::lround(ny * h)));
}

void Masking::normalizedFromImage(const rtengine::Coord& p, double& nx, double& ny) const
{
    viewFromImage(p, nx, ny);

    if (const MaskTransform* t = maskTransform()) {
        t->toSource(nx, ny);
    }
}

rtengine::Coord Masking::imageFromNormalized(double nx, double ny) const
{
    if (const MaskTransform* t = maskTransform()) {
        t->toOutput(nx, ny);
    }

    return imageFromView(nx, ny);
}

std::vector<rtengine::Coord> Masking::imagesFromNormalized(const std::vector<double>& values, size_t stride) const
{
    std::vector<double> xy;
    xy.reserve(values.size() / stride * 2);

    for (size_t i = 0; i + stride - 1 < values.size(); i += stride) {
        xy.push_back(values[i]);
        xy.push_back(values[i + 1]);
    }

    if (const MaskTransform* t = maskTransform()) {
        t->toOutput(xy);
    }

    std::vector<rtengine::Coord> out;
    out.reserve(xy.size() / 2);

    for (size_t i = 0; i + 1 < xy.size(); i += 2) {
        out.push_back(imageFromView(xy[i], xy[i + 1]));
    }

    return out;
}

Masking::MaskComponent Masking::toView(const MaskComponent& c) const
{
    MaskComponent v = c;

    if (const MaskTransform* t = maskTransform()) {
        t->componentToOutput(v);
        // keep the angle in the range of the angle sliders
        v.angle = std::remainder(v.angle, 360.0);
    }

    return v;
}

void Masking::fromView(MaskComponent& c, const MaskComponent& view) const
{
    c = view;

    if (const MaskTransform* t = maskTransform()) {
        t->componentToSource(c);
        c.angle = std::remainder(c.angle, 360.0);
    }
}

double Masking::transformScale() const
{
    const MaskTransform* t = maskTransform();
    return t ? t->scale() : 1.0;
}

double Masking::diagonalPixels() const
{
    int w, h;

    if (!imageSize(w, h)) {
        return 1.0;
    }

    return std::sqrt(double(w) * w + double(h) * h);
}

void Masking::setCanvasVisibility()
{
    const MaskComponent* c = currentComponent();
    const bool editing = isCurrentSubscriber() && c;
    const MaskComponentType t = c ? c->type : MaskComponentType::BRUSH;

    brushCursor->setActive(editing && (t == MaskComponentType::BRUSH || t == MaskComponentType::QUICK_SELECT));
    brushCursor->setHoverable(false);
    brushCore->setActive(editing && t == MaskComponentType::BRUSH);
    brushCore->setHoverable(false);

    const bool linear = editing && t == MaskComponentType::LINEAR_GRADIENT;
    gradientLine->setActive(linear);
    gradientLine->setHoverable(false);
    gradientStart->setActive(linear);
    gradientStart->setHoverable(false);
    gradientEnd->setActive(linear);
    gradientEnd->setHoverable(false);

    const bool radial = editing && t == MaskComponentType::RADIAL_GRADIENT;
    ellipseOutline->setActive(radial);
    ellipseOutline->setHoverable(false);
    ellipseCenter->setActive(radial);
    ellipseCenter->setHoverable(false);
    handleX->setActive(radial);
    handleX->setHoverable(false);
    handleY->setActive(radial);
    handleY->setHoverable(false);

    const bool pick = editing && (t == MaskComponentType::COLOR_RANGE || t == MaskComponentType::AI_OBJECT);
    pickMarker->setActive(pick && (t == MaskComponentType::AI_OBJECT || hasPickPos));
    pickMarker->setHoverable(false);

    const bool rect = editing && t == MaskComponentType::RECTANGLE;
    rectOutline->setActive(rect);
    rectOutline->setHoverable(false);
    rectCornerA->setActive(rect);
    rectCornerA->setHoverable(false);
    rectCornerB->setActive(rect);
    rectCornerB->setHoverable(false);
    rectCenter->setActive(rect);
    rectCenter->setHoverable(false);

    const bool poly = editing && t == MaskComponentType::POLYGON;
    polyOutline->setActive(poly && c->pointCount() >= 2);
    polyOutline->setHoverable(false);
    polyRubber->setActive(poly && polyOpen && c->pointCount() >= 1);
    polyRubber->setHoverable(false);
    polyFirst->setActive(poly && polyOpen && c->pointCount() >= 1);
    polyFirst->setHoverable(false);

    const bool wand = editing && t == MaskComponentType::MAGIC_WAND;

    for (size_t i = 0; i < seedMarkers.size(); ++i) {
        seedMarkers[i]->setActive(wand && i < c->seedCount());
        seedMarkers[i]->setHoverable(false);
    }

    moHandleA->setActive(linear || radial || rect);
    moHandleB->setActive(linear || radial || rect);
    moCenter->setActive(radial || rect);
    moPolyFirst->setActive(poly && polyOpen && c->pointCount() >= 3);
}

void Masking::updateGeometry()
{
    int w, h;

    if (!imageSize(w, h)) {
        return;
    }

    imgRect->setXYWH(0, 0, w, h);

    const MaskComponent* c = currentComponent();

    if (!c) {
        return;
    }

    const double diag = diagonalPixels();

    switch (c->type) {
        case MaskComponentType::BRUSH: {
            // Outer circle: brush edge. Inner circle: fully selected core, i.e.
            // where the feather zone starts.
            const double radius = std::max(brushSize->getValue() / 100.0 * diag * transformScale(), 1.0);
            const double feather = LIM(brushFeather->getValue() / 100.0, 0.0, 1.0);
            brushCursor->radius = int(std::lround(radius));
            brushCore->radius = std::max(int(std::lround(radius * (1.0 - feather))), 1);
            brushCore->center = brushCursor->center;
            brushCore->setVisible(brushCursor->isVisible() && feather > 0.0 && brushCore->radius < brushCursor->radius);
            break;
        }

        case MaskComponentType::LINEAR_GRADIENT: {
            gradientLine->begin = imageFromNormalized(c->x0, c->y0);
            gradientLine->end = imageFromNormalized(c->x1, c->y1);
            gradientStart->center = gradientLine->begin;
            gradientEnd->center = gradientLine->end;
            moHandleA->center = gradientLine->begin;
            moHandleB->center = gradientLine->end;
            break;
        }

        case MaskComponentType::RADIAL_GRADIENT: {
            const MaskComponent v = toView(*c);
            const rtengine::Coord center = imageFromView(v.centerX, v.centerY);
            const double rx = v.radiusX * diag;
            const double ry = v.radiusY * diag;
            const double rad = v.angle * RT_PI / 180.0;
            const double cosA = std::cos(rad);
            const double sinA = std::sin(rad);

            ellipseOutline->points.resize(ELLIPSE_POINTS + 1);

            for (int i = 0; i <= ELLIPSE_POINTS; ++i) {
                const double t = 2.0 * RT_PI * i / ELLIPSE_POINTS;
                const double u = rx * std::cos(t);
                const double v = ry * std::sin(t);
                ellipseOutline->points[i].set(int(std::lround(center.x + cosA * u - sinA * v)), int(std::lround(center.y + sinA * u + cosA * v)));
            }

            ellipseCenter->center = center;
            moCenter->center = center;
            handleX->center.set(int(std::lround(center.x + cosA * rx)), int(std::lround(center.y + sinA * rx)));
            handleY->center.set(int(std::lround(center.x - sinA * ry)), int(std::lround(center.y + cosA * ry)));
            moHandleA->center = handleX->center;
            moHandleB->center = handleY->center;
            break;
        }

        case MaskComponentType::AI_OBJECT: {
            pickMarker->center = imageFromNormalized(c->aiHintX, c->aiHintY);
            break;
        }

        case MaskComponentType::RECTANGLE: {
            const MaskComponent v = toView(*c);
            const rtengine::Coord a = imageFromView(v.x0, v.y0);
            const rtengine::Coord b = imageFromView(v.x1, v.y1);
            const double cx = 0.5 * (a.x + b.x);
            const double cy = 0.5 * (a.y + b.y);
            const double rad = v.angle * RT_PI / 180.0;
            const double cosA = std::cos(rad);
            const double sinA = std::sin(rad);
            auto rot = [&](double x, double y) {
                const double dx = x - cx;
                const double dy = y - cy;
                return rtengine::Coord(int(std::lround(cx + cosA * dx - sinA * dy)), int(std::lround(cy + sinA * dx + cosA * dy)));
            };
            rectOutline->points.resize(5);
            rectOutline->points[0] = rot(a.x, a.y);
            rectOutline->points[1] = rot(b.x, a.y);
            rectOutline->points[2] = rot(b.x, b.y);
            rectOutline->points[3] = rot(a.x, b.y);
            rectOutline->points[4] = rectOutline->points[0];
            rectCornerA->center = rectOutline->points[0];
            rectCornerB->center = rectOutline->points[2];
            rectCenter->center.set(int(std::lround(cx)), int(std::lround(cy)));
            moHandleA->center = rectCornerA->center;
            moHandleB->center = rectCornerB->center;
            moCenter->center = rectCenter->center;
            break;
        }

        case MaskComponentType::POLYGON: {
            const size_t n = c->pointCount();
            const std::vector<rtengine::Coord> pts = imagesFromNormalized(c->points, 2);
            polyOutline->points.resize(n + (polyOpen ? 0 : 1));

            for (size_t i = 0; i < n; ++i) {
                polyOutline->points[i] = pts[i];
            }

            if (!polyOpen && n > 0) {
                polyOutline->points[n] = polyOutline->points[0];
            }

            if (n > 0) {
                polyFirst->center = polyOutline->points[0];
                moPolyFirst->center = polyOutline->points[0];
                polyRubber->begin = polyOutline->points[n - 1];
            }

            break;
        }

        case MaskComponentType::QUICK_SELECT: {
            brushCursor->radius = int(std::lround(std::max(quickSize->getValue() / 100.0 * diag * transformScale(), 1.0)));
            break;
        }

        case MaskComponentType::MAGIC_WAND: {
            const std::vector<rtengine::Coord> seeds = imagesFromNormalized(c->seeds, 5);

            for (size_t i = 0; i < seedMarkers.size() && i < seeds.size(); ++i) {
                seedMarkers[i]->center = seeds[i];
            }

            break;
        }

        case MaskComponentType::COLOR_RANGE: {
            if (hasPickPos) {
                pickMarker->center = lastPickPos;
            }

            break;
        }

        default:
            break;
    }
}

CursorShape Masking::getCursor(int objectID, int xPos, int yPos) const
{
    (void)xPos;
    (void)yPos;
    const MaskComponent* c = currentComponent();

    if (!c) {
        return CSHandOpen;
    }

    if (objectID == MO_HANDLE_A || objectID == MO_HANDLE_B) {
        return (c->type == MaskComponentType::RADIAL_GRADIENT || c->type == MaskComponentType::RECTANGLE) ? CSResizeDiagonal : CSMove2D;
    }

    if (objectID == MO_CENTER) {
        return CSMove2D;
    }

    switch (c->type) {
        case MaskComponentType::BRUSH:
            return eraseActive(lastModifier) ? CSEraser : CSBrush;

        case MaskComponentType::LINEAR_GRADIENT:
        case MaskComponentType::RADIAL_GRADIENT:
        case MaskComponentType::RECTANGLE:
            return CSCrosshair;

        case MaskComponentType::POLYGON:
            return CSLasso;

        case MaskComponentType::MAGIC_WAND:
            return CSWand;

        case MaskComponentType::QUICK_SELECT:
            return CSQuickSelect;

        case MaskComponentType::COLOR_RANGE:
        case MaskComponentType::AI_OBJECT:
            return CSAddColPicker;

        default:
            return CSHandOpen;
    }
}

bool Masking::mouseOver(int modifierKey)
{
    EditDataProvider* provider = getEditProvider();

    if (!provider) {
        return false;
    }

    bool redraw = false;

    if (modifierKey != lastModifier) {
        lastModifier = modifierKey;
        updateBrushCursorStyle(modifierKey);
        redraw = true;
    }

    if (provider->getObject() != lastObject) {
        for (Circle* handle : {gradientStart, gradientEnd, ellipseCenter, handleX, handleY, rectCornerA, rectCornerB, rectCenter, polyFirst}) {
            handle->state = Geometry::NORMAL;
        }

        const MaskComponent* c = currentComponent();

        if (c) {
            const bool radial = c->type == MaskComponentType::RADIAL_GRADIENT;
            const bool rect = c->type == MaskComponentType::RECTANGLE;

            switch (provider->getObject()) {
                case MO_HANDLE_A:
                    (rect ? rectCornerA : (radial ? handleX : gradientStart))->state = Geometry::PRELIGHT;
                    break;

                case MO_HANDLE_B:
                    (rect ? rectCornerB : (radial ? handleY : gradientEnd))->state = Geometry::PRELIGHT;
                    break;

                case MO_CENTER:
                    (rect ? rectCenter : ellipseCenter)->state = Geometry::PRELIGHT;
                    break;

                case MO_POLY_FIRST:
                    polyFirst->state = Geometry::PRELIGHT;
                    break;

                default:
                    break;
            }
        }

        lastObject = provider->getObject();
        redraw = true;
    }

    if (brushCursor->isVisible()) {
        brushCursor->center = provider->posImage;
        brushCore->center = provider->posImage;
        redraw = true;
    }

    if (polyRubber->isVisible()) {
        polyRubber->end = provider->posImage;
        redraw = true;
    }

    return redraw;
}

bool Masking::button1Pressed(int modifierKey)
{
    EditDataProvider* provider = getEditProvider();
    MaskComponent* c = currentComponent();

    if (!provider || !c || (modifierKey & GDK_CONTROL_MASK)) {
        return false;
    }

    int w, h;

    if (!imageSize(w, h)) {
        return false;
    }

    const rtengine::Coord pos = provider->posImage;
    const bool inside = pos.x >= 0 && pos.y >= 0 && pos.x < w && pos.y < h;

    switch (c->type) {
        case MaskComponentType::BRUSH: {
            if (!inside) {
                return false;
            }

            currentStroke = MaskBrushStroke();
            currentStroke.size = brushSize->getValue() / 100.0;
            currentStroke.feather = brushFeather->getValue() / 100.0;
            currentStroke.flow = brushOpacity->getValue() / 100.0;
            currentStroke.erase = eraseActive(modifierKey);
            double nx, ny;
            normalizedFromImage(pos, nx, ny);
            currentStroke.points = {nx, ny};
            lastStrokePoint = pos;
            dragMode = DragMode::BRUSH_STROKE;
            EditSubscriber::action = EditSubscriber::Action::DRAGGING;

            // The stroke is part of the component while it is drawn, so the
            // preview follows the cursor; the history entry is made on release.
            c->strokes.push_back(currentStroke);
            strokeInProgress = true;
            liveRefresh();
            return true;
        }

        case MaskComponentType::QUICK_SELECT: {
            if (!inside) {
                return false;
            }

            // A quick selection stroke is a set of seeds: drawn like a brush
            // stroke, grown by the engine; Alt / Subtract strokes mark what to take out.
            currentStroke = MaskBrushStroke();
            currentStroke.size = quickSize->getValue() / 100.0;
            currentStroke.feather = 0.0;
            currentStroke.flow = 1.0;
            currentStroke.erase = quickEraseActive(modifierKey);
            double nx, ny;
            normalizedFromImage(pos, nx, ny);
            currentStroke.points = {nx, ny};
            lastStrokePoint = pos;
            dragMode = DragMode::BRUSH_STROKE;
            EditSubscriber::action = EditSubscriber::Action::DRAGGING;
            c->strokes.push_back(currentStroke);
            strokeInProgress = true;
            liveRefresh();
            return true;
        }

        case MaskComponentType::LINEAR_GRADIENT: {
            if (lastObject == MO_HANDLE_A) {
                dragMode = DragMode::GRADIENT_START;
                gradientStart->state = Geometry::DRAGGED;
            } else if (lastObject == MO_HANDLE_B) {
                dragMode = DragMode::GRADIENT_END;
                gradientEnd->state = Geometry::DRAGGED;
            } else {
                if (!inside) {
                    return false;
                }

                // start a new gradient at the click position, drag defines the end
                normalizedFromImage(pos, c->x0, c->y0);
                c->x1 = c->x0;
                c->y1 = c->y0;
                dragMode = DragMode::GRADIENT_END;
                gradientEnd->state = Geometry::DRAGGED;
                updateGeometry();
            }

            EditSubscriber::action = EditSubscriber::Action::DRAGGING;
            return true;
        }

        case MaskComponentType::RADIAL_GRADIENT: {
            if (lastObject == MO_CENTER) {
                dragMode = DragMode::RADIAL_CENTER;
                ellipseCenter->state = Geometry::DRAGGED;
            } else if (lastObject == MO_HANDLE_A) {
                dragMode = DragMode::RADIAL_RX;
                handleX->state = Geometry::DRAGGED;
            } else if (lastObject == MO_HANDLE_B) {
                dragMode = DragMode::RADIAL_RY;
                handleY->state = Geometry::DRAGGED;
            } else {
                if (!inside) {
                    return false;
                }

                normalizedFromImage(pos, c->centerX, c->centerY);
                c->radiusX = c->radiusY = 0.01;
                dragMode = DragMode::RADIAL_NEW;
                updateGeometry();
            }

            EditSubscriber::action = EditSubscriber::Action::DRAGGING;
            return true;
        }

        case MaskComponentType::RECTANGLE: {
            if (lastObject == MO_HANDLE_A) {
                dragMode = DragMode::RECT_CORNER_A;
                rectCornerA->state = Geometry::DRAGGED;
            } else if (lastObject == MO_HANDLE_B) {
                dragMode = DragMode::RECT_CORNER_B;
                rectCornerB->state = Geometry::DRAGGED;
            } else if (lastObject == MO_CENTER) {
                dragMode = DragMode::RECT_CENTER;
                rectCenter->state = Geometry::DRAGGED;
            } else {
                if (!inside) {
                    return false;
                }

                applyModifierOp(modifierKey);
                // drawn in the displayed frame, at the angle shown by the slider
                viewFromImage(pos, rectAnchorX, rectAnchorY);
                MaskComponent v = toView(*c);
                v.x0 = v.x1 = rectAnchorX;
                v.y0 = v.y1 = rectAnchorY;
                v.angle = rectAngle->getValue();
                fromView(*c, v);
                dragMode = DragMode::RECT_NEW;
                updateGeometry();
            }

            EditSubscriber::action = EditSubscriber::Action::DRAGGING;
            return true;
        }

        case MaskComponentType::POLYGON: {
            if (!inside) {
                return false;
            }

            double nx, ny;
            normalizedFromImage(pos, nx, ny);

            if (polyPolygonal->get_active()) {
                // polygonal lasso: one vertex per click, closed on the first vertex
                if (polyOpen && lastObject == MO_POLY_FIRST && c->pointCount() >= 3) {
                    closePolygon();
                    return true;
                }

                if (!polyOpen) {
                    applyModifierOp(modifierKey);
                    c->points.clear();
                    polyOpen = true;
                }

                c->points.push_back(nx);
                c->points.push_back(ny);
                polyRubber->end = pos;
                setCanvasVisibility();
                updateGeometry();
                liveRefresh();
                return true;
            }

            // freehand lasso: drag draws a new outline
            applyModifierOp(modifierKey);
            c->points = {nx, ny};
            polyOpen = true;
            polyInProgress = true;
            lastStrokePoint = pos;
            dragMode = DragMode::LASSO_FREEHAND;
            EditSubscriber::action = EditSubscriber::Action::DRAGGING;
            setCanvasVisibility();
            updateGeometry();
            return true;
        }

        case MaskComponentType::MAGIC_WAND: {
            if (!inside) {
                return false;
            }

            if (modifierKey & GDK_MOD1_MASK) {
                // Alt-click removes the nearest seed
                if (!c->seeds.empty()) {
                    size_t best = 0;
                    double bestD = 1e30;

                    for (size_t i = 0; i < c->seedCount(); ++i) {
                        const rtengine::Coord s = imageFromNormalized(c->seeds[5 * i], c->seeds[5 * i + 1]);
                        const double d = double(s.x - pos.x) * (s.x - pos.x) + double(s.y - pos.y) * (s.y - pos.y);

                        if (d < bestD) {
                            bestD = d;
                            best = i;
                        }
                    }

                    c->seeds.erase(c->seeds.begin() + 5 * best, c->seeds.begin() + 5 * best + 5);
                    updateComponentRow(selectedComponent);
                    setCanvasVisibility();
                    updateGeometry();
                    notify(EvMaskingSeeds, Glib::ustring::compose(M("TP_MASKING_WAND_SEED_COUNT"), c->seedCount()));
                }

                return true;
            }

            addWandSeed(pos, !(modifierKey & GDK_SHIFT_MASK));
            dragMode = DragMode::WAND_DRAG;
            EditSubscriber::action = EditSubscriber::Action::DRAGGING;
            setCanvasVisibility();
            updateGeometry();
            liveRefresh();
            return true;
        }

        case MaskComponentType::COLOR_RANGE:
        case MaskComponentType::AI_OBJECT: {
            if (!inside) {
                return false;
            }

            pickAt(pos);
            return true;
        }

        default:
            return false;
    }
}

bool Masking::drag1(int modifierKey)
{
    lastModifier = modifierKey;
    EditDataProvider* provider = getEditProvider();
    MaskComponent* c = currentComponent();

    if (!provider || !c || dragMode == DragMode::NONE) {
        return false;
    }

    const rtengine::Coord pos = provider->posImage + provider->deltaImage;
    const double diag = diagonalPixels();

    switch (dragMode) {
        case DragMode::BRUSH_STROKE: {
            const double minDist = std::max(currentStroke.size * diag * 0.15, 2.0);
            const double dx = pos.x - lastStrokePoint.x;
            const double dy = pos.y - lastStrokePoint.y;

            brushCursor->center = pos;
            brushCore->center = pos;

            if (dx * dx + dy * dy < minDist * minDist) {
                return true;
            }

            double nx, ny;
            normalizedFromImage(pos, nx, ny);
            currentStroke.points.push_back(nx);
            currentStroke.points.push_back(ny);
            lastStrokePoint = pos;

            if (strokeInProgress && !c->strokes.empty()) {
                c->strokes.back() = currentStroke;
                liveRefresh();
            }

            return true;
        }

        case DragMode::GRADIENT_START:
            normalizedFromImage(pos, c->x0, c->y0);
            break;

        case DragMode::GRADIENT_END:
            normalizedFromImage(pos, c->x1, c->y1);
            break;

        case DragMode::RADIAL_CENTER:
            normalizedFromImage(pos, c->centerX, c->centerY);
            break;

        // Rectangles are edited in the displayed frame (toView / fromView):
        // their corners only make sense together with the angle.
        case DragMode::RECT_NEW: {
            double nx, ny;
            viewFromImage(pos, nx, ny);
            MaskComponent r = toView(*c);
            r.x0 = std::min(rectAnchorX, nx);
            r.x1 = std::max(rectAnchorX, nx);
            r.y0 = std::min(rectAnchorY, ny);
            r.y1 = std::max(rectAnchorY, ny);
            fromView(*c, r);
            break;
        }

        case DragMode::RECT_CORNER_A:
        case DragMode::RECT_CORNER_B: {
            // drag the corner in the rectangle's own (rotated) frame
            MaskComponent r = toView(*c);
            const rtengine::Coord a = imageFromView(r.x0, r.y0);
            const rtengine::Coord b = imageFromView(r.x1, r.y1);
            const double cx = 0.5 * (a.x + b.x);
            const double cy = 0.5 * (a.y + b.y);
            const double rad = r.angle * RT_PI / 180.0;
            const double dx = pos.x - cx;
            const double dy = pos.y - cy;
            const double u = cx + std::cos(rad) * dx + std::sin(rad) * dy;
            const double v = cy - std::sin(rad) * dx + std::cos(rad) * dy;
            double nx, ny;
            viewFromImage(rtengine::Coord(int(std::lround(u)), int(std::lround(v))), nx, ny);

            if (dragMode == DragMode::RECT_CORNER_A) {
                r.x0 = std::min(nx, r.x1 - 0.002);
                r.y0 = std::min(ny, r.y1 - 0.002);
            } else {
                r.x1 = std::max(nx, r.x0 + 0.002);
                r.y1 = std::max(ny, r.y0 + 0.002);
            }

            fromView(*c, r);
            break;
        }

        case DragMode::RECT_CENTER: {
            double nx, ny;
            viewFromImage(pos, nx, ny);
            MaskComponent r = toView(*c);
            const double hw = 0.5 * (r.x1 - r.x0);
            const double hh = 0.5 * (r.y1 - r.y0);
            r.x0 = nx - hw;
            r.x1 = nx + hw;
            r.y0 = ny - hh;
            r.y1 = ny + hh;
            fromView(*c, r);
            break;
        }

        case DragMode::LASSO_FREEHAND: {
            const double minDist = std::max(diag * 0.002, 2.0);
            const double dx = pos.x - lastStrokePoint.x;
            const double dy = pos.y - lastStrokePoint.y;

            if (dx * dx + dy * dy < minDist * minDist) {
                return false;
            }

            double nx, ny;
            normalizedFromImage(pos, nx, ny);
            c->points.push_back(nx);
            c->points.push_back(ny);
            lastStrokePoint = pos;
            setCanvasVisibility();
            updateGeometry();
            liveRefresh();
            return true;
        }

        case DragMode::WAND_DRAG: {
            const double minDist = std::max(diag * 0.02, 4.0);
            const double dx = pos.x - lastSeedPos.x;
            const double dy = pos.y - lastSeedPos.y;

            if (dx * dx + dy * dy < minDist * minDist || c->seedCount() >= size_t(SEED_MARKER_COUNT)) {
                return false;
            }

            addWandSeed(pos, false);
            setCanvasVisibility();
            liveRefresh();
            return true;
        }

        case DragMode::RADIAL_RX:
        case DragMode::RADIAL_RY:
        case DragMode::RADIAL_NEW: {
            // radii along the ellipse axes as displayed
            MaskComponent e = toView(*c);
            const rtengine::Coord center = imageFromView(e.centerX, e.centerY);
            const double dx = pos.x - center.x;
            const double dy = pos.y - center.y;
            const double rad = e.angle * RT_PI / 180.0;
            const double u = std::cos(rad) * dx + std::sin(rad) * dy;
            const double v = -std::sin(rad) * dx + std::cos(rad) * dy;

            if (dragMode == DragMode::RADIAL_RX) {
                e.radiusX = std::max(std::fabs(u) / diag, 0.002);
            } else if (dragMode == DragMode::RADIAL_RY) {
                e.radiusY = std::max(std::fabs(v) / diag, 0.002);
            } else {
                e.radiusX = std::max(std::fabs(u) / diag, 0.002);
                e.radiusY = std::max(std::fabs(v) / diag, 0.002);
            }

            fromView(*c, e);
            break;
        }

        default:
            return false;
    }

    updateGeometry();
    // live preview while dragging; the history entry is made on release
    shapeDragged = true;

    if (listener && !batchMode) {
        listener->refreshPreview(EvMaskingShape);
    }

    return true;
}

bool Masking::button1Released()
{
    const DragMode mode = dragMode;
    dragMode = DragMode::NONE;
    EditSubscriber::action = EditSubscriber::Action::NONE;

    for (Circle* handle : {gradientStart, gradientEnd, ellipseCenter, handleX, handleY, rectCornerA, rectCornerB, rectCenter}) {
        if (handle->state == Geometry::DRAGGED) {
            handle->state = Geometry::PRELIGHT;
        }
    }

    if (mode == DragMode::BRUSH_STROKE) {
        commitStroke();
        return true;
    }

    if (mode == DragMode::LASSO_FREEHAND) {
        closePolygon();
        return true;
    }

    if (mode == DragMode::WAND_DRAG) {
        MaskComponent* c = currentComponent();

        if (c) {
            updateComponentRow(selectedComponent);
            notify(EvMaskingSeeds, Glib::ustring::compose(M("TP_MASKING_WAND_SEED_COUNT"), c->seedCount()));
        }

        return true;
    }

    if (shapeDragged) {
        shapeDragged = false;
        notify(EvMaskingShape, M("TP_MASKING_SHAPE_CHANGED"));
    }

    return mode != DragMode::NONE;
}

void Masking::commitStroke()
{
    if (!strokeInProgress) {
        return;
    }

    strokeInProgress = false;
    MaskComponent* c = currentComponent();

    if (!c || (c->type != MaskComponentType::BRUSH && c->type != MaskComponentType::QUICK_SELECT) || c->strokes.empty()) {
        return;
    }

    if (currentStroke.points.size() < 2) {
        c->strokes.pop_back();
        liveRefresh();
        return;
    }

    c->strokes.back() = currentStroke;
    currentStroke = MaskBrushStroke();
    updateComponentRow(selectedComponent);
    notify(EvMaskingStroke, Glib::ustring::compose(M("TP_MASKING_STROKE_COUNT"), c->strokes.size()));
}

void Masking::pickAt(const rtengine::Coord& pos)
{
    MaskComponent* c = currentComponent();

    if (!c) {
        return;
    }

    if (c->type == MaskComponentType::AI_OBJECT) {
        normalizedFromImage(pos, c->aiHintX, c->aiHintY);
        updateGeometry();
        notify(EvMaskingComponent, M("TP_MASKING_HINT_SET"));
        return;
    }

    if (c->type == MaskComponentType::COLOR_RANGE) {
        float L, a, b;

        if (maskingProvider && maskingProvider->getMaskingLabSample(pos, 8, L, a, b)) {
            c->sampleL = L;
            c->sampleA = a;
            c->sampleB = b;
            lastPickPos = pos;
            hasPickPos = true;
            updateColorSampleLabel();
            setCanvasVisibility();
            updateGeometry();
            notify(EvMaskingSample, Glib::ustring::compose("L=%1 a=%2 b=%3", int(std::lround(L)), int(std::lround(a)), int(std::lround(b))));
        }
    }
}

// ---------------------------------------------------------------------------
// Scripted session: RT_MASK_SELFTEST=1 replays a typical interactive session
// on the GUI thread (AI tools, painting, erasing, object pick, undo) so the
// whole GUI path can run under a sanitizer without input automation.

void Masking::selfTestStep()
{
    auto& reg = rtengine::AISegmentationRegistry::getInstance();
    EditDataProvider* provider = getEditProvider();
    int w = 0, h = 0;

    if (reg.getPending() > 0 || !listener || !provider || !imageSize(w, h)) {
        return;   // model running or preview not ready: try again on the next tick
    }

    auto hover = [&](double x, double y) {
        provider->posImage.set(int(x * w), int(y * h));
        provider->deltaImage.set(0, 0);
        mouseOver(0);
    };
    auto stroke = [&](double x0, double y0, double dx, double dy, int n) {
        hover(x0, y0);
        provider->posImage.set(int(x0 * w), int(y0 * h));
        provider->deltaImage.set(0, 0);
        button1Pressed(0);

        for (int i = 1; i <= n; ++i) {
            provider->deltaImage.set(int(i * dx * w), int(i * dy * h));
            drag1(0);
        }

        button1Released();
        hover(x0 + n * dx, y0 + n * dy);
    };
    auto pick = [&](double x, double y) {
        hover(x, y);
        button1Pressed(0);
        button1Released();
        hover(x + 0.01, y + 0.01);
    };

    printf("[selftest] step %d (masks=%zu)\n", selfTestState, masks.size());
    fflush(stdout);

    const char* mode = std::getenv("RT_MASK_SELFTEST");

    if (mode && std::string(mode) == "slider") {
        ++selfTestState;
        // a brush mask, then the layer's Exposure slider moved step by step
        switch (selfTestState - 1) {
            case 0:
                intentButtons[INTENT_NEW]->set_active(true);
                onPaletteClicked(MaskComponentType::BRUSH);
                stroke(0.3, 0.3, 0.01, 0.004, 30);
                break;

            case 1: case 2: case 3: case 4: case 5: {
                const double v = 0.5 * (selfTestState - 1);
                exposure->setValue(v);
                adjusterChanged(exposure, v);
                break;
            }

            default:
                printf("[selftest] done\n");
                fflush(stdout);
                std::_Exit(0);
        }

        return;
    }

    switch (selfTestState++) {
        case 0:
            // what the user did before each crash: Object tool, click on the image, overlay on
            intentButtons[INTENT_NEW]->set_active(true);
            onPaletteClicked(MaskComponentType::AI_OBJECT);
            pick(0.5, 0.5);
            break;

        case 1:
            showMask->set_active(true);
            pick(0.3, 0.6);
            break;

        case 2: {
            // switch to the next image of the folder (Shift+F4), as the user did
            // between the two crashes, while the previous image's AI job may be alive
            std::function<EditorPanel*(Gtk::Widget*)> findEditor = [&](Gtk::Widget* w) -> EditorPanel* {
                if (EditorPanel* ep = dynamic_cast<EditorPanel*>(w)) {
                    return ep;
                }

                if (Gtk::Container* c = dynamic_cast<Gtk::Container*>(w)) {
                    for (Gtk::Widget* child : c->get_children()) {
                        if (EditorPanel* ep = findEditor(child)) {
                            return ep;
                        }
                    }
                }

                return nullptr;
            };
            EditorPanel* ep = nullptr;

            for (Gtk::Window* win : Gtk::Window::list_toplevels()) {
                if (!ep) {
                    ep = findEditor(win);
                }
            }

            if (ep) {
                ep->openNextEditorImage();
            } else {
                printf("[selftest] no EditorPanel found under the toplevel\n");
            }

            break;
        }

        case 3:
            // first action on the new image: Object pick with the overlay on
            intentButtons[INTENT_NEW]->set_active(true);
            onPaletteClicked(MaskComponentType::AI_OBJECT);
            pick(0.45, 0.55);
            break;

        case 4:
            onPaletteClicked(MaskComponentType::BRUSH);
            stroke(0.3, 0.3, 0.01, 0.004, 30);
            break;

        case 5:
            onEraserClicked();
            stroke(0.35, 0.32, 0.008, 0.002, 20);
            break;

        case 6:
            intentButtons[INTENT_NEW]->set_active(true);
            onPaletteClicked(MaskComponentType::AI_SUBJECT);
            break;

        case 7:
            intentButtons[INTENT_NEW]->set_active(true);
            onPaletteClicked(MaskComponentType::AI_SKY);
            break;

        case 8:
            onPaletteClicked(MaskComponentType::BRUSH);
            stroke(0.5, 0.2, 0.01, 0.01, 30);
            break;

        case 9:
            onPaletteClicked(MaskComponentType::QUICK_SELECT);
            stroke(0.6, 0.6, 0.005, 0.005, 10);
            break;

        case 10:
            onUndoToolPressed();
            break;

        case 11:
            showMask->set_active(false);
            onPaletteClicked(MaskComponentType::AI_BACKGROUND);
            break;

        case 12:
            onClearMaskPressed();
            break;

        default:
            printf("[selftest] done\n");
            fflush(stdout);
            std::_Exit(0);
    }
}
