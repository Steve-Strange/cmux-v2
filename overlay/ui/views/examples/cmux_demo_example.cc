// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/views/examples/cmux_demo_example.h"

#include <algorithm>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/cmux_term/window_layout.h"
#include "chrome/browser/cmux_term/window_model.h"
#include "chrome/browser/ui/color/chrome_color_id.h"
#if __has_include("chrome/browser/ui/color/tab_group_color_ids.h")
#include "chrome/browser/ui/color/tab_group_color_ids.h"
#endif
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/color/color_mixer.h"
#include "ui/color/color_recipe.h"
#include "ui/events/event.h"
#include "ui/gfx/color_palette.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/insets.h"
#include "ui/gfx/geometry/size.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/background.h"
#include "ui/views/border.h"
#include "ui/views/controls/button/checkbox.h"
#include "ui/views/controls/button/label_button.h"
#include "ui/views/controls/label.h"
#include "ui/views/controls/scroll_view.h"
#include "ui/views/controls/slider.h"
#include "ui/views/controls/textfield/textfield.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/layout/fill_layout.h"
#include "ui/views/view.h"

namespace views::examples {

namespace {

cmux::SurfaceTab MakeTab(int64_t id,
                         const std::string& title,
                         cmux::SurfaceKind kind) {
  cmux::SurfaceTab t;
  t.id = id;
  t.title = title;
  t.kind = kind;
  return t;
}

constexpr SkColor kPaneTints[] = {
    SkColorSetRGB(0x1b, 0x24, 0x33), SkColorSetRGB(0x22, 0x1c, 0x30),
    SkColorSetRGB(0x1a, 0x2a, 0x26), SkColorSetRGB(0x2b, 0x24, 0x1a),
    SkColorSetRGB(0x18, 0x20, 0x2c),
};

bool ParseHex(const std::u16string& s16, SkColor* out) {
  std::string h = base::UTF16ToUTF8(s16);
  if (!h.empty() && h[0] == '#') {
    h = h.substr(1);
  }
  if (h.size() == 6) {
    h += "ff";
  }
  uint32_t v = 0;
  if (h.size() != 8 || !base::HexStringToUInt(h, &v)) {
    return false;
  }
  *out = SkColorSetARGB(v & 0xff, (v >> 24) & 0xff, (v >> 16) & 0xff,
                        (v >> 8) & 0xff);
  return true;
}

}  // namespace

void AddCmuxDemoColorMixers(ui::ColorProvider* color_provider,
                            const ui::ColorProviderKey& key) {
  CHECK(color_provider);
  const bool dark_mode =
      key.color_mode == ui::ColorProviderKey::ColorMode::kDark;
  const SkColor surface = dark_mode ? SkColorSetRGB(0x20, 0x21, 0x24)
                                    : SK_ColorWHITE;
  const SkColor inactive_surface =
      dark_mode ? SkColorSetRGB(0x35, 0x36, 0x3a)
                : SkColorSetRGB(0xf1, 0xf3, 0xf4);
  const SkColor on_surface = dark_mode ? gfx::kGoogleGrey200
                                       : gfx::kGoogleGrey900;
  const SkColor secondary = dark_mode ? gfx::kGoogleGrey400
                                      : gfx::kGoogleGrey700;

  ui::ColorMixer& mixer = color_provider->AddMixer();
  mixer[kColorToolbar] = {surface};
  mixer[kColorLocationBarBackground] = {surface};
  mixer[kColorToolbarInkDrop] = {on_surface};
  mixer[kColorToolbarButtonIconDisabled] = {
      SkColorSetA(secondary, static_cast<SkAlpha>(
                                 gfx::kDisabledControlAlpha * SK_AlphaOPAQUE))};
  mixer[kColorTabBackgroundActiveFrameActive] = {surface};
  mixer[kColorTabBackgroundInactiveFrameActive] = {inactive_surface};
  mixer[kColorTabBackgroundInactiveFrameInactive] = {inactive_surface};
  mixer[kColorTabBackgroundInactiveHoverFrameActive] = {
      SkColorSetA(on_surface, 0x73)};

  const auto set_group_colors = [&](ui::ColorId context_id,
                                    ui::ColorId dialog_id,
                                    SkColor dark_color,
                                    SkColor light_color) {
    const SkColor color = dark_mode ? dark_color : light_color;
    mixer[context_id] = {color};
    mixer[dialog_id] = {color};
  };
  set_group_colors(kColorTabGroupContextMenuGrey,
                   kColorTabGroupDialogGrey,
                   gfx::kTabGroupGreyDarkMode,
                   gfx::kTabGroupGreyLightMode);
  set_group_colors(kColorTabGroupContextMenuBlue,
                   kColorTabGroupDialogBlue,
                   gfx::kTabGroupBlueDarkMode,
                   gfx::kTabGroupBlueLightMode);
  set_group_colors(kColorTabGroupContextMenuRed,
                   kColorTabGroupDialogRed,
                   gfx::kTabGroupRedDarkMode,
                   gfx::kTabGroupRedLightMode);
  set_group_colors(kColorTabGroupContextMenuYellow,
                   kColorTabGroupDialogYellow,
                   gfx::kTabGroupLimeDarkMode,
                   gfx::kTabGroupLimeLightMode);
  set_group_colors(kColorTabGroupContextMenuGreen,
                   kColorTabGroupDialogGreen,
                   gfx::kTabGroupGreenDarkMode,
                   gfx::kTabGroupGreenLightMode);
  set_group_colors(kColorTabGroupContextMenuPink,
                   kColorTabGroupDialogPink,
                   gfx::kTabGroupMagentaDarkMode,
                   gfx::kTabGroupMagentaLightMode);
  set_group_colors(kColorTabGroupContextMenuPurple,
                   kColorTabGroupDialogPurple,
                   gfx::kTabGroupPurpleDarkMode,
                   gfx::kTabGroupPurpleLightMode);
  set_group_colors(kColorTabGroupContextMenuCyan,
                   kColorTabGroupDialogCyan,
                   gfx::kTabGroupCyanDarkMode,
                   gfx::kTabGroupCyanLightMode);
  set_group_colors(kColorTabGroupContextMenuOrange,
                   kColorTabGroupDialogOrange,
                   gfx::kTabGroupOrangeDarkMode,
                   gfx::kTabGroupOrangeLightMode);
  mixer[kColorTabGroupDialogIconEnabled] = {on_surface};

  mixer[kColorTabHoverCardBackground] = {surface};
  mixer[kColorTabHoverCardForeground] = {on_surface};
  mixer[kColorTabHoverCardSecondaryText] = {secondary};
}

class CmuxDemoStripView : public View {
  METADATA_HEADER(CmuxDemoStripView, View)

