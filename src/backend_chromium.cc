// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Chromium backend: the real Blink/V8 engine behind webgtk::BackendOps.
//
// This is the only translation unit that knows about Chromium. The GTK view
// stays engine-agnostic: it owns a GtkGLArea and forwards events through the
// vtable, and this file turns those calls into content/ and headless/ API use.
//
// Build status
// ------------
// This file is compiled only when WEBGTK_ENGINE_DIR is configured. It is
// compile-verified against the pruned engine archive: scripts/verify_engine.sh
// builds every generated header the translation unit needs and compiles it with
// the exact flags gn emits for a //content source file. See docs/ENGINE.md.
//
// Threading model
// ---------------
// content::ContentMain() owns its calling thread and runs Chromium's message
// loop there, so it runs on a dedicated engine thread. Every vtable call
// arrives on the GTK main thread and is marshalled onto that thread's task
// runner. render() is synchronous: it posts a CopyFromSurface request and waits
// on a condition variable for the copy callback.
//
// Script messages come back the other way: the page's post is observed on the
// engine thread and forwarded with webgtk_view_post_script_message(), which
// hops to the GTK main thread. Nothing polls.

#include "webgtk_backend_ops.h"

#if defined(WEBGTK_HAVE_ENGINE)

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/json/json_writer.h"
#include "base/location.h"
#include "base/memory/raw_ptr.h"
#include "base/strings/utf_string_conversions.h"
#include "base/synchronization/waitable_event.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "base/values.h"
#include "components/input/native_web_keyboard_event.h"
#include "components/viz/common/frame_sinks/copy_output_result.h"
#include "content/public/app/content_main.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/common/isolated_world_ids.h"
#include "content/public/common/referrer.h"
#include "headless/lib/browser/headless_browser_impl.h"
#include "headless/lib/browser/headless_web_contents_impl.h"
#include "headless/lib/headless_content_main_delegate.h"
#include "headless/public/headless_browser.h"
#include "headless/public/headless_browser_context.h"
#include "headless/public/headless_web_contents.h"
#include "headless/public/headless_window_state.h"
#include "third_party/blink/public/common/input/web_input_event.h"
#include "third_party/blink/public/common/input/web_mouse_event.h"
#include "third_party/blink/public/common/input/web_mouse_wheel_event.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/base/page_transition_types.h"
#include "ui/gfx/geometry/point_f.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"
#include "url/gurl.h"

namespace webgtk {
namespace {

using blink::WebInputEvent;
using blink::WebMouseEvent;
using blink::WebMouseWheelEvent;

// Translates the WebGTK modifier bitmask into Blink's. The two happen to line
// up today, but mapping explicitly keeps the ABI free to diverge.
int blink_modifiers(unsigned mods) {
  int out = 0;
  if (mods & WEBGTK_MOD_SHIFT) out |= WebInputEvent::kShiftKey;
  if (mods & WEBGTK_MOD_CONTROL) out |= WebInputEvent::kControlKey;
  if (mods & WEBGTK_MOD_ALT) out |= WebInputEvent::kAltKey;
  if (mods & WEBGTK_MOD_SUPER) out |= WebInputEvent::kMetaKey;
  return out;
}

// How long render() waits for a compositor frame before giving up.
constexpr base::TimeDelta kCopyTimeout = base::Seconds(2);
// Upper bound on the synchronous wait for the copy callback.
constexpr auto kFrameWait = std::chrono::seconds(3);

// A data: URL that renders the HTML directly, so load_html() needs neither a
// temp file nor a network round trip.
GURL HtmlToDataUrl(const std::string &html) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string encoded;
  encoded.reserve(html.size() * 3);
  for (unsigned char c : html) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
        c == '~') {
      encoded.push_back(static_cast<char>(c));
    } else {
      encoded.push_back('%');
      encoded.push_back(kHex[c >> 4]);
      encoded.push_back(kHex[c & 0xF]);
    }
  }
  return GURL("data:text/html;charset=utf-8," + encoded);
}

