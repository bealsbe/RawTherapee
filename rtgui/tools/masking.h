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
#pragma once

#include <memory>
#include <set>
#include <vector>

#include <gtkmm.h>

#include "colorprovider.h"
#include "curvelistener.h"
#include "editcallbacks.h"
#include "editwidgets.h"
#include "guiutils.h"
#include "toolpanel.h"
#include "widgets/basic/adjuster.h"

#include "rtengine/coord.h"
#include "rtengine/maskparams.h"
#include "rtengine/procevents.h"

namespace rtengine
{
class MaskTransform;
}

class CurveEditor;
class CurveEditorGroup;
class DiagonalCurveEditor;
class FlatCurveEditor;

/**
 * Gives the Masks panel access to pixel values of the preview, used by the
 * Color Range component's eyedropper. Implemented by ToolPanelCoordinator.
 */
class MaskingProvider
{
public:
    virtual ~MaskingProvider() = default;

    /// Samples the L*a*b* color (standard units) at 'pos' (full image coordinates); false if not available.
    virtual bool getMaskingLabSample(rtengine::Coord pos, int size, float& L, float& a, float& b) = 0;
};

/**
 * "Masks" tool panel: Lightroom-style localized adjustments.
 *
 * Mask list -> component list -> per component controls, on-canvas editing
 * (brush strokes, gradient handles, color / object picking) and the
 * adjustment sliders applied through the selected mask.
 */
