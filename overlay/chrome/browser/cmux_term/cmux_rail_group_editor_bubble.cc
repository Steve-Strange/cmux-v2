// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Workspace adaptation of Chromium 150's TabGroupEditorBubbleView and
// ColorPickerView. Helium's footer suppression is retained. Exact revisions
// and contributor attribution are recorded in THIRD_PARTY_NOTICES.md.

#include "chrome/browser/cmux_term/cmux_rail_group_editor_bubble.h"

#include <array>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/strings/utf_string_conversions.h"
#include "cc/paint/paint_flags.h"
#include "chrome/app/vector_icons/vector_icons.h"
#include "chrome/browser/cmux_term/cmux_rail.h"
#include "chrome/browser/ui/color/chrome_color_id.h"
#if __has_include("chrome/browser/ui/color/tab_group_color_ids.h")
#include "chrome/browser/ui/color/tab_group_color_ids.h"
#endif
#include "chrome/common/chrome_version.h"
#include "third_party/skia/include/core/SkPath.h"
#include "ui/accessibility/ax_enums.mojom.h"
#include "ui/base/accelerators/accelerator.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/models/image_model.h"
#include "ui/base/mojom/dialog_button.mojom.h"
#include "ui/base/mojom/ui_base_types.mojom-shared.h"
#include "ui/base/pointer/touch_ui_controller.h"
#include "ui/base/ui_base_features.h"
#include "ui/color/color_id.h"
#include "ui/color/color_provider.h"
#include "ui/events/event.h"
#include "ui/events/event_constants.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/favicon_size.h"
#include "ui/gfx/geometry/insets.h"
#include "ui/gfx/geometry/insets_f.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/animation/ink_drop.h"
#include "ui/views/border.h"
#include "ui/views/bubble/bubble_dialog_delegate_view.h"
#include "ui/views/bubble/bubble_frame_view.h"
#include "ui/views/controls/button/button.h"
#include "ui/views/controls/button/button_controller.h"
#include "ui/views/controls/button/button_controller_delegate.h"
#include "ui/views/controls/button/label_button.h"
#include "ui/views/controls/highlight_path_generator.h"
#include "ui/views/controls/label.h"
#include "ui/views/focus/focus_manager.h"
#include "ui/views/controls/separator.h"
#include "ui/views/controls/textfield/textfield.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/layout/flex_layout.h"
#include "ui/views/layout/flex_layout_types.h"
#include "ui/views/layout/layout_provider.h"
#include "ui/views/style/typography.h"
#include "ui/views/view_class_properties.h"
#include "ui/views/widget/widget.h"

