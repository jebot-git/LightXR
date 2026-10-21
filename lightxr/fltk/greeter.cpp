// SPDX-License-Identifier: GPL-3.0-or-later
extern "C" {
#include <lightdm.h>
}
#include "appearance.h"
#include "gpu-window.h"
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Return_Button.H>
#include <FL/Fl_Secret_Input.H>
#include <FL/fl_ask.H>
#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <ft2build.h>
#include <map>
#include <sys/socket.h>
#include <sys/un.h>
#include FT_FREETYPE_H
static constexpr int W = 1280, H = 800;
static std::string framed(const std::string &data) {
  uint32_t n = htonl(data.size());
  return std::string((char *)&n, 4) + data;
}
class Greeter : public dx::GpuWindow {
  LightDMGreeter *daemon = nullptr;
  bool demo, connected = false, authenticating = false, pending = false,
             starting = false;
  struct Prompt {
    std::string text;
    bool secret;
  };
  std::deque<Prompt> prompts;
  Fl_Box brand{50, 20, 1180, 40, "LightXR — Sign in to DomeD"};
  Fl_Input username{180, 100, 800, 40, "Account"},
      answer{180, 200, 800, 40, "Response"};
  Fl_Box prompt{180, 150, 800, 40, "Your password will be requested securely."},
      message{180, 370, 920, 60};
  Fl_Return_Button submit{180, 260, 240, 40, "Sign in"};
  Fl_Button cancel{440, 260, 240, 40, "Cancel authentication"},
      session{180, 320, 800, 35};
  std::vector<std::string> sessions, names;
  int selected = 0;
  Fl_Input *target = &username;
  std::vector<Fl_Button *> keys;
  std::map<Fl_Button *, std::string> keyBases;
  bool shifted = false;
  bool frameDirty = true;
  std::chrono::steady_clock::time_point lastFrame{};
  bool remoteDown = false, remoteVisible = false;
  int remoteX = 0, remoteY = 0;
  int bridge = -1;
  bool connecting = false;
  std::string bridgePath, incoming, outgoing;
  std::chrono::steady_clock::time_point reconnect{};
  FT_Library ft = nullptr;
  FT_Face face = nullptr;
  static void tick(void *p) {
    auto s = (Greeter *)p;
    for (int n = 0; n < 16 && g_main_context_pending(nullptr); ++n)
      g_main_context_iteration(nullptr, FALSE);
    s->pump();
    Fl::repeat_timeout(.1, tick, p);
  }
  void status(const std::string &s) {
    message.copy_label(s.c_str());
    frameDirty = true;
  }
  void clear() { answer.value(""); }
  void idle(const std::string &text) {
    prompts.clear();
    clear();
    authenticating = pending = starting = false;
    username.activate();
    answer.deactivate();
    submit.activate();
    submit.label("Sign in");
    target = &username;
    username.take_focus();
    status(text);
  }
  void showPrompt() {
    if (prompts.empty())
      return;
    prompt.copy_label(prompts.front().text.c_str());
    answer.type(prompts.front().secret ? FL_SECRET_INPUT : FL_NORMAL_INPUT);
    clear();
    answer.activate();
    submit.activate();
    submit.label("Continue");
    target = &answer;
    answer.take_focus();
  }
  void complete() {
    clear();
    prompts.clear();
    pending = false;
    if (!lightdm_greeter_get_is_authenticated(daemon)) {
      idle("Authentication was not accepted. Please try again.");
      return;
    }
    starting = true;
    status("Starting " + names[selected] + "...");
    submit.deactivate();
    outgoing += framed("H");
    lightdm_greeter_start_session(
        daemon, sessions[selected].c_str(), nullptr,
        [](GObject *obj, GAsyncResult *result, gpointer data) {
          auto s = (Greeter *)data;
          GError *e = nullptr;
          if (!lightdm_greeter_start_session_finish(LIGHTDM_GREETER(obj),
                                                    result, &e)) {
            s->idle(e ? e->message : "Unable to start session");
          }
          if (e)
            g_error_free(e);
        },
        this);
  }
  void signIn() {
    if (starting || pending)
      return;
    if (demo) {
      status("Preview only — authentication is disabled.");
      return;
    }
    if (!connected)
      return;
    if (sessions.empty()) {
      status("No installed desktop sessions are available.");
      return;
    }
    GError *e = nullptr;
    if (!authenticating) {
      if (!*username.value()) {
        status("Enter your account name.");
        return;
      }
      authenticating = true;
      username.deactivate();
      submit.deactivate();
      status("Authenticating...");
      if (!lightdm_greeter_authenticate(daemon, username.value(), &e))
        idle("Unable to start authentication.");
    } else if (!prompts.empty()) {
      std::string response = answer.value();
      clear();
      prompts.pop_front();
      submit.deactivate();
      answer.deactivate();
      bool ok = lightdm_greeter_respond(daemon, response.c_str(), &e);
      std::fill(response.begin(), response.end(), '\0');
      if (!ok)
        idle("Unable to send authentication response.");
      else if (!prompts.empty())
        showPrompt();
    }
    if (e) {
      status(e->message);
      g_error_free(e);
    }
  }
  void keyboard(const std::string &label) {
    if (label == "Enter") {
      signIn();
      return;
    }
    if (label == "Tab") {
      target = target == &username ? &answer : &username;
      target->take_focus();
      return;
    }
    if (label == "Shift") {
      shifted = !shifted;
      const std::string plain = "`1234567890-=[];',./\\",
                        upper = "~!@#$%^&*()_+{}:\"<>?|";
      for (auto &item : keyBases) {
        auto text = item.second;
        auto index = plain.find(text);
        if (shifted) {
          if (index != std::string::npos)
            text = upper.substr(index, 1);
          else
            std::transform(text.begin(), text.end(), text.begin(), ::toupper);
        }
        item.first->copy_label(text.c_str());
      }
      return;
    }
    if (!target->active())
      return;
    target->take_focus();
    int start = std::min(target->insert_position(), target->mark()),
        end = std::max(target->insert_position(), target->mark());
    if (label == "Backspace") {
      if (start == end && start > 0) {
        --start;
        while (start > 0 &&
               (static_cast<unsigned char>(target->value()[start]) & 0xc0) ==
                   0x80)
          --start;
      }
      target->replace(start, end, "");
    } else {
      auto text = label == "Space" ? std::string(" ") : label;
      if (shifted)
        std::transform(text.begin(), text.end(), text.begin(), ::toupper);
      target->replace(start, end, text.c_str());
    }
  }
  void disconnect() {
    if (bridge >= 0)
      close(bridge);
    bridge = -1;
    incoming.clear();
    outgoing.clear();
    connecting = false;
    frameDirty = true;
  }
  // Dispatch through the real widget handlers: selection, dragging, modifiers,
  // shortcuts and button press ownership are identical to the desktop seat.
  void input(const Json &j) {
    frameDirty = true;
    const auto type = j.value("type", std::string());
    struct EventState {
      int x = Fl::e_x, y = Fl::e_y, xr = Fl::e_x_root, yr = Fl::e_y_root,
          state = Fl::e_state, key = Fl::e_keysym,
          original = Fl::e_original_keysym, length = Fl::e_length,
          clicks = Fl::e_clicks, isclick = Fl::e_is_click;
      char *text = Fl::e_text;
      ~EventState() {
        Fl::e_x = x;
        Fl::e_y = y;
        Fl::e_x_root = xr;
        Fl::e_y_root = yr;
        Fl::e_state = state;
        Fl::e_keysym = key;
        Fl::e_original_keysym = original;
        Fl::e_length = length;
        Fl::e_text = text;
        Fl::e_clicks = clicks;
        Fl::e_is_click = isclick;
      }
    } saved;
    int mods = j.value("modifiers", 0);
    Fl::e_state = ((mods & 0x02000000) ? FL_SHIFT : 0) |
                  ((mods & 0x04000000) ? FL_CTRL : 0) |
                  ((mods & 0x08000000) ? FL_ALT : 0) |
                  ((mods & 0x10000000) ? FL_META : 0);
    if (type == "key") {
      int key = j.value("key", 0);
      auto text = j.value("text", std::string());
      if (key >= 0x01000030 && key <= 0x01000052)
        key = FL_F + 1 + key - 0x01000030;
      else
        switch (key) {
        case 0x01000000:
          key = FL_Escape;
          break;
        case 0x01000001:
          key = FL_Tab;
          break;
        case 0x01000002:
          key = FL_Tab;
          Fl::e_state |= FL_SHIFT;
          break;
        case 0x01000003:
          key = FL_BackSpace;
          break;
        case 0x01000004:
        case 0x01000005:
          key = FL_Enter;
          break;
        case 0x01000006:
          key = FL_Insert;
          break;
        case 0x01000007:
          key = FL_Delete;
          break;
        case 0x01000010:
          key = FL_Home;
          break;
        case 0x01000011:
          key = FL_End;
          break;
        case 0x01000012:
          key = FL_Left;
          break;
        case 0x01000013:
          key = FL_Up;
          break;
        case 0x01000014:
          key = FL_Right;
          break;
        case 0x01000015:
          key = FL_Down;
          break;
        case 0x01000016:
          key = FL_Page_Up;
          break;
        case 0x01000017:
          key = FL_Page_Down;
          break;
        }
      if (!key && !text.empty())
        key = (unsigned char)text[0];
      if (key >= 'A' && key <= 'Z')
        key += 'a' - 'A';
      Fl::e_keysym = Fl::e_original_keysym = key;
      Fl::e_text = text.data();
      Fl::e_length = text.size();
      bool pressed = j.value("pressed", true);
      Fl::handle(pressed ? FL_KEYDOWN : FL_KEYUP, this);
      if (!j.contains("pressed"))
        Fl::handle(FL_KEYUP, this);
      redraw();
      return;
    }
    if (type == "leave") {
      remoteVisible = false;
      remoteDown = false;
      if (Fl::pushed()) {
        Fl::e_keysym = FL_Button + 1;
        Fl::handle(FL_RELEASE, this);
      }
      redraw();
      return;
    }
    int x = j.value("x", -1), y = j.value("y", -1);
    if (x < 0 || y < 0 || x >= W || y >= H) {
      if (type != "up" || !remoteDown)
        return;
      x = std::clamp(x, 0, W - 1);
      y = std::clamp(y, 0, H - 1);
    }
    remoteX = x;
    remoteY = y;
    remoteVisible = true;
    Fl::e_x = x;
    Fl::e_y = y;
    Fl::e_x_root = this->x() + x;
    Fl::e_y_root = this->y() + y;
    Fl::e_clicks = std::clamp(j.value("clicks", 0), 0, 2);
    Fl::e_is_click = 1;
    Fl::e_keysym = FL_Button + 1;
    if (type == "down") {
      remoteDown = true;
      Fl::e_state |= FL_BUTTON1;
      Fl::handle(FL_PUSH, this);
    } else if (type == "up") {
      if (remoteDown)
        Fl::handle(FL_RELEASE, this);
      remoteDown = false;
    } else {
      if (remoteDown)
        Fl::e_state |= FL_BUTTON1;
      Fl::handle(remoteDown ? FL_DRAG : FL_MOVE, this);
    }
    if (auto input = dynamic_cast<Fl_Input *>(Fl::focus()))
      target = input;
    redraw();
  }
  void draw() override {
    dx::GpuWindow::draw();
    if (remoteVisible) {
      draw_begin();
      fl_color(FL_SELECTION_COLOR);
      fl_line_style(FL_SOLID, 2);
      fl_circle(remoteX, remoteY, 7);
      fl_line(remoteX - 11, remoteY, remoteX + 11, remoteY);
      fl_line(remoteX, remoteY - 11, remoteX, remoteY + 11);
      fl_line_style(0);
      draw_end();
    }
  }
  int handle(int event) override {
    int result = dx::GpuWindow::handle(event);
    if (event == FL_KEYDOWN || event == FL_PUSH || event == FL_RELEASE ||
        event == FL_DRAG || event == FL_FOCUS || event == FL_UNFOCUS)
      frameDirty = true;
    return result;
  }
  void frame() {
    frameDirty = false;
    lastFrame = std::chrono::steady_clock::now();
    Fl_Image_Surface surface(W, H);
    Fl_Surface_Device::push_current(&surface);
    surface.draw(this);
    auto image = surface.image();
    Fl_Surface_Device::pop_current();
    if (!image)
      return;
    std::string data(9 + W * H * 4, '\0');
    data[0] = 'F';
    uint32_t width = htonl(W), height = htonl(H);
    memcpy(data.data() + 1, &width, 4);
    memcpy(data.data() + 5, &height, 4);
    auto rgb = image->data()[0];
    int stride = image->ld() ? image->ld() : W * image->d();
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x) {
        auto d = data.data() + 9 + (y * W + x) * 4;
        memcpy(d, rgb + y * stride + x * image->d(), 3);
        d[3] = char(255);
      }
    delete image;
    outgoing += framed(data);
  }
  void pump() {
    if (bridge < 0) {
      if (std::chrono::steady_clock::now() - reconnect <
          std::chrono::seconds(1))
        return;
      reconnect = std::chrono::steady_clock::now();
      bridge = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
      sockaddr_un a{};
      a.sun_family = AF_UNIX;
      if (bridgePath.size() >= sizeof(a.sun_path)) {
        disconnect();
        return;
      }
      strcpy(a.sun_path, bridgePath.c_str());
      if (connect(bridge, (sockaddr *)&a, sizeof(a)) && errno != EINPROGRESS) {
        disconnect();
        return;
      }
      connecting = true;
    }
    if (connecting) {
      int error = 0;
      socklen_t n = sizeof(error);
      if (getsockopt(bridge, SOL_SOCKET, SO_ERROR, &error, &n) || error) {
        disconnect();
        return;
      }
      connecting = false;
    }
    char buffer[4096];
    for (;;) {
      auto n = recv(bridge, buffer, sizeof(buffer), 0);
      if (n > 0)
        incoming.append(buffer, n);
      else {
        if (!n || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
          disconnect();
          return;
        }
        break;
      }
    }
    while (incoming.size() >= 4) {
      uint32_t n;
      memcpy(&n, incoming.data(), 4);
      n = ntohl(n);
      if (n == 0 || n > 1024) {
        disconnect();
        return;
      }
      if (incoming.size() < n + 4)
        break;
      auto data = incoming.substr(4, n);
      incoming.erase(0, n + 4);
      if (data[0] == 'I')
        try {
          input(Json::parse(data.substr(1)));
        } catch (...) {
        };
    }
    if (outgoing.empty() && !starting &&
        (frameDirty || (target->active() && Fl::focus() == target &&
                        std::chrono::steady_clock::now() - lastFrame >
                            std::chrono::milliseconds(500))))
      frame();
    while (!outgoing.empty()) {
      auto n = send(bridge, outgoing.data(), outgoing.size(), MSG_NOSIGNAL);
      if (n > 0)
        outgoing.erase(0, n);
      else {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
          disconnect();
        break;
      }
    }
  }