class Masking final :
    public ToolParamBlock,
    public AdjusterListener,
    public FoldableToolPanel,
    public EditSubscriber,
    public CurveListener,
    public ColorProvider
{
public:
    static const Glib::ustring TOOL_NAME;

    Masking();
    ~Masking() override;

    void read(const rtengine::procparams::ProcParams* pp, const ParamsEdited* pedited = nullptr) override;
    void write(rtengine::procparams::ProcParams* pp, ParamsEdited* pedited = nullptr) override;
    void setDefaults(const rtengine::procparams::ProcParams* defParams, const ParamsEdited* pedited = nullptr) override;
    void setBatchMode(bool batchMode) override;
    void trimValues(rtengine::procparams::ProcParams* pp) override;

    void adjusterChanged(Adjuster* a, double newval) override;
    void enabledChanged() override;
    void curveChanged(CurveEditor* ce) override;
    void colorForValue(double valX, double valY, enum ColorCaller::ElemType elemType, int callerId, ColorCaller* caller) override;

    void setEditProvider(EditDataProvider* provider) override;
    void setMaskingProvider(MaskingProvider* provider);

    /// Called by the coordinator when the image geometry may have changed (photo loaded, rotation, ...).
    void updateGeometry();
    /// Coarse rotation / flips and geometric transform of the image: masks are
    /// stored relative to the untransformed image (see rtengine::MaskTransform).
    /// returns true if it changed
    bool setGeometry(const rtengine::procparams::ProcParams& pp);

    // EditSubscriber interface
    CursorShape getCursor(int objectID, int xPos, int yPos) const override;
    bool mouseOver(int modifierKey) override;
    bool button1Pressed(int modifierKey) override;
    bool button1Released() override;
    bool drag1(int modifierKey) override;
    void switchOffEditMode() override;

private:
    using MaskLayer = rtengine::procparams::MaskLayer;
    using MaskComponent = rtengine::procparams::MaskComponent;
    using MaskComponentType = rtengine::procparams::MaskComponentType;
    using MaskBooleanOp = rtengine::procparams::MaskBooleanOp;

    enum class DragMode {
        NONE,
        BRUSH_STROKE,
        GRADIENT_START,
        GRADIENT_END,
        RADIAL_CENTER,
        RADIAL_RX,
        RADIAL_RY,
        RADIAL_NEW,
        RECT_NEW,
        RECT_CORNER_A,
        RECT_CORNER_B,
        RECT_CENTER,
        LASSO_FREEHAND,
        WAND_DRAG
    };

    // mouse-over object ids
    enum {
        MO_IMAGE = 0,
        MO_HANDLE_A,
        MO_HANDLE_B,
        MO_CENTER,
        MO_POLY_FIRST,
        MO_COUNT
    };

    // ---- data ----
    std::vector<MaskLayer> masks;
    int selectedMask;
    int selectedComponent;
    bool listEdited;   ///< batch mode: the mask list has been modified

    MaskingProvider* maskingProvider;
    DragMode dragMode;
    int lastObject;
    int lastModifier;   ///< modifier keys seen on the last mouse move
    bool strokeInProgress;    ///< the last stroke of the current brush component is being drawn
    bool overlayAutoEnabled;  ///< "show mask" was switched on automatically for brushing
    bool shapeDragged;        ///< a handle drag changed the shape; history entry pending until release
    // Scripted session for sanitizer runs (RT_MASK_SELFTEST=1): see selfTestStep()
    int aiListenerId = 0;     ///< this panel's entry in AISegmentationRegistry's result listeners
    bool selfTestStarted = false;
    int selfTestState = 0;
    void selfTestStep();
    rtengine::procparams::MaskBrushStroke currentStroke;
    rtengine::Coord lastStrokePoint;
    rtengine::Coord lastPickPos;
    bool hasPickPos;
    bool polyOpen;              ///< a polygonal lasso is being built vertex by vertex
    bool polyInProgress;        ///< a freehand lasso is being dragged
    double rectAnchorX;         ///< press position when drawing a new rectangle, normalized in the displayed frame
    double rectAnchorY;
    rtengine::Coord lastSeedPos;

    // ---- events ----
    rtengine::ProcEvent EvMaskingEnabled;
    rtengine::ProcEvent EvMaskingList;
    rtengine::ProcEvent EvMaskingLayer;
    rtengine::ProcEvent EvMaskingComponentList;
    rtengine::ProcEvent EvMaskingComponent;
    rtengine::ProcEvent EvMaskingStroke;
    rtengine::ProcEvent EvMaskingShape;
    rtengine::ProcEvent EvMaskingSample;
    rtengine::ProcEvent EvMaskingShowMask;
    rtengine::ProcEvent EvMaskingAmount;
    rtengine::ProcEvent EvMaskingFeather;
    rtengine::ProcEvent EvMaskingExposure;
    rtengine::ProcEvent EvMaskingContrast;
    rtengine::ProcEvent EvMaskingHighlights;
    rtengine::ProcEvent EvMaskingMidtones;
    rtengine::ProcEvent EvMaskingShadows;
    rtengine::ProcEvent EvMaskingWhites;
    rtengine::ProcEvent EvMaskingBlacks;
    rtengine::ProcEvent EvMaskingTemperature;
    rtengine::ProcEvent EvMaskingTint;
    rtengine::ProcEvent EvMaskingHue;
    rtengine::ProcEvent EvMaskingSaturation;
    rtengine::ProcEvent EvMaskingClarity;
    rtengine::ProcEvent EvMaskingExpand;
    rtengine::ProcEvent EvMaskingSmooth;
    rtengine::ProcEvent EvMaskingPolygon;
    rtengine::ProcEvent EvMaskingSeeds;
    rtengine::ProcEvent EvMaskingCurveL;
    rtengine::ProcEvent EvMaskingCurveA;
    rtengine::ProcEvent EvMaskingCurveB;
    rtengine::ProcEvent EvMaskingCurveCC;
    rtengine::ProcEvent EvMaskingCurveCH;
    rtengine::ProcEvent EvMaskingCurveLH;
    rtengine::ProcEvent EvMaskingCurveHH;
    rtengine::ProcEvent EvMaskingCurveLC;
    rtengine::ProcEvent EvMaskingCurveCL;

    enum Intent {
        INTENT_NEW = 0,
        INTENT_ADD,
        INTENT_SUBTRACT,
        INTENT_INTERSECT,
        INTENT_COUNT
    };

    // ---- widgets: the single layer tree (masks with their tools nested) ----
    class TreeColumns : public Gtk::TreeModel::ColumnRecord
    {
    public:
        TreeColumns()
        {
            add(enabled);
            add(icon);
            add(label);
            add(mode);
            add(maskIdx);
            add(compIdx);
        }

        Gtk::TreeModelColumn<bool> enabled;
        Gtk::TreeModelColumn<Glib::ustring> icon;
        Gtk::TreeModelColumn<Glib::ustring> label;
        Gtk::TreeModelColumn<Glib::ustring> mode;
        Gtk::TreeModelColumn<int> maskIdx;
        Gtk::TreeModelColumn<int> compIdx;   ///< -1 for a mask row
    };

    TreeColumns treeColumns;
    Glib::RefPtr<Gtk::TreeStore> treeStore;
    Gtk::TreeView* tree;
    sigc::connection treeSelectionConn;
    Gtk::Button* maskRemove;
    Gtk::Button* maskDuplicate;
    Gtk::Button* maskUndoTool;
    Gtk::Button* maskClear;
    Gtk::Entry* maskName;
    Gtk::CheckButton* editedCheckBox;   ///< batch mode only
    sigc::connection maskNameConn;
    sigc::connection editedConn;

    // ---- widgets: intent bar and tool palette ----
    Gtk::ToggleButton* intentButtons[INTENT_COUNT];
    sigc::connection intentConn[INTENT_COUNT];
    Gtk::Label* intentLabel;
    Gtk::Grid* palette;
    std::vector<Gtk::Button*> paletteButtons;

    // ---- widgets: selected tool ----
    Gtk::Label* toolHeader;
    Gtk::ToggleButton* opButtons[3];   ///< Add / Subtract / Intersect of the selected component
    sigc::connection opConn[3];
    Gtk::CheckButton* componentInvert;
    sigc::connection componentInvertConn;

    // ---- widgets: on-canvas editing ----
    Gtk::Box* editBox;
    Gtk::ToggleButton* edit;
    Gtk::Label* editHint;
    sigc::connection editConn;

    // ---- widgets: per component type ----
    Gtk::Box* brushBox;
    Adjuster* brushSize;
    Adjuster* brushFeather;
    Adjuster* brushOpacity;
    Gtk::ToggleButton* brushPaint;
    Gtk::ToggleButton* brushErase;
    sigc::connection brushPaintConn;
    sigc::connection brushEraseConn;

    Gtk::Box* radialBox;
    Adjuster* radialFeather;
    Adjuster* radialAngle;

    Gtk::Box* rectBox;
    Adjuster* rectFeather;
    Adjuster* rectCorner;
    Adjuster* rectAngle;

    Gtk::Box* polyBox;
    Adjuster* polyFeather;
    Gtk::ToggleButton* polyFreehand;
    Gtk::ToggleButton* polyPolygonal;
    Gtk::Button* polyClose;
    sigc::connection polyFreehandConn;
    sigc::connection polyPolygonalConn;

    Gtk::Box* wandBox;
    Adjuster* wandTolerance;
    Adjuster* wandSmoothness;
    Gtk::CheckButton* wandContiguous;
    sigc::connection wandContiguousConn;

    Gtk::Box* quickBox;
    Adjuster* quickSize;
    Adjuster* quickTolerance;
    Adjuster* quickEdge;
    Adjuster* quickSmoothness;
    Gtk::ToggleButton* quickSubtract;

    Gtk::Box* colorBox;
    Adjuster* colorTolerance;
    Adjuster* colorSmoothness;
    Gtk::Label* colorSampleLabel;

    Gtk::Box* lumBox;
    Adjuster* lumLow;
    Adjuster* lumHigh;
    Adjuster* lumFeatherLow;
    Adjuster* lumFeatherHigh;

    Gtk::Box* aiBox;
    MyComboBoxText* aiProvider;
    Gtk::Entry* aiPrompt;
    Adjuster* aiThreshold;
    Gtk::CheckButton* aiRefine;
    sigc::connection aiRefineConn;
    Gtk::Label* aiHint;
    Gtk::Label* aiStatus;
    Gtk::Box* aiBusyBox;        ///< spinner + message shown at the top while an AI mask is computed
    Gtk::Spinner* aiSpinner;
    Gtk::Label* aiBusyLabel;
    sigc::connection aiPromptConn;
    IdleRegister idle_register;
    std::vector<std::string> aiProviderIds;
    std::set<std::string> aiProviderUnavailable;   ///< greyed out in the dropdown (gated weights, missing package, ...)
    sigc::connection aiProviderConn;

    // ---- widgets: mask display / layer ----
    Gtk::CheckButton* showMask;
    Gtk::CheckButton* showComponentOnly;
    sigc::connection showMaskConn;
    sigc::connection showComponentOnlyConn;
    Adjuster* amount;
    Adjuster* feather;
    Adjuster* expand;
    Adjuster* smooth;
    Gtk::CheckButton* layerInvert;
    sigc::connection layerInvertConn;

    // ---- widgets: adjustments ----
    Adjuster* exposure;
    Adjuster* contrast;
    Adjuster* highlights;
    Adjuster* midtones;
    Adjuster* shadows;
    Adjuster* whites;
    Adjuster* blacks;
    Adjuster* temperature;
    Adjuster* tint;
    Adjuster* hue;
    Adjuster* saturation;
    Adjuster* clarity;

    // ---- widgets: L*a*b* curves of the layer (like the L*a*b* Adjustments tool) ----
    MyExpander* curvesExpander;
    CurveEditorGroup* curveEditorG;
    DiagonalCurveEditor* lshape;
    DiagonalCurveEditor* ashape;
    DiagonalCurveEditor* bshape;
    DiagonalCurveEditor* ccshape;
    DiagonalCurveEditor* lcshape;
    DiagonalCurveEditor* clshape;
    FlatCurveEditor* chshape;
    FlatCurveEditor* lhshape;
    FlatCurveEditor* hhshape;

    // ---- geometry (owned) ----
    EditRectangle* imgRect;
    Circle* brushCursor;   ///< outer edge of the brush
    Circle* brushCore;     ///< fully selected core (inside the feather zone)
    Line* gradientLine;
    Circle* gradientStart;
    Circle* gradientEnd;
    Polyline* ellipseOutline;
    Circle* ellipseCenter;
    Circle* handleX;
    Circle* handleY;
    Circle* pickMarker;
    Polyline* rectOutline;
    Circle* rectCornerA;
    Circle* rectCornerB;
    Circle* rectCenter;
    Polyline* polyOutline;
    Line* polyRubber;
    Circle* polyFirst;
    std::vector<Circle*> seedMarkers;
    Circle* moHandleA;
    Circle* moHandleB;
    Circle* moCenter;
    Circle* moPolyFirst;

    // ---- helpers ----
    MaskLayer* currentLayer();
    const MaskLayer* currentLayer() const;
    MaskComponent* currentComponent();
    const MaskComponent* currentComponent() const;

    void populateTree();                 ///< rebuild the whole tree from 'masks' and select the current row
    void fillRow(const Gtk::TreeModel::Row& row, int maskIdx, int compIdx);
    Gtk::TreeModel::iterator findRow(int maskIdx, int compIdx);
    void populateMaskList();             ///< = populateTree()
    void updateMaskRow(int idx);
    void populateComponentList();        ///< = populateTree()
    void updateComponentRow(int idx);
    void showLayer();         ///< widgets <- data (selected layer)
    void layerGet();          ///< data <- widgets (selected layer)
    void showComponent();     ///< widgets <- data (selected component)
    void componentGet();      ///< data <- widgets (selected component)
    void updateTypeBoxes();
    void updateColorSampleLabel();
    Glib::ustring componentTypeName(MaskComponentType t) const;
    Glib::ustring componentShortName(MaskComponentType t) const;
    Glib::ustring componentIconName(MaskComponentType t) const;
    Gtk::Button* makeToolButton(MaskComponentType t);
    void setOpToggles(MaskBooleanOp op);
    MaskBooleanOp getOpToggles() const;
    void updateIntent();
    void createMaskWith(MaskComponentType t);
    void addComponent(MaskComponentType t, MaskBooleanOp op);
    void activateToolOnCanvas();   ///< switch on-canvas editing on for the selected tool (palette click)
    Glib::ustring componentSummary(const MaskComponent& c) const;
    Glib::ustring layerSummary(const MaskLayer& l) const;
    void notify(const rtengine::ProcEvent& ev, const Glib::ustring& descr);

    void onTreeSelectionChanged();
    void onUndoToolPressed();
    void onClearMaskPressed();
    void onEraserClicked();
    void onTreeEnabledToggled(const Glib::ustring& path);
    void onRemovePressed();
    void onIntentToggled(int which);
    void onPaletteClicked(MaskComponentType t);
    void onOpToggled(int which);
    void onMaskDuplicatePressed();
    void onMaskNameChanged();
    void onEditedToggled();
    void onComponentInvertToggled();
    bool ensureBrushComponent();   ///< select (or append) the brush component that edits the current mask
    void onPolyModeToggled(bool polygonal);
    void onPolyClosePressed();
    void onWandContiguousToggled();
    void closePolygon();
    void addWandSeed(const rtengine::Coord& pos, bool replace);
    void applyModifierOp(int modifierKey);
    bool typeUsesToggles(MaskComponentType t) const;
    void onBrushModeToggled(bool erase);
    bool eraseActive(int modifierKey) const;
    bool quickEraseActive(int modifierKey) const;   ///< quick selection: Subtract toggle or Alt
    void updateBrushCursorStyle(int modifierKey);
    void onAiProviderChanged();
    void onAiPromptChanged();
    void onAiRefineToggled();
    bool onAiPromptFocusOut(GdkEventFocus* event);
    void onSegmentationResult(bool finished);
    void updateAiStatus();
    void onShowMaskToggled();
    void onLayerInvertToggled();
    void editToggled();
    void releaseEdit();
    void setCanvasActive(bool active);   ///< subscribe / unsubscribe to on-canvas editing
    void deactivateCanvasControls();     ///< widgets -> "off" state and overlay restore
    void syncCanvasControls();           ///< widgets <- subscription state
    void liveRefresh();                  ///< reprocess the preview without a history entry

    bool imageSize(int& w, int& h) const;
    /// nullptr when the stored and the displayed frames are the same
    const rtengine::MaskTransform* maskTransform() const;
    /// displayed image position <-> stored (untransformed) normalized position
    void normalizedFromImage(const rtengine::Coord& p, double& nx, double& ny) const;
    rtengine::Coord imageFromNormalized(double nx, double ny) const;
    /// batch version for point lists ('stride' values per point, x and y first)
    std::vector<rtengine::Coord> imagesFromNormalized(const std::vector<double>& values, size_t stride) const;
    /// displayed image position <-> normalized position in the displayed frame
    void viewFromImage(const rtengine::Coord& p, double& nx, double& ny) const;
    rtengine::Coord imageFromView(double nx, double ny) const;
    /// a component expressed in the displayed frame, and back (radial / rectangle editing)
    MaskComponent toView(const MaskComponent& c) const;
    void fromView(MaskComponent& c, const MaskComponent& view) const;
    /// magnification of the stored frame on screen (rotation auto-fill, scale)
    double transformScale() const;
    double diagonalPixels() const;
    void setCanvasVisibility();
    void commitStroke();
    void pickAt(const rtengine::Coord& pos);

    std::unique_ptr<rtengine::procparams::ProcParams> geometryParams;
    mutable std::unique_ptr<rtengine::MaskTransform> transform;
    mutable int transformW = 0;
    mutable int transformH = 0;
};
