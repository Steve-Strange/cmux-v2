// Copyright 2014 The Chromium Authors
// Copyright 2026 Manaflow, Inc.
// Portions derived from Helium; Helium contributors retain copyright.
// SPDX-License-Identifier: GPL-3.0-or-later AND GPL-3.0-only AND BSD-3-Clause
//
// Contains Chromium-derived regions; see docs/source-provenance.md.

#include "chrome/browser/cmux_term/cmux_browser_pane.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "base/callback_list.h"
#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/observer_list.h"
#include "base/scoped_multi_source_observation.h"
#include "base/scoped_observation.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "cc/paint/paint_flags.h"
#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/cmux_term/cmux_browser_finder.h"
#include "chrome/browser/cmux_term/cmux_extension_strip.h"
#include "chrome/browser/cmux_term/cmux_layout_config.h"
#include "chrome/browser/cmux_term/cmux_side_panel.h"
#include "chrome/browser/cmux_term/cmux_strip_controller.h"
#include "chrome/browser/cmux_term/cmux_surface.h"
#include "chrome/browser/cmux_term/cmux_toolbar_menus.h"
#include "chrome/browser/cmux_term/cmux_views.h"
#include "chrome/browser/command_updater.h"
#include "chrome/browser/command_updater_delegate.h"
#include "chrome/browser/command_updater_impl.h"
#include "chrome/browser/content_settings/mixed_content_settings_tab_helper.h"
#include "chrome/browser/content_settings/page_specific_content_settings_delegate.h"
#include "chrome/browser/devtools/devtools_ui_bindings.h"
#include "chrome/browser/extensions/api/web_navigation/web_navigation_tab_observer.h"
#include "chrome/browser/extensions/app_tab_helper.h"
#include "chrome/browser/extensions/navigation_extension_enabler.h"
#include "chrome/browser/extensions/tab_helper.h"
#include "chrome/browser/favicon/favicon_utils.h"
#include "chrome/browser/password_manager/chrome_password_manager_client.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/sessions/session_tab_helper_factory.h"
#include "chrome/browser/ssl/chrome_security_state_tab_helper.h"
#include "chrome/browser/subresource_filter/chrome_content_subresource_filter_web_contents_helper_factory.h"
#include "chrome/browser/ui/autofill/chrome_autofill_client.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_content_setting_bubble_model_delegate.h"
#include "chrome/browser/ui/browser_window/public/browser_window_features.h"
#include "chrome/browser/ui/blocked_content/framebust_block_tab_helper.h"
#include "chrome/browser/ui/color/chrome_color_id.h"
#include "chrome/browser/ui/content_settings/content_setting_bubble_model_delegate.h"
#if BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC) || BUILDFLAG(IS_LINUX)
#include "chrome/browser/ui/global_media_controls/media_toolbar_button_controller.h"
#endif
#include "chrome/browser/ui/omnibox/chrome_omnibox_client.h"
#include "chrome/browser/ui/omnibox/omnibox_controller.h"
#include "chrome/browser/ui/omnibox/omnibox_edit_model.h"
#include "chrome/browser/ui/omnibox/omnibox_view.h"
#include "chrome/browser/ui/passwords/manage_passwords_ui_controller.h"
#include "chrome/browser/ui/search/search_tab_helper.h"
#include "chrome/browser/ui/tab_dialogs.h"
#include "chrome/browser/ui/toolbar/chrome_location_bar_model_delegate.h"
#if BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC) || BUILDFLAG(IS_LINUX)
#include "chrome/browser/ui/views/global_media_controls/media_toolbar_button_contextual_menu.h"
#include "chrome/browser/ui/views/global_media_controls/media_toolbar_button_view.h"
#endif
#include "chrome/browser/ui/views/location_bar/location_bar_view.h"
#include "chrome/browser/ui/views/toolbar/home_button.h"
#include "chrome/browser/ui/views/toolbar/pinned_toolbar_actions_container.h"
#include "chrome/browser/ui/views/toolbar/reload_button.h"
#include "chrome/browser/ui/views/toolbar/toolbar_button.h"
#include "chrome/common/chrome_version.h"
#include "components/blocked_content/popup_blocker_tab_helper.h"
#include "components/content_settings/browser/page_specific_content_settings.h"
#include "components/favicon/content/content_favicon_driver.h"
#include "components/favicon/core/favicon_driver_observer.h"
#include "components/infobars/content/content_infobar_manager.h"
#include "components/input/native_web_keyboard_event.h"
#include "components/omnibox/browser/autocomplete_match.h"
#include "components/omnibox/browser/location_bar_model_impl.h"
#include "components/omnibox/browser/omnibox_client.h"
#include "components/safe_browsing/buildflags.h"
#include "components/search_engines/template_url.h"
#include "components/vector_icons/vector_icons.h"
#include "components/web_modal/modal_dialog_host.h"
#include "components/web_modal/web_contents_modal_dialog_host.h"
#include "components/web_modal/web_contents_modal_dialog_manager.h"
#include "components/zoom/zoom_controller.h"
#include "content/public/browser/devtools_agent_host.h"
#include "content/public/browser/invalidate_type.h"
#include "content/public/browser/keyboard_event_processing_result.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/reload_type.h"
#include "content/public/browser/render_view_host.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_delegate.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/common/content_constants.h"
#include "content/public/common/referrer.h"
#include "extensions/browser/view_type_utils.h"
#include "extensions/common/mojom/view_type.mojom.h"
#include "third_party/blink/public/common/input/web_input_event.h"
#include "third_party/skia/include/core/SkPath.h"
#include "third_party/skia/include/core/SkRRect.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/page_transition_types.h"
#include "ui/base/window_open_disposition.h"
#include "ui/color/color_provider.h"
#include "ui/compositor/layer.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/insets.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/gfx/geometry/rounded_corners_f.h"
#include "ui/gfx/geometry/skia_conversions.h"
#include "ui/gfx/image/image.h"
#include "ui/gfx/image/image_skia.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/background.h"
#include "ui/views/border.h"
#include "ui/views/controls/label.h"
#include "ui/views/controls/webview/unhandled_keyboard_event_handler.h"
#include "ui/views/controls/webview/webview.h"
#include "ui/views/focus/focus_manager.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/layout/fill_layout.h"
#include "ui/views/view.h"
#include "ui/views/view_observer.h"
#include "ui/views/widget/widget.h"
#include "ui/views/widget/widget_delegate.h"
#include "ui/views/widget/widget_observer.h"
#include "url/gurl.h"

#if BUILDFLAG(SAFE_BROWSING_AVAILABLE)
#include "chrome/browser/safe_browsing/chrome_password_reuse_detection_manager_client.h"
#endif

namespace cmux {
namespace {

// Chromium 151 renamed the pre-Material-refresh vector icons by appending
// "Old". Keep cmux's existing Helium-era glyphs on both supported Chromium
// branches instead of silently changing the toolbar artwork during the 151
// rebase.
const gfx::VectorIcon& BackArrowIcon() {
#if CHROME_VERSION_MAJOR >= 151
  return vector_icons::kBackArrowChromeRefreshOldIcon;
#else
  return vector_icons::kBackArrowChromeRefreshIcon;
#endif
}

const gfx::VectorIcon& ForwardArrowIcon() {
#if CHROME_VERSION_MAJOR >= 151
  return vector_icons::kForwardArrowChromeRefreshOldIcon;
#else
  return vector_icons::kForwardArrowChromeRefreshIcon;
#endif
}

const gfx::VectorIcon& LaunchIcon() {
#if CHROME_VERSION_MAJOR >= 151
  return vector_icons::kLaunchChromeRefreshOldIcon;
#else
  return vector_icons::kLaunchChromeRefreshIcon;
#endif
}

const gfx::VectorIcon& CloseIcon() {
#if CHROME_VERSION_MAJOR >= 151
  return vector_icons::kCloseChromeRefreshOldIcon;
#else
  return vector_icons::kCloseChromeRefreshIcon;
#endif
}

gfx::RoundedCornersF RoundedFrameCorners(
    const RoundedFrameGeometry& geometry) {
  if (!geometry.enabled) {
    return gfx::RoundedCornersF();
  }
  return gfx::RoundedCornersF(
      geometry.top_left_radius, geometry.top_right_radius,
      geometry.bottom_right_radius, geometry.bottom_left_radius);
}

// Helium uses ContentsContainerOutline for this stroke. cmux's browser
// surface is intentionally independent of BrowserView, so reproduce that
// view's ordinary-browser path: a one-DIP anti-aliased separator stroke inset
// by half its thickness from the eight-DIP clipped content edge.
class RoundedFrameOutlineView : public views::View {
  METADATA_HEADER(RoundedFrameOutlineView, views::View)