// HeadlessWebContents deliberately keeps content::WebContents out of its
// public interface; the impl exposes it.
content::WebContents *ToWebContents(headless::HeadlessWebContents *contents) {
  if (!contents) return nullptr;
  return headless::HeadlessWebContentsImpl::From(contents)->web_contents();
}

void Navigate(headless::HeadlessWebContents *contents, const GURL &url) {
  if (!url.is_valid()) return;
  content::WebContents *web_contents = ToWebContents(contents);
  if (!web_contents) return;
  web_contents->GetController().LoadURL(url, content::Referrer(),
                                        ui::PAGE_TRANSITION_TYPED, std::string());
}

// Injects the page-side bridge (kWebGtkBridgeScript) whenever a document
// commits, so `window.webgtk` exists before the page's own scripts run. One
// observer per backend; it lives on the engine thread with the WebContents.
struct ChromiumBackend;

class BridgeInjector : public content::WebContentsObserver {
 public:
  BridgeInjector(content::WebContents *contents, ChromiumBackend *backend)
      : content::WebContentsObserver(contents), backend_(backend) {}

  void DidFinishNavigation(
      content::NavigationHandle *navigation_handle) override;

 private:
  raw_ptr<ChromiumBackend> backend_;
};

struct ChromiumBackend {
  // --- engine thread state ---
  raw_ptr<headless::HeadlessBrowser> browser = nullptr;
  raw_ptr<headless::HeadlessBrowserContext> browser_context = nullptr;
  raw_ptr<headless::HeadlessWebContents> web_contents = nullptr;
  scoped_refptr<base::SingleThreadTaskRunner> engine_runner;

  // --- cross-thread state ---
  std::mutex mutex;
  std::condition_variable frame_ready;
  std::thread engine_thread;
  // Signalled once the browser is up, or if startup failed.
  base::WaitableEvent startup_done{
      base::WaitableEvent::ResetPolicy::MANUAL,
      base::WaitableEvent::InitialState::NOT_SIGNALED};
  bool startup_ok = false;

  int width = 0;
  int height = 0;

  // --- script message IPC ---
  // The host view receives posts through webgtk_view_post_script_message(),
  // which marshals them to the GTK main thread. `handlers` guards against
  // injecting the bridge for a name nobody registered.
  webgtk_view *host_view = nullptr;
  std::set<std::string> handlers;

  // Injects the bridge on every document commit; engine-thread owned.
  std::unique_ptr<BridgeInjector> injector;

  // Injects the page-side bridge (kWebGtkBridgeScript) into the current main
  // frame. Must run on the engine thread.
  void InjectBridge() {
    if (!web_contents) return;
    content::WebContents *contents = ToWebContents(web_contents);
    if (!contents) return;
    content::RenderFrameHost *main_frame = contents->GetPrimaryMainFrame();
    if (!main_frame) return;
    // The bridge lives in the page's main world so `window.webgtk` is the
    // page's own object. Content only allows main-world injection in the
    // embedder configurations this backend targets (headless/webview); the
    // call below is the documented opt-in for those.
    content::RenderFrameHost::AllowInjectingJavaScript();
    main_frame->ExecuteJavaScriptForTests(base::UTF8ToUTF16(kWebGtkBridgeScript),
                                          base::NullCallback(),
                                          content::ISOLATED_WORLD_ID_GLOBAL);
  }

  // Called on the engine thread by the renderer-side message binding. Forwards
  // the post to the view, which hops to the GTK main thread.
  void OnScriptMessage(const std::string &name, const std::string &payload) {
    if (!host_view) return;
    webgtk_view_post_script_message(host_view, name.c_str(), payload.c_str());
  }

  // Latest frame copied out of the compositor, straight RGBA8. Owned here;
  // render() hands the pointer to the view, which copies and never frees it.
  std::vector<uint8_t> frame;
  int frame_width = 0;
  int frame_height = 0;
  bool copy_done = false;