namespace cmux {

namespace {

// These are copied from Chromium 150's TabGroupEditorBubbleView.
constexpr int kDialogWidth = 240;
constexpr int kDefaultIconSize = 20;

int GetHorizontalSpacing() {
  return views::LayoutProvider::Get()->GetDistanceMetric(
      views::DISTANCE_RELATED_CONTROL_HORIZONTAL);
}

int GetVerticalSpacing() {
  return views::LayoutProvider::Get()->GetDistanceMetric(
      views::DISTANCE_RELATED_CONTROL_VERTICAL);
}

gfx::Insets GetControlInsets() {
  const int horizontal_spacing = GetHorizontalSpacing();
  const int vertical_spacing = GetVerticalSpacing();
  return ui::TouchUiController::Get()->touch_ui()
             ? gfx::Insets::VH(5 * vertical_spacing / 4, horizontal_spacing)
             : gfx::Insets::VH(vertical_spacing, horizontal_spacing);
}

ui::ColorId GroupDialogColorId(GroupColor color) {
  switch (color) {
    case GroupColor::kGrey:
      return kColorTabGroupDialogGrey;
    case GroupColor::kBlue:
      return kColorTabGroupDialogBlue;
    case GroupColor::kRed:
      return kColorTabGroupDialogRed;
    case GroupColor::kYellow:
      return kColorTabGroupDialogYellow;
    case GroupColor::kGreen:
      return kColorTabGroupDialogGreen;
    case GroupColor::kPink:
      return kColorTabGroupDialogPink;
    case GroupColor::kPurple:
      return kColorTabGroupDialogPurple;
    case GroupColor::kCyan:
      return kColorTabGroupDialogCyan;
    case GroupColor::kOrange:
      return kColorTabGroupDialogOrange;
  }
  return kColorTabGroupDialogGrey;
}

const gfx::VectorIcon& MenuIcon(WorkspaceGroupContextAction action) {
  switch (action) {
    case WorkspaceGroupContextAction::kNewWorkspaceInGroup:
#if CHROME_VERSION_MAJOR >= 151
      return features::IsRoundedIconsEnabled()
                 ? kLibraryAddIcon
                 : kNewTabInGroupRefreshOldIcon;
#else
      return kNewTabInGroupRefreshIcon;
#endif
    case WorkspaceGroupContextAction::kMoveGroupToNewWindow:
#if CHROME_VERSION_MAJOR >= 151
      return features::IsRoundedIconsEnabled()
                 ? kMoveGroupIcon
                 : kMoveGroupToNewWindowRefreshOldIcon;
#else
      return kMoveGroupToNewWindowRefreshIcon;
#endif
    case WorkspaceGroupContextAction::kUngroup:
#if CHROME_VERSION_MAJOR >= 151
      return features::IsRoundedIconsEnabled() ? kUngroupIcon
                                                : kUngroupRefreshOldIcon;
#else
      return kUngroupRefreshIcon;
#endif
    case WorkspaceGroupContextAction::kCloseGroup:
#if CHROME_VERSION_MAJOR >= 151
      return features::IsRoundedIconsEnabled() ? kTabCloseIcon
                                                : kCloseGroupRefreshOldIcon;
#else
      return kCloseGroupRefreshIcon;
#endif
  }
#if CHROME_VERSION_MAJOR >= 151
  return features::IsRoundedIconsEnabled()
             ? kLibraryAddIcon
             : kNewTabInGroupRefreshOldIcon;
#else
  return kNewTabInGroupRefreshIcon;
#endif
}

std::optional<ui::Accelerator> MenuAccelerator(
    WorkspaceGroupContextAction action) {
  // Pinned Chromium 150 accelerator_table.cc mappings. cmux registers these
  // on the workspace editor itself because workspace actions intentionally do
  // not reuse Chromium's tab command IDs.
  switch (action) {
    case WorkspaceGroupContextAction::kNewWorkspaceInGroup:
      return ui::Accelerator(ui::VKEY_C,
                             ui::EF_SHIFT_DOWN | ui::EF_ALT_DOWN);
    case WorkspaceGroupContextAction::kCloseGroup:
      return ui::Accelerator(ui::VKEY_W,
                             ui::EF_SHIFT_DOWN | ui::EF_ALT_DOWN);
    case WorkspaceGroupContextAction::kMoveGroupToNewWindow:
    case WorkspaceGroupContextAction::kUngroup:
      return std::nullopt;
  }
  return std::nullopt;
}

// Local, //ui-only copies of the Chromium 150 HoverButton interaction core,
// HoverButtonController, and BubbleMenuItemButton. This preserves the exact
// release-trigger, keyboard, right-click, focus, and ink-drop behavior without
// making cmux_chrome_ui depend on //chrome/browser/ui.
class WorkspaceHoverButton;

// ButtonController is an event controller, not a views::View subclass and
// therefore intentionally has no Views class metadata.
using WorkspaceButtonControllerBase = views::ButtonController;
class WorkspaceHoverButtonController : public WorkspaceButtonControllerBase {
 public:
  WorkspaceHoverButtonController(
      WorkspaceHoverButton* button,
      std::unique_ptr<views::ButtonControllerDelegate> delegate);
  ~WorkspaceHoverButtonController() override;

  bool OnMousePressed(const ui::MouseEvent& event) override;
  void OnMouseReleased(const ui::MouseEvent& event) override;
  bool OnKeyPressed(const ui::KeyEvent& event) override;
  void OnGestureEvent(ui::GestureEvent* event) override;

 private:
  views::Button::PressedCallback& callback();
};

class WorkspaceHoverButton : public views::LabelButton {
  METADATA_HEADER(WorkspaceHoverButton, views::LabelButton)