 public:
  RoundedFrameOutlineView() {
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);
    SetCanProcessEventsWithinSubtree(false);
    GetViewAccessibility().SetIsInvisible(true);
    SetVisible(false);
  }

  void SetFrameGeometry(const RoundedFrameGeometry& geometry) {
    geometry_ = geometry;
    SetVisible(geometry.enabled);
    if (geometry.enabled) {
      SchedulePaint();
    }
  }

  void OnThemeChanged() override {
    views::View::OnThemeChanged();
    SchedulePaint();
  }

  void OnPaint(gfx::Canvas* canvas) override {
    if (!GetColorProvider()) {
      return;
    }
    cc::PaintFlags flags;
    flags.setStrokeWidth(kRoundedFrameOutlineThickness);
    flags.setColor(
        GetColorProvider()->GetColor(kColorToolbarContentAreaSeparator));
    flags.setStyle(cc::PaintFlags::kStroke_Style);
    flags.setAntiAlias(true);

    gfx::RectF bounds(GetLocalBounds());
    const float half_thickness = kRoundedFrameOutlineThickness / 2.0f;
    bounds.Inset(half_thickness);
    const auto adjusted_radius = [half_thickness](int radius) {
      return std::max(0.0f, radius - half_thickness);
    };
    const SkVector radii[4] = {
        {adjusted_radius(geometry_.top_left_radius),
         adjusted_radius(geometry_.top_left_radius)},
        {adjusted_radius(geometry_.top_right_radius),
         adjusted_radius(geometry_.top_right_radius)},
        {adjusted_radius(geometry_.bottom_right_radius),
         adjusted_radius(geometry_.bottom_right_radius)},
        {adjusted_radius(geometry_.bottom_left_radius),
         adjusted_radius(geometry_.bottom_left_radius)}};
    canvas->DrawPath(
        SkPath::RRect(
            SkRRect::MakeRectRadii(gfx::RectFToSkRect(bounds), radii)),
        flags);
  }

 private:
  RoundedFrameGeometry geometry_;
};

BEGIN_METADATA(RoundedFrameOutlineView)
END_METADATA

// DevToolsUIBindings::SetDelegate takes UNIQUE_PTR OWNERSHIP of the delegate
// (delegate_.reset(delegate)) -- upstream's DevToolsWindow is deliberately
// owned-and-deleted by its bindings that way. Our surface view is owned by the
// views hierarchy, so it must never be handed to SetDelegate directly (that
// double-destroys the surface when the pane closes: ~CmuxBrowserSurface ->
// ~WebContents -> ~DevToolsUIBindings -> delete delegate_ == the surface
// again). Instead the bindings own this small proxy, which forwards to the
// surface while it is alive. Methods are defined after CmuxBrowserSurface
// (they need its definition).
class CmuxBrowserSurface;
class CmuxDevToolsDelegateProxy : public DevToolsUIBindings::Delegate {
 public:
  explicit CmuxDevToolsDelegateProxy(base::WeakPtr<CmuxBrowserSurface> surface)
      : surface_(std::move(surface)) {}

  content::WebContents* GetInspectedWebContents() override;
  void ActivateWindow() override;
  void CloseWindow() override;
  void Inspect(scoped_refptr<content::DevToolsAgentHost> host) override {}
  void SetInspectedPageBounds(const gfx::Rect& rect) override {}
  void InspectElementCompleted() override {}
  void SetIsDocked(bool is_docked) override {}
  void OpenInNewTab(const std::string& url) override {}
  void OpenSearchResultsInNewTab(const std::string& query) override {}
  void SetWhitelistedShortcuts(const std::string& message) override {}
  void SetEyeDropperActive(bool active) override {}
  void OpenNodeFrontend() override {}
  void InspectedContentsClosing() override {}
  void OnLoadCompleted() override {}
  void ReadyForTest() override {}
  void ConnectionReady() override {}
  void SetOpenNewWindowForPopups(bool value) override {}
  infobars::ContentInfoBarManager* GetInfoBarManager() override;
  void RenderProcessGone(bool crashed) override {}
  void ShowCertificateViewer(const std::string& cert_chain) override {}
  int GetDockStateForLogging() override { return 0; }
  int GetOpenedByForLogging() override { return 0; }
  int GetClosedByForLogging() override { return 0; }

