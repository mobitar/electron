// Copyright (c) 2013 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "atom/browser/api/atom_api_browser_window.h"

#include "atom/browser/api/atom_api_browser_view.h"
#include "atom/browser/api/atom_api_menu.h"
#include "atom/browser/api/atom_api_web_contents.h"
#include "atom/browser/browser.h"
#include "atom/browser/native_window.h"
#include "atom/browser/unresponsive_suppressor.h"
#include "atom/browser/web_contents_preferences.h"
#include "atom/browser/window_list.h"
#include "atom/common/api/api_messages.h"
#include "atom/common/color_util.h"
#include "atom/common/native_mate_converters/callback.h"
#include "atom/common/native_mate_converters/file_path_converter.h"
#include "atom/common/native_mate_converters/gfx_converter.h"
#include "atom/common/native_mate_converters/gurl_converter.h"
#include "atom/common/native_mate_converters/image_converter.h"
#include "atom/common/native_mate_converters/string16_converter.h"
#include "atom/common/native_mate_converters/value_converter.h"
#include "atom/common/options_switches.h"
#include "base/command_line.h"
#include "base/threading/thread_task_runner_handle.h"
#include "content/browser/renderer_host/render_widget_host_impl.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/render_view_host.h"
#include "content/public/common/content_switches.h"
#include "native_mate/constructor.h"
#include "native_mate/dictionary.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gl/gpu_switching_manager.h"

#if defined(TOOLKIT_VIEWS)
#include "atom/browser/native_window_views.h"
#endif

#if defined(OS_WIN)
#include "atom/browser/ui/win/taskbar_host.h"
#include "ui/base/win/shell.h"
#endif

#include "atom/common/node_includes.h"

#if defined(OS_WIN)
namespace mate {

template<>
struct Converter<atom::TaskbarHost::ThumbarButton> {
  static bool FromV8(v8::Isolate* isolate, v8::Handle<v8::Value> val,
                     atom::TaskbarHost::ThumbarButton* out) {
    mate::Dictionary dict;
    if (!ConvertFromV8(isolate, val, &dict))
      return false;
    dict.Get("click", &(out->clicked_callback));
    dict.Get("tooltip", &(out->tooltip));
    dict.Get("flags", &out->flags);
    return dict.Get("icon", &(out->icon));
  }
};

}  // namespace mate
#endif