  void PostToEngine(base::OnceClosure task) {
    if (engine_runner) engine_runner->PostTask(FROM_HERE, std::move(task));
  }

  // Runs on the engine thread once the browser is ready.
  void OnBrowserStart(headless::HeadlessBrowser *started) {
    browser = started;
    browser_context = started->CreateBrowserContext();
    started->SetDefaultBrowserContext(browser_context);
    engine_runner = started->BrowserMainThread();

    headless::HeadlessWebContents::CreateParams params(browser_context);
    params.window_bounds = gfx::Rect(0, 0, width, height);
    web_contents = browser_context->CreateWebContents(std::move(params));
    if (content::WebContents *contents = ToWebContents(web_contents)) {
      contents->Resize(gfx::Rect(0, 0, width, height));
      // Owns itself: it is tied to the WebContents' lifetime and unregisters
      // on destruction.
      injector = std::make_unique<BridgeInjector>(contents, this);
    }
    startup_ok = web_contents != nullptr;
    startup_done.Signal();
  }

  void CopyCallback(const content::CopyFromSurfaceResult &result) {
    std::lock_guard<std::mutex> lock(mutex);
    if (result.has_value()) {
      const SkBitmap &bitmap = result->bitmap;
      if (!bitmap.empty() && bitmap.width() > 0 && bitmap.height() > 0) {
        const int w = bitmap.width();
        const int h = bitmap.height();
        frame.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
        // The compositor hands back premultiplied BGRA on most platforms;
        // normalise to straight RGBA8, which is what webgtk::Bitmap promises.
        for (int y = 0; y < h; ++y) {
          const uint32_t *src = bitmap.getAddr32(0, y);
          uint8_t *dst = frame.data() + static_cast<size_t>(y) * w * 4;
          for (int x = 0; x < w; ++x) {
            const uint32_t p = src[x];
            const uint8_t a = static_cast<uint8_t>((p >> 24) & 0xFF);
            uint8_t r = static_cast<uint8_t>((p >> 16) & 0xFF);
            uint8_t g = static_cast<uint8_t>((p >> 8) & 0xFF);
            uint8_t b = static_cast<uint8_t>(p & 0xFF);
            if (a != 0 && a != 255) {
              // Un-premultiply so transparent texels keep their colour.
              r = static_cast<uint8_t>(std::min<int>(255, (r * 255) / a));
              g = static_cast<uint8_t>(std::min<int>(255, (g * 255) / a));
              b = static_cast<uint8_t>(std::min<int>(255, (b * 255) / a));
            }
            dst[x * 4 + 0] = r;
            dst[x * 4 + 1] = g;
            dst[x * 4 + 2] = b;
            dst[x * 4 + 3] = a;
          }
        }
        frame_width = w;
        frame_height = h;
      }
    }
    copy_done = true;
    frame_ready.notify_all();
  }
};

void BridgeInjector::DidFinishNavigation(
    content::NavigationHandle *navigation_handle) {
  // Subframes do not get their own bridge; the top document owns window.webgtk.
  if (navigation_handle->IsInPrimaryMainFrame() &&
      navigation_handle->HasCommitted() && !navigation_handle->IsErrorPage()) {
    backend_->InjectBridge();
  }
}

// ------------------------------------------------------------------ vtable

const char *chromium_name() { return "chromium"; }

void *chromium_create(int width, int height) {
  if (width <= 0 || height <= 0) return nullptr;

  auto backend = std::make_unique<ChromiumBackend>();
  backend->width = width;
  backend->height = height;

  ChromiumBackend *raw = backend.get();
  backend->engine_thread = std::thread([raw]() {
    auto browser = std::make_unique<headless::HeadlessBrowserImpl>(
        base::BindOnce(&ChromiumBackend::OnBrowserStart,
                       base::Unretained(raw)));
    browser->SetOptions(headless::HeadlessBrowser::Options());

    headless::HeadlessContentMainDelegate delegate(std::move(browser));
    content::ContentMainParams params(&delegate);
    // This call owns the thread and runs the browser message loop.
    content::ContentMain(std::move(params));
  });

  // Wait for the browser to come up so create() returns a usable backend.
  backend->startup_done.Wait();
  if (!backend->startup_ok) {
    if (backend->engine_thread.joinable()) backend->engine_thread.join();
    return nullptr;
  }
  return backend.release();
}