 public:
  explicit CmuxDemoStripView(cmux::StripPaneDelegate* delegate)
      : delegate_(delegate) {}
  CmuxDemoStripView(const CmuxDemoStripView&) = delete;
  CmuxDemoStripView& operator=(const CmuxDemoStripView&) = delete;
  ~CmuxDemoStripView() override = default;

  void SetLayoutConfig(const cmux::LayoutConfig& config) {
    config_ = config;
    UpdateFocusBorders();
    InvalidateLayout();
    SchedulePaint();
  }

  void SetWorkspace(const cmux::WindowModel* model,
                    cmux::WorkspaceId workspace,
                    cmux::PaneId focused) {
    model_ = model;
    workspace_id_ = workspace;
    focused_ = focused;

    std::map<cmux::PaneId, const cmux::Pane*> current;
    if (const cmux::Workspace* ws = this->workspace()) {
      for (const cmux::LayoutNode& column : ws->columns) {
        CollectPanes(column, &current);
      }
    }

    for (auto it = panes_.begin(); it != panes_.end();) {
      if (current.count(it->first) == 0) {
        View* doomed = it->second.get();
        it = panes_.erase(it);
        RemoveChildViewT(doomed);
      } else {
        ++it;
      }
    }

    for (const auto& [id, pane] : current) {
      if (panes_.count(id)) {
        continue;
      }
      auto wrapper = std::make_unique<View>();
      wrapper->SetLayoutManager(std::make_unique<FillLayout>());
      if (delegate_) {
        wrapper->AddChildView(delegate_->CreateStripPane(id, *pane));
      }
      panes_[id] = AddChildView(std::move(wrapper));
    }

    UpdateFocusBorders();
    InvalidateLayout();
    SchedulePaint();
  }

  void Layout(PassKey) override { ApplyLayout(); }

  bool OnMouseWheel(const ui::MouseWheelEvent& event) override {
    const int delta =
        event.x_offset() != 0 ? event.x_offset() : event.y_offset();
    if (delta == 0) {
      return false;
    }
    const double before = scroll_x_;
    scroll_x_ = std::max(0.0, scroll_x_ - delta);
    ApplyLayout();
    return scroll_x_ != before;
  }

  bool OnMousePressed(const ui::MouseEvent& event) override {
    for (const auto& [id, wrapper] : panes_) {
      if (wrapper->bounds().Contains(event.location())) {
        if (focused_ != id) {
          focused_ = id;
          UpdateFocusBorders();
          ApplyLayout();
        }
        return true;
      }
    }
    return false;
  }

  void OnPaintBackground(gfx::Canvas* canvas) override {
    canvas->DrawColor(SkColorSetRGB(0x10, 0x12, 0x17));
  }

 private:
  static void CollectPanes(const cmux::LayoutNode& node,
                           std::map<cmux::PaneId, const cmux::Pane*>* out) {
    if (node.is_pane()) {
      (*out)[node.pane->id] = node.pane.get();
      return;
    }
    if (node.is_split()) {
      CollectPanes(node.split->first, out);
      CollectPanes(node.split->second, out);
    }
  }

  cmux::LayoutMetrics Metrics() const {
    cmux::LayoutMetrics metrics;
    metrics.gap = std::max(0, config_.gap);
    metrics.margin = std::max(0, config_.margin);
    metrics.min_column_width = std::max(1, config_.min_column_width);
    metrics.column_fraction = std::clamp(config_.column_fraction, 0.1, 2.0);
    return metrics;
  }

  void ApplyLayout() {
    const cmux::Workspace* ws = workspace();
    if (!ws) {
      return;
    }
    cmux::StripLayout sl = cmux::ComputeStripLayout(
        *ws, width(), height(), scroll_x_, focused_, Metrics());
    scroll_x_ = sl.scroll_x;
    for (const cmux::PaneBox& b : sl.panes) {
      auto it = panes_.find(b.pane);
      if (it == panes_.end()) {
        continue;
      }
      it->second->SetBoundsRect(gfx::Rect(
          static_cast<int>(b.rect.x), static_cast<int>(b.rect.y),
          static_cast<int>(b.rect.width), static_cast<int>(b.rect.height)));
    }
  }

  void UpdateFocusBorders() {
    const int border = std::max(0, config_.focus_border);
    for (const auto& [id, wrapper] : panes_) {
      wrapper->SetBorder(views::CreateSolidBorder(
          border, id == focused_ ? cmux::LayoutFocusColor(config_)
                                 : SkColorSetRGB(0x2a, 0x2e, 0x38)));
    }
  }

  const cmux::Workspace* workspace() const {
    return model_ ? model_->GetWorkspace(workspace_id_) : nullptr;
  }