 private:
  base::WeakPtr<CmuxBrowserSurface> surface_;
};

// BrowserView normally owns a TabModalDialogHost for each contents container.
// cmux deliberately uses a pane-local WebView inside one shared Widget, so
// provide the same WebContentsModalDialogHost contract at that pane boundary.
// Chromium's constrained-window machinery remains responsible for dialog
// lifecycle, tab blocking, focus, and security z-order.
class CmuxWebContentsModalDialogHost final
    : public web_modal::WebContentsModalDialogHost,
      public views::WidgetObserver,
      public views::ViewObserver {
 public:
  explicit CmuxWebContentsModalDialogHost(views::View* content_view)
      : content_view_(content_view) {
    CHECK(content_view_);
    content_view_observation_.Observe(content_view_);
    RefreshAncestorObservations();
    RefreshWidgetObservation();
    visible_bounds_observation_ = std::make_unique<
        views::View::ScopedNotifyObserversOnVisibleBoundsChanged>(
        *content_view_);
  }

  CmuxWebContentsModalDialogHost(const CmuxWebContentsModalDialogHost&) =
      delete;
  CmuxWebContentsModalDialogHost& operator=(
      const CmuxWebContentsModalDialogHost&) = delete;

  ~CmuxWebContentsModalDialogHost() override {
    // The visible-bounds scope can synchronously notify this ViewObserver when
    // it unregisters. Tear it down while every dependent member is still
    // alive, then detach the remaining callback sources before member
    // destruction reaches observer_list_.
    visible_bounds_observation_.reset();
    content_view_observation_.Reset();
    ancestor_view_observations_.RemoveAllObservations();
    widget_observation_.Reset();
    observer_list_.Notify(
        &web_modal::ModalDialogHostObserver::OnHostDestroying);
  }

  // web_modal::ModalDialogHost:
  gfx::NativeView GetHostView() const override {
    views::Widget* widget = content_view_->GetWidget();
    return widget ? widget->GetNativeView() : gfx::NativeView();
  }

  gfx::Point GetDialogPosition(const gfx::Size& dialog_size) override {
    gfx::Rect content_bounds = GetVisibleContentBoundsInWidget();
    const int minimum_x = content_bounds.x();
    const int maximum_x =
        std::max(minimum_x, content_bounds.right() - dialog_size.width());
    const int centered_x =
        content_bounds.CenterPoint().x() - dialog_size.width() / 2;
    return gfx::Point(
        std::clamp(centered_x, minimum_x, maximum_x),
        std::max(0, content_bounds.y() - kConstrainedWindowOverlap));
  }

  bool ShouldActivateDialog() const override {
    views::Widget* widget = content_view_->GetWidget();
    return widget && !GetVisibleContentBoundsInWidget().IsEmpty() &&
           widget->ShouldPaintAsActive();
  }

  bool ShouldConstrainDialogBoundsByHost() override { return true; }

  void AddObserver(
      web_modal::ModalDialogHostObserver* observer) override {
    observer_list_.AddObserver(observer);
  }

  void RemoveObserver(
      web_modal::ModalDialogHostObserver* observer) override {
    observer_list_.RemoveObserver(observer);
  }

  // web_modal::WebContentsModalDialogHost:
  gfx::Size GetMaximumDialogSize() override {
    gfx::Rect content_bounds = GetVisibleContentBoundsInWidget();
    const int dialog_y =
        std::max(0, content_bounds.y() - kConstrainedWindowOverlap);
    return gfx::Size(std::max(0, content_bounds.width()),
                     std::max(0, content_bounds.bottom() - dialog_y));
  }

  // views::ViewObserver:
  void OnViewAddedToWidget(views::View* observed_view) override {
    if (observed_view == content_view_) {
      RefreshWidgetObservation();
    }
    NotifyPositionRequiresUpdate();
  }

  void OnViewVisibleBoundsChanged(views::View*) override {
    NotifyPositionRequiresUpdate();
  }

  void OnViewBoundsChanged(views::View*) override {
    NotifyPositionRequiresUpdate();
  }

  void OnViewLayerBoundsSet(views::View*) override {
    NotifyPositionRequiresUpdate();
  }

  void OnViewLayerTransformed(views::View*) override {
    NotifyPositionRequiresUpdate();
  }

  void OnViewHierarchyChanged(
      views::View* observed_view,
      const views::ViewHierarchyChangedDetails& details) override {
    if (observed_view == content_view_) {
      // Removal notifications run before Chromium clears the old parent
      // pointer. Stop before that old parent so a detached/reparented surface
      // never retains observations on an ancestor that can be destroyed
      // before the matching add notification.
      RefreshAncestorObservations(details.is_add ? nullptr
                                                 : details.parent.get());
      NotifyPositionRequiresUpdate();
    }
  }

  void OnViewRemovedFromWidget(views::View* observed_view) override {
    if (observed_view == content_view_) {
      widget_observation_.Reset();
    }
    NotifyPositionRequiresUpdate();
  }

  // views::WidgetObserver:
  void OnWidgetDestroying(views::Widget*) override {
    widget_observation_.Reset();
  }

  void OnWidgetBoundsChanged(views::Widget*,
                             const gfx::Rect&) override {
    NotifyPositionRequiresUpdate();
  }

 private:
  static constexpr int kConstrainedWindowOverlap = 3;

  void RefreshWidgetObservation() {
    views::Widget* widget = content_view_->GetWidget();
    if (!widget) {
      widget_observation_.Reset();
      return;
    }
    if (widget_observation_.IsObservingSource(widget)) {
      return;
    }
    widget_observation_.Reset();
    widget_observation_.Observe(widget);
  }

  gfx::Rect GetVisibleContentBoundsInWidget() const {
    if (!content_view_->GetWidget()) {
      return gfx::Rect();
    }
    // Panes scroll beneath an ancestor layer that masks to the workspace
    // viewport. Keep constrained dialogs inside the portion users can
    // actually see, not the WebView's potentially offscreen local bounds.
    return content_view_->ConvertRectToWidget(
        content_view_->GetVisibleBounds());
  }

  void RefreshAncestorObservations(views::View* stop_before = nullptr) {
    ancestor_view_observations_.RemoveAllObservations();
    for (views::View* ancestor = content_view_->parent(); ancestor;
         ancestor = ancestor->parent()) {
      if (ancestor == stop_before) {
        break;
      }
      ancestor_view_observations_.AddObservation(ancestor);
    }
  }

  void NotifyPositionRequiresUpdate() {
    observer_list_.Notify(
        &web_modal::ModalDialogHostObserver::OnPositionRequiresUpdate);
  }

  const raw_ptr<views::View> content_view_;
  base::ScopedObservation<views::View, views::ViewObserver>
      content_view_observation_{this};
  base::ScopedMultiSourceObservation<views::View, views::ViewObserver>
      ancestor_view_observations_{this};
  std::unique_ptr<
      views::View::ScopedNotifyObserversOnVisibleBoundsChanged>
      visible_bounds_observation_;
  base::ScopedObservation<views::Widget, views::WidgetObserver>
      widget_observation_{this};
  base::ObserverList<web_modal::ModalDialogHostObserver> observer_list_;
};

// ---- Chrome pane: real LocationBarView over a Browser-owned WebContents. ---

class CmuxBrowserSurface : public views::View,
                           public CmuxSurface,
                           public content::WebContentsObserver,
                           public content::RenderWidgetHost::InputEventObserver,
                           public favicon::FaviconDriverObserver,
                           public LocationBarView::Delegate,
                           public ChromeLocationBarModelDelegate,
                           public CommandUpdaterDelegate,
                           public views::WidgetObserver {
  METADATA_HEADER(CmuxBrowserSurface, views::View)

 public:
  CmuxBrowserSurface(content::WebContents* web_contents, PaneId pane)
      : web_contents_(web_contents),
        pane_(pane),
        location_bar_model_(std::make_unique<LocationBarModelImpl>(
            this,
            content::kMaxURLDisplayChars)),
        command_updater_(this) {
    CHECK(web_contents_);
    CHECK_NE(pane_, kInvalidId);
    browser_ = cmux::FindBrowserWithTab(web_contents_);
    CHECK(browser_);
    // The pane-local ReloadButton drives reload/stop through this surface's
    // CommandUpdater while Browser owns the underlying tab lifecycle.
    command_updater_.UpdateCommandEnabled(IDC_RELOAD, true);
    command_updater_.UpdateCommandEnabled(IDC_RELOAD_BYPASSING_CACHE, true);
    command_updater_.UpdateCommandEnabled(IDC_RELOAD_CLEARING_CACHE, true);
    command_updater_.UpdateCommandEnabled(IDC_STOP, true);
    // BrowserTabStripModelDelegate/TabHelpers owns the complete Chrome helper
    // attachment path. Creating helpers here would duplicate per-tab Chrome
    // state and trip singleton WebContentsUserData checks.
    favicon_driver_ =
        favicon::ContentFaviconDriver::FromWebContents(web_contents_);
    if (favicon_driver_) {
      favicon_driver_->AddObserver(this);
    }

    auto* profile =
        Profile::FromBrowserContext(web_contents_->GetBrowserContext());
    Observe(web_contents_);

    web_view_ = new views::WebView(profile);
    web_view_->SetWebContents(web_contents_);
    web_view_->set_allow_accelerators(true);
    // Reliable signal that the page body took focus (a native first-responder
    // change the Views FocusManager may not see); drives the strip's column
    // focus + niri scroll-into-view. See OnWebViewFocused.
    web_focus_sub_ =
        web_view_->AddWebContentsFocusedCallback(base::BindRepeating(
            &CmuxBrowserSurface::OnWebViewFocused,
            weak_factory_.GetWeakPtr()));
    RefreshRenderWidgetInputObserver();
    location_bar_view_ = new LocationBarView(
        browser_, profile, &command_updater_, this,
        /*is_popup_mode=*/false);

    auto box = std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kVertical);
    box->set_cross_axis_alignment(
        views::BoxLayout::CrossAxisAlignment::kStretch);
    auto* layout = SetLayoutManager(std::move(box));

    // Top toolbar row reusing Chrome's ToolbarButton + nav vector icons:
    // [back][forward][reload][omnibox]. Wired straight to the WebContents'
    // NavigationController.
    auto* toolbar_row = new views::View();
    toolbar_row_ = toolbar_row;
    // Sit the buttons + omnibox on Chrome's themed toolbar background so the
    // standard ToolbarButton icon colors have proper contrast (otherwise they
    // render faint on the dark strip background and look disabled).
    toolbar_row->SetBackground(views::CreateSolidBackground(kColorToolbar));
    auto trow = std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kHorizontal);
    trow->set_cross_axis_alignment(
        views::BoxLayout::CrossAxisAlignment::kCenter);
    // Helium's desktop toolbar uses 28-DIP controls with a 3-DIP interior
    // margin. Its ordinary classic-window layout then overlaps the 34-DIP tab
    // strip and toolbar by 3 DIP, placing the omnibox at the tab-strip bottom.
    // cmux stacks those rows in separate views, so fold that overlap into the
    // toolbar row as 0 DIP above / 3 DIP below: the painted tab-to-omnibox gap
    // is 3 DIP and the combined rows are 65 DIP tall. Horizontal margins
    // remain Helium's 6 DIP.
    trow->set_between_child_spacing(3);
    trow->set_inside_border_insets(gfx::Insets::TLBR(0, 6, 3, 6));
    auto* trow_layout = toolbar_row->SetLayoutManager(std::move(trow));
    auto add_nav_button = [&](const gfx::VectorIcon& icon,
                              const std::u16string& name, auto fn) {
      auto* b = new ToolbarButton(
          base::BindRepeating(fn, weak_factory_.GetWeakPtr()));
      b->SetVectorIcon(icon);
      b->SetTooltipText(name);
      b->GetViewAccessibility().SetName(name);
      toolbar_row->AddChildViewRaw(b);
      return b;
    };
    back_button_ =
        add_nav_button(BackArrowIcon(), u"Back", &CmuxBrowserSurface::GoBack);
    forward_button_ = add_nav_button(ForwardArrowIcon(), u"Forward",
                                     &CmuxBrowserSurface::GoForward);
    // Reload/Stop: Chrome's real ReloadButton. It presses through our
    // CommandUpdater (IDC_RELOAD/IDC_STOP, enabled above and handled in
    // ExecuteCommandWithDisposition) and gets the upstream hover guard,
    // double-click guard, stop semantics, reload-mode menu, and a11y for free.
    // We drive its reload<->stop(X) mode from the page load state via
    // ChangeMode(), exactly as BrowserView::UpdateReloadStopState does.
    auto* reload = new ReloadButton(profile, &command_updater_,
                                    /*window_metrics_manager=*/nullptr);
    reload_button_ = reload;
    toolbar_row->AddChildViewRaw(reload);
    home_button_ = toolbar_row->AddChildView(
        std::make_unique<HomeButton>(
            browser_,
            base::BindRepeating(&CmuxBrowserSurface::GoHome,
                                weak_factory_.GetWeakPtr())));
    toolbar_row->AddChildViewRaw(location_bar_view_.get());
    trow_layout->SetFlexForView(location_bar_view_, 1);
    // Chromium's native pinned-action model owns Helium's full Customize
    // Toolbar inventory (passwords, bookmarks, history, print, translate,
    // cast, reading mode, developer tools, and the rest). The small upstream
    // constructor seam lets this pane-local toolbar host the same container
    // from its real Browser without manufacturing a mutually exclusive
    // BrowserView. CmuxPaneView's pre-target mouse handler activates this
    // pane before a child button dispatches its Browser action.
    auto pinned_actions =
        std::make_unique<PinnedToolbarActionsContainer>(
            browser_, gfx::Size(28, 28));
    pinned_toolbar_actions_ = pinned_actions.get();
    toolbar_row->AddChildView(std::move(pinned_actions));
    ActivateToolbarHost();
    // Native extension action buttons, including Chrome's popup/toggle path.
    extensions_group_ =
        toolbar_row->AddChildView(std::make_unique<views::View>());
    auto extensions_layout = std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kHorizontal);
    extensions_layout->set_cross_axis_alignment(
        views::BoxLayout::CrossAxisAlignment::kCenter);
    extensions_group_->SetLayoutManager(std::move(extensions_layout));
    extensions_group_->AddChildViewRaw(
        new CmuxExtensionStrip(web_contents_, pane_));
    auto downloads = CreateCmuxDownloadsButton(
        web_contents_,
        base::BindRepeating(&CmuxBrowserSurface::ActivateForToolbarCommand,
                            weak_factory_.GetWeakPtr()));
    downloads_button_ = downloads.get();
    toolbar_row->AddChildView(std::move(downloads));