 public:
  WorkspaceHoverButton()
      : views::LabelButton(base::BindRepeating(
            &WorkspaceHoverButton::OnPressed, base::Unretained(this))) {
    SetButtonController(std::make_unique<WorkspaceHoverButtonController>(
        this,
        std::make_unique<views::Button::DefaultButtonControllerDelegate>(
            this)));

    views::InstallRectHighlightPathGenerator(this);
    SetInstallFocusRingOnFocus(false);
    SetFocusBehavior(FocusBehavior::ALWAYS);

    const int horizontal_spacing =
        views::LayoutProvider::Get()->GetDistanceMetric(
            views::DISTANCE_BUTTON_HORIZONTAL_PADDING);
    const int vertical_spacing =
        views::LayoutProvider::Get()->GetDistanceMetric(
            views::DISTANCE_CONTROL_LIST_VERTICAL) /
        2;
    SetBorder(views::CreateEmptyBorder(
        gfx::Insets::VH(vertical_spacing, horizontal_spacing)));

    views::InkDrop::Get(this)->SetMode(
        views::InkDropHost::InkDropMode::ON);
    views::InkDrop::UseInkDropForFloodFillRipple(
        views::InkDrop::Get(this), /*highlight_on_hover=*/false,
        /*highlight_on_focus=*/true);
    views::InkDrop::Get(this)->SetBaseColor(
        kColorHoverButtonBackgroundHovered);
    views::InkDrop::Get(this)->SetVisibleOpacity(1.0f);
    views::InkDrop::Get(this)->SetHighlightOpacity(1.0f);

    SetTriggerableEventFlags(ui::EF_LEFT_MOUSE_BUTTON |
                             ui::EF_RIGHT_MOUSE_BUTTON);
    button_controller()->set_notify_action(
        views::ButtonController::NotifyAction::kOnRelease);
  }

  WorkspaceHoverButton(PressedCallback callback,
                       const ui::ImageModel& icon,
                       const std::u16string& text)
      : WorkspaceHoverButton() {
    SetCallback(std::move(callback));
    SetText(text);
    SetImageModel(STATE_NORMAL, icon);
  }

  void SetCallback(PressedCallback callback) override {
    callback_ = std::move(callback);
  }

 protected:
  void StateChanged(ButtonState old_state) override {
    views::LabelButton::StateChanged(old_state);
    if (GetState() == STATE_HOVERED && old_state != STATE_PRESSED) {
      RequestFocus();
    } else if (GetState() == STATE_NORMAL && HasFocus()) {
      GetFocusManager()->SetFocusedView(nullptr);
    }
  }

 private:
  friend class WorkspaceHoverButtonController;

  void OnPressed(const ui::Event& event) {
    if (callback_) {
      callback_.Run(event);
    }
  }

  PressedCallback callback_;
};

BEGIN_METADATA(WorkspaceHoverButton)
END_METADATA

WorkspaceHoverButtonController::WorkspaceHoverButtonController(
    WorkspaceHoverButton* button,
    std::unique_ptr<views::ButtonControllerDelegate> delegate)
    : WorkspaceButtonControllerBase(button, std::move(delegate)) {
  set_notify_action(views::ButtonController::NotifyAction::kOnRelease);
}

WorkspaceHoverButtonController::~WorkspaceHoverButtonController() = default;

bool WorkspaceHoverButtonController::OnKeyPressed(
    const ui::KeyEvent& event) {
  const bool pressed =
      callback() && (event.key_code() == ui::VKEY_SPACE ||
                     event.key_code() == ui::VKEY_RETURN);
  if (pressed) {
    delegate()->NotifyClick(event);
  }
  return pressed;
}

bool WorkspaceHoverButtonController::OnMousePressed(
    const ui::MouseEvent& event) {
  DCHECK(notify_action() ==
         views::ButtonController::NotifyAction::kOnRelease);
  if (button()->GetRequestFocusOnPress()) {
    button()->RequestFocus();
  }
  views::InkDrop::Get(button())->AnimateToState(
      callback() ? views::InkDropState::ACTION_PENDING
                 : views::InkDropState::HIDDEN,
      ui::LocatedEvent::FromIfValid(&event));
  return true;
}

void WorkspaceHoverButtonController::OnMouseReleased(
    const ui::MouseEvent& event) {
  DCHECK(notify_action() ==
         views::ButtonController::NotifyAction::kOnRelease);
  views::InkDrop::Get(button())->AnimateToState(
      views::InkDropState::HIDDEN, &event);
  if (button()->GetState() != views::Button::STATE_DISABLED &&
      delegate()->IsTriggerableEvent(event) &&
      button()->HitTestPoint(event.location()) && !delegate()->InDrag()) {
    if (callback()) {
      delegate()->NotifyClick(event);
    }
  } else {
    views::ButtonController::OnMouseReleased(event);
  }
}

void WorkspaceHoverButtonController::OnGestureEvent(
    ui::GestureEvent* event) {
  if (event->type() == ui::EventType::kGestureTap) {
    button()->SetState(views::Button::STATE_NORMAL);
    if (callback()) {
      delegate()->NotifyClick(*event);
    }
  } else {
    views::ButtonController::OnGestureEvent(event);
  }
}

views::Button::PressedCallback&
WorkspaceHoverButtonController::callback() {
  return static_cast<WorkspaceHoverButton*>(button())->callback_;
}

class WorkspaceBubbleMenuItemButton : public WorkspaceHoverButton {
  METADATA_HEADER(WorkspaceBubbleMenuItemButton, WorkspaceHoverButton)