  raw_ptr<cmux::StripPaneDelegate> delegate_;
  raw_ptr<const cmux::WindowModel> model_ = nullptr;
  cmux::WorkspaceId workspace_id_ = cmux::kInvalidId;
  cmux::PaneId focused_ = cmux::kInvalidId;
  cmux::LayoutConfig config_;
  double scroll_x_ = 0;
  std::map<cmux::PaneId, raw_ptr<View>> panes_;
};

BEGIN_METADATA(CmuxDemoStripView)
END_METADATA

CmuxDemoExample::CmuxDemoExample(int rail_style, const char* name)
    : ExampleBase(name), rail_style_(rail_style) {}
CmuxDemoExample::~CmuxDemoExample() = default;

void CmuxDemoExample::CreateExampleView(View* parent) {
  using K = cmux::SurfaceKind;
  cmux::WorkspaceId personal = model_.AddWorkspace(K::kWeb, "Personal");
  model_.AddChildWorkspace(personal, K::kWeb, "Email");
  model_.AddChildWorkspace(personal, K::kWeb, "Calendar");
  cmux::WorkspaceId work = model_.AddWorkspace(K::kWeb, "Work");
  cmux::WorkspaceId proj_a =
      model_.AddChildWorkspace(work, K::kWeb, "Project A");
  model_.AddChildWorkspace(proj_a, K::kWeb, "Docs");
  model_.AddChildWorkspace(proj_a, K::kTerminal, "Code");
  model_.AddChildWorkspace(work, K::kWeb, "Project B");
  model_.AddWorkspace(K::kWeb, "Scratch");

  cmux::WorkspaceId dev = model_.AddWorkspace(K::kWeb, "Dev");

  model_.SetWorkspaceColor(personal, cmux::GroupColor::kGreen);
  model_.SetWorkspaceColor(work, cmux::GroupColor::kBlue);
  model_.SetWorkspaceColor(dev, cmux::GroupColor::kOrange);

  // Dev shows both layers of the layout: three columns, two of them split
  // in-column (one stacked, one side-by-side).
  cmux::PaneId dev_term = model_.AddColumn(dev, K::kTerminal);
  model_.SplitPane(dev, dev_term, cmux::SplitOrientation::kVertical, 0.5,
                   K::kTerminal);
  cmux::PaneId dev_web = model_.AddColumn(dev, K::kWeb);
  model_.SplitPane(dev, dev_web, cmux::SplitOrientation::kHorizontal, 0.5,
                   K::kTerminal);
  model_.SelectWorkspace(dev);

  tabs_.push_back(MakeTab(next_tab_id_++, "example.com", K::kWeb));
  tabs_.push_back(MakeTab(next_tab_id_++, "news.ycombinator.com", K::kWeb));
  tabs_.push_back(MakeTab(next_tab_id_++, std::string(), K::kTerminal));
  selected_tab_ = tabs_.front().id;

  // Layout: [ rail | content | controls ].
  auto* hbox = parent->SetLayoutManager(std::make_unique<views::BoxLayout>(
      views::BoxLayout::Orientation::kHorizontal));
  hbox->set_cross_axis_alignment(
      views::BoxLayout::CrossAxisAlignment::kStretch);

  rail_ =
      parent->AddChildView(std::make_unique<cmux::CmuxRail>(this, rail_style_));
  rail_->SetPreferredSize(gfx::Size(std::max(0, layout_cfg_.rail_width), 0));
  hbox->SetFlexForView(rail_, 0);
  // Reflect the rail's initial preset/default config locally; persisted config
  // arrives asynchronously below.
  cfg_ = rail_->config();
  rail_->SetConfig(RailConfigForDemo());

  auto* content = parent->AddChildView(std::make_unique<views::View>());
  hbox->SetFlexForView(content, 1);
  auto* cbox = content->SetLayoutManager(std::make_unique<views::BoxLayout>(
      views::BoxLayout::Orientation::kVertical));
  cbox->set_cross_axis_alignment(
      views::BoxLayout::CrossAxisAlignment::kStretch);

  strip_ = content->AddChildView(std::make_unique<cmux::CmuxTabStrip>(this));
  strip_->SetAnimationConfig(layout_cfg_.animations,
                             std::max(0, layout_cfg_.animation_ms));
  strip_->SetAccentColor(cmux::LayoutAccentColor(layout_cfg_));
  cbox->SetFlexForView(strip_, 0);
  strip_view_ =
      content->AddChildView(std::make_unique<CmuxDemoStripView>(this));
  strip_view_->SetLayoutConfig(layout_cfg_);
  cbox->SetFlexForView(strip_view_, 1);

  // Controls column (scrollable).
  auto* scroll = parent->AddChildView(std::make_unique<views::ScrollView>());
  scroll->SetPreferredSize(gfx::Size(330, 0));
  hbox->SetFlexForView(scroll, 0);
  auto* panel = scroll->SetContents(std::make_unique<views::View>());
  panel->SetBackground(
      views::CreateSolidBackground(SkColorSetRGB(0x23, 0x25, 0x2b)));
  auto* pbox = panel->SetLayoutManager(std::make_unique<views::BoxLayout>(
      views::BoxLayout::Orientation::kVertical, gfx::Insets(10), 3));
  pbox->set_cross_axis_alignment(
      views::BoxLayout::CrossAxisAlignment::kStretch);
  BuildControls(panel);
  cmux::LoadRailConfigAsync(
      cfg_, base::BindOnce(
                [](base::WeakPtr<CmuxDemoExample> self,
                   std::optional<cmux::RailConfig> config) {
                  if (!self || !config) {
                    return;
                  }
                  self->cfg_ = *config;
                  self->SyncControlsFromConfig();
                  if (self->rail_) {
                    self->rail_->SetConfig(self->RailConfigForDemo());
                  }
                },
                weak_factory_.GetWeakPtr()));
  cmux::LoadLayoutConfigAsync(
      layout_cfg_,
      base::BindOnce(
          [](base::WeakPtr<CmuxDemoExample> self,
             std::optional<cmux::LayoutConfig> config) {
            if (!self || !config) {
              return;
            }
            self->layout_cfg_ = *config;
            self->SyncControlsFromConfig();
            if (self->rail_) {
              self->rail_->SetPreferredSize(
                  gfx::Size(std::max(0, self->layout_cfg_.rail_width), 0));
              self->rail_->SetConfig(self->RailConfigForDemo());
              if (self->rail_->parent()) {
                self->rail_->parent()->InvalidateLayout();
              }
            }
            if (self->strip_view_) {
              self->strip_view_->SetLayoutConfig(self->layout_cfg_);
              self->strip_view_->InvalidateLayout();
            }
            if (self->strip_) {
              self->strip_->SetAnimationConfig(
                  self->layout_cfg_.animations,
                  std::max(0, self->layout_cfg_.animation_ms));
              self->strip_->SetAccentColor(
                  cmux::LayoutAccentColor(self->layout_cfg_));
            }
          },
          weak_factory_.GetWeakPtr()));

  RefreshRail();
  RefreshTabs();
  RefreshStrip();
}

// ---- Controls panel ---------------------------------------------------------
namespace {
views::Label* SectionLabel(views::View* parent, const char* text) {
  auto* l = parent->AddChildView(
      std::make_unique<views::Label>(base::UTF8ToUTF16(std::string(text))));
  l->SetHorizontalAlignment(gfx::ALIGN_LEFT);
  l->SetEnabledColor(SkColorSetA(SK_ColorWHITE, 0x77));
  l->SetFontList(
      l->font_list().Derive(-1, gfx::Font::NORMAL, gfx::Font::Weight::BOLD));
  return l;
}
}  // namespace

void CmuxDemoExample::AddSliderRow(View* parent,
                                   const char* label,
                                   int* field,
                                   int lo,
                                   int hi,
                                   bool layout) {
  auto* row = parent->AddChildView(std::make_unique<views::View>());
  auto* box = row->SetLayoutManager(std::make_unique<views::BoxLayout>(
      views::BoxLayout::Orientation::kHorizontal, gfx::Insets(0), 6));
  box->set_cross_axis_alignment(views::BoxLayout::CrossAxisAlignment::kCenter);

  auto* lab = row->AddChildView(
      std::make_unique<views::Label>(base::UTF8ToUTF16(std::string(label))));
  lab->SetHorizontalAlignment(gfx::ALIGN_LEFT);
  lab->SetEnabledColor(SkColorSetA(SK_ColorWHITE, 0xCC));
  lab->SetPreferredSize(gfx::Size(120, 22));

  auto* slider = row->AddChildView(std::make_unique<views::Slider>(this));
  slider->GetViewAccessibility().SetName(base::UTF8ToUTF16(std::string(label)));
  slider->SetValue(static_cast<float>(*field - lo) / std::max(1, hi - lo));
  box->SetFlexForView(slider, 1);

  auto* val = row->AddChildView(
      std::make_unique<views::Label>(base::NumberToString16(*field)));
  val->SetEnabledColor(SK_ColorWHITE);
  val->SetPreferredSize(gfx::Size(32, 22));

  slider_binds_.push_back({slider, field, lo, hi, val, layout});
}

void CmuxDemoExample::AddDoubleSliderRow(View* parent,
                                         const char* label,
                                         double* field,
                                         double lo,
                                         double hi,
                                         bool layout) {
  auto* row = parent->AddChildView(std::make_unique<views::View>());
  auto* box = row->SetLayoutManager(std::make_unique<views::BoxLayout>(
      views::BoxLayout::Orientation::kHorizontal, gfx::Insets(0), 6));
  box->set_cross_axis_alignment(views::BoxLayout::CrossAxisAlignment::kCenter);

  auto* lab = row->AddChildView(
      std::make_unique<views::Label>(base::UTF8ToUTF16(std::string(label))));
  lab->SetHorizontalAlignment(gfx::ALIGN_LEFT);
  lab->SetEnabledColor(SkColorSetA(SK_ColorWHITE, 0xCC));
  lab->SetPreferredSize(gfx::Size(120, 22));

  auto* slider = row->AddChildView(std::make_unique<views::Slider>(this));
  slider->GetViewAccessibility().SetName(base::UTF8ToUTF16(std::string(label)));
  slider->SetValue(static_cast<float>((*field - lo) / std::max(0.01, hi - lo)));
  box->SetFlexForView(slider, 1);

  auto* val = row->AddChildView(std::make_unique<views::Label>(
      base::UTF8ToUTF16(base::StringPrintf("%.2f", *field))));
  val->SetEnabledColor(SK_ColorWHITE);
  val->SetPreferredSize(gfx::Size(40, 22));

  double_slider_binds_.push_back({slider, field, lo, hi, val, layout});
}

void CmuxDemoExample::AddCheckRow(View* parent,
                                  const char* label,
                                  bool* field,
                                  bool layout) {
  auto* cb = parent->AddChildView(
      std::make_unique<views::Checkbox>(base::UTF8ToUTF16(std::string(label))));
  cb->SetChecked(*field);
  cb->SetTextColor(views::Button::STATE_NORMAL,
                   SkColorSetA(SK_ColorWHITE, 0xCC));
  cb->SetTextColor(views::Button::STATE_HOVERED, SK_ColorWHITE);
  views::Checkbox* cb_raw = cb;
  cb->SetCallback(base::BindRepeating(
      [](CmuxDemoExample* self, bool* f, bool layout, views::Checkbox* c) {
        if (self->syncing_) {
          return;
        }
        *f = c->GetChecked();
        if (layout) {
          self->ApplyLayoutAndSave();
        } else {
          self->ApplyAndSave();
        }
      },
      base::Unretained(this), field, layout, cb_raw));
  checks_.push_back({cb, field, layout});
}

void CmuxDemoExample::AddColorRow(View* parent,
                                  const char* label,
                                  SkColor* field) {
  auto* row = parent->AddChildView(std::make_unique<views::View>());
  auto* box = row->SetLayoutManager(std::make_unique<views::BoxLayout>(
      views::BoxLayout::Orientation::kHorizontal, gfx::Insets(0), 6));
  box->set_cross_axis_alignment(views::BoxLayout::CrossAxisAlignment::kCenter);
  auto* lab = row->AddChildView(
      std::make_unique<views::Label>(base::UTF8ToUTF16(std::string(label))));
  lab->SetHorizontalAlignment(gfx::ALIGN_LEFT);
  lab->SetEnabledColor(SkColorSetA(SK_ColorWHITE, 0xCC));
  lab->SetPreferredSize(gfx::Size(110, 22));
  auto* sw = row->AddChildView(std::make_unique<views::View>());
  sw->SetPreferredSize(gfx::Size(22, 22));
  sw->SetBackground(views::CreateSolidBackground(*field));
  auto* tf = row->AddChildView(std::make_unique<views::Textfield>());
  tf->GetViewAccessibility().SetName(base::UTF8ToUTF16(std::string(label)));
  tf->SetText(base::UTF8ToUTF16(cmux::ColorToHex(*field)));
  tf->set_controller(this);
  box->SetFlexForView(tf, 1);
  color_binds_[tf] = field;
  swatches_[tf] = sw;
}

void CmuxDemoExample::AddLayoutColorRow(View* parent,
                                        const char* label,
                                        std::string* field) {
  auto* row = parent->AddChildView(std::make_unique<views::View>());
  auto* box = row->SetLayoutManager(std::make_unique<views::BoxLayout>(
      views::BoxLayout::Orientation::kHorizontal, gfx::Insets(0), 6));
  box->set_cross_axis_alignment(views::BoxLayout::CrossAxisAlignment::kCenter);
  auto* lab = row->AddChildView(
      std::make_unique<views::Label>(base::UTF8ToUTF16(std::string(label))));
  lab->SetHorizontalAlignment(gfx::ALIGN_LEFT);
  lab->SetEnabledColor(SkColorSetA(SK_ColorWHITE, 0xCC));
  lab->SetPreferredSize(gfx::Size(110, 22));
  auto* sw = row->AddChildView(std::make_unique<views::View>());
  sw->SetPreferredSize(gfx::Size(22, 22));
  sw->SetBackground(views::CreateSolidBackground(LayoutColorForField(field)));
  auto* tf = row->AddChildView(std::make_unique<views::Textfield>());
  tf->GetViewAccessibility().SetName(base::UTF8ToUTF16(std::string(label)));
  tf->SetText(base::UTF8ToUTF16(*field));
  tf->set_controller(this);
  box->SetFlexForView(tf, 1);
  layout_color_binds_[tf] = field;
  swatches_[tf] = sw;
}

SkColor CmuxDemoExample::LayoutColorForField(const std::string* field) const {
  if (field == &layout_cfg_.focus_color) {
    return cmux::LayoutFocusColor(layout_cfg_);
  }
  return cmux::LayoutAccentColor(layout_cfg_);
}

void CmuxDemoExample::BuildControls(View* panel) {
  auto* title = panel->AddChildView(std::make_unique<views::Label>(
      u"Cmux controls (persisted rail + layout JSON)"));
  title->SetHorizontalAlignment(gfx::ALIGN_LEFT);
  title->SetEnabledColor(SK_ColorWHITE);
  title->SetFontList(
      title->font_list().Derive(1, gfx::Font::NORMAL, gfx::Font::Weight::BOLD));

  SectionLabel(panel, "LAYOUT");
  AddSliderRow(panel, "rail width", &layout_cfg_.rail_width, 0, 360,
               /*layout=*/true);
  AddSliderRow(panel, "gap", &layout_cfg_.gap, 0, 40, /*layout=*/true);
  AddSliderRow(panel, "margin", &layout_cfg_.margin, 0, 40, /*layout=*/true);
  AddSliderRow(panel, "min column", &layout_cfg_.min_column_width, 220, 720,
               /*layout=*/true);
  AddDoubleSliderRow(panel, "column frac", &layout_cfg_.column_fraction, 0.35,
                     1.10, /*layout=*/true);
  AddSliderRow(panel, "header height", &layout_cfg_.header_height, 30, 64,
               /*layout=*/true);
  AddSliderRow(panel, "focus border", &layout_cfg_.focus_border, 0, 4,
               /*layout=*/true);
  AddLayoutColorRow(panel, "accent", &layout_cfg_.accent_color);
  AddLayoutColorRow(panel, "focus color", &layout_cfg_.focus_color);
  AddCheckRow(panel, "animations", &layout_cfg_.animations, /*layout=*/true);
  AddSliderRow(panel, "animation ms", &layout_cfg_.animation_ms, 0, 360,
               /*layout=*/true);

  SectionLabel(panel, "GEOMETRY");
  AddSliderRow(panel, "row height", &cfg_.row_height, 20, 60);
  AddSliderRow(panel, "row gap", &cfg_.row_gap, 0, 18);
  AddSliderRow(panel, "top pad", &cfg_.top_pad, 0, 28);
  AddSliderRow(panel, "left pad", &cfg_.left_pad, 0, 36);
  AddSliderRow(panel, "right pad", &cfg_.right_pad, 0, 36);
  AddSliderRow(panel, "indent/depth", &cfg_.indent_per_depth, 0, 32);
  AddSliderRow(panel, "lead width", &cfg_.lead_width, 0, 36);
  AddSliderRow(panel, "corner radius", &cfg_.row_corner, 0, 16);
  AddSliderRow(panel, "hover corner", &cfg_.hover_corner, 0, 16);
  AddSliderRow(panel, "pill inset V", &cfg_.pill_inset_v, 0, 12);
  AddSliderRow(panel, "pill inset H", &cfg_.pill_inset_h, 0, 18);
  AddSliderRow(panel, "traffic clear", &cfg_.traffic_light_clearance, 48, 96);
  AddSliderRow(panel, "header button", &cfg_.header_button_size, 18, 34);
  AddSliderRow(panel, "dot size", &cfg_.color_dot_size, 0, 14);
  AddSliderRow(panel, "dot gap", &cfg_.color_dot_gap, 0, 14);
  AddSliderRow(panel, "scrollbar w", &cfg_.scrollbar_width, 2, 16);

  SectionLabel(panel, "TYPE");
  AddSliderRow(panel, "font size (0=def)", &cfg_.font_size, 0, 24);
  AddSliderRow(panel, "font weight 0-3", &cfg_.font_weight, 0, 3);
  AddSliderRow(panel, "selected weight", &cfg_.selected_font_weight, 0, 3);
  {
    auto* row = panel->AddChildView(std::make_unique<views::View>());
    auto* box = row->SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kHorizontal, gfx::Insets(0), 6));
    box->set_cross_axis_alignment(
        views::BoxLayout::CrossAxisAlignment::kCenter);
    auto* lab =
        row->AddChildView(std::make_unique<views::Label>(u"font family"));
    lab->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    lab->SetEnabledColor(SkColorSetA(SK_ColorWHITE, 0xCC));
    lab->SetPreferredSize(gfx::Size(120, 22));
    family_field_ = row->AddChildView(std::make_unique<views::Textfield>());
    family_field_->GetViewAccessibility().SetName(u"font family");
    family_field_->SetText(base::UTF8ToUTF16(cfg_.font_family));
    family_field_->SetPlaceholderText(u"e.g. Menlo, Georgia");
    family_field_->set_controller(this);
    box->SetFlexForView(family_field_, 1);
  }
  AddCheckRow(panel, "folder bold", &cfg_.folder_bold);

  SectionLabel(panel, "TOGGLES");
  AddCheckRow(panel, "close (X) on hover", &cfg_.show_close_on_hover);
  AddCheckRow(panel, "folder count badge", &cfg_.show_count_badge);
  AddCheckRow(panel, "group-color chevron", &cfg_.group_color_chevron);

  SectionLabel(panel, "COLORS (#rrggbb or #rrggbbaa)");
  AddColorRow(panel, "background", &cfg_.bg);
  AddColorRow(panel, "selected bg", &cfg_.sel_bg);
  AddColorRow(panel, "hover bg", &cfg_.hover_bg);
  AddColorRow(panel, "text", &cfg_.text);
  AddColorRow(panel, "hover text", &cfg_.hover_text);
  AddColorRow(panel, "selected text", &cfg_.sel_text);
  AddColorRow(panel, "folder text", &cfg_.folder_text);
  AddColorRow(panel, "chevron", &cfg_.chevron);
  AddColorRow(panel, "badge bg", &cfg_.badge_bg);
  AddColorRow(panel, "badge text", &cfg_.badge_text);
  AddColorRow(panel, "close X", &cfg_.close_color);
  AddColorRow(panel, "plus text", &cfg_.plus_text);
  AddColorRow(panel, "plus bg", &cfg_.plus_bg);
  AddColorRow(panel, "plus hover", &cfg_.plus_hover_bg);
  AddColorRow(panel, "indent guide", &cfg_.indent_guide);
  AddColorRow(panel, "scrollbar", &cfg_.scrollbar_thumb);

  SectionLabel(panel, "ICON for selected workspace");
  {
    icon_field_ = panel->AddChildView(std::make_unique<views::Textfield>());
    icon_field_->GetViewAccessibility().SetName(u"icon for selected workspace");
    icon_field_->SetPlaceholderText(u"glyph/emoji, e.g.  ●  ▶  $");
    icon_field_->set_controller(this);
  }

  auto* reset = panel->AddChildView(std::make_unique<views::LabelButton>(
      base::BindRepeating(
          [](CmuxDemoExample* self) {
            self->cfg_ = cmux::RailConfig::Preset(
                self->rail_style_ >= 0 ? self->rail_style_ : 0);
            self->SyncControlsFromConfig();
            self->ApplyAndSave();
          },
          base::Unretained(this)),
      u"Reset to preset"));
  reset->SetTextColor(views::Button::STATE_NORMAL, SK_ColorWHITE);
}

