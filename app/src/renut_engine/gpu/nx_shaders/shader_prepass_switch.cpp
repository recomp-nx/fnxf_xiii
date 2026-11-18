// fnxf_xiii (2026-10-21): the shader pre-pass on the Switch, before the game starts (FnxfXiiiApp::OnPreSetup): the
// screen (prepass_screen.h) through libnx's framebuffer on the default window, which is free until the renderer makes
// its swapchain; the work (shader_prepass.h) on three threads pinned to cores 0-2 with the CPU boosted; uam compiling
// into the deko3d back end's DKSH cache.
#include "renut_engine/gpu/nx_shaders/shader_prepass_switch.h"
#include "renut_engine/gpu/nx_shaders/front_end_store.h"
#include "renut_engine/gpu/nx_shaders/late_spec.h"
#include "renut_engine/gpu/nx_shaders/nx_lock.h"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <malloc.h>  // fnxf_xiii (2026-11-08): mallinfo

#include <switch.h>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>

#include "renut_engine/gpu/fnxf_xiii_renut_deko3d.h"
#include "renut_engine/gpu/fnxf_xiii_renut_tuning.h"
#include "renut_engine/gpu/nx_shaders/prepass_screen.h"
#include "renut_engine/gpu/nx_shaders/boot_menu_switch.h"  // fnxf_xiii (2026-11-07): ScreenWants1080p, ScreenFramebuffer
#include "renut_engine/gpu/nx_shaders/shader_prepass.h"
#include "renut_engine/gpu/nx_shaders/shader_store.h"
#include "renut_engine/gpu/nx_shaders/thread_alloc.h"
#include "renut_engine/gpu/nx_shaders/thread_waits.h"
#include "renut_engine/gpu/nx_shaders/ui_language.h"
#include "renut_engine/gpu/nx_shaders/early_exit.h"
#include "cpu/fnxf_xiii_lang_pack.h"  // fnxf_xiii (2026-10-25): a language pack's text
#include "renut_engine/gpu/nx_shaders/shader_store.h"
#include "uam_api.h"
REXCVAR_DECLARE(bool, log_diagnostics);  // fnxf_xiii (2026-10-25): sdk/src/system/xobject.cpp