 public:
  WorkspaceBubbleMenuItemButton(PressedCallback callback,
                                const ui::ImageModel& icon,
                                const std::u16string& text)
      : WorkspaceHoverButton(std::move(callback), icon, text) {}

  void StateChanged(ButtonState old_state) override {
    // Explicitly bypass WorkspaceHoverButton::StateChanged so hovering a menu
    // item does not steal focus from the title textfield.
    views::LabelButton::StateChanged(old_state);
  }
};

BEGIN_METADATA(WorkspaceBubbleMenuItemButton)
END_METADATA

std::unique_ptr<views::LabelButton> CreateBubbleMenuItem(
    int button_id,
    const std::u16string& name,
    views::Button::PressedCallback callback,
    const ui::ImageModel& icon,
    const std::u16string& accelerator_text) {
  auto button = std::make_unique<WorkspaceBubbleMenuItemButton>(
      std::move(callback), icon, name);
  button->SetInstallFocusRingOnFocus(false);
  views::InkDrop::Get(button.get())
      ->SetMode(views::InkDropHost::InkDropMode::ON);
  views::InkDrop::Get(button.get())
      ->GetInkDrop()
      ->SetShowHighlightOnHover(true);
  views::InkDrop::Get(button.get())
      ->GetInkDrop()
      ->SetHoverHighlightFadeDuration(base::TimeDelta());
  views::InstallRectHighlightPathGenerator(button.get());
  button->SetID(button_id);
  button->SetFocusBehavior(views::View::FocusBehavior::ALWAYS);
  button->SetBorder(views::CreateEmptyBorder(gfx::Insets(12)));

  if (!accelerator_text.empty()) {
    views::BoxLayout* layout =
        button->SetLayoutManager(std::make_unique<views::BoxLayout>(
            views::BoxLayout::Orientation::kHorizontal, gfx::Insets(),
            views::LayoutProvider::Get()->GetDistanceMetric(
                views::DISTANCE_RELATED_LABEL_HORIZONTAL)));
    layout->set_cross_axis_alignment(
        views::BoxLayout::CrossAxisAlignment::kCenter);
    button->AddChildView(std::make_unique<views::View>());
    layout->SetFlexForView(button->children().back(), 1);

    auto accelerator_label = std::make_unique<views::Label>(accelerator_text);
    accelerator_label->SetEnabledColor(ui::kColorLabelForegroundSecondary);
    accelerator_label->SetTextStyle(views::style::STYLE_BODY_4);
    accelerator_label->SetHorizontalAlignment(gfx::ALIGN_RIGHT);
    button->AddChildView(std::move(accelerator_label));
    button->SetAccessibleName(name + u" " + accelerator_text);
  }

  return button;
}

class ColorPickerHighlightPathGenerator
    : public views::HighlightPathGenerator {
 public:
  SkPath GetHighlightPath(const views::View* view) override {
    gfx::RectF bounds(view->GetContentsBounds());
    bounds.Inset(-2.0f);
    const gfx::PointF center = bounds.CenterPoint();
    return SkPath::Circle(center.x(), center.y(), bounds.width() / 2.0f);
  }
};

class WorkspaceGroupColorButton : public views::Button {
  METADATA_HEADER(WorkspaceGroupColorButton, views::Button)

 public:
  using SelectedCallback =
      base::RepeatingCallback<void(WorkspaceGroupColorButton*)>;