void CmuxDemoExample::SyncControlsFromConfig() {
  syncing_ = true;
  for (auto& b : slider_binds_) {
    b.slider->SetValue(static_cast<float>(*b.field - b.lo) /
                       std::max(1, b.hi - b.lo));
    b.value_label->SetText(base::NumberToString16(*b.field));
  }
  for (auto& b : double_slider_binds_) {
    b.slider->SetValue(
        static_cast<float>((*b.field - b.lo) / std::max(0.01, b.hi - b.lo)));
    b.value_label->SetText(
        base::UTF8ToUTF16(base::StringPrintf("%.2f", *b.field)));
  }
  for (auto& b : checks_) {
    if (b.checkbox && b.field) {
      b.checkbox->SetChecked(*b.field);
    }
  }
  for (auto& [tf, field] : color_binds_) {
    tf->SetText(base::UTF8ToUTF16(cmux::ColorToHex(*field)));
    auto sit = swatches_.find(tf);
    if (sit != swatches_.end()) {
      sit->second->SetBackground(views::CreateSolidBackground(*field));
    }
  }
  for (auto& [tf, field] : layout_color_binds_) {
    tf->SetText(base::UTF8ToUTF16(*field));
    auto sit = swatches_.find(tf);
    if (sit != swatches_.end()) {
      sit->second->SetBackground(
          views::CreateSolidBackground(LayoutColorForField(field.get())));
    }
  }
  if (family_field_) {
    family_field_->SetText(base::UTF8ToUTF16(cfg_.font_family));
  }
  syncing_ = false;
}