namespace atom {

namespace api {

namespace {

// Converts binary data to Buffer.
v8::Local<v8::Value> ToBuffer(v8::Isolate* isolate, void* val, int size) {
  auto buffer = node::Buffer::Copy(isolate, static_cast<char*>(val), size);
  if (buffer.IsEmpty())
    return v8::Null(isolate);
  else
    return buffer.ToLocalChecked();
}

}  // namespace


BrowserWindow::BrowserWindow(v8::Isolate* isolate,
                             v8::Local<v8::Object> wrapper,
                             const mate::Dictionary& options)
    : weak_factory_(this) {
  mate::Handle<class WebContents> web_contents;

  // Use options.webPreferences in WebContents.
  mate::Dictionary web_preferences = mate::Dictionary::CreateEmpty(isolate);
  options.Get(options::kWebPreferences, &web_preferences);

  // Copy the backgroundColor to webContents.
  v8::Local<v8::Value> value;
  if (options.Get(options::kBackgroundColor, &value))
    web_preferences.Set(options::kBackgroundColor, value);

  v8::Local<v8::Value> transparent;
  if (options.Get("transparent", &transparent))
    web_preferences.Set("transparent", transparent);

#if defined(ENABLE_OSR)
  // Offscreen windows are always created frameless.
  bool offscreen;
  if (web_preferences.Get("offscreen", &offscreen) && offscreen) {
    auto window_options = const_cast<mate::Dictionary&>(options);
    window_options.Set(options::kFrame, false);
  }
#endif

  if (options.Get("webContents", &web_contents) && !web_contents.IsEmpty()) {
    // Set webPreferences from options if using an existing webContents.
    // These preferences will be used when the webContent launches new
    // render processes.
    auto* existing_preferences =
        WebContentsPreferences::FromWebContents(web_contents->web_contents());
    base::DictionaryValue web_preferences_dict;
    if (mate::ConvertFromV8(isolate, web_preferences.GetHandle(),
                        &web_preferences_dict)) {
      existing_preferences->web_preferences()->Clear();
      existing_preferences->Merge(web_preferences_dict);
    }

  } else {
    // Creates the WebContents used by BrowserWindow.
    web_contents = WebContents::Create(isolate, web_preferences);
  }

  Init(isolate, wrapper, options, web_contents);
}

void BrowserWindow::Init(v8::Isolate* isolate,
                         v8::Local<v8::Object> wrapper,
                         const mate::Dictionary& options,
                         mate::Handle<class WebContents> web_contents) {
  web_contents_.Reset(isolate, web_contents.ToV8());
  api_web_contents_ = web_contents.get();
  api_web_contents_->AddObserver(this);
  Observe(api_web_contents_->web_contents());

  // Keep a copy of the options for later use.
  mate::Dictionary(isolate, web_contents->GetWrapper()).Set(
      "browserWindowOptions", options);

  // The parent window.
  mate::Handle<BrowserWindow> parent;
  if (options.Get("parent", &parent) && !parent.IsEmpty())
    parent_window_.Reset(isolate, parent.ToV8());

  // Creates BrowserWindow.
  window_.reset(NativeWindow::Create(
      web_contents->managed_web_contents(),
      options,
      parent.IsEmpty() ? nullptr : parent->window_.get()));
  web_contents->SetOwnerWindow(window_.get());
  window_->set_is_offscreen_dummy(api_web_contents_->IsOffScreen());

  // Tell the content module to initialize renderer widget with transparent
  // mode.
  ui::GpuSwitchingManager::SetTransparent(window_->transparent());

#if defined(TOOLKIT_VIEWS)
  // Sets the window icon.
  mate::Handle<NativeImage> icon;
  if (options.Get(options::kIcon, &icon) && !icon.IsEmpty())
    SetIcon(icon);
#endif

  window_->InitFromOptions(options);
  window_->AddObserver(this);

  InitWith(isolate, wrapper);
  AttachAsUserData(window_.get());

  // We can only append this window to parent window's child windows after this
  // window's JS wrapper gets initialized.
  if (!parent.IsEmpty())
    parent->child_windows_.Set(isolate, ID(), wrapper);

  auto* host = web_contents->web_contents()->GetRenderViewHost();
  if (host)
    host->GetWidget()->AddInputEventObserver(this);
}

BrowserWindow::~BrowserWindow() {
  if (!window_->IsClosed())
    window_->CloseImmediately();

  api_web_contents_->RemoveObserver(this);

  // Destroy the native window in next tick because the native code might be
  // iterating all windows.
  base::ThreadTaskRunnerHandle::Get()->DeleteSoon(FROM_HERE, window_.release());
}

void BrowserWindow::OnInputEvent(const blink::WebInputEvent& event) {
  switch (event.GetType()) {
    case blink::WebInputEvent::kGestureScrollBegin:
    case blink::WebInputEvent::kGestureScrollUpdate:
    case blink::WebInputEvent::kGestureScrollEnd:
      Emit("scroll-touch-edge");
      break;
    default:
      break;
  }
}

void BrowserWindow::RenderViewHostChanged(content::RenderViewHost* old_host,
                                          content::RenderViewHost* new_host) {
  if (old_host)
    old_host->GetWidget()->RemoveInputEventObserver(this);
  if (new_host)
    new_host->GetWidget()->AddInputEventObserver(this);
}

void BrowserWindow::RenderViewCreated(
    content::RenderViewHost* render_view_host) {
  if (!window_->transparent())
    return;

  content::RenderWidgetHostImpl* impl = content::RenderWidgetHostImpl::FromID(
      render_view_host->GetProcess()->GetID(),
      render_view_host->GetRoutingID());
  if (impl)
    impl->SetBackgroundOpaque(false);
}

void BrowserWindow::DidFirstVisuallyNonEmptyPaint() {
  if (window_->IsVisible())
    return;

  // When there is a non-empty first paint, resize the RenderWidget to force
  // Chromium to draw.
  const auto view = web_contents()->GetRenderWidgetHostView();
  view->Show();
  view->SetSize(window_->GetContentSize());

  // Emit the ReadyToShow event in next tick in case of pending drawing work.
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE,
      base::Bind([](base::WeakPtr<BrowserWindow> self) {
        if (self)
          self->Emit("ready-to-show");
      }, GetWeakPtr()));
}

void BrowserWindow::BeforeUnloadDialogCancelled() {
  WindowList::WindowCloseCancelled(window_.get());
  // Cancel unresponsive event when window close is cancelled.
  window_unresponsive_closure_.Cancel();
}

void BrowserWindow::OnRendererUnresponsive(content::RenderWidgetHost*) {
  // Schedule the unresponsive shortly later, since we may receive the
  // responsive event soon. This could happen after the whole application had
  // blocked for a while.
  // Also notice that when closing this event would be ignored because we have
  // explicitly started a close timeout counter. This is on purpose because we
  // don't want the unresponsive event to be sent too early when user is closing
  // the window.
  ScheduleUnresponsiveEvent(50);
}

bool BrowserWindow::OnMessageReceived(const IPC::Message& message,
                                      content::RenderFrameHost* rfh) {
  bool handled = true;
  IPC_BEGIN_MESSAGE_MAP_WITH_PARAM(BrowserWindow, message, rfh)
    IPC_MESSAGE_HANDLER(AtomFrameHostMsg_UpdateDraggableRegions,
                        UpdateDraggableRegions)
    IPC_MESSAGE_UNHANDLED(handled = false)
  IPC_END_MESSAGE_MAP()
  return handled;
}

void BrowserWindow::OnCloseContents() {
  DCHECK(web_contents());

  // Close all child windows before closing current window.
  v8::Locker locker(isolate());
  v8::HandleScope handle_scope(isolate());
  for (v8::Local<v8::Value> value : child_windows_.Values(isolate())) {
    mate::Handle<BrowserWindow> child;
    if (mate::ConvertFromV8(isolate(), value, &child) && !child.IsEmpty())
      child->window_->CloseImmediately();
  }

  // When the web contents is gone, close the window immediately, but the
  // memory will not be freed until you call delete.
  // In this way, it would be safe to manage windows via smart pointers. If you
  // want to free memory when the window is closed, you can do deleting by
  // overriding the OnWindowClosed method in the observer.
  window_->CloseImmediately();

  // Do not sent "unresponsive" event after window is closed.
  window_unresponsive_closure_.Cancel();
}

void BrowserWindow::OnRendererResponsive() {
  window_unresponsive_closure_.Cancel();
  Emit("responsive");
}

void BrowserWindow::WillCloseWindow(bool* prevent_default) {
  *prevent_default = Emit("close");
}

void BrowserWindow::RequestPreferredWidth(int* width) {
  *width = web_contents()->GetPreferredSize().width();
}

void BrowserWindow::OnCloseButtonClicked(bool* prevent_default) {
  // When user tries to close the window by clicking the close button, we do
  // not close the window immediately, instead we try to close the web page
  // first, and when the web page is closed the window will also be closed.
  *prevent_default = true;

  // Assume the window is not responding if it doesn't cancel the close and is
  // not closed in 5s, in this way we can quickly show the unresponsive
  // dialog when the window is busy executing some script withouth waiting for
  // the unresponsive timeout.
  if (window_unresponsive_closure_.IsCancelled())
    ScheduleUnresponsiveEvent(5000);

  if (!web_contents())
    // Already closed by renderer
    return;

  if (web_contents()->NeedToFireBeforeUnload())
    web_contents()->DispatchBeforeUnload();
  else
    web_contents()->Close();
}

void BrowserWindow::OnWindowClosed() {
  auto* host = web_contents()->GetRenderViewHost();
  if (host)
    host->GetWidget()->RemoveInputEventObserver(this);

  api_web_contents_->DestroyWebContents(true /* async */);

  Observe(nullptr);
  RemoveFromWeakMap();
  window_->RemoveObserver(this);

  // We can not call Destroy here because we need to call Emit first, but we
  // also do not want any method to be used, so just mark as destroyed here.
  MarkDestroyed();

  Emit("closed");

  RemoveFromParentChildWindows();

  ResetBrowserView();

  // Destroy the native class when window is closed.
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE, GetDestroyClosure());
}