REXCVAR_DEFINE_BOOL(fnxf_xiii_shader_prepass, true, "fnxf_xiii",
                    "Switch: before the game starts, find the shaders in the game files, translate them and compile "
                    "them with uam (once; resumable)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DECLARE(bool, fnxf_xiii_gpu_bool_spec);
REXCVAR_DECLARE(int32_t, fnxf_xiii_fsr3);  // render/fsr3.cpp (2026-11-16: the motion-vector twins)
REXCVAR_DEFINE_BOOL(fnxf_xiii_prepass_compile, true, "fnxf_xiii",
                    "Switch: the pre-pass compiles the shaders with uam (false: it only finds and translates them; the "
                    "game then compiles each one when it first draws with it: a diagnostic, for a fresh DKSH cache)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(fnxf_xiii_prepass_retry_failed, false, "fnxf_xiii",
                    "Switch: a diagnostic: the pre-pass compiles again the variants it recorded as failed")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(fnxf_xiii_diag_prepass_recompile, false, "Game/Diagnostics",
                    "Test (2026-11-08): the pre-pass compiles every shader and variant again, as after an update that "
                    "changes the translation, and keeps none of it (the DKSH cache stays as it is): its memory over a "
                    "whole run")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// fnxf_xiii (2026-11-15, UamPaths below).
REXCVAR_DEFINE_BOOL(fnxf_xiii_diag_uam_paths_check, false, "Game/Diagnostics",
                    "Test (2026-11-15): a pre-pass compile with a recorded uam flag set (fnxf_xiii_uam_paths.txt) is made a "
                    "second time with the whole search, and the two DKSH compared (logged: [uampaths]). Twice the time");
REXCVAR_DEFINE_INT32(fnxf_xiii_prepass_threads, 3, "fnxf_xiii",
                     "Switch: threads of the shader pre-pass, spread over cores 0-2 (3: one a core)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

extern "C" u32 __nx_applet_exit_mode;  // libnx: 0 returns to the loader at exit
extern "C" NX_NORETURN void __libnx_exit(int rc);


REXCVAR_DECLARE(bool, fnxf_xiii_deko3d_late_spec);

namespace renut::gpu {
std::string SymbolName(uintptr_t address);  // backend/memory_report.cpp (fnxf_xiii, 2026-11-11)
}

namespace renut::nxs {

namespace {

// "m:ss", or "h:mm:ss" from an hour.
std::string Clock(double seconds) {
  const long t = long(seconds);
  return t >= 3600 ? fmt::format("{}:{:02}:{:02}", t / 3600, (t / 60) % 60, t % 60)
                   : fmt::format("{}:{:02}", t / 60, t % 60);
}

// fnxf_xiii (2026-10-25): the pre-pass screen's text in the game's language (ui_language.h: the boot menu's
// language, set before the pre-pass). Format strings for fmt (runtime): "{}" are numbers or times.
struct PrepassStrings {
  const char *thousands;  // the thousands separator
  bool decimal_comma;
  const char *title, *footer;
  const char *step_cached, *step_scan, *step_recompile, *step_compile, *step_front_end, *step_index, *step_check;
  const char *n_of_total, *scan_detail, *failed, *compiled;
  const char *eta_short, *eta_minutes, *eta_hours;
  const char *this_step, *total, *step1_took;
};
constexpr PrepassStrings kPrepassStrings[kUiLanguageCount] = {
    {   // English
        ",", false, "Preparing shaders",
        "Done once, and kept on the SD card. The game starts when this is finished.\n"
        "You can close the game at any time: the next start continues where this one stopped.",
        "Step 1 of 4 · Optimizing cached shaders", "Step 1 of 4 · Finding the shaders in the game files",
        "Step 2 of 4 · Recompiling cached shaders", "Step 2 of 4 · Compiling the shaders",
        "Step 3 of 4 · Generating intermediates", "Step 4 of 4 · Creating sorted index", "Checking the shaders...",
        "{} of {} shaders", "{} of {} MB · {} shaders found, {} new", " · {} failed", "{} shaders compiled",
        "less than 2 minutes left", "about {} minutes left", "about {} hours left",
        "This step: {} so far", " · Total: {}", " (step 1 took {})",
    },
    {   // Deutsch
        ".", true, "Shader werden vorbereitet",
        "Geschieht nur einmal und wird auf der SD-Karte gespeichert. Das Spiel startet, sobald dies fertig ist.\n"
        "Das Spiel kann jederzeit geschlossen werden: Der nächste Start macht dort weiter, wo dieser aufgehört hat.",
        "Schritt 1 von 4 · Gespeicherte Shader optimieren", "Schritt 1 von 4 · Shader in den Spieldateien suchen",
        "Schritt 2 von 4 · Gespeicherte Shader neu kompilieren", "Schritt 2 von 4 · Shader kompilieren",
        "Schritt 3 von 4 · Zwischenformen erzeugen", "Schritt 4 von 4 · Sortierten Index erstellen",
        "Shader werden geprüft...",
        "{} von {} Shadern", "{} von {} MB · {} Shader gefunden, {} neu", " · {} fehlgeschlagen",
        "{} Shader kompiliert",
        "noch weniger als 2 Minuten", "noch etwa {} Minuten", "noch etwa {} Stunden",
        "Dieser Schritt: bisher {}", " · Gesamt: {}", " (Schritt 1 dauerte {})",
    },
    {   // Français
        " ", true, "Préparation des shaders",
        "Effectué une seule fois et conservé sur la carte SD. Le jeu démarre une fois cette étape terminée.\n"
        "Vous pouvez fermer le jeu à tout moment : le prochain lancement reprendra là où celui-ci s'est arrêté.",
        "Étape 1 sur 4 · Optimisation des shaders en cache",
        "Étape 1 sur 4 · Recherche des shaders dans les fichiers du jeu",
        "Étape 2 sur 4 · Recompilation des shaders en cache", "Étape 2 sur 4 · Compilation des shaders",
        "Étape 3 sur 4 · Génération des formes intermédiaires", "Étape 4 sur 4 · Création de l'index trié",
        "Vérification des shaders...",
        "{} sur {} shaders", "{} sur {} Mo · {} shaders trouvés, {} nouveaux", " · {} en échec",
        "{} shaders compilés",
        "moins de 2 minutes restantes", "environ {} minutes restantes", "environ {} heures restantes",
        "Cette étape : {} écoulées", " · Total : {}", " (étape 1 : {})",
    },
    {   // Español
        ".", true, "Preparando los shaders",
        "Se hace una sola vez y se guarda en la tarjeta SD. El juego se inicia cuando termine.\n"
        "Puedes cerrar el juego en cualquier momento: el próximo inicio continuará donde se quedó este.",
        "Paso 1 de 4 · Optimizando los shaders guardados", "Paso 1 de 4 · Buscando los shaders en los archivos del juego",
        "Paso 2 de 4 · Recompilando los shaders guardados", "Paso 2 de 4 · Compilando los shaders",
        "Paso 3 de 4 · Generando formas intermedias", "Paso 4 de 4 · Creando el índice ordenado",
        "Comprobando los shaders...",
        "{} de {} shaders", "{} de {} MB · {} shaders encontrados, {} nuevos", " · {} fallidos",
        "{} shaders compilados",
        "quedan menos de 2 minutos", "quedan unos {} minutos", "quedan unas {} horas",
        "Este paso: {} hasta ahora", " · Total: {}", " (el paso 1 tardó {})",
    },
    {   // Italiano
        ".", true, "Preparazione degli shader",
        "Si fa una sola volta e viene salvato sulla scheda SD. Il gioco si avvia al termine.\n"
        "Puoi chiudere il gioco in qualsiasi momento: al prossimo avvio si riprenderà da dove si è interrotto.",
        "Passo 1 di 4 · Ottimizzazione degli shader in cache", "Passo 1 di 4 · Ricerca degli shader nei file del gioco",
        "Passo 2 di 4 · Ricompilazione degli shader in cache", "Passo 2 di 4 · Compilazione degli shader",
        "Passo 3 di 4 · Generazione delle forme intermedie", "Passo 4 di 4 · Creazione dell'indice ordinato",
        "Verifica degli shader...",
        "{} di {} shader", "{} di {} MB · {} shader trovati, {} nuovi", " · {} non riusciti", "{} shader compilati",
        "meno di 2 minuti rimanenti", "circa {} minuti rimanenti", "circa {} ore rimanenti",
        "Questo passo: {} finora", " · Totale: {}", " (passo 1: {})",
    },
};

// fnxf_xiii (2026-10-25): a language pack's [port] prepass_* text over the base language's (cpu/fnxf_xiii_lang_pack.h).
// A format string only when it has as many "{}" as the base's (a wrong one would throw).
PrepassStrings PackPrepassStrings(PrepassStrings t) {
  auto holes = [](const char *s) {
    int n = 0;
    for (; *s; ++s)
      n += *s == '{' ? (s[1] == '}' ? 1 : 100) : 0;
    return n;
  };
  struct Field {
    const char *key;
    const char *PrepassStrings::*member;
  };
  static constexpr Field kFields[] = {
      {"prepass_thousands", &PrepassStrings::thousands}, {"prepass_title", &PrepassStrings::title},
      {"prepass_footer", &PrepassStrings::footer}, {"prepass_step_cached", &PrepassStrings::step_cached},
      {"prepass_step_scan", &PrepassStrings::step_scan}, {"prepass_step_recompile", &PrepassStrings::step_recompile},
      {"prepass_step_compile", &PrepassStrings::step_compile},
      {"prepass_step_front_end", &PrepassStrings::step_front_end},
      {"prepass_step_index", &PrepassStrings::step_index}, {"prepass_step_check", &PrepassStrings::step_check},
      {"prepass_n_of_total", &PrepassStrings::n_of_total}, {"prepass_scan_detail", &PrepassStrings::scan_detail},
      {"prepass_failed", &PrepassStrings::failed}, {"prepass_compiled", &PrepassStrings::compiled},
      {"prepass_eta_short", &PrepassStrings::eta_short}, {"prepass_eta_minutes", &PrepassStrings::eta_minutes},
      {"prepass_eta_hours", &PrepassStrings::eta_hours}, {"prepass_this_step", &PrepassStrings::this_step},
      {"prepass_total", &PrepassStrings::total}, {"prepass_step1_took", &PrepassStrings::step1_took},
  };
  for (const Field &f : kFields) {
    const char *v = fnxf_xiii::PortText(f.key, nullptr);
    if (v && holes(v) == holes(t.*f.member))
      t.*f.member = v;
  }
  if (const char *v = fnxf_xiii::PortText("prepass_decimal_comma", nullptr))
    t.decimal_comma = v[0] == 'y' || v[0] == '1' || v[0] == 't';
  return t;
}

std::string Thousands(const PrepassStrings &t, uint64_t v) {
  std::string s = std::to_string(v);
  for (int i = int(s.size()) - 3; i > 0; i -= 3)
    s.insert(size_t(i), t.thousands);
  return s;
}

// The current step's remaining time (the screen says so: "this step ...").
std::string Minutes(const PrepassStrings &t, double seconds) {
  if (seconds < 90)
    return t.eta_short;
  if (seconds < 3600 * 2)
    return fmt::format(fmt::runtime(t.eta_minutes), int(seconds / 60 + 0.5));
  std::string hours = fmt::format("{:.1f}", seconds / 3600);
  if (t.decimal_comma)
    std::replace(hours.begin(), hours.end(), '.', ',');
  return fmt::format(fmt::runtime(t.eta_hours), hours);
}

// fnxf_xiii (2026-10-25): the applet-mode screen's text, in the console's language (it shows before the settings are
// read): title, two lines of what to do, the button.
struct AppletStrings {
  const char *title, *step, *detail, *eta, *footer;
};
constexpr AppletStrings kAppletStrings[kUiLanguageCount] = {
    {"Applet mode is not supported", "The game needs the memory only full application mode has.",
     "Hold R while you start any game from the HOME menu, then start fnxf_xiii",
     "from the homebrew menu that opens (not from the Album).", "Press any button to exit."},
    {"Der Applet-Modus wird nicht unterstützt",
     "Das Spiel braucht den Speicher, den nur der volle Anwendungsmodus bietet.",
     "R gedrückt halten, während ein beliebiges Spiel im HOME-Menü gestartet wird, dann fnxf_xiii",
     "aus dem sich öffnenden Homebrew-Menü starten (nicht aus dem Album).", "Eine beliebige Taste beendet."},
    {"Le mode applet n'est pas pris en charge",
     "Le jeu a besoin de la mémoire que seul le mode application complet fournit.",
     "Maintenez R en lançant n'importe quel jeu depuis le menu HOME, puis lancez fnxf_xiii",
     "depuis le menu homebrew qui s'ouvre (pas depuis l'Album).", "Appuyez sur un bouton pour quitter."},
    {"El modo applet no es compatible",
     "El juego necesita la memoria que solo ofrece el modo de aplicación completo.",
     "Mantén pulsado R al iniciar cualquier juego desde el menú HOME y luego inicia fnxf_xiii",
     "desde el menú homebrew que se abre (no desde el Álbum).", "Pulsa cualquier botón para salir."},
    {"La modalità applet non è supportata",
     "Il gioco richiede la memoria disponibile solo in modalità applicazione completa.",
     "Tieni premuto R mentre avvii un gioco qualsiasi dal menu HOME, poi avvia fnxf_xiii",
     "dal menu homebrew che si apre (non dall'Album).", "Premi un pulsante qualsiasi per uscire."},
};

}  // namespace

bool RunningAsApplet() {
  const AppletType type = appletGetAppletType();
  return type != AppletType_Application && type != AppletType_SystemApplication;
}

// fnxf_xiii (2026-10-21): the applet-mode screen, before main (AppletModeGuard below): nothing of the SDK has run yet
// (no threads, no logging, no window), so nothing outlives the screen, and the process returns to hbloader the way
// libnx returns any homebrew (__nx_applet_exit_mode 0). The SDK's own exit asks the system to close an application
// (ISelfController::Exit), which in applet mode took the HOME menu down with it (qlaunch 2144-0001).
// Memory: an applet has about 250 MB with the NRO in it; the screen peaks around 25 MB (the 1920x1080 picture while
// it is decoded and scaled) and frees it all before returning.
static void ShowAppletModeError(const std::string &game_root) {
  PlFontData font{};
  const bool pl = R_SUCCEEDED(plInitialize(PlServiceType_User));
  if (pl)
    plGetSharedFontByType(&font, PlSharedFontType_Standard);
  {
    PrepassScreen screen;
    const bool hd = ScreenWants1080p();  // fnxf_xiii (2026-11-07)
    screen.Init(game_root + "/nxeart", static_cast<const uint8_t *>(font.address), font.size, hd ? 1.5f : 1.0f);
    ScreenText text;
    const AppletStrings &a = kAppletStrings[SystemUiLanguage()];
    text.title = a.title;
    text.step = a.step;
    text.detail = a.detail;
    text.eta = a.eta;
    text.footer = a.footer;
    text.progress = -1.0f;
    ScreenFramebuffer sfb;
    const bool have_fb = sfb.Open(hd);
    Framebuffer &fb = sfb.fb;
    padConfigureInput(8, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    bool drawn = false;
    while (appletMainLoop()) {
      padUpdate(&pad);
      if (padGetButtonsDown(&pad))
        break;
      // The screen does not change: drawn into both buffers, then only waiting for a button.
      if (have_fb && !drawn) {
        for (int i = 0; i < 2; i++) {
          u32 stride = 0;
          auto *pixels = static_cast<uint32_t *>(framebufferBegin(&fb, &stride));
          screen.Draw(pixels, stride / 4, text);
          framebufferEnd(&fb);
        }
        drawn = true;
      }
      svcSleepThread(16'000'000);
    }
    sfb.Close();
  }
  if (pl)
    plExit();
}

namespace {
// Before main and before the SDK's constructors (101 is the first priority a program may use; the SDK's own at 101
// only installs a terminate handler). libnx's services are up: __appInit runs before the constructors.
__attribute__((constructor(101))) void AppletModeGuard() {
  if (!RunningAsApplet())
    return;
  ShowAppletModeError("sdmc:/switch/fnxf_xiii/assets");
  // Not exit(): the SDK's crash hooks (switch_crash_hooks.c) register an atexit handler that aborts on any exit()
  // outside its own shutdown (diagAbortWithResult), and that abort of the album applet took the HOME menu down
  // (qlaunch, 2144-0001). __libnx_exit runs no atexit handlers: it closes the services and returns to hbloader
  // (__nx_applet_exit_mode 0), as any homebrew in applet mode.
  std::fflush(nullptr);
  __nx_applet_exit_mode = 0;
  __libnx_exit(0);
}
}  // namespace

namespace {
// fnxf_xiii (2026-11-15, the user: "writing what path it chose ... so i can put that to release and lower amount of time
// necessary to compile them"). uam keeps, of up to 7 codegens of a shader (the flags as set; for a pixel shader four
// more each without one of the fragment passes, uam's TryDrops; above 56 registers two register rescues, TryRescue),
// the one with the fewest registers. Which one it kept (uam::Result::path) is recorded by the variant's DKSH key in
// shaders/fnxf_xiii_uam_paths.txt: a header line with uam's flags, then "<key> <path>" lines (a later line for a key wins).
// A key found there is compiled with that flag set alone: the same DKSH, one codegen. A release ships the file its
// own full pre-pass wrote. A file made with other flags is started over (the sets are of the flags).
class UamPaths {
 public:
  static UamPaths &Get() {
    static UamPaths p;
    return p;
  }
  void Open(const std::string &path, unsigned flags) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string header = fmt::format("# fnxf_xiii uam paths 1, uam flags {}", flags);
    size_t read = 0;
    bool same = false;
    // fnxf_xiii (2026-11-18, the user: ship the lists with a release without overwriting the player's own): a release's
    // flag sets first, from the same name with "_release" (read only, never written; only with the same uam flags),
    // then the recorded file's, whose line wins for a key in both. A set found here is not recorded again (Record:
    // only a search's result that differs).
    size_t from_release = 0;
    {
      std::string release = path;
      const size_t slash = release.find_last_of("/\\");
      const size_t dot = release.find_last_of('.');
      release.insert(dot != std::string::npos && (slash == std::string::npos || dot > slash) ? dot : release.size(),
                     "_release");
      if (FILE *f = std::fopen(release.c_str(), "r")) {
        char line[128];
        const bool ok = std::fgets(line, sizeof(line), f) && std::string(line).rfind(header, 0) == 0 &&
                        (line[header.size()] == '\n' || line[header.size()] == '\r' || line[header.size()] == 0);
        while (ok && std::fgets(line, sizeof(line), f)) {
          unsigned long long key = 0;
          int p = -1;
          if (std::sscanf(line, "%llx %d", &key, &p) == 2 && p >= 0 && p <= 6) {
            paths_[uint64_t(key)] = uint8_t(p);
            ++from_release;
          }
        }
        std::fclose(f);
      }
    }
    if (FILE *f = std::fopen(path.c_str(), "r")) {
      char line[128];
      same = std::fgets(line, sizeof(line), f) && std::string(line).rfind(header, 0) == 0 &&
             (line[header.size()] == '\n' || line[header.size()] == '\r' || line[header.size()] == 0);
      while (same && std::fgets(line, sizeof(line), f)) {
        unsigned long long key = 0;
        int p = -1;
        if (std::sscanf(line, "%llx %d", &key, &p) == 2 && p >= 0 && p <= 6) {
          paths_[uint64_t(key)] = uint8_t(p);
          ++read;
        }
      }
      std::fclose(f);
    }
    file_ = std::fopen(path.c_str(), same ? "a" : "w");
    if (file_ && !same) {
      std::fprintf(file_, "%s\n", header.c_str());
      std::fflush(file_);
    }
    opened_ = fmt::format("{} recorded flag sets read ({} of them the release's){}{}", paths_.size(), from_release,
                          same || read ? "" : " (a new file)", file_ ? "" : "; the file cannot be written");
  }
  // The recorded set of 'key', or -1.
  int Find(uint64_t key) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = paths_.find(key);
    return it == paths_.end() ? -1 : int(it->second);
  }
  void Record(uint64_t key, int path) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (path < 0 || path > 6)
      return;
    paths_[key] = uint8_t(path);
    if (file_) {
      std::fprintf(file_, "%016llx %d\n", (unsigned long long)key, path);
      if (++unflushed_ >= 128) {  // (2026-11-15: 128, was 16)
        std::fflush(file_);
        unflushed_ = 0;
      }
    }
  }
  void Flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_)
      std::fflush(file_);
    unflushed_ = 0;
  }
  const std::string &Opened() const { return opened_; }
  std::atomic<uint64_t> known{0}, searched{0}, by_path[7] = {}, checked{0}, mismatched{0};

 private:
  std::mutex mutex_;
  std::unordered_map<uint64_t, uint8_t> paths_;
  FILE *file_ = nullptr;
  uint32_t unflushed_ = 0;
  std::string opened_;
};
}  // namespace