void CmuxDemoExample::ApplyAndSave() {
  if (rail_) {
    rail_->SetConfig(RailConfigForDemo());
  }
  if (strip_) {
    strip_->SetAnimationConfig(layout_cfg_.animations,
                               std::max(0, layout_cfg_.animation_ms));
    strip_->SetAccentColor(cmux::LayoutAccentColor(layout_cfg_));
  }
  cmux::SaveRailConfig(cfg_);
}

void CmuxDemoExample::ApplyLayoutAndSave() {
  if (rail_) {
    rail_->SetPreferredSize(gfx::Size(std::max(0, layout_cfg_.rail_width), 0));
    rail_->SetConfig(RailConfigForDemo());
    if (rail_->parent()) {
      rail_->parent()->InvalidateLayout();
    }
  }
  if (strip_view_) {
    strip_view_->SetLayoutConfig(layout_cfg_);
  }
  if (strip_) {
    strip_->SetAnimationConfig(layout_cfg_.animations,
                               std::max(0, layout_cfg_.animation_ms));
    strip_->SetAccentColor(cmux::LayoutAccentColor(layout_cfg_));
  }
  cmux::SaveLayoutConfig(layout_cfg_);
  if (rail_) {
    rail_->InvalidateLayout();
  }
  if (strip_view_) {
    strip_view_->InvalidateLayout();
  }
}