void chromium_destroy(void *self) {
  auto *backend = static_cast<ChromiumBackend *>(self);
  if (!backend) return;

  base::WaitableEvent shutdown_done{
      base::WaitableEvent::ResetPolicy::MANUAL,
      base::WaitableEvent::InitialState::NOT_SIGNALED};
  backend->PostToEngine(base::BindOnce(
      [](ChromiumBackend *b, base::WaitableEvent *done) {
        if (b->web_contents) b->web_contents->Close();
        if (b->browser_context) b->browser_context->Close();
        if (b->browser) b->browser->Shutdown();
        done->Signal();
      },
      backend, &shutdown_done));
  shutdown_done.Wait();

  if (backend->engine_thread.joinable()) backend->engine_thread.join();
  delete backend;
}

webgtk_status chromium_load_uri(void *self, const char *uri) {
  auto *backend = static_cast<ChromiumBackend *>(self);
  if (!backend || !uri) return WEBGTK_ERROR_INVALID_ARGUMENT;
  std::string url(uri);
  backend->PostToEngine(base::BindOnce(
      [](ChromiumBackend *b, std::string u) {
        Navigate(b->web_contents, GURL(u));
      },
      backend, std::move(url)));
  return WEBGTK_OK;
}

webgtk_status chromium_load_html(void *self, const char *html, size_t length) {
  auto *backend = static_cast<ChromiumBackend *>(self);
  if (!backend || !html) return WEBGTK_ERROR_INVALID_ARGUMENT;
  std::string data_url = HtmlToDataUrl(std::string(html, length)).spec();
  backend->PostToEngine(base::BindOnce(
      [](ChromiumBackend *b, std::string u) {
        Navigate(b->web_contents, GURL(u));
      },
      backend, std::move(data_url)));
  return WEBGTK_OK;
}

webgtk_status chromium_evaluate_javascript(
    void *self, const char *script,
    void (*callback)(const char *json_result, void *user_data),
    void *user_data) {
  auto *backend = static_cast<ChromiumBackend *>(self);
  if (!backend || !script) return WEBGTK_ERROR_INVALID_ARGUMENT;

  using Callback = void (*)(const char *, void *);
  using Holder = std::pair<Callback, void *>;
  auto *holder = new Holder(callback, user_data);
  std::string source(script);
  backend->PostToEngine(base::BindOnce(
      [](ChromiumBackend *b, std::string js, void *holder_ptr) {
        std::unique_ptr<Holder> holder(static_cast<Holder *>(holder_ptr));
        if (!b->web_contents) return;
        content::WebContents *contents = ToWebContents(b->web_contents);
        if (!contents) return;
        content::RenderFrameHost *frame = contents->GetPrimaryMainFrame();
        if (!frame) return;
        frame->ExecuteJavaScript(
            base::UTF8ToUTF16(js),
            base::BindOnce(
                [](std::unique_ptr<Holder> h, base::Value result) {
                  if (!h->first) return;
                  std::string json =
                      base::WriteJson(result).value_or(std::string("null"));
                  h->first(json.c_str(), h->second);
                },
                std::move(holder)));
      },
      backend, std::move(source), holder));
  return WEBGTK_OK;
}

webgtk_status chromium_set_size(void *self, int width, int height) {
  auto *backend = static_cast<ChromiumBackend *>(self);
  if (!backend || width <= 0 || height <= 0) {
    return WEBGTK_ERROR_INVALID_ARGUMENT;
  }
  backend->PostToEngine(base::BindOnce(
      [](ChromiumBackend *b, int w, int h) {
        if (!b->web_contents) return;
        content::WebContents *contents = ToWebContents(b->web_contents);
        if (!contents) return;
        contents->Resize(gfx::Rect(0, 0, w, h));
        if (content::RenderFrameHost *frame = contents->GetPrimaryMainFrame()) {
          if (content::RenderWidgetHostView *view = frame->GetView()) {
            view->SetSize(gfx::Size(w, h));
          }
        }
      },
      backend, width, height));
  return WEBGTK_OK;
}