void BrowserWindow::OnWindowEndSession() {
  Emit("session-end");
}

void BrowserWindow::OnWindowBlur() {
  web_contents()->StoreFocus();
#if defined(OS_MACOSX)
  auto* rwhv = web_contents()->GetRenderWidgetHostView();
  if (rwhv)
    rwhv->SetActive(false);
#endif

  Emit("blur");
}

void BrowserWindow::OnWindowFocus() {
  web_contents()->RestoreFocus();
#if defined(OS_MACOSX)
  auto* rwhv = web_contents()->GetRenderWidgetHostView();
  if (rwhv)
    rwhv->SetActive(true);
#else
  if (!api_web_contents_->IsDevToolsOpened())
    web_contents()->Focus();
#endif

  Emit("focus");
}

void BrowserWindow::OnWindowShow() {
  Emit("show");
}

void BrowserWindow::OnWindowHide() {
  Emit("hide");
}

void BrowserWindow::OnWindowMaximize() {
  Emit("maximize");
}

void BrowserWindow::OnWindowUnmaximize() {
  Emit("unmaximize");
}

void BrowserWindow::OnWindowMinimize() {
  Emit("minimize");
}

void BrowserWindow::OnWindowRestore() {
  Emit("restore");
}

void BrowserWindow::OnWindowResize() {
#if defined(OS_MACOSX)
  if (!draggable_regions_.empty())
    UpdateDraggableRegions(nullptr, draggable_regions_);
#endif
  Emit("resize");
}

void BrowserWindow::OnWindowMove() {
  Emit("move");
}

void BrowserWindow::OnWindowMoved() {
  Emit("moved");
}

void BrowserWindow::OnWindowEnterFullScreen() {
  Emit("enter-full-screen");
}

void BrowserWindow::OnWindowLeaveFullScreen() {
  Emit("leave-full-screen");
}

void BrowserWindow::OnWindowScrollTouchBegin() {
  Emit("scroll-touch-begin");
}

void BrowserWindow::OnWindowScrollTouchEnd() {
  Emit("scroll-touch-end");
}

void BrowserWindow::OnWindowSwipe(const std::string& direction) {
  Emit("swipe", direction);
}

void BrowserWindow::OnWindowSheetBegin() {
  Emit("sheet-begin");
}

void BrowserWindow::OnWindowSheetEnd() {
  Emit("sheet-end");
}

void BrowserWindow::OnWindowEnterHtmlFullScreen() {
  Emit("enter-html-full-screen");
}

void BrowserWindow::OnWindowLeaveHtmlFullScreen() {
  Emit("leave-html-full-screen");
}

void BrowserWindow::OnExecuteWindowsCommand(const std::string& command_name) {
  Emit("app-command", command_name);
}

void BrowserWindow::OnTouchBarItemResult(const std::string& item_id,
                                  const base::DictionaryValue& details) {
  Emit("-touch-bar-interaction", item_id, details);
}

void BrowserWindow::OnNewWindowForTab() {
  Emit("new-window-for-tab");
}

#if defined(OS_WIN)
void BrowserWindow::OnWindowMessage(UINT message,
                                    WPARAM w_param,
                                    LPARAM l_param) {
  if (IsWindowMessageHooked(message)) {
    messages_callback_map_[message].Run(
        ToBuffer(isolate(), static_cast<void*>(&w_param), sizeof(WPARAM)),
        ToBuffer(isolate(), static_cast<void*>(&l_param), sizeof(LPARAM)));
  }
}
#endif

// static
mate::WrappableBase* BrowserWindow::New(mate::Arguments* args) {
  if (!Browser::Get()->is_ready()) {
    args->ThrowError("Cannot create BrowserWindow before app is ready");
    return nullptr;
  }

  if (args->Length() > 1) {
    args->ThrowError();
    return nullptr;
  }

  mate::Dictionary options;
  if (!(args->Length() == 1 && args->GetNext(&options))) {
    options = mate::Dictionary::CreateEmpty(args->isolate());
  }

  return new BrowserWindow(args->isolate(), args->GetThis(), options);
}

void BrowserWindow::Close() {
  window_->Close();
}

void BrowserWindow::Focus() {
  window_->Focus(true);
}

void BrowserWindow::Blur() {
  window_->Focus(false);
}

bool BrowserWindow::IsFocused() {
  return window_->IsFocused();
}

void BrowserWindow::Show() {
  window_->Show();
}

void BrowserWindow::ShowInactive() {
  // This method doesn't make sense for modal window..
  if (IsModal())
    return;

  window_->ShowInactive();
}

void BrowserWindow::Hide() {
  window_->Hide();
}

bool BrowserWindow::IsVisible() {
  return window_->IsVisible();
}

bool BrowserWindow::IsEnabled() {
  return window_->IsEnabled();
}

void BrowserWindow::SetEnabled(bool enable) {
  window_->SetEnabled(enable);
}

void BrowserWindow::Maximize() {
  window_->Maximize();
}

void BrowserWindow::Unmaximize() {
  window_->Unmaximize();
}

bool BrowserWindow::IsMaximized() {
  return window_->IsMaximized();
}

void BrowserWindow::Minimize() {
  window_->Minimize();
}

void BrowserWindow::Restore() {
  window_->Restore();
}

bool BrowserWindow::IsMinimized() {
  return window_->IsMinimized();
}

void BrowserWindow::SetFullScreen(bool fullscreen) {
  window_->SetFullScreen(fullscreen);
}

bool BrowserWindow::IsFullscreen() {
  return window_->IsFullscreen();
}

void BrowserWindow::SetBounds(const gfx::Rect& bounds, mate::Arguments* args) {
  bool animate = false;
  args->GetNext(&animate);
  window_->SetBounds(bounds, animate);
}

gfx::Rect BrowserWindow::GetBounds() {
  return window_->GetBounds();
}

void BrowserWindow::SetContentBounds(const gfx::Rect& bounds,
                                     mate::Arguments* args) {
  bool animate = false;
  args->GetNext(&animate);
  window_->SetContentBounds(bounds, animate);
}