cmux::RailConfig CmuxDemoExample::RailConfigForDemo() const {
  cmux::RailConfig rail_config = cfg_;
  rail_config.header_height = std::max(0, layout_cfg_.header_height);
  rail_config.animations = layout_cfg_.animations;
  rail_config.animation_ms = std::max(0, layout_cfg_.animation_ms);
  return rail_config;
}

void CmuxDemoExample::SliderValueChanged(views::Slider* sender,
                                         float value,
                                         float,
                                         views::SliderChangeReason) {
  if (syncing_) {
    return;
  }
  for (auto& b : slider_binds_) {
    if (b.slider != sender) {
      continue;
    }
    int v = b.lo + static_cast<int>(value * (b.hi - b.lo) + 0.5f);
    *b.field = v;
    b.value_label->SetText(base::NumberToString16(v));
    if (b.layout) {
      ApplyLayoutAndSave();
    } else {
      ApplyAndSave();
    }
    return;
  }
  for (auto& b : double_slider_binds_) {
    if (b.slider != sender) {
      continue;
    }
    double v = b.lo + value * (b.hi - b.lo);
    *b.field = v;
    b.value_label->SetText(base::UTF8ToUTF16(base::StringPrintf("%.2f", v)));
    if (b.layout) {
      ApplyLayoutAndSave();
    } else {
      ApplyAndSave();
    }
    return;
  }
}