public:
  Greeter(bool preview, std::string endpoint)
      : dx::GpuWindow(W, H, "LightXR"), demo(preview),
        bridgePath(std::move(endpoint)) {
    xclass("org.domed.LightXR");
    // The bundled fixed strike is required even outside Dome sessions.
    // Installed greeters must not depend on traversing the developer's home.
    std::error_code bitmapError;
    const char *bitmapPath =
        std::filesystem::is_regular_file(LIGHTXR_INSTALLED_BITMAP_FONT,
                                         bitmapError)
            ? LIGHTXR_INSTALLED_BITMAP_FONT
            : LIGHTXR_BITMAP_FONT;
    if (!FcConfigAppFontAddFile(nullptr, (FcChar8 *)bitmapPath))
      throw std::runtime_error("Cannot load bundled bitmap font");
    FcConfigBuildFonts(nullptr);
    if (FT_Init_FreeType(&ft) || FT_New_Face(ft, bitmapPath, 0, &face) ||
        FT_IS_SCALABLE(face) || !face->num_fixed_sizes)
      throw std::runtime_error("LightXR requires a bitmap font");
    dx::font_name = face->family_name;
    dx::font_names.push_back(dx::font_name);
    Fl::set_font(FL_FREE_FONT, dx::font_names.back().c_str());
    dx::font = FL_FREE_FONT;
    dx::size = 16;
    color(FL_BACKGROUND_COLOR);
    brand.align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    message.align(FL_ALIGN_LEFT | FL_ALIGN_WRAP | FL_ALIGN_INSIDE);
    prompt.align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    answer.type(FL_SECRET_INPUT);
    answer.deactivate();
    username.when(FL_WHEN_ENTER_KEY_ALWAYS);
    answer.when(FL_WHEN_ENTER_KEY_ALWAYS);
    username.callback(
        [](Fl_Widget *, void *p) {
          auto s = (Greeter *)p;
          s->target = &s->username;
          s->signIn();
        },
        this);
    answer.callback([](Fl_Widget *, void *p) { ((Greeter *)p)->signIn(); },
                    this);
    submit.callback([](Fl_Widget *, void *p) { ((Greeter *)p)->signIn(); },
                    this);
    cancel.callback(
        [](Fl_Widget *, void *p) {
          auto s = (Greeter *)p;
          if (s->daemon && s->authenticating) {
            GError *e = nullptr;
            lightdm_greeter_cancel_authentication(s->daemon, &e);
            s->clear();
            s->prompts.clear();
            s->submit.deactivate();
            s->status("Cancelling...");
            if (e)
              g_error_free(e);
          }
        },
        this);
    for (GList *item = lightdm_get_sessions(); item; item = item->next) {
      auto s = LIGHTDM_SESSION(item->data);
      auto key = std::string(lightdm_session_get_key(s));
      if (std::find(sessions.begin(), sessions.end(), key) != sessions.end())
        continue;
      if (key == "dome")
        selected = sessions.size();
      sessions.push_back(key);
      names.emplace_back(lightdm_session_get_name(s));
    }
    if (sessions.empty() && demo) {
      sessions = {"dome"};
      names = {"DomeD"};
    }
    session.copy_label(names.empty() ? "No sessions installed"
                                     : names[selected].c_str());
    session.callback(
        [](Fl_Widget *, void *p) {
          auto s = (Greeter *)p;
          if (!s->authenticating && !s->sessions.empty()) {
            s->selected = (s->selected + 1) % s->sessions.size();
            s->session.copy_label(s->names[s->selected].c_str());
          }
        },
        this);
    int row = 0;
    for (auto chars :
         {"`1234567890-=", "qwertyuiop[]", "asdfghjkl;'", "zxcvbnm,./\\"}) {
      int i = 0;
      for (char c : std::string(chars)) {
        auto b = new Fl_Button(80 + i++ * 86, 460 + row * 55, 80, 45);
        b->copy_label(std::string(1, c).c_str());
        b->clear_visible_focus();
        keys.push_back(b);
        keyBases[b] = std::string(1, c);
        b->callback(
            [](Fl_Widget *w, void *p) { ((Greeter *)p)->keyboard(w->label()); },
            this);
      }
      ++row;
    }
    int x = 80;
    for (auto label : {"Shift", "Space", "Backspace", "Tab", "Enter"}) {
      int width = std::string(label) == "Space" ? 400 : 170;
      auto b = new Fl_Button(x, 690, width, 45, label);
      x += width + 8;
      b->clear_visible_focus();
      keys.push_back(b);
      b->callback(
          [](Fl_Widget *w, void *p) { ((Greeter *)p)->keyboard(w->label()); },
          this);
    }
    end();
    dx::style(this);
    if (demo)
      idle("Preview only — authentication is disabled.");
    else {
      submit.deactivate();
      username.deactivate();
      daemon = lightdm_greeter_new();
      g_signal_connect(daemon, "show-prompt",
                       G_CALLBACK(+[](LightDMGreeter *, const gchar *text,
                                      LightDMPromptType type, gpointer p) {
                         auto s = (Greeter *)p;
                         s->prompts.push_back(
                             {text, type == LIGHTDM_PROMPT_TYPE_SECRET});
                         if (s->prompts.size() == 1)
                           s->showPrompt();
                       }),
                       this);
      g_signal_connect(daemon, "show-message",
                       G_CALLBACK(+[](LightDMGreeter *, const gchar *text,
                                      LightDMMessageType, gpointer p) {
                         ((Greeter *)p)->status(text);
                       }),
                       this);
      g_signal_connect(daemon, "authentication-complete",
                       G_CALLBACK(+[](LightDMGreeter *, gpointer p) {
                         ((Greeter *)p)->complete();
                       }),
                       this);
      g_signal_connect(daemon, "idle",
                       G_CALLBACK(+[](LightDMGreeter *, gpointer p) {
                         auto s = (Greeter *)p;
                         s->starting = true;
                         s->outgoing += framed("H");
                         s->clear();
                       }),
                       this);
      g_signal_connect(daemon, "reset",
                       G_CALLBACK(+[](LightDMGreeter *, gpointer p) {
                         auto s = (Greeter *)p;
                         s->idle("Sign in to continue.");
                       }),
                       this);
      lightdm_greeter_connect_to_daemon(
          daemon, nullptr,
          [](GObject *obj, GAsyncResult *r, gpointer p) {
            auto s = (Greeter *)p;
            GError *e = nullptr;
            s->connected = lightdm_greeter_connect_to_daemon_finish(
                LIGHTDM_GREETER(obj), r, &e);
            s->idle(s->connected ? "Sign in to continue."
                                 : "Display manager connection failed.");
            if (!s->connected) {
              s->username.deactivate();
              s->submit.deactivate();
            }
            if (e)
              g_error_free(e);
            const char *user = lightdm_greeter_get_select_user_hint(s->daemon);
            if (user)
              s->username.value(user);
          },
          this);
    }
    Fl::add_timeout(.1, tick, this);
  }
  ~Greeter() {
    Fl::remove_timeout(tick, this);
    clear();
    disconnect();
    if (daemon)
      g_object_unref(daemon);
    if (face)
      FT_Done_Face(face);
    if (ft)
      FT_Done_FreeType(ft);
  }
};
int main(int argc, char **argv) try {
  bool demo = false, windowed = false;
  std::string endpoint = "/run/lightxr-broker/bridge.sock", screenshot;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--demo")
      demo = true;
    else if (a == "--windowed")
      windowed = true;
    else if (a == "--bridge" && i + 1 < argc)
      endpoint = argv[++i];
    else if ((a == "--screenshot" || a == "--smoke-test") && i + 1 < argc) {
      screenshot = argv[++i];
      demo = true;
    } else if (a == "--help") {
      puts("LightXR FLTK: --demo --windowed --bridge PATH --screenshot PPM");
      return 0;
    } else
      throw std::runtime_error("Unknown option: " + a);
  }
  Fl::scheme("gtk+");
  Fl::background(17, 25, 35);
  Fl::background2(32, 47, 65);
  Fl::foreground(237, 243, 251);
  Greeter window(demo, endpoint);
  FL_NORMAL_SIZE = dx::size;
  fl_message_font(dx::font, dx::size);
  dx::init();
  dx::style(&window);
  window.show();
  if (!windowed)
    window.fullscreen();
  if (!screenshot.empty()) {
    Fl::check();
    return dx::screenshot(window, screenshot.c_str()) ? 0 : 1;
  }
  return Fl::run();
} catch (const std::exception &e) {
  fprintf(stderr, "LightXR: %s\n", e.what());
  return 1;
}