gfx::Rect BrowserWindow::GetContentBounds() {
  return window_->GetContentBounds();
}

void BrowserWindow::SetSize(int width, int height, mate::Arguments* args) {
  bool animate = false;
  args->GetNext(&animate);
  window_->SetSize(gfx::Size(width, height), animate);
}

std::vector<int> BrowserWindow::GetSize() {
  std::vector<int> result(2);
  gfx::Size size = window_->GetSize();
  result[0] = size.width();
  result[1] = size.height();
  return result;
}

void BrowserWindow::SetContentSize(int width, int height,
                                   mate::Arguments* args) {
  bool animate = false;
  args->GetNext(&animate);
  window_->SetContentSize(gfx::Size(width, height), animate);
}

std::vector<int> BrowserWindow::GetContentSize() {
  std::vector<int> result(2);
  gfx::Size size = window_->GetContentSize();
  result[0] = size.width();
  result[1] = size.height();
  return result;
}

void BrowserWindow::SetMinimumSize(int width, int height) {
  window_->SetMinimumSize(gfx::Size(width, height));
}

std::vector<int> BrowserWindow::GetMinimumSize() {
  std::vector<int> result(2);
  gfx::Size size = window_->GetMinimumSize();
  result[0] = size.width();
  result[1] = size.height();
  return result;
}

void BrowserWindow::SetMaximumSize(int width, int height) {
  window_->SetMaximumSize(gfx::Size(width, height));
}

std::vector<int> BrowserWindow::GetMaximumSize() {
  std::vector<int> result(2);
  gfx::Size size = window_->GetMaximumSize();
  result[0] = size.width();
  result[1] = size.height();
  return result;
}

void BrowserWindow::SetSheetOffset(double offsetY, mate::Arguments* args) {
  double offsetX = 0.0;
  args->GetNext(&offsetX);
  window_->SetSheetOffset(offsetX, offsetY);
}

void BrowserWindow::SetResizable(bool resizable) {
  window_->SetResizable(resizable);
}

bool BrowserWindow::IsResizable() {
  return window_->IsResizable();
}

void BrowserWindow::SetMovable(bool movable) {
  window_->SetMovable(movable);
}

bool BrowserWindow::IsMovable() {
  return window_->IsMovable();
}

void BrowserWindow::SetMinimizable(bool minimizable) {
  window_->SetMinimizable(minimizable);
}

bool BrowserWindow::IsMinimizable() {
  return window_->IsMinimizable();
}

void BrowserWindow::SetMaximizable(bool maximizable) {
  window_->SetMaximizable(maximizable);
}

bool BrowserWindow::IsMaximizable() {
  return window_->IsMaximizable();
}

void BrowserWindow::SetFullScreenable(bool fullscreenable) {
  window_->SetFullScreenable(fullscreenable);
}

bool BrowserWindow::IsFullScreenable() {
  return window_->IsFullScreenable();
}

void BrowserWindow::SetClosable(bool closable) {
  window_->SetClosable(closable);
}

bool BrowserWindow::IsClosable() {
  return window_->IsClosable();
}

void BrowserWindow::SetAlwaysOnTop(bool top, mate::Arguments* args) {
  std::string level = "floating";
  int relativeLevel = 0;
  std::string error;

  args->GetNext(&level);
  args->GetNext(&relativeLevel);

  window_->SetAlwaysOnTop(top, level, relativeLevel, &error);

  if (!error.empty()) {
    args->ThrowError(error);
  }
}

bool BrowserWindow::IsAlwaysOnTop() {
  return window_->IsAlwaysOnTop();
}

void BrowserWindow::Center() {
  window_->Center();
}

void BrowserWindow::SetPosition(int x, int y, mate::Arguments* args) {
  bool animate = false;
  args->GetNext(&animate);
  window_->SetPosition(gfx::Point(x, y), animate);
}

std::vector<int> BrowserWindow::GetPosition() {
  std::vector<int> result(2);
  gfx::Point pos = window_->GetPosition();
  result[0] = pos.x();
  result[1] = pos.y();
  return result;
}

void BrowserWindow::SetTitle(const std::string& title) {
  window_->SetTitle(title);
}

std::string BrowserWindow::GetTitle() {
  return window_->GetTitle();
}

void BrowserWindow::FlashFrame(bool flash) {
  window_->FlashFrame(flash);
}

void BrowserWindow::SetSkipTaskbar(bool skip) {
  window_->SetSkipTaskbar(skip);
}

void BrowserWindow::SetSimpleFullScreen(bool simple_fullscreen) {
  window_->SetSimpleFullScreen(simple_fullscreen);
}

bool BrowserWindow::IsSimpleFullScreen() {
  return window_->IsSimpleFullScreen();
}

void BrowserWindow::SetKiosk(bool kiosk) {
  window_->SetKiosk(kiosk);
}

bool BrowserWindow::IsKiosk() {
  return window_->IsKiosk();
}

void BrowserWindow::SetBackgroundColor(const std::string& color_name) {
  SkColor color = ParseHexColor(color_name);
  window_->SetBackgroundColor(color);
  auto* view = web_contents()->GetRenderWidgetHostView();
  if (view)
    view->SetBackgroundColor(color);
}

void BrowserWindow::SetHasShadow(bool has_shadow) {
  window_->SetHasShadow(has_shadow);
}

bool BrowserWindow::HasShadow() {
  return window_->HasShadow();
}

void BrowserWindow::SetOpacity(const double opacity) {
  window_->SetOpacity(opacity);
}

double BrowserWindow::GetOpacity() {
  return window_->GetOpacity();
}

void BrowserWindow::FocusOnWebView() {
  web_contents()->GetRenderViewHost()->GetWidget()->Focus();
}

void BrowserWindow::BlurWebView() {
  web_contents()->GetRenderViewHost()->GetWidget()->Blur();
}

bool BrowserWindow::IsWebViewFocused() {
  auto host_view = web_contents()->GetRenderViewHost()->GetWidget()->GetView();
  return host_view && host_view->HasFocus();
}

void BrowserWindow::SetRepresentedFilename(const std::string& filename) {
  window_->SetRepresentedFilename(filename);
}