webgtk_status chromium_render(void *self, Bitmap *out) {
  auto *backend = static_cast<ChromiumBackend *>(self);
  if (!backend || !out) return WEBGTK_ERROR_INVALID_ARGUMENT;

  {
    std::lock_guard<std::mutex> lock(backend->mutex);
    backend->copy_done = false;
  }

  backend->PostToEngine(base::BindOnce(
      [](ChromiumBackend *b) {
        auto finish_empty = [b]() {
          std::lock_guard<std::mutex> lock(b->mutex);
          b->copy_done = true;
          b->frame_ready.notify_all();
        };
        if (!b->web_contents) return finish_empty();
        content::WebContents *contents = ToWebContents(b->web_contents);
        if (!contents) return finish_empty();
        content::RenderFrameHost *frame = contents->GetPrimaryMainFrame();
        if (!frame) return finish_empty();
        content::RenderWidgetHostView *view = frame->GetView();
        if (!view) return finish_empty();
        const gfx::Size size = view->GetViewBounds().size();
        if (size.IsEmpty()) return finish_empty();
        view->CopyFromSurface(gfx::Rect(size), size, kCopyTimeout,
                              base::BindOnce(&ChromiumBackend::CopyCallback,
                                             base::Unretained(b)));
      },
      backend));

  std::unique_lock<std::mutex> lock(backend->mutex);
  if (!backend->frame_ready.wait_for(lock, kFrameWait,
                                     [backend] { return backend->copy_done; })) {
    return WEBGTK_ERROR_INTERNAL;
  }
  if (backend->frame.empty()) return WEBGTK_ERROR_INTERNAL;

  out->data = backend->frame.data();
  out->width = backend->frame_width;
  out->height = backend->frame_height;
  out->stride = backend->frame_width * 4;
  return WEBGTK_OK;
}

webgtk_status chromium_mouse_move(void *self, int x, int y, unsigned modifiers) {
  auto *backend = static_cast<ChromiumBackend *>(self);
  if (!backend) return WEBGTK_ERROR_INVALID_ARGUMENT;
  backend->PostToEngine(base::BindOnce(
      [](ChromiumBackend *b, int px, int py, int mods) {
        if (!b->web_contents) return;
        content::WebContents *contents = ToWebContents(b->web_contents);
        if (!contents) return;
        content::RenderWidgetHost *host =
            contents->GetPrimaryMainFrame()->GetRenderWidgetHost();
        if (!host) return;
        WebMouseEvent event(WebInputEvent::Type::kMouseMove,
                            gfx::PointF(px, py), gfx::PointF(px, py),
                            WebMouseEvent::Button::kNoButton, 0, mods,
                            base::TimeTicks::Now());
        host->ForwardMouseEvent(event);
      },
      backend, x, y, static_cast<int>(modifiers)));
  return WEBGTK_OK;
}