  WorkspaceGroupColorButton(SelectedCallback selected_callback,
                            views::View* bubble_contents,
                            GroupColor color,
                            std::u16string color_name)
      : views::Button(base::BindRepeating(
            &WorkspaceGroupColorButton::ButtonPressed,
            base::Unretained(this))),
        selected_callback_(std::move(selected_callback)),
        bubble_contents_(bubble_contents),
        color_(color),
        color_name_(std::move(color_name)) {
    GetViewAccessibility().SetName(color_name_);
    GetViewAccessibility().SetRole(ax::mojom::Role::kRadioButton);
    SetInstallFocusRingOnFocus(true);
    views::HighlightPathGenerator::Install(
        this, std::make_unique<ColorPickerHighlightPathGenerator>());

    const int padding =
        views::LayoutProvider::Get()->GetDistanceMetric(
            views::DISTANCE_RELATED_BUTTON_HORIZONTAL) /
        2;
    SetBorder(views::CreateEmptyBorder(
        ui::TouchUiController::Get()->touch_ui() ? gfx::Insets(padding * 2)
                                                 : gfx::Insets(padding)));
    views::InkDrop::Get(this)->SetMode(views::InkDropHost::InkDropMode::OFF);
    SetAnimateOnStateChange(true);
    UpdateAccessibleCheckedState();
    SetTooltipText(color_name_);
  }

  GroupColor color() const { return color_; }
  bool selected() const { return selected_; }

  void SetSelected(bool selected) {
    if (selected_ == selected) {
      return;
    }
    selected_ = selected;
    UpdateAccessibleCheckedState();
    SchedulePaint();
  }

  void UpdateAccessibleCheckedState() override {
    GetViewAccessibility().SetCheckedState(
        selected_ ? ax::mojom::CheckedState::kTrue
                  : ax::mojom::CheckedState::kFalse);
  }

  bool IsGroupFocusTraversable() const override { return false; }

  views::View* GetSelectedViewForGroup(int group) override {
    return parent() ? parent()->GetSelectedViewForGroup(group) : nullptr;
  }

  gfx::Size CalculatePreferredSize(
      const views::SizeBounds& available_size) const override {
    const int circle_size = ui::TouchUiController::Get()->touch_ui()
                                ? 3 * gfx::kFaviconSize / 2
                                : gfx::kFaviconSize;
    gfx::Size size(circle_size, circle_size);
    size.Enlarge(GetInsets().width(), GetInsets().height());
    return size;
  }

  void PaintButtonContents(gfx::Canvas* canvas) override {
    gfx::RectF bounds(GetContentsBounds());
    cc::PaintFlags flags;
    flags.setStyle(cc::PaintFlags::kFill_Style);
    flags.setColor(GetColorProvider()->GetColor(GroupDialogColorId(color_)));
    flags.setAntiAlias(true);
    canvas->DrawCircle(bounds.CenterPoint(), bounds.width() / 2.0f, flags);

    if (!selected_) {
      return;
    }
    constexpr float kInset = 3.0f;
    constexpr float kThickness = 2.0f;
    flags.setStyle(cc::PaintFlags::kStroke_Style);
    flags.setStrokeWidth(kThickness);
    flags.setColor(bubble_contents_->GetColorProvider()->GetColor(
        ui::kColorBubbleBackground));
    bounds.Inset(gfx::InsetsF(kInset));
    canvas->DrawCircle(bounds.CenterPoint(), bounds.width() / 2.0f, flags);
  }

 private:
  void ButtonPressed() {
    if (selected_) {
      return;
    }
    SetSelected(true);
    selected_callback_.Run(this);
  }

  const SelectedCallback selected_callback_;
  const raw_ptr<views::View> bubble_contents_ = nullptr;
  const GroupColor color_;
  const std::u16string color_name_;
  bool selected_ = false;
};

BEGIN_METADATA(WorkspaceGroupColorButton)
END_METADATA

class WorkspaceGroupColorPicker : public views::View {
  METADATA_HEADER(WorkspaceGroupColorPicker, views::View)

 public:
  using ColorSelectedCallback = base::RepeatingCallback<void(GroupColor)>;