std::string BrowserWindow::GetRepresentedFilename() {
  return window_->GetRepresentedFilename();
}

void BrowserWindow::SetDocumentEdited(bool edited) {
  window_->SetDocumentEdited(edited);
}

bool BrowserWindow::IsDocumentEdited() {
  return window_->IsDocumentEdited();
}

void BrowserWindow::SetIgnoreMouseEvents(bool ignore, mate::Arguments* args) {
  mate::Dictionary options;
  bool forward = false;
  args->GetNext(&options) && options.Get("forward", &forward);
  return window_->SetIgnoreMouseEvents(ignore, forward);
}

void BrowserWindow::SetContentProtection(bool enable) {
  return window_->SetContentProtection(enable);
}

void BrowserWindow::SetFocusable(bool focusable) {
  return window_->SetFocusable(focusable);
}

void BrowserWindow::SetProgressBar(double progress, mate::Arguments* args) {
  mate::Dictionary options;
  std::string mode;
  NativeWindow::ProgressState state = NativeWindow::PROGRESS_NORMAL;

  args->GetNext(&options) && options.Get("mode", &mode);

  if (mode == "error") {
    state = NativeWindow::PROGRESS_ERROR;
  } else if (mode == "paused") {
    state = NativeWindow::PROGRESS_PAUSED;
  } else if (mode == "indeterminate") {
    state = NativeWindow::PROGRESS_INDETERMINATE;
  } else if (mode == "none") {
    state = NativeWindow::PROGRESS_NONE;
  }

  window_->SetProgressBar(progress, state);
}

void BrowserWindow::SetOverlayIcon(const gfx::Image& overlay,
                            const std::string& description) {
  window_->SetOverlayIcon(overlay, description);
}

bool BrowserWindow::SetThumbarButtons(mate::Arguments* args) {
#if defined(OS_WIN)
  std::vector<TaskbarHost::ThumbarButton> buttons;
  if (!args->GetNext(&buttons)) {
    args->ThrowError();
    return false;
  }
  auto window = static_cast<NativeWindowViews*>(window_.get());
  return window->taskbar_host().SetThumbarButtons(
      window_->GetAcceleratedWidget(), buttons);
#else
  return false;
#endif
}

void BrowserWindow::SetMenu(v8::Isolate* isolate, v8::Local<v8::Value> value) {
  mate::Handle<Menu> menu;
  if (value->IsObject() &&
      mate::V8ToString(value->ToObject()->GetConstructorName()) == "Menu" &&
      mate::ConvertFromV8(isolate, value, &menu) && !menu.IsEmpty()) {
    menu_.Reset(isolate, menu.ToV8());
    window_->SetMenu(menu->model());
  } else if (value->IsNull()) {
    menu_.Reset();
    window_->SetMenu(nullptr);
  } else {
    isolate->ThrowException(v8::Exception::TypeError(
        mate::StringToV8(isolate, "Invalid Menu")));
  }
}

void BrowserWindow::SetAutoHideMenuBar(bool auto_hide) {
  window_->SetAutoHideMenuBar(auto_hide);
}

bool BrowserWindow::IsMenuBarAutoHide() {
  return window_->IsMenuBarAutoHide();
}

void BrowserWindow::SetMenuBarVisibility(bool visible) {
  window_->SetMenuBarVisibility(visible);
}

bool BrowserWindow::IsMenuBarVisible() {
  return window_->IsMenuBarVisible();
}

#if defined(OS_WIN)
bool BrowserWindow::HookWindowMessage(UINT message,
                               const MessageCallback& callback) {
  messages_callback_map_[message] = callback;
  return true;
}

void BrowserWindow::UnhookWindowMessage(UINT message) {
  if (!ContainsKey(messages_callback_map_, message))
    return;

  messages_callback_map_.erase(message);
}

bool BrowserWindow::IsWindowMessageHooked(UINT message) {
  return ContainsKey(messages_callback_map_, message);
}

void BrowserWindow::UnhookAllWindowMessages() {
  messages_callback_map_.clear();
}

bool BrowserWindow::SetThumbnailClip(const gfx::Rect& region) {
  auto window = static_cast<NativeWindowViews*>(window_.get());
  return window->taskbar_host().SetThumbnailClip(
      window_->GetAcceleratedWidget(), region);
}

bool BrowserWindow::SetThumbnailToolTip(const std::string& tooltip) {
  auto window = static_cast<NativeWindowViews*>(window_.get());
  return window->taskbar_host().SetThumbnailToolTip(
      window_->GetAcceleratedWidget(), tooltip);
}

void BrowserWindow::SetAppDetails(const mate::Dictionary& options) {
  base::string16 app_id;
  base::FilePath app_icon_path;
  int app_icon_index = 0;
  base::string16 relaunch_command;
  base::string16 relaunch_display_name;

  options.Get("appId", &app_id);
  options.Get("appIconPath", &app_icon_path);
  options.Get("appIconIndex", &app_icon_index);
  options.Get("relaunchCommand", &relaunch_command);
  options.Get("relaunchDisplayName", &relaunch_display_name);

  ui::win::SetAppDetailsForWindow(
      app_id, app_icon_path, app_icon_index,
      relaunch_command, relaunch_display_name,
      window_->GetAcceleratedWidget());
}
#endif

#if defined(TOOLKIT_VIEWS)
void BrowserWindow::SetIcon(mate::Handle<NativeImage> icon) {
#if defined(OS_WIN)
  static_cast<NativeWindowViews*>(window_.get())->SetIcon(
      icon->GetHICON(GetSystemMetrics(SM_CXSMICON)),
      icon->GetHICON(GetSystemMetrics(SM_CXICON)));
#elif defined(USE_X11)
  static_cast<NativeWindowViews*>(window_.get())->SetIcon(
      icon->image().AsImageSkia());
#endif
}
#endif

void BrowserWindow::SetAspectRatio(double aspect_ratio, mate::Arguments* args) {
  gfx::Size extra_size;
  args->GetNext(&extra_size);
  window_->SetAspectRatio(aspect_ratio, extra_size);
}