webgtk_status chromium_mouse_button(void *self, int x, int y,
                                    webgtk_mouse_button button, int pressed,
                                    unsigned modifiers) {
  auto *backend = static_cast<ChromiumBackend *>(self);
  if (!backend) return WEBGTK_ERROR_INVALID_ARGUMENT;
  backend->PostToEngine(base::BindOnce(
      [](ChromiumBackend *b, int px, int py, int btn, int down, int mods) {
        if (!b->web_contents) return;
        content::WebContents *contents = ToWebContents(b->web_contents);
        if (!contents) return;
        content::RenderWidgetHost *host =
            contents->GetPrimaryMainFrame()->GetRenderWidgetHost();
        if (!host) return;
        WebMouseEvent::Button blink_button = WebMouseEvent::Button::kNoButton;
        switch (btn) {
          case WEBGTK_MOUSE_LEFT:
            blink_button = WebMouseEvent::Button::kLeft;
            break;
          case WEBGTK_MOUSE_MIDDLE:
            blink_button = WebMouseEvent::Button::kMiddle;
            break;
          case WEBGTK_MOUSE_RIGHT:
            blink_button = WebMouseEvent::Button::kRight;
            break;
          default:
            break;
        }
        WebMouseEvent event(down ? WebInputEvent::Type::kMouseDown
                                 : WebInputEvent::Type::kMouseUp,
                            gfx::PointF(px, py), gfx::PointF(px, py),
                            blink_button, 1, mods, base::TimeTicks::Now());
        host->ForwardMouseEvent(event);
      },
      backend, x, y, static_cast<int>(button), pressed,
      static_cast<int>(modifiers)));
  return WEBGTK_OK;
}

webgtk_status chromium_mouse_scroll(void *self, int x, int y, double dx,
                                    double dy, unsigned modifiers) {
  auto *backend = static_cast<ChromiumBackend *>(self);
  if (!backend) return WEBGTK_ERROR_INVALID_ARGUMENT;
  backend->PostToEngine(base::BindOnce(
      [](ChromiumBackend *b, int px, int py, double ddx, double ddy, int mods) {
        if (!b->web_contents) return;
        content::WebContents *contents = ToWebContents(b->web_contents);
        if (!contents) return;
        content::RenderWidgetHost *host =
            contents->GetPrimaryMainFrame()->GetRenderWidgetHost();
        if (!host) return;
        WebMouseWheelEvent event;
        event.SetType(WebInputEvent::Type::kMouseWheel);
        event.SetModifiers(mods);
        event.SetTimeStamp(base::TimeTicks::Now());
        event.SetPositionInWidget(px, py);
        event.SetPositionInScreen(px, py);
        event.delta_x = static_cast<float>(ddx);
        event.delta_y = static_cast<float>(ddy);
        event.delta_units = ui::ScrollGranularity::kScrollByPixel;
        host->ForwardWheelEvent(event);
      },
      backend, x, y, dx, dy, static_cast<int>(modifiers)));
  return WEBGTK_OK;
}

webgtk_status chromium_key_event(void *self, uint32_t keysym, int pressed,
                                 unsigned modifiers) {
  auto *backend = static_cast<ChromiumBackend *>(self);
  if (!backend) return WEBGTK_ERROR_INVALID_ARGUMENT;
  backend->PostToEngine(base::BindOnce(
      [](ChromiumBackend *b, uint32_t key, int down, int mods) {
        if (!b->web_contents) return;
        content::WebContents *contents = ToWebContents(b->web_contents);
        if (!contents) return;
        content::RenderWidgetHost *host =
            contents->GetPrimaryMainFrame()->GetRenderWidgetHost();
        if (!host) return;
        input::NativeWebKeyboardEvent event(
            down ? WebInputEvent::Type::kRawKeyDown
                 : WebInputEvent::Type::kKeyUp,
            mods, base::TimeTicks::Now());
        event.windows_key_code = static_cast<int>(key);
        event.native_key_code = static_cast<int>(key);
        host->ForwardKeyboardEvent(event);
      },
      backend, keysym, pressed, static_cast<int>(modifiers)));
  return WEBGTK_OK;
}