void RunSwitchShaderPrepass(const std::string &game_root) {
  if (!REXCVAR_GET(fnxf_xiii_shader_prepass))
    return;

  static NxMutex log_mutex;
  static FILE *log_file = nullptr;
  PrepassConfig config;
  config.game_root = game_root;
  config.store_path = fnxf_xiii::renut_gpu::ShaderDataPath("fnxf_xiii_shaders.bin").string();
  config.threads = std::clamp(REXCVAR_GET(fnxf_xiii_prepass_threads), 1, 12);
  config.cores = {0, 1, 2};
  // Each compile thread: uam's tables for this thread first (a small vertex and pixel shader, on newlib's heap: they
  // live as long as the process), then its own allocator for the compiles (thread_alloc.h: newlib's malloc has one
  // lock, and uam calls it ~670,000 times a shader).
  config.compile_thread_start = [] {
    static const char vs[] = "#version 460\nlayout(location = 0) in vec4 pos;\nvoid main() { gl_Position = pos; }\n";
    static const char fs[] = "#version 460\nlayout(location = 0) out vec4 c;\nvoid main() { c = vec4(1.0); }\n";
    uam::compile(uam::Stage::Vertex, vs);
    uam::compile(uam::Stage::Fragment, fs);
    ThreadAllocBegin();
  };
  config.compile_thread_end = [] { ThreadAllocEnd(); };
  // fnxf_xiii (2026-10-21): the scan threads' translations too (XenosRecomp's strings and maps, the trim's): newlib's
  // one malloc lock had the three threads waiting for each other, the cores idle.
  config.scan_thread_start = [] { ThreadAllocBegin(); };
  config.scan_thread_end = [] { ThreadAllocEnd(); };
  config.translate_scope = [](bool on) { ThreadAllocSetActive(on); };
  config.have_dksh = [](uint64_t key) {
    static const bool recompile = REXCVAR_GET(fnxf_xiii_diag_prepass_recompile);  // fnxf_xiii (2026-11-08)
    return !recompile && fnxf_xiii::renut_gpu::deko3d::CacheHas(key);
  };
  config.dksh_fast = [](uint64_t key) { return fnxf_xiii::renut_gpu::deko3d::CacheIsFast(key); };
  config.retry_failed = REXCVAR_GET(fnxf_xiii_prepass_retry_failed);
  config.compile_peak = [] { return ThreadAllocScopePeak(); };  // fnxf_xiii (2026-11-08)
  // fnxf_xiii (2026-11-11): the process, newlib's heap and the slabs, then the largest heap sites (HeapSitesEnable:
  // log_diagnostics), named from the NRO's .sym.
  config.memory_report = [&config](const char *when) {
    u64 used = 0, total = 0;
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    const struct mallinfo mi = mallinfo();
    const ThreadAllocStats ts = GetThreadAllocStats();
    config.log(fmt::format("memory {}: process {} of {} MB, newlib heap {} MB in use of {} MB (largest free {} MB), "
                           "slabs {} ({} MB), large blocks {}, remote frees {}",
                           when, used >> 20, total >> 20, size_t(mi.uordblks) >> 20, size_t(mi.arena) >> 20,
                           size_t(mi.keepcost) >> 20, ts.slabs_live, ts.slab_bytes >> 20, ts.blocks_large,
                           ts.frees_remote));
    struct Ctx {
      const PrepassConfig *config;
      unsigned n;
    } ctx{&config, 0};
    const uint64_t big = HeapSitesReport(
        [](uint64_t bytes, uint32_t count, const uintptr_t *frames, void *arg) {
          Ctx &c = *static_cast<Ctx *>(arg);
          std::string where;
          for (unsigned i = 0; i < kHeapSiteFrames; ++i)
            if (frames[i])
              where += (where.empty() ? "" : " <- ") + renut::gpu::SymbolName(frames[i]);
          c.config->log(fmt::format("memory: heap site {}: {} MB in {} blocks: {}", ++c.n, bytes >> 20, count, where));
        },
        &ctx, 12);
    config.log(fmt::format("memory: heap blocks of 256 KB and more {} MB, the rest {} MB", big >> 20,
                           size_t(mi.uordblks) > big ? (size_t(mi.uordblks) - big) >> 20 : 0));
  };
  // fnxf_xiii (2026-10-21): the variants played (recorded by the renderer), and a release's whole play-through.
  config.variant_lists = {fnxf_xiii::renut_gpu::ShaderDataPath("fnxf_xiii_variants.txt").string(),
                          fnxf_xiii::renut_gpu::ShaderDataPath("fnxf_xiii_variants_release.txt").string()};
  // fnxf_xiii (2026-11-16): with FSR 3.1 on (the boot menu has set it by now) the variants' motion-vector twins too
  // (in no list, shader_prepass.h).
  config.motion = REXCVAR_GET(fnxf_xiii_fsr3) != 0;
  config.store_dksh = [](uint64_t key, const std::vector<uint8_t> &dksh) {
    // fnxf_xiii (2026-11-08): the recompile test keeps the cache as it is (the same results, and 600 MB more on the
    // card otherwise).
    static const bool recompile = REXCVAR_GET(fnxf_xiii_diag_prepass_recompile);
    if (!recompile)
      fnxf_xiii::renut_gpu::deko3d::CacheAdd(key, dksh);
  };
  // Each compile's wall time and the time its thread was on a core (svcGetInfo ThreadTickCount): wall well above the
  // CPU time means the threads wait (locks: the allocator, Mesa's tables), not compute. Logged every 100 compiles.
  static std::atomic<uint64_t> wall_ticks{0}, cpu_ticks{0}, measured{0}, window_start{0};
  // fnxf_xiii (2026-11-15): the recorded uam flag sets (UamPaths).
  UamPaths::Get().Open(fnxf_xiii::renut_gpu::ShaderDataPath("fnxf_xiii_uam_paths.txt").string(),
                       fnxf_xiii::renut_gpu::Deko3DUamFlags());
  config.compile = [&config](bool pixel, uint64_t key, const std::string &glsl, std::vector<uint8_t> &dksh,
                             std::string &log) {
    u64 expected = 0;
    window_start.compare_exchange_strong(expected, armGetSystemTick());  // the first compile starts the first window
    u64 cpu0 = 0, cpu1 = 0;
    svcGetInfo(&cpu0, InfoType_ThreadTickCount, CUR_THREAD_HANDLE, UINT64_MAX);
    const u64 t0 = armGetSystemTick();
    uam::Result r;
    // fnxf_xiii (2026-11-15): the flag set an earlier search kept, when recorded (UamPaths). The record and the log are
    // made OUTSIDE the thread's allocator scope: a map entry made inside it stayed in a 1 MB slab of the thread's for
    // good, a slab a compile (2,496 MB of slabs after 2,500 compiles, then the threads waited for memory).
    UamPaths &paths = UamPaths::Get();
    const int known = paths.Find(key);
    static const bool check = REXCVAR_GET(fnxf_xiii_diag_uam_paths_check);
    uam::Result full;
    {
      ThreadAllocScope scope;  // uam's allocations from this thread's slabs
      r = uam::compile(pixel ? uam::Stage::Fragment : uam::Stage::Vertex, glsl.c_str(), known);
      if (check && known >= 0 && r.ok)
        full = uam::compile(pixel ? uam::Stage::Fragment : uam::Stage::Vertex, glsl.c_str());
    }
    if (r.ok && r.path >= 0) {
      ++paths.by_path[r.path];
      ++(known >= 0 && r.path == known ? paths.known : paths.searched);
      if (r.path != known)
        paths.Record(key, r.path);
    }
    if (check && known >= 0 && r.ok) {
      ++paths.checked;
      if (!full.ok || full.dksh != r.dksh || full.path != known) {
        ++paths.mismatched;
        config.log(fmt::format("[uampaths] {} variant {:016X}: recorded set {}, the search keeps {}; DKSH {} ({} / {} "
                               "bytes)", pixel ? "pixel" : "vertex", key, known, full.path,
                               full.ok && full.dksh == r.dksh ? "the same" : "DIFFERENT", r.dksh.size(),
                               full.dksh.size()));
      }
    }
    const u64 t1 = armGetSystemTick();
    svcGetInfo(&cpu1, InfoType_ThreadTickCount, CUR_THREAD_HANDLE, UINT64_MAX);
    wall_ticks += t1 - t0;
    cpu_ticks += cpu1 - cpu0;
    const uint64_t n = ++measured;
    if (n % 100 == 0) {
      const double freq = double(armGetSystemTickFreq());
      const double window = double(t1 - window_start.exchange(t1)) / freq;
      const double wall_ms = double(wall_ticks.exchange(0)) * 1000.0 / freq / 100.0;
      const double cpu_ms = double(cpu_ticks.exchange(0)) * 1000.0 / freq / 100.0;
      // fnxf_xiii (2026-11-08): and the memory (a full pre-pass ran out of it in uam near its end, std::bad_alloc in
      // nv50_ir's register allocation, three times in a row; resumed it finished): the process's used and total, newlib's
      // heap in use and its arena, the threads' slabs.
      u64 used = 0, total = 0;
      svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
      svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
      const struct mallinfo mi = mallinfo();
      const ThreadAllocStats ts = GetThreadAllocStats();
      config.log(fmt::format("compile: {} done; the last 100 in {:.1f} s ({:.0f} a minute): {:.0f} ms a shader, {:.0f} ms "
                             "of it on a core ({:.0f} % waiting); memory: process {} of {} MB, newlib heap {} MB in use "
                             "of {} MB, slabs {} ({} MB), large blocks {}, remote frees {}",
                             n, window, window > 0 ? 6000.0 / window : 0.0, wall_ms, cpu_ms,
                             wall_ms > 0 ? 100.0 * (1.0 - cpu_ms / wall_ms) : 0.0, used >> 20, total >> 20,
                             size_t(mi.uordblks) >> 20, size_t(mi.arena) >> 20, ts.slabs_live, ts.slab_bytes >> 20,
                             ts.blocks_large, ts.frees_remote));
    }
    // fnxf_xiii (2026-10-21): per thread, every 50 of its compiles: where its time went (thread_waits.h). A core
    // below 100 % has a thread that waits; this says on what.
    {
      struct Mine {
        uint32_t n = 0;
        u64 start = 0, wall = 0, cpu = 0;
        ThreadWaits at;
      };
      static thread_local Mine m;
      if (m.n == 0) {
        m.start = t0;
        m.at = MyWaits();
      }
      m.wall += t1 - t0;
      m.cpu += cpu1 - cpu0;
      if (++m.n == 50) {
        const ThreadWaits &w = MyWaits();
        const double per = 1.0 / 50.0;
        const double span = WaitTicksToMs(t1 - m.start);
        auto ms = [&](uint64_t a, uint64_t b) { return WaitTicksToMs(a - b) * per; };
        config.log(fmt::format(
            "thread on core {}: 50 compiles in {:.1f} s; a shader: {:.0f} ms compiling ({:.0f} on the core), outside "
            "it {:.0f} ms reading GLSL, {:.0f} ms adding DKSH; waited: our locks {:.1f} ms ({} times), pthread / "
            "Mesa locks {:.1f} ms ({}), newlib heap {:.1f} ms ({} calls; {:.1f} ms of it for its lock; outside the scope {}, "
            "over 512 KB {}, frees of newlib's blocks {}, reallocs of them {}, libc's own {}, aligned {})",
            svcGetCurrentProcessorNumber(), span / 1000.0, WaitTicksToMs(m.wall) * per, WaitTicksToMs(m.cpu) * per,
            ms(w.read, m.at.read), ms(w.store, m.at.store), ms(w.nx_lock, m.at.nx_lock), w.nx_waits - m.at.nx_waits,
            ms(w.pthread_lock, m.at.pthread_lock), w.pthread_waits - m.at.pthread_waits, ms(w.heap, m.at.heap),
            w.heap_calls - m.at.heap_calls, ms(w.heap_lock, m.at.heap_lock), w.heap_inactive - m.at.heap_inactive,
            w.heap_big - m.at.heap_big, w.heap_free_foreign - m.at.heap_free_foreign,
            w.heap_realloc_foreign - m.at.heap_realloc_foreign, w.heap_libc - m.at.heap_libc,
            w.heap_align - m.at.heap_align));
        m = {};
      }
    }
    dksh = std::move(r.dksh);
    log = std::move(r.log);
    return r.ok && !dksh.empty();
  };
  // Every draw of a shader with bool constants specialises them (fnxf_xiii_gpu_bool_spec): its default variant is never
  // used, so the pre-pass leaves it to the first draw.
  const bool bool_spec = REXCVAR_GET(fnxf_xiii_gpu_bool_spec);
  config.wanted = [bool_spec](bool, const uint32_t mask[8]) {
    if (!bool_spec)
      return true;
    for (int i = 0; i < 8; i++)
      if (mask[i])
        return false;
    return true;
  };
  // fnxf_xiii (2026-10-23, late specialisation: late_spec.h): every shader's front end, kept next to the store.
  // A store that could not be opened has nothing to make: no pre-pass at every start for it.
  config.have_front_end = [](uint64_t key) { return !FrontEndStore::Get().IsOpen() || FrontEndStore::Get().Has(key); };
  if (REXCVAR_GET(fnxf_xiii_deko3d_late_spec))
  config.front_end = [&config](bool pixel, const std::string &glsl, std::vector<uint8_t> &bytes, std::string &log) {
    std::string base;
    std::vector<std::pair<uint32_t, uint32_t>> defaults;
    if (!LateSpecBase(glsl, base, defaults))
      return false;
    const u64 t0 = armGetSystemTick();
    std::vector<uint8_t> fe;
    bool ok;
    {
      ThreadAllocScope scope;
      ok = uam::frontEnd(pixel ? uam::Stage::Fragment : uam::Stage::Vertex, base.c_str(), fe, log);
    }
    if (ok)
      LateSpecPack(defaults, fe, bytes);
    static std::atomic<uint64_t> n{0}, ticks{0};
    ticks += armGetSystemTick() - t0;
    if (++n % 500 == 0)
      config.log(fmt::format("front ends: {} done, {:.0f} ms each (the last 500)", n.load(),
                             double(ticks.exchange(0)) * 1000.0 / double(armGetSystemTickFreq()) / 500.0));
    return ok && !bytes.empty();
  };
  config.store_front_end = [](uint64_t key, const std::vector<uint8_t> &bytes) { FrontEndStore::Get().Add(key, bytes); };
  config.front_end_failed = [](uint64_t key) { FrontEndStore::Get().AddFailure(key); };
  // fnxf_xiii (2026-10-23): step 3: the last batches to the card, then the DKSH cache's index sorted and written.
  config.finish = [&config] {
    // fnxf_xiii (2026-11-15): the recorded uam flag sets out, and what they did.
    {
      UamPaths &p = UamPaths::Get();
      p.Flush();
      config.log(fmt::format("uam paths: {} compiles with a recorded flag set (one codegen), {} searched and recorded; "
                             "kept sets 0-6: {} {} {} {} {} {} {}{}",
                             p.known.load(), p.searched.load(), p.by_path[0].load(), p.by_path[1].load(),
                             p.by_path[2].load(), p.by_path[3].load(), p.by_path[4].load(), p.by_path[5].load(),
                             p.by_path[6].load(),
                             p.checked ? fmt::format("; checked against the search {}, different {}", p.checked.load(),
                                                     p.mismatched.load())
                                       : std::string()));
    }
    FrontEndStore::Get().Sync();
    ShaderStore::Get().Sync();
    fnxf_xiii::renut_gpu::deko3d::SaveShaderCache();
  };
  config.log = [](const std::string &line) {
    // fnxf_xiii (2026-10-25): the start check's timings in the game log only with log_diagnostics (shader_prepass.log
    // keeps every line).
    if (line.rfind("start check: ", 0) != 0 || REXCVAR_GET(log_diagnostics))
      REXLOG_INFO("[prepass] {}", line);
    std::lock_guard<NxMutex> lock(log_mutex);
    if (log_file) {
      std::fprintf(log_file, "%s\n", line.c_str());
      std::fflush(log_file);
    }
  };

  // The compiler as the renderer's thread sets it up (plume_deko3d.cpp compileWorker): the same flags, so the DKSH is
  // the one the game would compile.
  const u64 t0 = armGetSystemTick();
  uam::setFNXF_XIIIFlags(fnxf_xiii::renut_gpu::Deko3DUamFlags());
  uam::init(true);
  const u64 t1 = armGetSystemTick();
  const bool needed = PrepassNeeded(config);
  const u64 t2 = armGetSystemTick();
  const double tick_ms = 1000.0 / double(armGetSystemTickFreq());
  // fnxf_xiii (2026-10-23): the time before the game starts, step by step (PrepassNeeded's line has its parts).
  if (REXCVAR_GET(log_diagnostics)) REXLOG_INFO("[prepass] start: compiler set up in {:.0f} ms, check in {:.0f} ms", double(t1 - t0) * tick_ms,
              double(t2 - t1) * tick_ms);
  if (!needed) {
    REXLOG_INFO("[prepass] nothing to do ({} shaders in the store)", ShaderStore::Get().Count());
    return;
  }
  // fnxf_xiii (2026-10-25): in log/, a new file each run (it was appended to).
  log_file = std::fopen(fnxf_xiii::renut_gpu::NextToExecutable("log/shader_prepass.log").string().c_str(), "w");
  config.log(fmt::format("---- shader pre-pass: game files {}, store {}", config.game_root, config.store_path));
  {
    u64 core_mask = 0;
    svcGetInfo(&core_mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
    s32 priority = 0;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    config.log(fmt::format("{} threads on cores 0-2; the process may use cores 0x{:X}; the main thread is on core {} at "
                           "priority 0x{:X}", config.threads, core_mask, svcGetCurrentProcessorNumber(), priority));
  }

  // The screen.
  PlFontData font{};
  const bool pl = R_SUCCEEDED(plInitialize(PlServiceType_User));
  if (pl)
    plGetSharedFontByType(&font, PlSharedFontType_Standard);
  PrepassScreen screen;
  // fnxf_xiii (2026-11-07): 1920x1080 docked on a 1080p TV, switched as the console is docked or not (below).
  bool hd = ScreenWants1080p();
  screen.Init(game_root + "/nxeart", static_cast<const uint8_t *>(font.address), font.size, hd ? 1.5f : 1.0f);
  ScreenFramebuffer sfb;
  bool have_fb = sfb.Open(hd);
  Framebuffer &fb = sfb.fb;
  // fnxf_xiii (2026-10-21): the boost asked for again every 5 s for the whole pre-pass. Nothing in the NRO turns it
  // off before the end, but the system can drop it (a start was seen at normal clocks throughout): asking again is
  // one IPC call, and puts it back. Logged when the answer changes.
  Result boost_rc = appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
  // (2026-11-15: here, once config.log is set: logged before it, an empty std::function, the game ended at start
  // with std::bad_function_call whenever the pre-pass had work.)
  config.log("uam paths: " + UamPaths::Get().Opened());
  config.log(R_SUCCEEDED(boost_rc) ? std::string("CPU boost asked for: accepted")
                                   : fmt::format("CPU boost asked for: refused (0x{:X})", boost_rc));
  u32 boost_tries = 1;
  u64 last_boost = armGetSystemTick();
  PrepassProgress progress;
  progress.skip_compile = !REXCVAR_GET(fnxf_xiii_prepass_compile);  // fnxf_xiii (2026-10-22): diagnostic
  std::thread worker([&] { RunPrepass(config, progress); });

  using SteadyClock = std::chrono::steady_clock;
  const SteadyClock::time_point start = SteadyClock::now();
  SteadyClock::time_point phase_start = start;
  double scan_seconds = 0.0;
  double front_end_seconds = 0.0;  // fnxf_xiii (2026-10-23): step 3, the intermediates
  int last_phase = -1;
  uint64_t base_done = 0;
  bool leaving = false;
  // The screen 4 times a second (HOME checked every 16 ms): a full-rate redraw (two 3.6 MB copies, the text rasterised and
  // the linear buffer swizzled into the framebuffer) took core 0 and memory bandwidth from the compile threads.
  u64 last_draw = 0;
  const u64 draw_every = armGetSystemTickFreq() / 4;
  while (progress.phase.load() != int(PrepassPhase::kDone)) {
    svcSleepThread(16'000'000);
    if (!appletMainLoop()) {
      // HOME closed the software: what is done stays; the next start goes on from there.
      progress.cancel = true;
      leaving = true;
    }
    // No way to skip it (fnxf_xiii, 2026-10-21): the game starts once every shader is compiled. Closing the software
    // from HOME keeps what is done; the next start goes on from there.
    // fnxf_xiii (2026-10-23): a new step is drawn at once (step 3 can be shorter than the redraw's quarter second).
    if (armGetSystemTick() - last_draw < draw_every && progress.phase.load() == last_phase)
      continue;
    last_draw = armGetSystemTick();
    if (last_draw - last_boost >= armGetSystemTickFreq() * 5) {
      last_boost = last_draw;
      const Result rc = appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
      ++boost_tries;
      if (rc != boost_rc)
        config.log(fmt::format("CPU boost asked for again (time {}): {} (0x{:X})", boost_tries,
                               R_SUCCEEDED(rc) ? "accepted" : "refused", rc));
      boost_rc = rc;
    }

    const int phase = progress.phase.load();
    if (phase != last_phase) {
      if (last_phase == int(PrepassPhase::kScan) || last_phase == int(PrepassPhase::kCached))
        scan_seconds += std::chrono::duration<double>(SteadyClock::now() - phase_start).count();
      else if (last_phase == int(PrepassPhase::kFrontEnd))
        front_end_seconds += std::chrono::duration<double>(SteadyClock::now() - phase_start).count();
      last_phase = phase;
      phase_start = SteadyClock::now();
      base_done = 0;
    }
    const double elapsed = std::chrono::duration<double>(SteadyClock::now() - phase_start).count();
    const double total_elapsed = std::chrono::duration<double>(SteadyClock::now() - start).count();
    const PrepassStrings t = PackPrepassStrings(kPrepassStrings[UiLanguage()]);  // fnxf_xiii: a language pack's
    ScreenText text;
    text.title = t.title;
    text.footer = t.footer;
    if (phase == int(PrepassPhase::kCached)) {
      // fnxf_xiii (2026-10-23): the shaders found before, translated again for this version (container_cache.h).
      const uint32_t total = progress.cached_total.load(), done = progress.cached_done.load();
      text.step = t.step_cached;
      text.progress = total ? float(double(done) / double(total)) : 0.0f;
      text.detail = fmt::format(fmt::runtime(t.n_of_total), Thousands(t, done), Thousands(t, total));
      if (done >= 200 && elapsed > 3)
        text.eta = Minutes(t, elapsed * double(total - done) / double(done));
    }
    else if (phase == int(PrepassPhase::kScan)) {
      const uint64_t total = progress.bytes_total.load(), done = progress.scan.bytes.load();
      text.step = t.step_scan;
      text.progress = total ? float(double(done) / double(total)) : 0.0f;
      text.detail = fmt::format(fmt::runtime(t.scan_detail), Thousands(t, done >> 20), Thousands(t, total >> 20),
                                Thousands(t, progress.found.load()), Thousands(t, progress.added.load()));
      if (done > (64u << 20) && elapsed > 5)
        text.eta = Minutes(t, elapsed * double(total - done) / double(done));
    }
    else if (phase == int(PrepassPhase::kCompile)) {
      const uint32_t total = progress.compile_total.load(), done = progress.compile_done.load();
      // fnxf_xiii (2026-10-23): after the cached shaders were translated again (a new version), the compile is a
      // recompile of those.
      text.step = progress.cached_total.load() ? t.step_recompile : t.step_compile;
      text.progress = total ? float(double(done) / double(total)) : 0.0f;
      text.detail = fmt::format(fmt::runtime(t.n_of_total), Thousands(t, done), Thousands(t, total));
      if (progress.compile_failed.load())
        text.detail += fmt::format(fmt::runtime(t.failed), Thousands(t, progress.compile_failed.load()));
      if (done >= 30 && elapsed > 10)
        text.eta = Minutes(t, elapsed * double(total - done) / double(done - base_done));
    }
    else if (phase == int(PrepassPhase::kFrontEnd)) {
      // fnxf_xiii (2026-10-23): each shader's front end for late specialisation (late_spec.h): Mesa's intermediate
      // form, from which a variant compiles in about a quarter of the time.
      const uint32_t total = progress.front_end_total.load(), done = progress.front_end_done.load();
      text.step = t.step_front_end;
      text.progress = total ? float(double(done) / double(total)) : 0.0f;
      text.detail = fmt::format(fmt::runtime(t.n_of_total), Thousands(t, done), Thousands(t, total));
      if (done >= 30 && elapsed > 10)
        text.eta = Minutes(t, elapsed * double(total - done) / double(done));
    }
    else if (phase == int(PrepassPhase::kIndex)) {
      text.step = t.step_index;
      text.progress = -1.0f;
      text.detail = fmt::format(fmt::runtime(t.compiled), Thousands(t, progress.compile_done.load()));
    }
    else {
      text.step = t.step_check;
      text.progress = -1.0f;
    }
    // The timer: this step's time and the whole pre-pass's.
    // "This step: 3:12 so far, about 25 minutes left · Total: 4:40" (the estimate is this step's alone).
    {
      std::string line = fmt::format(fmt::runtime(t.this_step), Clock(elapsed));
      if (!text.eta.empty())
        line += ", " + text.eta;
      line += fmt::format(fmt::runtime(t.total), Clock(total_elapsed));
      if (phase == int(PrepassPhase::kCompile) && scan_seconds > 0.0)
        line += fmt::format(fmt::runtime(t.step1_took), Clock(scan_seconds));
      text.eta = line;
    }
    if (ScreenWants1080p() != hd) {  // fnxf_xiii (2026-11-07): docked or undocked
      hd = !hd;
      sfb.Close();
      screen.Init(game_root + "/nxeart", static_cast<const uint8_t *>(font.address), font.size, hd ? 1.5f : 1.0f);
      have_fb = sfb.Open(hd);
    }
    if (have_fb) {
      u32 stride = 0;
      auto *pixels = static_cast<uint32_t *>(framebufferBegin(&fb, &stride));
      screen.Draw(pixels, stride / 4, text);
      framebufferEnd(&fb);
    }
  }
  worker.join();
  const double total_seconds = std::chrono::duration<double>(SteadyClock::now() - start).count();
  if (last_phase == int(PrepassPhase::kScan) || last_phase == int(PrepassPhase::kCached))
    scan_seconds += std::chrono::duration<double>(SteadyClock::now() - phase_start).count();
  else if (last_phase == int(PrepassPhase::kFrontEnd))
    front_end_seconds += std::chrono::duration<double>(SteadyClock::now() - phase_start).count();
  const double compile_seconds = last_phase == int(PrepassPhase::kCompile) || last_phase == int(PrepassPhase::kFrontEnd) ||
                                         last_phase == int(PrepassPhase::kIndex) || last_phase == int(PrepassPhase::kDone)
                                     ? std::max(0.0, total_seconds - scan_seconds - front_end_seconds)
                                     : 0.0;
  appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
  ShaderStore::Get().Sync();
  fnxf_xiii::renut_gpu::deko3d::SaveShaderCache();
  const uint32_t compiled = progress.compile_done.load();
  {
    const ThreadAllocStats a = GetThreadAllocStats();
    config.log(fmt::format("compile threads' allocator: {} slabs ({} MB) still hold blocks, {} large blocks went to "
                           "newlib, {} blocks freed by another thread", a.slabs_live, a.slab_bytes >> 20,
                           a.blocks_large, a.frees_remote));
  }
  const uint32_t front_ends = progress.front_end_done.load();
  config.log(fmt::format("---- pre-pass {} in {}: finding the shaders {}, compiling {} ({} shaders, {:.0f} ms each on "
                         "{} threads), intermediates {} ({} shaders, {:.0f} ms each, {} without one); {} shaders "
                         "stored, {} failed to compile",
                         progress.cancel ? "stopped" : "done", Clock(total_seconds), Clock(scan_seconds),
                         Clock(compile_seconds), compiled,
                         compiled ? compile_seconds * 1000.0 * config.threads / compiled : 0.0, config.threads,
                         Clock(front_end_seconds), front_ends,
                         front_ends ? front_end_seconds * 1000.0 * config.threads / front_ends : 0.0,
                         progress.front_end_failed.load(), progress.stored.load(), progress.compile_failed.load()));
  sfb.Close();
  if (pl)
    plExit();
  if (log_file) {
    std::fclose(log_file);
    log_file = nullptr;
  }
  if (leaving)
    ExitBeforeGame();  // fnxf_xiii (2026-10-25): not std::exit (early_exit.h)
}

}  // namespace renut::nxs