void BrowserWindow::PreviewFile(const std::string& path,
                                mate::Arguments* args) {
  std::string display_name;
  if (!args->GetNext(&display_name))
    display_name = path;
  window_->PreviewFile(path, display_name);
}

void BrowserWindow::CloseFilePreview() {
  window_->CloseFilePreview();
}

void BrowserWindow::SetParentWindow(v8::Local<v8::Value> value,
                             mate::Arguments* args) {
  if (IsModal()) {
    args->ThrowError("Can not be called for modal window");
    return;
  }

  mate::Handle<BrowserWindow> parent;
  if (value->IsNull() || value->IsUndefined()) {
    RemoveFromParentChildWindows();
    parent_window_.Reset();
    window_->SetParentWindow(nullptr);
  } else if (mate::ConvertFromV8(isolate(), value, &parent)) {
    parent_window_.Reset(isolate(), value);
    window_->SetParentWindow(parent->window_.get());
    parent->child_windows_.Set(isolate(), ID(), GetWrapper());
  } else {
    args->ThrowError("Must pass BrowserWindow instance or null");
  }
}

v8::Local<v8::Value> BrowserWindow::GetParentWindow() const {
  if (parent_window_.IsEmpty())
    return v8::Null(isolate());
  else
    return v8::Local<v8::Value>::New(isolate(), parent_window_);
}

std::vector<v8::Local<v8::Object>> BrowserWindow::GetChildWindows() const {
  return child_windows_.Values(isolate());
}

v8::Local<v8::Value> BrowserWindow::GetBrowserView() const {
  if (browser_view_.IsEmpty()) {
    return v8::Null(isolate());
  }

  return v8::Local<v8::Value>::New(isolate(), browser_view_);
}

void BrowserWindow::SetBrowserView(v8::Local<v8::Value> value) {
  ResetBrowserView();

  mate::Handle<BrowserView> browser_view;
  if (value->IsNull() || value->IsUndefined()) {
    window_->SetBrowserView(nullptr);
  } else if (mate::ConvertFromV8(isolate(), value, &browser_view)) {
    window_->SetBrowserView(browser_view->view());
    browser_view->web_contents()->SetOwnerWindow(window_.get());
    browser_view_.Reset(isolate(), value);

    UpdateDraggableRegions(nullptr, draggable_regions_);
  }
}

void BrowserWindow::ResetBrowserView() {
  if (browser_view_.IsEmpty()) {
    return;
  }

  mate::Handle<BrowserView> browser_view;
  if (mate::ConvertFromV8(isolate(), GetBrowserView(), &browser_view)
    && !browser_view.IsEmpty()) {
    browser_view->web_contents()->SetOwnerWindow(nullptr);
  }

  browser_view_.Reset();
}

bool BrowserWindow::IsModal() const {
  return window_->is_modal();
}

v8::Local<v8::Value> BrowserWindow::GetNativeWindowHandle() {
  gfx::AcceleratedWidget handle = window_->GetAcceleratedWidget();
  return ToBuffer(
      isolate(), static_cast<void*>(&handle), sizeof(gfx::AcceleratedWidget));
}

void BrowserWindow::SetVisibleOnAllWorkspaces(bool visible) {
  return window_->SetVisibleOnAllWorkspaces(visible);
}

bool BrowserWindow::IsVisibleOnAllWorkspaces() {
  return window_->IsVisibleOnAllWorkspaces();
}

void BrowserWindow::SetAutoHideCursor(bool auto_hide) {
  window_->SetAutoHideCursor(auto_hide);
}

void BrowserWindow::SelectPreviousTab() {
  window_->SelectPreviousTab();
}

void BrowserWindow::SelectNextTab() {
  window_->SelectNextTab();
}

void BrowserWindow::MergeAllWindows() {
  window_->MergeAllWindows();
}

void BrowserWindow::MoveTabToNewWindow() {
  window_->MoveTabToNewWindow();
}

void BrowserWindow::ToggleTabBar() {
  window_->ToggleTabBar();
}

void BrowserWindow::AddTabbedWindow(NativeWindow* window,
                                    mate::Arguments* args) {
  const bool windowAdded = window_->AddTabbedWindow(window);
  if (!windowAdded)
    args->ThrowError("AddTabbedWindow cannot be called by a window on itself.");
}

void BrowserWindow::SetVibrancy(mate::Arguments* args) {
  std::string type;
  args->GetNext(&type);

  auto* render_view_host = web_contents()->GetRenderViewHost();
  if (render_view_host) {
    auto* impl = content::RenderWidgetHostImpl::FromID(
        render_view_host->GetProcess()->GetID(),
        render_view_host->GetRoutingID());
    if (impl)
      impl->SetBackgroundOpaque(type.empty() ? !window_->transparent() : false);
  }

  window_->SetVibrancy(type);
}

void BrowserWindow::SetTouchBar(
    const std::vector<mate::PersistentDictionary>& items) {
  window_->SetTouchBar(items);
}

void BrowserWindow::RefreshTouchBarItem(const std::string& item_id) {
  window_->RefreshTouchBarItem(item_id);
}

void BrowserWindow::SetEscapeTouchBarItem(
    const mate::PersistentDictionary& item) {
  window_->SetEscapeTouchBarItem(item);
}

int32_t BrowserWindow::ID() const {
  return weak_map_id();
}

v8::Local<v8::Value> BrowserWindow::WebContents(v8::Isolate* isolate) {
  if (web_contents_.IsEmpty()) {
    return v8::Null(isolate);
  }

  return v8::Local<v8::Value>::New(isolate, web_contents_);
}

void BrowserWindow::RemoveFromParentChildWindows() {
  if (parent_window_.IsEmpty())
    return;

  mate::Handle<BrowserWindow> parent;
  if (!mate::ConvertFromV8(isolate(), GetParentWindow(), &parent)
    || parent.IsEmpty()) {
    return;
  }

  parent->child_windows_.Remove(ID());
}

// Convert draggable regions in raw format to SkRegion format.
std::unique_ptr<SkRegion> BrowserWindow::DraggableRegionsToSkRegion(
    const std::vector<DraggableRegion>& regions) {
  std::unique_ptr<SkRegion> sk_region(new SkRegion);
  for (const DraggableRegion& region : regions) {
    sk_region->op(
        region.bounds.x(),
        region.bounds.y(),
        region.bounds.right(),
        region.bounds.bottom(),
        region.draggable ? SkRegion::kUnion_Op : SkRegion::kDifference_Op);
  }
  return sk_region;
}