  WorkspaceGroupColorPicker(views::View* bubble_contents,
                            GroupColor initial_color,
                            ColorSelectedCallback callback)
      : callback_(std::move(callback)) {
    // Helium enables Chromium's tab-group color refresh, whose exact ordering
    // and refreshed Yellow/Pink labels are retained here.
    static constexpr std::array<std::pair<GroupColor, std::u16string_view>, 9>
        kColors = {{{GroupColor::kBlue, u"Blue"},
                    {GroupColor::kPurple, u"Purple"},
                    {GroupColor::kPink, u"Magenta"},
                    {GroupColor::kRed, u"Red"},
                    {GroupColor::kOrange, u"Orange"},
                    {GroupColor::kYellow, u"Lime"},
                    {GroupColor::kGreen, u"Green"},
                    {GroupColor::kCyan, u"Cyan"},
                    {GroupColor::kGrey, u"Grey"}}};

    for (const auto& [color, name] : kColors) {
      WorkspaceGroupColorButton* element =
          AddChildView(std::make_unique<WorkspaceGroupColorButton>(
              base::BindRepeating(&WorkspaceGroupColorPicker::OnColorSelected,
                                  base::Unretained(this)),
              bubble_contents, color, std::u16string(name)));
      element->SetGroup(0);
      element->SetSelected(color == initial_color);
      elements_.push_back(element);
    }

    const gfx::Insets child_insets = elements_.front()->GetInsets();
    SetProperty(views::kInternalPaddingKey,
                gfx::Insets::TLBR(0, child_insets.left(), 0,
                                  child_insets.right()));
    SetFocusBehavior(FocusBehavior::NEVER);
    SetLayoutManager(std::make_unique<views::FlexLayout>())
        ->SetOrientation(views::LayoutOrientation::kHorizontal)
        .SetDefault(views::kFlexBehaviorKey,
                    views::FlexSpecification().WithAlignment(
                        views::LayoutAlignment::kCenter));
  }

  views::View* GetSelectedViewForGroup(int group) override {
    for (WorkspaceGroupColorButton* element : elements_) {
      if (element->selected()) {
        return element;
      }
    }
    return nullptr;
  }

 private:
  void OnColorSelected(WorkspaceGroupColorButton* selected) {
    for (WorkspaceGroupColorButton* element : elements_) {
      if (element != selected) {
        element->SetSelected(false);
      }
    }
    callback_.Run(selected->color());
  }