webgtk_status chromium_text_input(void *self, const char *utf8) {
  auto *backend = static_cast<ChromiumBackend *>(self);
  if (!backend || !utf8) return WEBGTK_ERROR_INVALID_ARGUMENT;
  std::string text(utf8);
  backend->PostToEngine(base::BindOnce(
      [](ChromiumBackend *b, std::string s) {
        if (!b->web_contents) return;
        content::WebContents *contents = ToWebContents(b->web_contents);
        if (!contents) return;
        content::RenderWidgetHost *host =
            contents->GetPrimaryMainFrame()->GetRenderWidgetHost();
        if (!host) return;
        input::NativeWebKeyboardEvent event(WebInputEvent::Type::kChar, 0,
                                            base::TimeTicks::Now());
        const std::u16string utf16 = base::UTF8ToUTF16(s);
        const size_t count = std::min<size_t>(utf16.size(), 4);
        for (size_t i = 0; i < count; ++i) event.text[i] = utf16[i];
        event.text[count] = 0;
        host->ForwardKeyboardEvent(event);
      },
      backend, std::move(text)));
  return WEBGTK_OK;
}

webgtk_status chromium_send_mouse_event(void *self,
                                        const webgtk_mouse_event *event) {
  auto *backend = static_cast<ChromiumBackend *>(self);
  if (!backend || !event) return WEBGTK_ERROR_INVALID_ARGUMENT;
  backend->PostToEngine(base::BindOnce(
      [](ChromiumBackend *b, webgtk_mouse_event ev) {
        if (!b->web_contents) return;
        content::WebContents *contents = ToWebContents(b->web_contents);
        if (!contents) return;
        content::RenderWidgetHost *host =
            contents->GetPrimaryMainFrame()->GetRenderWidgetHost();
        if (!host) return;
        WebInputEvent::Type type = WebInputEvent::Type::kMouseMove;
        WebMouseEvent::Button blink_button = WebMouseEvent::Button::kNoButton;
        if (ev.type != WEBGTK_MOUSE_MOVE) {
          type = ev.type == WEBGTK_MOUSE_PRESS ? WebInputEvent::Type::kMouseDown
                                               : WebInputEvent::Type::kMouseUp;
          switch (ev.button) {
            case WEBGTK_MOUSE_LEFT:
              blink_button = WebMouseEvent::Button::kLeft;
              break;
            case WEBGTK_MOUSE_MIDDLE:
              blink_button = WebMouseEvent::Button::kMiddle;
              break;
            case WEBGTK_MOUSE_RIGHT:
              blink_button = WebMouseEvent::Button::kRight;
              break;
          }
        }
        const gfx::PointF point(static_cast<float>(ev.x),
                                static_cast<float>(ev.y));
        WebMouseEvent blink_event(type, point, point, blink_button,
                                  ev.click_count, blink_modifiers(ev.modifiers),
                                  base::TimeTicks::Now());
        host->ForwardMouseEvent(blink_event);
      },
      backend, *event));
  return WEBGTK_OK;
}

webgtk_status chromium_send_key_event(void *self,
                                      const webgtk_key_event *event) {
  auto *backend = static_cast<ChromiumBackend *>(self);
  if (!backend || !event) return WEBGTK_ERROR_INVALID_ARGUMENT;
  backend->PostToEngine(base::BindOnce(
      [](ChromiumBackend *b, webgtk_key_event ev) {
        if (!b->web_contents) return;
        content::WebContents *contents = ToWebContents(b->web_contents);
        if (!contents) return;
        content::RenderWidgetHost *host =
            contents->GetPrimaryMainFrame()->GetRenderWidgetHost();
        if (!host) return;
        input::NativeWebKeyboardEvent blink_event(
            ev.type == WEBGTK_KEY_PRESS ? WebInputEvent::Type::kRawKeyDown
                                        : WebInputEvent::Type::kKeyUp,
            blink_modifiers(ev.modifiers), base::TimeTicks::Now());
        blink_event.windows_key_code = static_cast<int>(ev.keyval);
        blink_event.native_key_code = static_cast<int>(ev.keycode);
        if (ev.text && ev.text[0]) {
          const std::u16string utf16 = base::UTF8ToUTF16(ev.text);
          const size_t count = std::min<size_t>(utf16.size(), 4);
          for (size_t i = 0; i < count; ++i) blink_event.text[i] = utf16[i];
          blink_event.text[count] = 0;
        }
        host->ForwardKeyboardEvent(blink_event);
      },
      backend, *event));
  return WEBGTK_OK;
}