void BrowserWindow::ScheduleUnresponsiveEvent(int ms) {
  if (!window_unresponsive_closure_.IsCancelled())
    return;

  window_unresponsive_closure_.Reset(
      base::Bind(&BrowserWindow::NotifyWindowUnresponsive, GetWeakPtr()));
  base::ThreadTaskRunnerHandle::Get()->PostDelayedTask(
      FROM_HERE,
      window_unresponsive_closure_.callback(),
      base::TimeDelta::FromMilliseconds(ms));
}

void BrowserWindow::NotifyWindowUnresponsive() {
  window_unresponsive_closure_.Cancel();
  if (!window_->IsClosed() && window_->IsEnabled() &&
      !IsUnresponsiveEventSuppressed()) {
    Emit("unresponsive");
  }
}

// static
void BrowserWindow::BuildPrototype(v8::Isolate* isolate,
                                   v8::Local<v8::FunctionTemplate> prototype) {
  prototype->SetClassName(mate::StringToV8(isolate, "BrowserWindow"));
  mate::ObjectTemplateBuilder(isolate, prototype->PrototypeTemplate())
      .MakeDestroyable()
      .SetMethod("close", &BrowserWindow::Close)
      .SetMethod("focus", &BrowserWindow::Focus)
      .SetMethod("blur", &BrowserWindow::Blur)
      .SetMethod("isFocused", &BrowserWindow::IsFocused)
      .SetMethod("show", &BrowserWindow::Show)
      .SetMethod("showInactive", &BrowserWindow::ShowInactive)
      .SetMethod("hide", &BrowserWindow::Hide)
      .SetMethod("isVisible", &BrowserWindow::IsVisible)
      .SetMethod("isEnabled", &BrowserWindow::IsEnabled)
      .SetMethod("setEnabled", & BrowserWindow::SetEnabled)
      .SetMethod("maximize", &BrowserWindow::Maximize)
      .SetMethod("unmaximize", &BrowserWindow::Unmaximize)
      .SetMethod("isMaximized", &BrowserWindow::IsMaximized)
      .SetMethod("minimize", &BrowserWindow::Minimize)
      .SetMethod("restore", &BrowserWindow::Restore)
      .SetMethod("isMinimized", &BrowserWindow::IsMinimized)
      .SetMethod("setFullScreen", &BrowserWindow::SetFullScreen)
      .SetMethod("isFullScreen", &BrowserWindow::IsFullscreen)
      .SetMethod("setAspectRatio", &BrowserWindow::SetAspectRatio)
      .SetMethod("previewFile", &BrowserWindow::PreviewFile)
      .SetMethod("closeFilePreview", &BrowserWindow::CloseFilePreview)
#if !defined(OS_WIN)
      .SetMethod("setParentWindow", &BrowserWindow::SetParentWindow)
#endif
      .SetMethod("getParentWindow", &BrowserWindow::GetParentWindow)
      .SetMethod("getChildWindows", &BrowserWindow::GetChildWindows)
      .SetMethod("getBrowserView", &BrowserWindow::GetBrowserView)
      .SetMethod("setBrowserView", &BrowserWindow::SetBrowserView)
      .SetMethod("isModal", &BrowserWindow::IsModal)
      .SetMethod("getNativeWindowHandle", &BrowserWindow::GetNativeWindowHandle)
      .SetMethod("getBounds", &BrowserWindow::GetBounds)
      .SetMethod("setBounds", &BrowserWindow::SetBounds)
      .SetMethod("getSize", &BrowserWindow::GetSize)
      .SetMethod("setSize", &BrowserWindow::SetSize)
      .SetMethod("getContentBounds", &BrowserWindow::GetContentBounds)
      .SetMethod("setContentBounds", &BrowserWindow::SetContentBounds)
      .SetMethod("getContentSize", &BrowserWindow::GetContentSize)
      .SetMethod("setContentSize", &BrowserWindow::SetContentSize)
      .SetMethod("setMinimumSize", &BrowserWindow::SetMinimumSize)
      .SetMethod("getMinimumSize", &BrowserWindow::GetMinimumSize)
      .SetMethod("setMaximumSize", &BrowserWindow::SetMaximumSize)
      .SetMethod("getMaximumSize", &BrowserWindow::GetMaximumSize)
      .SetMethod("setSheetOffset", &BrowserWindow::SetSheetOffset)
      .SetMethod("setResizable", &BrowserWindow::SetResizable)
      .SetMethod("isResizable", &BrowserWindow::IsResizable)
      .SetMethod("setMovable", &BrowserWindow::SetMovable)
      .SetMethod("isMovable", &BrowserWindow::IsMovable)
      .SetMethod("setMinimizable", &BrowserWindow::SetMinimizable)
      .SetMethod("isMinimizable", &BrowserWindow::IsMinimizable)
      .SetMethod("setMaximizable", &BrowserWindow::SetMaximizable)
      .SetMethod("isMaximizable", &BrowserWindow::IsMaximizable)
      .SetMethod("setFullScreenable", &BrowserWindow::SetFullScreenable)
      .SetMethod("isFullScreenable", &BrowserWindow::IsFullScreenable)
      .SetMethod("setClosable", &BrowserWindow::SetClosable)
      .SetMethod("isClosable", &BrowserWindow::IsClosable)
      .SetMethod("setAlwaysOnTop", &BrowserWindow::SetAlwaysOnTop)
      .SetMethod("isAlwaysOnTop", &BrowserWindow::IsAlwaysOnTop)
      .SetMethod("center", &BrowserWindow::Center)
      .SetMethod("setPosition", &BrowserWindow::SetPosition)
      .SetMethod("getPosition", &BrowserWindow::GetPosition)
      .SetMethod("setTitle", &BrowserWindow::SetTitle)
      .SetMethod("getTitle", &BrowserWindow::GetTitle)
      .SetMethod("flashFrame", &BrowserWindow::FlashFrame)
      .SetMethod("setSkipTaskbar", &BrowserWindow::SetSkipTaskbar)
      .SetMethod("setSimpleFullScreen", &BrowserWindow::SetSimpleFullScreen)
      .SetMethod("isSimpleFullScreen", &BrowserWindow::IsSimpleFullScreen)
      .SetMethod("setKiosk", &BrowserWindow::SetKiosk)
      .SetMethod("isKiosk", &BrowserWindow::IsKiosk)
      .SetMethod("setBackgroundColor", &BrowserWindow::SetBackgroundColor)
      .SetMethod("setHasShadow", &BrowserWindow::SetHasShadow)
      .SetMethod("hasShadow", &BrowserWindow::HasShadow)
      .SetMethod("setOpacity", &BrowserWindow::SetOpacity)
      .SetMethod("getOpacity", &BrowserWindow::GetOpacity)
      .SetMethod("setRepresentedFilename",
                 &BrowserWindow::SetRepresentedFilename)
      .SetMethod("getRepresentedFilename",
                 &BrowserWindow::GetRepresentedFilename)
      .SetMethod("setDocumentEdited", &BrowserWindow::SetDocumentEdited)
      .SetMethod("isDocumentEdited", &BrowserWindow::IsDocumentEdited)
      .SetMethod("setIgnoreMouseEvents", &BrowserWindow::SetIgnoreMouseEvents)
      .SetMethod("setContentProtection", &BrowserWindow::SetContentProtection)
      .SetMethod("setFocusable", &BrowserWindow::SetFocusable)
      .SetMethod("focusOnWebView", &BrowserWindow::FocusOnWebView)
      .SetMethod("blurWebView", &BrowserWindow::BlurWebView)
      .SetMethod("isWebViewFocused", &BrowserWindow::IsWebViewFocused)
      .SetMethod("setProgressBar", &BrowserWindow::SetProgressBar)
      .SetMethod("setOverlayIcon", &BrowserWindow::SetOverlayIcon)
      .SetMethod("setThumbarButtons", &BrowserWindow::SetThumbarButtons)
      .SetMethod("setMenu", &BrowserWindow::SetMenu)
      .SetMethod("setAutoHideMenuBar", &BrowserWindow::SetAutoHideMenuBar)
      .SetMethod("isMenuBarAutoHide", &BrowserWindow::IsMenuBarAutoHide)
      .SetMethod("setMenuBarVisibility", &BrowserWindow::SetMenuBarVisibility)
      .SetMethod("isMenuBarVisible", &BrowserWindow::IsMenuBarVisible)
      .SetMethod("setVisibleOnAllWorkspaces",
                 &BrowserWindow::SetVisibleOnAllWorkspaces)
      .SetMethod("isVisibleOnAllWorkspaces",
                 &BrowserWindow::IsVisibleOnAllWorkspaces)
#if defined(OS_MACOSX)
      .SetMethod("setAutoHideCursor", &BrowserWindow::SetAutoHideCursor)
      .SetMethod("mergeAllWindows", &BrowserWindow::MergeAllWindows)
      .SetMethod("selectPreviousTab", &BrowserWindow::SelectPreviousTab)
      .SetMethod("selectNextTab", &BrowserWindow::SelectNextTab)
      .SetMethod("moveTabToNewWindow", &BrowserWindow::MoveTabToNewWindow)
      .SetMethod("toggleTabBar", &BrowserWindow::ToggleTabBar)
      .SetMethod("addTabbedWindow", &BrowserWindow::AddTabbedWindow)
#endif
      .SetMethod("setVibrancy", &BrowserWindow::SetVibrancy)
      .SetMethod("_setTouchBarItems", &BrowserWindow::SetTouchBar)
      .SetMethod("_refreshTouchBarItem", &BrowserWindow::RefreshTouchBarItem)
      .SetMethod("_setEscapeTouchBarItem",
                 &BrowserWindow::SetEscapeTouchBarItem)
#if defined(OS_WIN)
      .SetMethod("hookWindowMessage", &BrowserWindow::HookWindowMessage)
      .SetMethod("isWindowMessageHooked",
                 &BrowserWindow::IsWindowMessageHooked)
      .SetMethod("unhookWindowMessage", &BrowserWindow::UnhookWindowMessage)
      .SetMethod("unhookAllWindowMessages",
                 &BrowserWindow::UnhookAllWindowMessages)
      .SetMethod("setThumbnailClip", &BrowserWindow::SetThumbnailClip)
      .SetMethod("setThumbnailToolTip", &BrowserWindow::SetThumbnailToolTip)
      .SetMethod("setAppDetails", &BrowserWindow::SetAppDetails)
#endif
#if defined(TOOLKIT_VIEWS)
      .SetMethod("setIcon", &BrowserWindow::SetIcon)
#endif
      .SetProperty("id", &BrowserWindow::ID)
      .SetProperty("webContents", &BrowserWindow::WebContents);
}