void CmuxDemoExample::ContentsChanged(views::Textfield* sender,
                                      const std::u16string& new_contents) {
  if (syncing_) {
    return;
  }
  if (sender == family_field_) {
    cfg_.font_family = base::UTF16ToUTF8(new_contents);
    ApplyAndSave();
    return;
  }
  if (sender == icon_field_) {
    const cmux::Workspace* ws =
        model_.GetWorkspace(model_.selected_workspace());
    if (ws) {
      if (new_contents.empty()) {
        cfg_.icons.erase(ws->title);
      } else {
        cfg_.icons[ws->title] = base::UTF16ToUTF8(new_contents);
      }
      ApplyAndSave();
    }
    return;
  }
  auto it = color_binds_.find(sender);
  if (it != color_binds_.end()) {
    SkColor c;
    if (ParseHex(new_contents, &c)) {
      *it->second = c;
      auto sit = swatches_.find(sender);
      if (sit != swatches_.end()) {
        sit->second->SetBackground(views::CreateSolidBackground(c));
        sit->second->SchedulePaint();
      }
      ApplyAndSave();
    }
    return;
  }
  auto layout_it = layout_color_binds_.find(sender);
  if (layout_it != layout_color_binds_.end()) {
    const std::string value = base::UTF16ToUTF8(new_contents);
    SkColor c;
    if ((layout_it->second.get() == &layout_cfg_.focus_color &&
         value.empty()) ||
        cmux::ParseLayoutHexColor(value, &c)) {
      *layout_it->second = value;
      auto sit = swatches_.find(sender);
      if (sit != swatches_.end()) {
        sit->second->SetBackground(views::CreateSolidBackground(
            LayoutColorForField(layout_it->second.get())));
        sit->second->SchedulePaint();
      }
      ApplyLayoutAndSave();
    }
  }
}

// ---- Delegates --------------------------------------------------------------
void CmuxDemoExample::OnSelectWorkspace(cmux::WorkspaceId id) {
  model_.SelectWorkspace(id);
  RefreshRail();
  RefreshStrip();
  // Reflect the now-selected workspace's custom icon in the icon field.
  if (icon_field_) {
    const cmux::Workspace* ws = model_.GetWorkspace(id);
    syncing_ = true;
    std::string glyph;
    if (ws) {
      auto it = cfg_.icons.find(ws->title);
      if (it != cfg_.icons.end()) {
        glyph = it->second;
      }
    }
    icon_field_->SetText(base::UTF8ToUTF16(glyph));
    syncing_ = false;
  }
}

void CmuxDemoExample::OnExtendWorkspaceSelection(cmux::WorkspaceId id) {
  model_.ExtendWorkspaceSelectionTo(id);
  RefreshRail();
  RefreshStrip();
}

void CmuxDemoExample::OnAddWorkspaceSelectionFromAnchorTo(
    cmux::WorkspaceId id) {
  model_.AddWorkspaceSelectionFromAnchorTo(id);
  RefreshRail();
  RefreshStrip();
}

void CmuxDemoExample::OnToggleWorkspaceSelection(cmux::WorkspaceId id) {
  model_.ToggleWorkspaceSelection(id);
  RefreshRail();
  RefreshStrip();
}

void CmuxDemoExample::OnActivateWorkspaceInSelection(cmux::WorkspaceId id) {
  model_.ActivateWorkspaceInSelection(id);
  RefreshRail();
  RefreshStrip();
}

bool CmuxDemoExample::IsWorkspaceSelected(cmux::WorkspaceId id) const {
  return model_.IsWorkspaceSelected(id);
}

cmux::WorkspaceSelectionState CmuxDemoExample::GetWorkspaceSelectionState()
    const {
  return model_.GetWorkspaceSelectionState();
}

void CmuxDemoExample::OnRestoreWorkspaceSelectionState(
    const cmux::WorkspaceSelectionState& state) {
  model_.RestoreWorkspaceSelectionState(state);
  RefreshRail();
  RefreshStrip();
}

void CmuxDemoExample::OnNewWorkspace() {
  model_.AddWorkspace(cmux::SurfaceKind::kWeb, "New workspace");
  RefreshRail();
}

void CmuxDemoExample::OnToggleExpanded(cmux::WorkspaceId id) {
  const cmux::Workspace* ws = model_.GetWorkspace(id);
  const cmux::WorkspaceGroup* group =
      ws ? model_.GetWorkspaceGroup(ws->group) : nullptr;
  if (group) {
    model_.SetWorkspaceGroupCollapsed(group->id, !group->collapsed);
    RefreshRail();
    RefreshStrip();
  }
}

void CmuxDemoExample::OnRenameWorkspace(cmux::WorkspaceId id,
                                        const std::string& name) {
  if (!name.empty()) {
    model_.SetWorkspaceTitle(id, name);
    RefreshRail();
  }
}

void CmuxDemoExample::OnMoveWorkspace(cmux::WorkspaceId id,
                                      cmux::WorkspaceId new_parent,
                                      int index) {
  model_.MoveWorkspace(id, new_parent, index);
  RefreshRail();
}

void CmuxDemoExample::OnMoveWorkspaceGroup(cmux::WorkspaceGroupId id,
                                           int index) {
  const std::vector<cmux::WorkspaceId> members =
      model_.WorkspacesInGroup(id);
  if (members.empty()) {
    return;
  }
  model_.MoveWorkspaces(members, cmux::kInvalidId, index);
  RefreshRail();
}

void CmuxDemoExample::OnCloseWorkspace(cmux::WorkspaceId id) {
  model_.CloseWorkspace(id);
  RefreshRail();
  RefreshStrip();
}