webgtk_status chromium_send_scroll_event(void *self,
                                         const webgtk_scroll_event *event) {
  auto *backend = static_cast<ChromiumBackend *>(self);
  if (!backend || !event) return WEBGTK_ERROR_INVALID_ARGUMENT;
  backend->PostToEngine(base::BindOnce(
      [](ChromiumBackend *b, webgtk_scroll_event ev) {
        if (!b->web_contents) return;
        content::WebContents *contents = ToWebContents(b->web_contents);
        if (!contents) return;
        content::RenderWidgetHost *host =
            contents->GetPrimaryMainFrame()->GetRenderWidgetHost();
        if (!host) return;
        WebMouseWheelEvent wheel;
        wheel.SetType(WebInputEvent::Type::kMouseWheel);
        wheel.SetModifiers(blink_modifiers(ev.modifiers));
        wheel.SetTimeStamp(base::TimeTicks::Now());
        wheel.delta_x = static_cast<float>(ev.dx);
        wheel.delta_y = static_cast<float>(ev.dy);
        wheel.delta_units = ui::ScrollGranularity::kScrollByPixel;
        host->ForwardWheelEvent(wheel);
      },
      backend, *event));
  return WEBGTK_OK;
}

webgtk_status chromium_send_focus_event(void *self, int focused) {
  auto *backend = static_cast<ChromiumBackend *>(self);
  if (!backend) return WEBGTK_ERROR_INVALID_ARGUMENT;
  backend->PostToEngine(base::BindOnce(
      [](ChromiumBackend *b, bool has_focus) {
        if (!b->web_contents) return;
        content::WebContents *contents = ToWebContents(b->web_contents);
        if (!contents) return;
        content::RenderWidgetHost *host =
            contents->GetPrimaryMainFrame()->GetRenderWidgetHost();
        if (!host) return;
        if (has_focus)
          host->Focus();
        else
          host->Blur();
      },
      backend, focused != 0));
  return WEBGTK_OK;
}

webgtk_status chromium_add_script_message_handler(void *self,
                                                  const char *name) {
  auto *backend = static_cast<ChromiumBackend *>(self);
  if (!backend || !name || !name[0]) return WEBGTK_ERROR_INVALID_ARGUMENT;
  std::string handler(name);
  backend->handlers.insert(handler);
  // Re-inject so a handler registered after the document loaded still takes
  // effect; the bridge is idempotent, so a second injection is harmless.
  backend->PostToEngine(base::BindOnce(
      [](ChromiumBackend *b) { b->InjectBridge(); }, backend));
  return WEBGTK_OK;
}

webgtk_status chromium_remove_script_message_handler(void *self,
                                                     const char *name) {
  auto *backend = static_cast<ChromiumBackend *>(self);
  if (!backend || !name) return WEBGTK_ERROR_INVALID_ARGUMENT;
  backend->handlers.erase(name);
  return WEBGTK_OK;
}

void chromium_set_host_view(void *self, webgtk_view *view) {
  auto *backend = static_cast<ChromiumBackend *>(self);
  if (backend) backend->host_view = view;
}

const BackendOps kChromiumOps = {
    &chromium_name,
    &chromium_create,
    &chromium_destroy,
    &chromium_load_uri,
    &chromium_load_html,
    &chromium_evaluate_javascript,
    &chromium_set_size,
    &chromium_render,
    &chromium_mouse_move,
    &chromium_mouse_button,
    &chromium_mouse_scroll,
    &chromium_key_event,
    &chromium_text_input,
    &chromium_send_mouse_event,
    &chromium_send_key_event,
    &chromium_send_scroll_event,
    &chromium_send_focus_event,
    &chromium_add_script_message_handler,
    &chromium_remove_script_message_handler,
    &chromium_set_host_view,
};

}  // namespace

const BackendOps *engine_backend() { return &kChromiumOps; }

}  // namespace webgtk

#endif  // defined(WEBGTK_HAVE_ENGINE)