// static
v8::Local<v8::Value> BrowserWindow::From(v8::Isolate* isolate,
                                         NativeWindow* native_window) {
  auto existing = TrackableObject::FromWrappedClass(isolate, native_window);
  if (existing)
    return existing->GetWrapper();
  else
    return v8::Null(isolate);
}

}  // namespace api

}  // namespace atom


namespace {

using atom::api::BrowserWindow;

void Initialize(v8::Local<v8::Object> exports, v8::Local<v8::Value> unused,
                v8::Local<v8::Context> context, void* priv) {
  v8::Isolate* isolate = context->GetIsolate();
  BrowserWindow::SetConstructor(isolate, base::Bind(&BrowserWindow::New));

  mate::Dictionary browser_window(
      isolate, BrowserWindow::GetConstructor(isolate)->GetFunction());
  browser_window.SetMethod(
      "fromId",
      &mate::TrackableObject<BrowserWindow>::FromWeakMapID);
  browser_window.SetMethod(
      "getAllWindows",
      &mate::TrackableObject<BrowserWindow>::GetAll);

  mate::Dictionary dict(isolate, exports);
  dict.Set("BrowserWindow", browser_window);
}

}  // namespace

NODE_BUILTIN_MODULE_CONTEXT_AWARE(atom_browser_window, Initialize)