#if BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC) || BUILDFLAG(IS_LINUX)
    auto media_button = std::make_unique<MediaToolbarButtonView>(
        browser_,
        std::make_unique<MediaToolbarButtonContextualMenu>(browser_));
    media_button_ = media_button.get();
    toolbar_row->AddChildView(std::move(media_button));
#endif
    auto profile_button = CreateCmuxProfileButton(
        web_contents_,
        base::BindRepeating(&CmuxBrowserSurface::ActivateForToolbarCommand,
                            weak_factory_.GetWeakPtr()));
    profile_button_ = profile_button.get();
    toolbar_row->AddChildView(std::move(profile_button));
    auto menu_button = CreateCmuxAppMenuButton(
        web_contents_,
        base::BindRepeating(&CmuxBrowserSurface::ActivateForToolbarCommand,
                            weak_factory_.GetWeakPtr()));
    menu_button_ = menu_button.get();
    toolbar_row->AddChildView(std::move(menu_button));
    layout_config_subscription_ = AddLayoutConfigChangedCallback(
        base::BindRepeating(&CmuxBrowserSurface::ApplyToolbarCustomization,
                            weak_factory_.GetWeakPtr()));
    ApplyToolbarCustomization(
        GetPublishedLayoutConfig().value_or(LayoutConfig()));
    AddChildViewRaw(toolbar_row);
    layout->SetFlexForView(toolbar_row, 0);

    // Horizontal content row holding the page WebView and, when DevTools is
    // open, the inspector WebView beside it. Keeping DevTools INSIDE the pane
    // (vs. a separate strip column) binds a page and its inspector into one
    // niri column, so they're always visible next to each other and
    // move/scroll/close together.
    content_frame_ = new views::View();
    content_frame_->SetBackground(
        views::CreateSolidBackground(kColorToolbar));
    content_frame_->SetLayoutManager(std::make_unique<views::FillLayout>());
    AddChildViewRaw(content_frame_.get());
    layout->SetFlexForView(content_frame_, 1);

    content_row_ = new views::View();
    auto row = std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kHorizontal);
    row->set_cross_axis_alignment(
        views::BoxLayout::CrossAxisAlignment::kStretch);
    content_row_layout_ = content_row_->SetLayoutManager(std::move(row));
    content_row_->SetBackground(views::CreateSolidBackground(kColorToolbar));
    content_row_->SetPaintToLayer();
    content_row_->layer()->SetMasksToBounds(true);
    content_row_->layer()->SetIsFastRoundedCorner(true);
    content_frame_->AddChildViewRaw(content_row_.get());
    content_row_->AddChildViewRaw(web_view_.get());
    content_row_layout_->SetFlexForView(web_view_, 1);
    // Chrome normally owns one SidePanel inside BrowserView. cmux has no
    // BrowserView and can present several selected tabs from the workspace's
    // Browser at once, so every browser surface carries its own host. The host
    // is a fixed-width child of this row: opening it reduces WebView width and
    // never changes the surrounding pane/column geometry.
    side_panel_host_ =
        AddCmuxSidePanelHost(content_row_, browser_, web_contents_, pane_);
    content_row_layout_->SetFlexForView(side_panel_host_, 0);
    modal_dialog_host_ =
        std::make_unique<CmuxWebContentsModalDialogHost>(web_view_);
    rounded_frame_outline_ =
        content_frame_->AddChildView(std::make_unique<RoundedFrameOutlineView>());
  }

  CmuxBrowserSurface(const CmuxBrowserSurface&) = delete;
  CmuxBrowserSurface& operator=(const CmuxBrowserSurface&) = delete;
  ~CmuxBrowserSurface() override {
    // Invalidate weak pointers FIRST: destroying devtools_contents_ below runs
    // ~DevToolsUIBindings, whose owned CmuxDevToolsDelegateProxy must already
    // see this surface as gone (its raw delegate pointer would otherwise be a
    // half-destroyed view).
    weak_factory_.InvalidateWeakPtrs();
    // Constrained dialogs must observe host destruction before the WebView is
    // detached from its Browser-owned WebContents.
    modal_dialog_host_.reset();
    if (browser_ && pinned_toolbar_actions_ &&
        browser_->GetFeatures().pinned_toolbar_actions() ==
            pinned_toolbar_actions_) {
      browser_->GetFeatures().SetPinnedToolbarActionsForCustomWindow(nullptr);
    }
    if (favicon_driver_) {
      favicon_driver_->RemoveObserver(this);
      favicon_driver_ = nullptr;
    }
    // If the inspector is undocked, destroy its window first (synchronously),
    // with the WebView detached so the window teardown never touches the
    // WebContents we are about to destroy. RemoveObserver first: the manual
    // detach below replaces what OnWidgetDestroying would do.
    if (devtools_window_) {
      devtools_window_->RemoveObserver(this);
      UnregisterCmuxChromeSurfaceThemeWidget(devtools_window_);
      if (devtools_window_view_) {
        devtools_window_view_->SetWebContents(nullptr);
        devtools_window_view_ = nullptr;
      }
      views::Widget* w = devtools_window_;
      devtools_window_ = nullptr;
      w->CloseNow();
    }
    // Destroy the DevTools contents before our other members; its bindings
    // (and their proxy delegate) die with it.
    devtools_bindings_ = nullptr;  // weak; real object dies with the contents
    if (devtools_view_) {
      devtools_view_->SetWebContents(nullptr);
    }
    devtools_contents_.reset();
    // Detach the Browser-owned page before destroying this presentation.
    if (web_view_) {
      web_view_->SetWebContents(nullptr);
    }
    UnregisterRenderWidgetInputObserver();
    Observe(nullptr);
    web_contents_ = nullptr;
  }

  void AddedToWidget() override {
    if (!location_bar_inited_) {
      location_bar_inited_ = true;
      location_bar_view_->Init();
    }
    SyncCmuxSidePanelForWebContents(browser_, web_contents_);
  }

  void VisibilityChanged(views::View* starting_from,
                         bool is_visible) override {
    views::View::VisibilityChanged(starting_from, is_visible);
    if (is_visible) {
      SyncCmuxSidePanelForWebContents(browser_, web_contents_);
    }
  }

  // CmuxSurface:
  views::View* AsView() override { return this; }
  SurfaceKind kind() const override { return SurfaceKind::kWeb; }
  void ActivateToolbarHost() override {
    if (browser_ && pinned_toolbar_actions_) {
      browser_->GetFeatures().SetPinnedToolbarActionsForCustomWindow(
          pinned_toolbar_actions_);
    }
  }
  void FocusContent() override {
    // Route through the views FocusManager (not web_contents_->Focus()) so the
    // views focus and the NSWindow first responder stay in sync; views::WebView
    // forwards focus into the WebContents itself.
    if (web_view_) {
      web_view_->RequestFocus();
    }
  }
  // The strip's uniform activation signal. A click on the page *body* takes the
  // RenderWidgetHostView native first responder and never reaches the Views
  // FocusManager, so we subscribe to views::WebView's web-contents-focused
  // callback (wired in the ctor) and forward it here -- this is what makes
  // clicking a partially-offscreen page focus + scroll in its column.
  void SetActivationCallback(base::RepeatingClosure callback) override {
    on_activated_ = std::move(callback);
  }
  void SetInteractionCallback(base::RepeatingClosure callback) override {
    on_interaction_ = std::move(callback);
  }
  void SetTitleChangedCallback(
      base::RepeatingCallback<void(const std::u16string&)> callback) override {
    on_title_changed_ = std::move(callback);
  }
  void SetFaviconChangedCallback(
      base::RepeatingCallback<void(const gfx::ImageSkia&)> callback) override {
    on_favicon_changed_ = std::move(callback);
    if (on_favicon_changed_ && !favicon_.isNull()) {
      on_favicon_changed_.Run(favicon_);
    }
  }
  void SetLoadingChangedCallback(
      base::RepeatingCallback<void(bool)> callback) override {
    on_loading_changed_ = std::move(callback);
    if (on_loading_changed_) {
      on_loading_changed_.Run(loading_);
    }
  }
  void SetCloseRequestedCallback(base::RepeatingClosure callback) override {
    on_close_requested_ = std::move(callback);
  }
  void FireCloseRequestedForTesting() override {
    if (on_close_requested_) {
      on_close_requested_.Run();
    }
  }
  bool ShowPageInfoForTesting() override {
    // Exercise the exact LocationIconView delegate path. LocationBarView owns
    // the bubble's weak close callback and anchors it to itself, so this stays
    // valid without a BrowserView and safely no-ops after surface teardown.
    return location_bar_view_ && location_bar_view_->ShowPageInfoDialog();
  }
  void SetRoundedFrame(const RoundedFrameGeometry& geometry) override {
    if (rounded_frame_geometry_ == geometry) {
      return;
    }
    rounded_frame_geometry_ = geometry;
    content_frame_->SetBorder(
        geometry.enabled
            ? views::CreateEmptyBorder(gfx::Insets::TLBR(
                  geometry.top_inset, geometry.left_inset,
                  geometry.bottom_inset, geometry.right_inset))
            : nullptr);
    content_row_->layer()->SetRoundedCornerRadius(
        RoundedFrameCorners(geometry));
    rounded_frame_outline_->SetFrameGeometry(geometry);
    content_frame_->InvalidateLayout();
    content_frame_->SchedulePaint();
  }
  void OnWebViewFocused(views::WebView*) {
    if (on_activated_) {
      on_activated_.Run();
    }
  }
  void ActivateForToolbarCommand() {
    if (on_activated_) {
      on_activated_.Run();
    }
  }
  void OnInputEvent(
      const content::RenderWidgetHost&,
      const blink::WebInputEvent& event,
      content::RenderWidgetHost::InputEventObserver::InputEventSource)
      override {
    if (event.GetType() == blink::WebInputEvent::Type::kMouseDown) {
      if (on_interaction_) {
        on_interaction_.Run();
      }
      if (on_activated_) {
        on_activated_.Run();
      }
    }
  }
  void FocusOmnibar() override {
    if (location_bar_view_) {
      location_bar_view_->FocusLocation(/*is_user_initiated=*/true,
                                        /*clear_focus_if_failed=*/false);
    }
  }
  void CloseOmniboxPopupUnlessFocused(views::View* focused) override {
    // If focus is not inside our location bar (i.e. the omnibox isn't focused),
    // close the popup -- otherwise it would linger and a later click could
    // route a mouse event to the orphaned results widget and crash.
    if (location_bar_view_ &&
        (!focused || !location_bar_view_->Contains(focused))) {
      location_bar_view_->GetOmniboxController()->StopAutocomplete(
          /*clear_result=*/true);
    }
  }
  content::WebContents* GetInspectableWebContents() override {
    return web_contents_;
  }
  LocationBar* GetLocationBar() override { return location_bar_view_; }
  web_modal::WebContentsModalDialogHost* GetWebContentsModalDialogHost()
      override {
    return modal_dialog_host_.get();
  }
  // Session history (Cmd-[ / Cmd-]) -- reuse the WebContents
  // NavigationController.
  void GoBack() override {
    content::NavigationController& c = web_contents_->GetController();
    if (c.CanGoBack()) {
      c.GoBack();
      if (web_view_) {
        web_view_->RequestFocus();
      }
    }
  }
  void GoForward() override {
    content::NavigationController& c = web_contents_->GetController();
    if (c.CanGoForward()) {
      c.GoForward();
      if (web_view_) {
        web_view_->RequestFocus();
      }
    }
  }
  void GoHome() {
    ActivateForToolbarCommand();
    Profile* profile =
        Profile::FromBrowserContext(web_contents_->GetBrowserContext());
    const GURL home = profile ? profile->GetHomePage() : GURL();
    web_contents_->GetController().LoadURL(
        home.is_valid() ? home : GURL("chrome://newtab/"),
        content::Referrer(), ui::PAGE_TRANSITION_HOME_PAGE, std::string());
    if (web_view_) {
      web_view_->RequestFocus();
    }
  }
  void Reload() override {
    web_contents_->GetController().Reload(content::ReloadType::NORMAL,
                                          /*check_for_repost=*/true);
  }
  bool HandleOmniboxEscape() override {
    if (!location_bar_view_) {
      return false;
    }
    views::FocusManager* fm = location_bar_view_->GetFocusManager();
    views::View* focused = fm ? fm->GetFocusedView() : nullptr;
    // Only act when focus is actually inside our location bar (the omnibox).
    if (!focused || !location_bar_view_->Contains(focused)) {
      return false;
    }
    // Revert any in-progress edit and close the autocomplete popup, then move
    // focus back to the page -- the normal-browser behavior of Escape blurring
    // the omnibox back to the web contents (which we otherwise lack, having no
    // Browser to route the unconsumed Escape to).
    if (OmniboxView* ov = location_bar_view_->GetOmniboxView()) {
      ov->RevertAll();
    }
    if (web_view_) {
      web_view_->RequestFocus();
    }
    return true;
  }
  // Toggle a DevTools inspector docked to the right of the page, inside this
  // same pane/column. The page and inspector share the column width evenly.
  // Uses Chrome's real DevToolsUIBindings (the same machinery DevToolsWindow
  // drives): navigate a WebContents to the bundled devtools:// frontend, grab
  // the WebUI-created bindings for it, become their delegate, and AttachTo the
  // page's tab agent host. (A hand-rolled DevToolsFrontendHost does NOT work
  // here -- the devtools:// WebUI creates its own DevToolsUIBindings that the
  // frontend talks to, so we must use that one.)
  // Toggle DevTools by SHOW/HIDE. The inspector WebContents + WebView +
  // bindings are created lazily on first open and then never destroyed until
  // this pane dies. Destroying them on every toggle re-enters layout / focus /
  // agent-host callbacks against half-torn-down state and use-after-frees (we
  // hit crashes in ApplyColumnBounds, ~DevToolsUIBindings, and
  // DoCloseDevTools). Hiding a BoxLayout child gives its space back to
  // web_view_ automatically, so this is both robust and visually identical.
  void ToggleDevTools() override {
    if (devtools_window_) {
      // Undocked: toggle means close the window (OnWidgetDestroying re-docks
      // the contents into the hidden panel, i.e. DevTools ends up closed).
      devtools_window_->Close();
      return;
    }
    if (devtools_panel_) {
      SetDevToolsVisible(!devtools_panel_->GetVisible());
      return;
    }
    Profile* profile =
        Profile::FromBrowserContext(web_contents_->GetBrowserContext());
    devtools_contents_ = content::WebContents::Create(
        content::WebContents::CreateParams(profile));

    // Docked panel = [header | inspector]. The undocked-mode frontend has no
    // close/dock controls of its own (those are docked-frontend UI driven by
    // DevToolsWindow), so the header carries them: an open-in-new-window
    // button (undock) and a close X.
    auto* panel = new views::View();
    auto pbox = std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kVertical);
    pbox->set_cross_axis_alignment(
        views::BoxLayout::CrossAxisAlignment::kStretch);
    auto* panel_layout = panel->SetLayoutManager(std::move(pbox));

    auto* header = new views::View();
    header->SetBackground(views::CreateSolidBackground(kColorToolbar));
    auto hbox = std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kHorizontal);
    hbox->set_cross_axis_alignment(
        views::BoxLayout::CrossAxisAlignment::kCenter);
    hbox->set_between_child_spacing(2);
    hbox->set_inside_border_insets(gfx::Insets::TLBR(2, 8, 2, 4));
    auto* header_layout = header->SetLayoutManager(std::move(hbox));
    auto* header_title = new views::Label(u"DevTools");
    header_title->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    header->AddChildViewRaw(header_title);
    // The label absorbs the slack so the buttons sit flush right.
    header_layout->SetFlexForView(header_title, 1);
    auto add_header_button = [&](const gfx::VectorIcon& icon,
                                 const std::u16string& name, auto fn) {
      auto* b = new ToolbarButton(
          base::BindRepeating(fn, weak_factory_.GetWeakPtr()));
      b->SetVectorIcon(icon);
      b->SetTooltipText(name);
      b->GetViewAccessibility().SetName(name);
      header->AddChildViewRaw(b);
      return b;
    };
    add_header_button(LaunchIcon(), u"Open DevTools in new window",
                      &CmuxBrowserSurface::UndockDevTools);
    add_header_button(CloseIcon(), u"Close DevTools",
                      &CmuxBrowserSurface::CloseDevTools);
    panel->AddChildViewRaw(header);
    panel_layout->SetFlexForView(header, 0);

    auto* dv = new views::WebView(profile);
    dv->SetWebContents(devtools_contents_.get());
    dv->set_allow_accelerators(true);
    panel->AddChildViewRaw(dv);
    panel_layout->SetFlexForView(dv, 1);

    content_row_->AddChildViewRaw(panel);
    devtools_panel_ = panel;
    devtools_view_ = dv;
    content_row_layout_->SetFlexForView(web_view_, 1);
    content_row_layout_->SetFlexForView(devtools_panel_, 1);
    // targetType=tab selects the tab-agent connection. We deliberately DON'T
    // pass can_dock=true: docked mode makes the frontend reserve a blank region
    // for the inspected page (expecting the embedder to overlay the real page
    // there via SetInspectedPageBounds). We instead render the page in a
    // separate web_view_ beside this one, so undocked mode -- where the
    // frontend fills its whole view with inspector panels -- is what we want.
    devtools_contents_->GetController().LoadURL(
        GURL("devtools://devtools/bundled/devtools_app.html?targetType=tab"),
        content::Referrer(), ui::PAGE_TRANSITION_AUTO_TOPLEVEL, std::string());
    devtools_bindings_ =
        DevToolsUIBindings::ForWebContents(devtools_contents_.get());
    if (devtools_bindings_) {
      // SetDelegate takes ownership; hand it a proxy, never `this` (the views
      // hierarchy owns this surface -- see CmuxDevToolsDelegateProxy).
      devtools_bindings_->SetDelegate(
          new CmuxDevToolsDelegateProxy(weak_factory_.GetWeakPtr()));
      devtools_bindings_->AttachTo(
          content::DevToolsAgentHost::GetOrCreateForTab(web_contents_));
    }
    InvalidateLayout();
    devtools_view_->RequestFocus();
  }
  void SetDevToolsVisible(bool show) {
    if (!devtools_panel_) {
      return;
    }
    devtools_panel_->SetVisible(show);
    if (show && devtools_view_) {
      devtools_view_->RequestFocus();
    } else if (!show && web_view_) {
      web_view_->RequestFocus();
    }
    InvalidateLayout();
  }

  // Close DevTools from our header X (and the frontend's own close path):
  // undocked -> close the window (which re-docks hidden via
  // OnWidgetDestroying); docked -> hide the panel.
  void CloseDevTools() {
    if (devtools_window_) {
      devtools_window_->Close();
    } else {
      SetDevToolsVisible(false);
    }
  }

  // "Undock": move the inspector into its own top-level window (opening it
  // first if needed). The DevTools WebContents (and its bindings/agent
  // attachment) stays owned by this pane; only the hosting WebView changes --
  // detach from the docked view, attach to a fresh WebView in a new Widget,
  // exactly the WebContents hand-off tab dragging performs. Closing the window
  // (native close button, our X, or ToggleDevTools) re-attaches the contents
  // to the hidden docked panel, so window-close == DevTools closed and the
  // next toggle shows it docked again.
  void UndockDevTools() override {
    if (devtools_window_) {
      devtools_window_->Activate();
      return;
    }
    if (!devtools_panel_) {
      ToggleDevTools();
    }
    if (!devtools_view_ || !devtools_contents_) {
      return;
    }
    Profile* profile =
        Profile::FromBrowserContext(web_contents_->GetBrowserContext());
    devtools_view_->SetWebContents(nullptr);
    if (devtools_panel_) {
      devtools_panel_->SetVisible(false);
    }
    InvalidateLayout();
    if (web_view_) {
      web_view_->RequestFocus();
    }

    // Bare WidgetDelegate, leaked per undock -- same pattern as the main cmux
    // window (WidgetDelegateView subclassing is friend-gated to new code).
    auto* delegate = new views::WidgetDelegate();
    delegate->SetHasWindowSizeControls(true);
    delegate->SetTitle(u"DevTools — " + TabTitleForDisplay());
    auto window_view = std::make_unique<views::WebView>(profile);
    window_view->set_allow_accelerators(true);
    devtools_window_view_ = window_view.get();
    delegate->SetContentsView(std::move(window_view));

    devtools_window_ = new views::Widget();
    views::Widget::InitParams init(
        views::Widget::InitParams::NATIVE_WIDGET_OWNS_WIDGET,
        views::Widget::InitParams::TYPE_WINDOW);
    init.delegate = delegate;
    init.bounds = gfx::Rect(120, 120, 1100, 750);
    devtools_window_->Init(std::move(init));
    devtools_window_->AddObserver(this);
    RegisterCmuxChromeSurfaceThemeWidget(devtools_window_);
    devtools_window_view_->SetWebContents(devtools_contents_.get());
    devtools_window_->Show();
    devtools_window_view_->RequestFocus();
  }

  // views::WidgetObserver: the undocked DevTools window is going away (native
  // close, our Close() calls, or app teardown). Detach the WebView before it
  // dies and hand the contents back to the docked (hidden) panel.
  void OnWidgetDestroying(views::Widget* widget) override {
    if (widget != devtools_window_) {
      return;
    }
    widget->RemoveObserver(this);
    UnregisterCmuxChromeSurfaceThemeWidget(widget);
    if (devtools_window_view_) {
      devtools_window_view_->SetWebContents(nullptr);
      devtools_window_view_ = nullptr;
    }
    devtools_window_ = nullptr;
    if (devtools_view_ && devtools_contents_) {
      devtools_view_->SetWebContents(devtools_contents_.get());
    }
  }

  std::u16string E2EOmniboxText() override {
    OmniboxView* ov =
        location_bar_view_ ? location_bar_view_->GetOmniboxView() : nullptr;
    return ov ? ov->GetText() : std::u16string();
  }

  // content::WebContentsObserver: Browser remains the WebContentsDelegate;
  // presentation-only updates observe the tab without intercepting Chrome's
  // new-tab, window.close(), keyboard, dialog, or popup behavior.
  void TitleWasSet(content::NavigationEntry*) override {
    UpdatePageState(/*title_or_url_changed=*/true);
  }

  void DidFinishNavigation(
      content::NavigationHandle* navigation_handle) override {
    if (navigation_handle && navigation_handle->HasCommitted() &&
        navigation_handle->IsInPrimaryMainFrame()) {
      UpdatePageState(/*title_or_url_changed=*/true);
    }
  }

  void DidStartLoading() override { SetLoadingState(true); }
  void DidStopLoading() override { SetLoadingState(false); }

  void WebContentsDestroyed() override {
    UnregisterRenderWidgetInputObserver();
    if (web_view_) {
      web_view_->SetWebContents(nullptr);
    }
    Observe(nullptr);
    web_contents_ = nullptr;
  }

  void UpdatePageState(bool title_or_url_changed) {
    if (location_bar_view_) {
      location_bar_view_->Update(nullptr);
    }
    if (on_title_changed_ && title_or_url_changed) {
      on_title_changed_.Run(TabTitleForDisplay());
    }
    if (!web_contents_) {
      return;
    }
    content::NavigationController& c = web_contents_->GetController();
    if (back_button_) {
      back_button_->SetEnabled(c.CanGoBack());
    }
    if (forward_button_) {
      forward_button_->SetEnabled(c.CanGoForward());
    }
  }
  void SetLoadingState(bool loading) {
    if (!reload_button_) {
      return;
    }
    reload_button_->ChangeMode(
        loading ? ReloadButton::Mode::kStop : ReloadButton::Mode::kReload,
        /*force=*/false);
    if (loading_ == loading) {
      return;
    }
    loading_ = loading;
    if (on_loading_changed_) {
      on_loading_changed_.Run(loading_);
    }
  }

  // content::WebContentsObserver: the active renderer widget can change across
  // process swaps/crash recovery. Keep the mouse-down observer attached to the
  // current widget only.
  void RenderViewHostChanged(content::RenderViewHost* old_host,
                             content::RenderViewHost* new_host) override {
    if (old_host && old_host->GetWidget() == observed_widget_) {
      UnregisterRenderWidgetInputObserver();
    }
    RegisterRenderWidgetInputObserver(new_host ? new_host->GetWidget()
                                               : nullptr);
  }

  void RenderViewDeleted(content::RenderViewHost* render_view_host) override {
    if (render_view_host && render_view_host->GetWidget() == observed_widget_) {
      UnregisterRenderWidgetInputObserver();
    }
  }

  // favicon::FaviconDriverObserver:
  void OnFaviconUpdated(favicon::FaviconDriver*,
                        favicon::FaviconDriverObserver::NotificationIconType
                            notification_icon_type,
                        const GURL&,
                        bool,
                        const gfx::Image& image) override {
    if (notification_icon_type !=
        favicon::FaviconDriverObserver::NON_TOUCH_16_DIP) {
      return;
    }
    favicon_ = image.AsImageSkia();
    if (on_favicon_changed_) {
      on_favicon_changed_.Run(favicon_);
    }
  }

  // LocationBarView::Delegate. The tab now belongs to a real Browser, so page
  // actions and content-setting surfaces can consume the actual WebContents.
  content::WebContents* GetWebContents() override { return web_contents_; }
  LocationBarModel* GetLocationBarModel() override {
    return location_bar_model_.get();
  }
  const LocationBarModel* GetLocationBarModel() const override {
    return location_bar_model_.get();
  }
  ContentSettingBubbleModelDelegate* GetContentSettingBubbleModelDelegate()
      override {
    return browser_->GetFeatures().content_setting_bubble_model_delegate();
  }

  // ChromeLocationBarModelDelegate:
  content::WebContents* GetActiveWebContents() const override {
    return web_contents_;
  }

  // CommandUpdaterDelegate: the ReloadButton routes its press here. Chromium
  // 151 renamed this entry point and began forwarding the event timestamp.