void CmuxDemoExample::OnSelectTab(cmux::SurfaceTabId id) {
  selected_tab_ = id;
  RefreshTabs();
}

void CmuxDemoExample::OnCloseTab(cmux::SurfaceTabId id) {
  for (size_t i = 0; i < tabs_.size(); ++i) {
    if (tabs_[i].id != id) {
      continue;
    }
    tabs_.erase(tabs_.begin() + i);
    if (selected_tab_ == id && !tabs_.empty()) {
      selected_tab_ = tabs_[i < tabs_.size() ? i : tabs_.size() - 1].id;
    }
    break;
  }
  RefreshTabs();
}

void CmuxDemoExample::OnCloseOtherTabs(cmux::SurfaceTabId id) {
  auto it =
      std::find_if(tabs_.begin(), tabs_.end(),
                   [id](const cmux::SurfaceTab& tab) { return tab.id == id; });
  if (it == tabs_.end()) {
    return;
  }
  cmux::SurfaceTab keep = *it;
  tabs_.clear();
  tabs_.push_back(std::move(keep));
  selected_tab_ = id;
  RefreshTabs();
}

void CmuxDemoExample::OnNewTab(cmux::SurfaceKind kind) {
  cmux::SurfaceTab t = MakeTab(
      next_tab_id_++,
      kind == cmux::SurfaceKind::kTerminal ? "terminal" : "new tab", kind);
  selected_tab_ = t.id;
  tabs_.push_back(t);
  RefreshTabs();
}

void CmuxDemoExample::OnNewTabRight(cmux::SurfaceTabId id) {
  auto it =
      std::find_if(tabs_.begin(), tabs_.end(),
                   [id](const cmux::SurfaceTab& tab) { return tab.id == id; });
  const size_t index = it == tabs_.end()
                           ? tabs_.size()
                           : static_cast<size_t>(it - tabs_.begin()) + 1;
  cmux::SurfaceTab t =
      MakeTab(next_tab_id_++, "new tab", cmux::SurfaceKind::kWeb);
  selected_tab_ = t.id;
  tabs_.insert(tabs_.begin() + index, std::move(t));
  RefreshTabs();
}

void CmuxDemoExample::OnMoveTabToNewColumn(cmux::SurfaceTabId) {}

void CmuxDemoExample::OnSplitRightWithTab(cmux::SurfaceTabId) {}

void CmuxDemoExample::OnSplitDownWithTab(cmux::SurfaceTabId) {}

bool CmuxDemoExample::IsTabContextActionEnabled(cmux::SurfaceTabId,
                                                cmux::TabContextAction) const {
  return false;
}

bool CmuxDemoExample::IsTabContextActionToggled(cmux::SurfaceTabId,
                                                cmux::TabContextAction) const {
  return false;
}

void CmuxDemoExample::OnTabContextAction(cmux::SurfaceTabId,
                                         cmux::TabContextAction) {}

void CmuxDemoExample::OnTabDragStarted(cmux::SurfaceTabId,
                                       const gfx::Point&,
                                       const gfx::Point&) {}

void CmuxDemoExample::OnTabDragUpdated(const gfx::Point&) {}

void CmuxDemoExample::OnTabDragEnded(bool) {}

bool CmuxDemoExample::OnLiveReorderTab(cmux::SurfaceTabId, int) {
  return false;
}

void CmuxDemoExample::OnPaneDragStarted(const gfx::Point&, const gfx::Point&) {}

void CmuxDemoExample::OnPaneDragUpdated(const gfx::Point&) {}

void CmuxDemoExample::OnPaneDragEnded(bool) {}

void CmuxDemoExample::RefreshRail() {
  if (rail_) {
    rail_->Update(model_);
  }
}

void CmuxDemoExample::RefreshTabs() {
  if (strip_) {
    std::vector<cmux::TabVisual> visuals;
    visuals.reserve(tabs_.size());
    for (const cmux::SurfaceTab& tab : tabs_) {
      cmux::TabVisual visual;
      visual.tab = tab;
      visuals.push_back(std::move(visual));
    }
    strip_->SetTabs(visuals, selected_tab_);
  }
}

void CmuxDemoExample::RefreshStrip() {
  if (!strip_view_) {
    return;
  }
  strip_view_->SetLayoutConfig(layout_cfg_);
  const cmux::WorkspaceId selected = model_.selected_workspace();
  const cmux::Workspace* ws = model_.GetWorkspace(selected);
  strip_view_->SetWorkspace(&model_, selected,
                            ws ? ws->focused : cmux::kInvalidId);
}

std::unique_ptr<View> CmuxDemoExample::CreateStripPane(cmux::PaneId id,
                                                       const cmux::Pane& pane) {
  auto view = std::make_unique<views::View>();
  const SkColor tint =
      kPaneTints[static_cast<size_t>(id) % std::size(kPaneTints)];
  view->SetBackground(views::CreateSolidBackground(tint));
  auto* box = view->SetLayoutManager(std::make_unique<views::BoxLayout>(
      views::BoxLayout::Orientation::kVertical, gfx::Insets(14), 6));
  box->set_main_axis_alignment(views::BoxLayout::MainAxisAlignment::kStart);
  box->set_cross_axis_alignment(views::BoxLayout::CrossAxisAlignment::kStart);

  const cmux::SurfaceTab* tab = pane.SelectedTab();
  const bool is_term = tab && tab->kind == cmux::SurfaceKind::kTerminal;
  std::u16string title = tab && !tab->title.empty()
                             ? base::UTF8ToUTF16(tab->title)
                             : (is_term ? u"Terminal" : u"Web");
  auto* header = view->AddChildView(std::make_unique<views::Label>(title));
  header->SetEnabledColor(SkColorSetRGB(0xf2, 0xf3, 0xf6));
  header->SetHorizontalAlignment(gfx::ALIGN_LEFT);

  auto* sub = view->AddChildView(
      std::make_unique<views::Label>(is_term ? u"$ █" : u"a column pane"));
  sub->SetEnabledColor(SkColorSetRGB(0x8b, 0x93, 0xa6));
  sub->SetHorizontalAlignment(gfx::ALIGN_LEFT);
  return view;
}

}  // namespace views::examples