  const ColorSelectedCallback callback_;
  std::vector<raw_ptr<WorkspaceGroupColorButton>> elements_;
};

BEGIN_METADATA(WorkspaceGroupColorPicker)
END_METADATA

}  // namespace

// static
base::WeakPtr<CmuxRailGroupEditorBubble> CmuxRailGroupEditorBubble::Show(
    RailDelegate* delegate,
    WorkspaceGroupId group,
    const std::string& title,
    GroupColor color,
    views::View* anchor_view,
    base::OnceClosure closed_callback) {
  if (!delegate || group == kInvalidId || !anchor_view ||
      !anchor_view->GetWidget()) {
    return {};
  }

  auto bubble_delegate = std::make_unique<views::BubbleDialogDelegate>(
      anchor_view, views::BubbleBorder::Arrow::TOP_LEFT,
      views::BubbleBorder::DIALOG_SHADOW, /*autosize=*/true);
  auto contents = std::unique_ptr<CmuxRailGroupEditorBubble>(
      new CmuxRailGroupEditorBubble(delegate, group, title, color,
                                    std::move(closed_callback)));
  base::WeakPtr<CmuxRailGroupEditorBubble> weak_contents =
      contents->weak_factory_.GetWeakPtr();
  views::Textfield* title_field = contents->title_field_;

  bubble_delegate->set_margins(gfx::Insets());
  bubble_delegate->SetModalType(ui::mojom::ModalType::kNone);
  bubble_delegate->SetButtons(
      static_cast<int>(ui::mojom::DialogButton::kNone));
  bubble_delegate->set_adjust_if_offscreen(true);
  bubble_delegate->SetContentsView(std::move(contents));
  bubble_delegate->SetInitiallyFocusedView(title_field);

  views::BubbleDialogDelegate* raw_delegate = bubble_delegate.get();
#if CHROME_VERSION_MAJOR >= 151
  // M151 renamed the ownership-transferring factory. Keep the Widget-owned
  // lifetime used by Chromium 150's group editor across every supported
  // Chromium version.
  views::Widget* widget = views::BubbleDialogDelegate::CreateBubbleDeprecated(
      std::move(bubble_delegate),
      views::Widget::InitParams::NATIVE_WIDGET_OWNS_WIDGET);
#else
  views::Widget* widget =
      views::BubbleDialogDelegate::CreateBubble(std::move(bubble_delegate));
#endif
  raw_delegate->GetBubbleFrameView()->SetPreferredArrowAdjustment(
      views::BubbleFrameView::PreferredArrowAdjustment::kOffset);
  widget->Show();
  return weak_contents;
}

CmuxRailGroupEditorBubble::CmuxRailGroupEditorBubble(
    RailDelegate* delegate,
    WorkspaceGroupId group,
    const std::string& title,
    GroupColor color,
    base::OnceClosure closed_callback)
    : delegate_(delegate),
      group_(group),
      closed_callback_(std::move(closed_callback)) {
  const gfx::Insets control_insets = GetControlInsets();
  const int vertical_spacing = control_insets.top();
  const int horizontal_spacing = control_insets.left();

  title_field_ = AddChildView(std::make_unique<views::Textfield>());
  title_field_->SetText(base::UTF8ToUTF16(title));
  title_field_->SetPlaceholderText(u"Name this group");
  title_field_->GetViewAccessibility().SetName(u"Workspace group title");
  title_field_->set_controller(this);
  title_field_->SetProperty(
      views::kMarginsKey,
      gfx::Insets::VH(vertical_spacing, horizontal_spacing));

  auto color_picker = std::make_unique<WorkspaceGroupColorPicker>(
      this, color,
      base::BindRepeating(&CmuxRailGroupEditorBubble::OnColorSelected,
                          base::Unretained(this)));
  color_picker->SetProperty(views::kMarginsKey,
                            gfx::Insets::VH(0, horizontal_spacing));
  AddChildView(std::move(color_picker));

  auto first_separator = std::make_unique<views::Separator>();
  first_separator->SetProperty(views::kMarginsKey,
                               gfx::Insets::VH(vertical_spacing, 0));
  AddChildView(std::move(first_separator));

  AddMenuItem(WorkspaceGroupContextAction::kNewWorkspaceInGroup,
              u"New workspace in group");
  AddMenuItem(WorkspaceGroupContextAction::kMoveGroupToNewWindow,
              u"Move group to new window");

  auto second_separator = std::make_unique<views::Separator>();
  second_separator->SetProperty(views::kMarginsKey,
                                gfx::Insets::VH(vertical_spacing, 0));
  AddChildView(std::move(second_separator));

  AddMenuItem(WorkspaceGroupContextAction::kUngroup, u"Ungroup");
  AddMenuItem(WorkspaceGroupContextAction::kCloseGroup, u"Close group");

  AddAccelerator(*MenuAccelerator(
      WorkspaceGroupContextAction::kNewWorkspaceInGroup));
  AddAccelerator(
      *MenuAccelerator(WorkspaceGroupContextAction::kCloseGroup));

  SetLayoutManager(std::make_unique<views::FlexLayout>())
      ->SetOrientation(views::LayoutOrientation::kVertical)
      .SetInteriorMargin(gfx::Insets::VH(vertical_spacing, 0));
  // Helium commit 830b7bcc disables Chromium's saved-group footer; cmux
  // workspaces are unsaved groups, so no footer is added.
}

CmuxRailGroupEditorBubble::~CmuxRailGroupEditorBubble() {
  if (closed_callback_) {
    std::move(closed_callback_).Run();
  }
}

void CmuxRailGroupEditorBubble::AddedToWidget() {
  const auto* const color_provider = GetColorProvider();

  for (views::LabelButton* menu_item : simple_menu_items_) {
    const bool enabled = menu_item->GetEnabled();
    const views::Button::ButtonState button_state =
        enabled ? views::Button::STATE_NORMAL
                : views::Button::STATE_DISABLED;
    const SkColor text_color = menu_item->GetCurrentTextColor();
    const SkColor enabled_icon_color =
        color_provider->GetColor(kColorTabGroupDialogIconEnabled);
    const SkColor icon_color = enabled ? enabled_icon_color : text_color;

    const std::optional<ui::ImageModel>& old_image_model =
        menu_item->GetImageModel(button_state);
    if (old_image_model.has_value() && !old_image_model->IsEmpty() &&
        old_image_model->IsVectorIcon()) {
      const ui::VectorIconModel vector_icon_model =
          old_image_model->GetVectorIcon();
      const gfx::VectorIcon* icon = vector_icon_model.vector_icon();
      const ui::ImageModel new_image_model =
          ui::ImageModel::FromVectorIcon(
              *icon, icon_color, old_image_model->Size().width());
      menu_item->SetImageModel(button_state, new_image_model);
    }
  }
}

gfx::Size CmuxRailGroupEditorBubble::CalculatePreferredSize(
    const views::SizeBounds& available_size) const {
  gfx::Size size = views::View::CalculatePreferredSize(available_size);
  size.set_width(kDialogWidth);
  return size;
}

bool CmuxRailGroupEditorBubble::AcceleratorPressed(
    const ui::Accelerator& accelerator) {
  if (accelerator == *MenuAccelerator(
                         WorkspaceGroupContextAction::kNewWorkspaceInGroup)) {
    RunAction(WorkspaceGroupContextAction::kNewWorkspaceInGroup);
    return true;
  }
  if (accelerator ==
      *MenuAccelerator(WorkspaceGroupContextAction::kCloseGroup)) {
    RunAction(WorkspaceGroupContextAction::kCloseGroup);
    return true;
  }
  return views::View::AcceleratorPressed(accelerator);
}

void CmuxRailGroupEditorBubble::ContentsChanged(
    views::Textfield* sender,
    const std::u16string& new_contents) {
  if (sender == title_field_ && delegate_) {
    delegate_->OnSetWorkspaceGroupTitle(group_,
                                        base::UTF16ToUTF8(new_contents));
  }
}

bool CmuxRailGroupEditorBubble::HandleKeyEvent(
    views::Textfield* sender,
    const ui::KeyEvent& key_event) {
  if (sender != title_field_ || key_event.type() != ui::EventType::kKeyPressed) {
    return false;
  }
  if (key_event.key_code() == ui::VKEY_ESCAPE) {
    if (GetWidget()) {
      GetWidget()->CloseWithReason(views::Widget::ClosedReason::kEscKeyPressed);
    }
    return true;
  }
  if (key_event.key_code() == ui::VKEY_RETURN) {
    if (GetWidget()) {
      GetWidget()->CloseWithReason(views::Widget::ClosedReason::kUnspecified);
    }
    return true;
  }
  return false;
}

void CmuxRailGroupEditorBubble::AddMenuItem(
    WorkspaceGroupContextAction action,
    const std::u16string& label) {
  const std::optional<ui::Accelerator> accelerator = MenuAccelerator(action);
  auto button = CreateBubbleMenuItem(
      static_cast<int>(action), label,
      base::BindRepeating(&CmuxRailGroupEditorBubble::RunAction,
                          base::Unretained(this), action),
      ui::ImageModel::FromVectorIcon(MenuIcon(action), ui::kColorMenuIcon,
                                     kDefaultIconSize),
      accelerator ? accelerator->GetShortcutText() : std::u16string());
  button->SetLabelStyle(views::style::STYLE_BODY_3_EMPHASIS);
  button->SetBorder(views::CreateEmptyBorder(GetControlInsets()));

  const bool enabled =
      delegate_ &&
      delegate_->IsWorkspaceGroupContextActionEnabled(group_, action);
  button->SetEnabled(enabled);
  if (action == WorkspaceGroupContextAction::kMoveGroupToNewWindow) {
    // Chromium builds this row but hides it when CanMoveGroupToNewWindow() is
    // false. cmux uses the delegate capability as that model-boundary check.
    button->SetVisible(enabled);
  }
  simple_menu_items_.push_back(AddChildView(std::move(button)));
}

void CmuxRailGroupEditorBubble::RunAction(
    WorkspaceGroupContextAction action) {
  if (!delegate_ ||
      !delegate_->IsWorkspaceGroupContextActionEnabled(group_, action)) {
    return;
  }
  if (GetWidget()) {
    GetWidget()->Close();
  }
  delegate_->OnWorkspaceGroupContextAction(group_, action);
}

void CmuxRailGroupEditorBubble::OnColorSelected(GroupColor color) {
  if (delegate_) {
    delegate_->OnSetWorkspaceGroupColor(group_, color);
  }
}

BEGIN_METADATA(CmuxRailGroupEditorBubble)
END_METADATA

}  // namespace cmux