#if CHROME_VERSION_MAJOR >= 151
  void HandleCommandWithDisposition(int id,
                                    WindowOpenDisposition,
                                    base::TimeTicks) override {
#else
  void ExecuteCommandWithDisposition(int id, WindowOpenDisposition) override {
#endif
    content::NavigationController& c = web_contents_->GetController();
    switch (id) {
      case IDC_RELOAD:
        c.Reload(content::ReloadType::NORMAL, /*check_for_repost=*/true);
        break;
      case IDC_RELOAD_BYPASSING_CACHE:
      case IDC_RELOAD_CLEARING_CACHE:
        c.Reload(content::ReloadType::BYPASSING_CACHE, true);
        break;
      case IDC_STOP:
        web_contents_->Stop();
        break;
      default:
        break;
    }
  }

 private:
  void ApplyToolbarCustomization(const LayoutConfig& config) {
    if (back_button_) {
      back_button_->SetVisible(config.toolbar_show_back);
    }
    if (forward_button_) {
      forward_button_->SetVisible(config.toolbar_show_forward);
    }
    if (reload_button_) {
      reload_button_->SetVisible(config.toolbar_show_reload);
    }
    if (home_button_) {
      home_button_->SetVisible(config.toolbar_show_home);
    }
    if (extensions_group_) {
      extensions_group_->SetVisible(config.toolbar_show_extensions);
    }
    if (downloads_button_) {
      downloads_button_->SetVisible(config.toolbar_show_downloads);
    }
#if BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC) || BUILDFLAG(IS_LINUX)
    if (media_button_) {
      media_button_->media_toolbar_button_controller()
          ->SetCanShowToolbarButton(config.toolbar_show_media);
    }
#endif
    if (profile_button_) {
      profile_button_->SetVisible(config.toolbar_show_profile);
    }
    if (menu_button_) {
      menu_button_->SetVisible(config.toolbar_show_menu);
    }
    if (toolbar_row_) {
      toolbar_row_->InvalidateLayout();
    }
  }

  // The tab label: the page title, else the URL host, else a placeholder.
  std::u16string TabTitleForDisplay() const {
    std::u16string title = web_contents_->GetTitle();
    if (!title.empty()) {
      return title;
    }
    const GURL& url = web_contents_->GetLastCommittedURL();
    if (url.is_valid() && !url.host().empty()) {
      return base::UTF8ToUTF16(url.host());
    }
    return u"New Tab";
  }

  void RefreshRenderWidgetInputObserver() {
    content::RenderViewHost* rvh =
        web_contents_ ? web_contents_->GetRenderViewHost() : nullptr;
    RegisterRenderWidgetInputObserver(rvh ? rvh->GetWidget() : nullptr);
  }

  void RegisterRenderWidgetInputObserver(content::RenderWidgetHost* widget) {
    if (observed_widget_ == widget) {
      return;
    }
    UnregisterRenderWidgetInputObserver();
    observed_widget_ = widget;
    if (observed_widget_) {
      observed_widget_->AddInputEventObserver(this);
    }
  }

  void UnregisterRenderWidgetInputObserver() {
    if (!observed_widget_) {
      return;
    }
    observed_widget_->RemoveInputEventObserver(this);
    observed_widget_ = nullptr;
  }

  // Non-owning: the workspace Browser's TabStripModel owns this WebContents.
  raw_ptr<content::WebContents> web_contents_;
  PaneId pane_ = kInvalidId;
  // Run when this pane's content is activated (web-contents focused or the
  // renderer sees a mouse-down); set by the strip via SetActivationCallback so
  // native page-body clicks focus + scroll in this column. Held subscription
  // keeps the WebView callback alive.
  base::RepeatingClosure on_activated_;
  // Direct renderer input is distinct from focus/activation: FocusContent can
  // activate a WebView programmatically during a cross-frontend handoff.
  base::RepeatingClosure on_interaction_;
  raw_ptr<content::RenderWidgetHost> observed_widget_ = nullptr;
  base::RepeatingCallback<void(const std::u16string&)> on_title_changed_;
  base::RepeatingCallback<void(const gfx::ImageSkia&)> on_favicon_changed_;
  base::RepeatingCallback<void(bool)> on_loading_changed_;
  base::RepeatingClosure on_close_requested_;
  base::CallbackListSubscription web_focus_sub_;
  raw_ptr<favicon::ContentFaviconDriver> favicon_driver_ = nullptr;
  gfx::ImageSkia favicon_;
  bool loading_ = false;
  const std::unique_ptr<LocationBarModelImpl> location_bar_model_;
  CommandUpdaterImpl command_updater_;
  std::unique_ptr<CmuxWebContentsModalDialogHost> modal_dialog_host_;
  raw_ptr<views::WebView> web_view_ = nullptr;
  raw_ptr<Browser> browser_ = nullptr;
  raw_ptr<views::View> toolbar_row_ = nullptr;
  raw_ptr<LocationBarView> location_bar_view_ = nullptr;
  raw_ptr<ToolbarButton> back_button_ = nullptr;
  raw_ptr<ToolbarButton> forward_button_ = nullptr;
  raw_ptr<ReloadButton> reload_button_ = nullptr;
  raw_ptr<HomeButton> home_button_ = nullptr;
  raw_ptr<PinnedToolbarActionsContainer> pinned_toolbar_actions_ = nullptr;
  raw_ptr<views::View> extensions_group_ = nullptr;
  raw_ptr<views::View> downloads_button_ = nullptr;
#if BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC) || BUILDFLAG(IS_LINUX)
  raw_ptr<MediaToolbarButtonView> media_button_ = nullptr;
#endif
  raw_ptr<views::View> profile_button_ = nullptr;
  raw_ptr<views::View> menu_button_ = nullptr;
  base::CallbackListSubscription layout_config_subscription_;
  bool location_bar_inited_ = false;
  // Horizontal [web | devtools] row (web_view_ lives here, not directly on us).
  raw_ptr<views::View> content_frame_ = nullptr;
  raw_ptr<views::View> content_row_ = nullptr;
  raw_ptr<views::BoxLayout> content_row_layout_ = nullptr;
  raw_ptr<views::View> side_panel_host_ = nullptr;
  raw_ptr<RoundedFrameOutlineView> rounded_frame_outline_ = nullptr;
  RoundedFrameGeometry rounded_frame_geometry_;
  // DevTools embed, created on demand by ToggleDevTools(). The bindings are
  // owned by devtools_contents_ (the WebUI), so we hold only a weak pointer and
  // clear our delegate link in the destructor before the contents is destroyed.
  std::unique_ptr<content::WebContents> devtools_contents_;
  raw_ptr<DevToolsUIBindings> devtools_bindings_ = nullptr;
  // Docked inspector: panel = [header (undock + close buttons) | WebView].
  raw_ptr<views::View> devtools_panel_ = nullptr;
  raw_ptr<views::WebView> devtools_view_ = nullptr;
  // Undocked inspector window (UndockDevTools), non-null only while open;
  // its WebView hosts devtools_contents_ instead of devtools_view_.
  raw_ptr<views::Widget> devtools_window_ = nullptr;
  raw_ptr<views::WebView> devtools_window_view_ = nullptr;
  base::WeakPtrFactory<CmuxBrowserSurface> weak_factory_{this};
};

BEGIN_METADATA(CmuxBrowserSurface)
END_METADATA

// CmuxDevToolsDelegateProxy forwarding (defined here: needs the full
// CmuxBrowserSurface). The weak pointer is invalidated at the very top of
// ~CmuxBrowserSurface, so during the bindings' teardown these all no-op.
content::WebContents* CmuxDevToolsDelegateProxy::GetInspectedWebContents() {
  return surface_ ? surface_->GetInspectableWebContents() : nullptr;
}
void CmuxDevToolsDelegateProxy::ActivateWindow() {}
void CmuxDevToolsDelegateProxy::CloseWindow() {
  if (surface_) {
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(&CmuxBrowserSurface::CloseDevTools, surface_));
  }
}
infobars::ContentInfoBarManager*
CmuxDevToolsDelegateProxy::GetInfoBarManager() {
  return surface_ ? infobars::ContentInfoBarManager::FromWebContents(
                        surface_->GetInspectableWebContents())
                  : nullptr;
}

}  // namespace

// Factory: CmuxBrowserSurface above is private to this file. Panes build web
// surfaces through this entry point, which adds the surface as a child of
// `parent` (taking ownership) and returns it as the CmuxSurface interface.
CmuxSurface* AddBrowserSurface(views::View* parent,
                               content::WebContents* web_contents,
                               PaneId pane) {
  return parent->AddChildView(
      std::make_unique<CmuxBrowserSurface>(web_contents, pane));
}

}  // namespace cmux
