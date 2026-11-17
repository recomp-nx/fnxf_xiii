// fnxf_xiii (2026-10-25): boot_menu_switch.h.
//
// The menu draws on the shader pre-pass's screen (prepass_screen.h: the game's dashboard art and the system font)
// through libnx's framebuffer on the default window, which is free until the renderer makes its swapchain. One page:
//   Language       English, Deutsch, Français, Español, Italiano (each in its own language): user_language, the
//                  Xbox 360 language the game reads (XGetLanguage and XConfig: 1, 3, 4, 5, 6); the game has the
//                  text of all five (txtres_us / _gr / _fr / _sp / _it).
//   Frame interval 1-4 refreshes a frame: fnxf_xiii_frame_interval (frame_interval.h; the game's own 30 fps lock off,
//                  except for prerendered movies).
//   Resolution     1024x576 (the game's) or 1280x720 (fnxf_xiii_render_720p, experimental). At 1280x720 nothing is
//                  upscaled: the upscaler is Off and cannot be changed.
//   Anti-aliasing  Off, MSAA 2x (the game's own), 4x or 8x: fnxf_xiii_diag_msaa_off, fnxf_xiii_msaa_samples (2026-11-06:
//                  the game's 2x targets made with 4 or 8 samples on the host, render/surfaces.cpp). (SMAA was an option on 2026-10-25 and
//                  was removed the same day: it looked worse than the game's MSAA. A settings file's "smaa" reads as
//                  MSAA 2x.) fnxf_xiii (2026-11-07): or FSR 3.1 (fnxf_xiii_fsr3, render/fsr3.cpp: MSAA off, the scene's
//                  motion vectors): temporal anti-aliasing at 1280x720, and the upscale too at 1024x576, where the
//                  Upscaler row then shows FSR 3.1 and cannot be changed.
//   Upscaler       Lanczos (fnxf_xiii_renut_upscale_filter 1) or FSR 1 (3) where the picture is enlarged, bilinear (4)
//                  where it is reduced or kept (2026-11-08: Bilinear is no longer a choice of its own).
//                  At 1280x720 the upscale draw is 1:1: the row shows Off (bilinear does the copy).
// fnxf_xiii (2026-11-07): the main page is Language, Handheld settings, Docked settings, Achievements, Reset, Start; the
// rows above (Frame interval to Upscaler) are on each mode's page, Resolution also 1600x900 and 1920x1080
// (fnxf_xiii_render_height). Docked on a 1080p TV the output is 1920x1080 (render/graphics_options.cpp), except with a
// 1280x720 scene. The page of the mode the console starts
// the game in (Reverse-NX's when its overlay rules) is applied; the output follows docking live.
// Settings file: fnxf_xiii_settings.ini next to the NRO, "key = value" lines (language, interval, resolution, aa,
// upscaler). Applied at every start, over fnxf_xiii.toml's values of the same options.
// fnxf_xiii (2026-10-28): an Achievements row opens a page of two lists, the unlocked achievements and all of them
// (the locked ones dimmed), on the background without the logo. The names come from <user data>/achievements/
// names.toml, which the game writes at each start from default.xex (backend/achievement_popup.cpp: none before the
// game has run once), a language pack's achievements.toml over them; the unlocked ones from the runtime's
// <user data>/achievements/<title id>.toml.
// The menu's own text follows the Language row (kStrings: English, German, French, Spanish, Italian).
// fnxf_xiii (2026-10-25): the Language row also lists the language packs in sdmc:/switch/fnxf_xiii/lang (each zip's
// language.ini name, cpu/fnxf_xiii_lang_pack.h): choosing one sets fnxf_xiii_lang to it and the game's language to the
// pack's base; the menu's text is then the pack's [port] menu_* strings (its base language's where one is missing).
#include "renut_engine/gpu/nx_shaders/boot_menu_switch.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <cstdio>
#include <ctime>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

#include <switch.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/flags.h>  // user_language
#include <rex/ui/switch_saltynx.h>  // fnxf_xiii (2026-11-07): Reverse-NX's mode (BaseMode)

#include "renut_engine/gpu/fnxf_xiii_renut_tuning.h"  // NextToExecutable
#include "renut_engine/gpu/nx_shaders/early_exit.h"
#include "renut_engine/gpu/nx_shaders/prepass_screen.h"
#include "cpu/fnxf_xiii_lang_pack.h"  // fnxf_xiii (2026-10-25): language packs
#include <toml++/toml.hpp>  // fnxf_xiii (2026-10-28): the achievement lists

REXCVAR_DECLARE(int32_t, fnxf_xiii_frame_interval);        // renut_engine/frame_interval.cpp
REXCVAR_DECLARE(bool, fnxf_xiii_render_720p);              // cpu/fnxf_xiii_render_resolution.cpp
REXCVAR_DECLARE(bool, fnxf_xiii_diag_msaa_off);
REXCVAR_DECLARE(int32_t, fnxf_xiii_msaa_samples);  // render/surfaces.cpp (2026-11-06)            // render/surfaces.cpp
REXCVAR_DECLARE(int32_t, fnxf_xiii_renut_upscale_filter);  // render/draw.cpp
REXCVAR_DECLARE(int32_t, fnxf_xiii_fsr3);                   // render/fsr3.cpp (2026-11-07: Anti-aliasing FSR 3.1)
REXCVAR_DECLARE(std::string, fnxf_xiii_lang);              // cpu/fnxf_xiii_lang_pack.cpp
REXCVAR_DECLARE(int32_t, fnxf_xiii_diag_output);            // render/graphics_options.cpp (2026-11-07)
REXCVAR_DECLARE(int32_t, fnxf_xiii_render_height);         // cpu/fnxf_xiii_render_resolution.cpp (2026-11-07)
REXCVAR_DECLARE(int32_t, fnxf_xiii_drs);                    // render/dynamic_resolution.cpp (2026-11-07)
REXCVAR_DECLARE(int32_t, fnxf_xiii_drs_min);                // render/dynamic_resolution.cpp (2026-11-08)
// fnxf_xiii (2026-11-01): diagnostic: the game paces itself as on the console (fnxf_xiii_frame_interval 0), whatever the
// menu's Frame interval says.
REXCVAR_DEFINE_BOOL(fnxf_xiii_diag_game_frame_lock, false, "Game/Diagnostics",
                    "Diagnostic: the game's own 30 fps lock, as on the console (fnxf_xiii_frame_interval 0), over the boot "
                    "menu's Frame interval")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(fnxf_xiii_diag_boot_menu, false, "Game/Diagnostics",
                    "Opens the boot menu at every start, as A held does (a remote controller's A is not seen there)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace renut::nxs {

namespace {

// fnxf_xiii (2026-11-07): the graphics settings of one console mode (the Handheld and Docked settings pages).
struct Profile {
  int interval = 2;   // 1-4
  int resolution = 0; // kResolutions: 0 = 1024x576, 1 = 1280x720, 2 = 1600x900, 3 = 1920x1080
  int aa = 1;         // 0 off, 1 MSAA 2x, 2 MSAA 4x, 3 MSAA 8x, 4 FSR 3.1 (kAaFsr3)
  // 1 Lanczos (the default), 2 FSR 1: each where the picture is enlarged, bilinear where it is reduced or kept
  // (render/draw.cpp). 2026-11-08: no Bilinear choice any more (a file's "bilinear" reads as Lanczos).
  int upscaler = 1;
  // fnxf_xiii (2026-11-07): dynamic resolution (fnxf_xiii_drs): 0 off, else its lowest height in percent of the
  // Resolution's (fnxf_xiii_drs_min; kDynresChoices). 2026-11-08: the player picks the lowest. 2026-11-09: off by
  // default (the defaults are the game's own: 1024x576, MSAA 2x, Lanczos, no dynamic resolution).
  int dynres = 0;
};
constexpr int kHandheld = 0, kDocked = 1;
constexpr const char *kModeKeys[] = {"handheld", "docked"};

struct Settings {
  int language = 0;   // index into Choices() (kLanguages, then the language packs)
  Profile mode[2];    // kHandheld, kDocked: the one of the mode the console starts the game in is applied
};


struct Language {
  const char *key;
  const char *name;  // in its own language
  uint32_t xbox;     // XLanguage
};
constexpr Language kLanguages[] = {
    {"english", "English", 1}, {"german", "Deutsch", 3},   {"french", "Fran\xC3\xA7" "ais", 4},
    {"spanish", "Espa\xC3\xB1" "ol", 5}, {"italian", "Italiano", 6},
};
constexpr int kLanguageCount = int(sizeof(kLanguages) / sizeof(kLanguages[0]));
constexpr const char *kResolutions[] = {"1024x576", "1280x720", "1600x900", "1920x1080"};
constexpr int32_t kResolutionHeights[] = {576, 720, 900, 1080};  // fnxf_xiii_render_height
// fnxf_xiii (2026-11-07): above 1280x720 with the game's EDRAM tiling and, at 1920x1080, its render target pool made
// larger (cpu/fnxf_xiii_render_resolution.cpp).
constexpr int kResolutionCount = 4, kResolutionMenuCount = 4;
constexpr const char *kAa[] = {"Off", "MSAA 2x", "MSAA 4x", "MSAA 8x", "FSR 3.1"};  // the log's (the menu's: Strings)
constexpr const char *kAaKeys[] = {"off", "msaa", "msaa4", "msaa8", "fsr3"};
constexpr int32_t kAaSamples[] = {2, 2, 4, 8, 2};  // fnxf_xiii_msaa_samples for each (FSR 3.1: MSAA off)
constexpr int kAaCount = 5;
constexpr int kAaFsr3 = 4;  // fnxf_xiii (2026-11-07): FSR 3.1, which also upscales at 1024x576
constexpr const char *kUpscalers[] = {"Bilinear", "Lanczos", "FSR 1"};  // the log's names (the menu's: Strings)
constexpr const char *kUpscalerKeys[] = {"bilinear", "lanczos", "fsr1"};
// fnxf_xiii (2026-11-08): the Dynamic resolution row's choices: its lowest height (percent), 50 the lowest; Right
// raises it, and past 90 is off (the full height always).
constexpr int kDynresChoices[] = {50, 60, 70, 80, 90, 0};
constexpr int kDynresCount = int(sizeof(kDynresChoices) / sizeof(kDynresChoices[0]));
int DynresIndex(int value) {
  for (int i = 0; i < kDynresCount; i++)
    if (kDynresChoices[i] == value)
      return i;
  return 2;  // 70
}
// A settings file's value: "off", "on" (2026-11-07's files: 70), or a percent (the nearest choice, 50 at least).
int DynresFromText(const std::string &v) {
  if (v == "off")
    return 0;
  const int pct = std::atoi(v.c_str());
  if (v == "on" || pct <= 0)
    return 70;
  int best = 50;
  for (int c : kDynresChoices)
    if (c && std::abs(c - pct) < std::abs(best - pct))
      best = c;
  return best;
}

// fnxf_xiii (2026-10-25): the menu's text in each language of kLanguages (same order), shown in the language selected
// on the Language row as it changes.
struct Strings {
  const char *title, *language, *interval, *resolution, *aa, *upscaler, *start, *off, *bilinear;
  const char *interval_value;  // printf format: the interval, then its frame rate at 60 Hz
  const char *note_language, *note_interval, *note_res_576, *note_res_720, *note_aa_off, *note_aa_msaa;
  const char *note_up_720, *note_up_bilinear, *note_up_lanczos, *note_up_fsr1, *note_start;
  const char *footer;  // two lines
  const char *reset, *note_reset;  // fnxf_xiii (2026-10-25): "Reset settings"
  const char *note_aa_fsr3, *note_up_fsr3;  // fnxf_xiii (2026-11-07): Anti-aliasing FSR 3.1, and the Upscaler row with it
  // fnxf_xiii (2026-11-07): the Handheld / Docked settings pages.
  const char *handheld, *docked, *note_handheld, *note_docked, *current;
  // fnxf_xiii (2026-11-07): Resolution 1600x900 and 1920x1080, and the Upscaler row above 1280x720.
  const char *note_res_900, *note_res_1080, *note_up_hi;
  const char *note_up_tv;  // fnxf_xiii (2026-11-07): appended on the Docked page at 1280x720 and 1600x900
  const char *note_up_down_tv;  // fnxf_xiii (2026-11-07): the Docked page at 1920x1080 (scaled down on a 720p TV)
  const char *sub_footer;
  const char *dynres, *on, *note_dynres;  // fnxf_xiii (2026-11-07): the Dynamic resolution row
};
constexpr Strings kStrings[] = {
    {   // English
        "Settings", "Language", "Frame interval", "Resolution", "Anti-aliasing", "Upscaler",
        "Start the game", "Off", "Bilinear", "%d  (%d fps at 60 Hz)",
        "The game's text and menus.",
        "Display refreshes a frame: 1 = every refresh. Prerendered movies always play at 30 fps.",
        "The game's own 3D resolution, upscaled to the output.",
        "Experimental: the 3D scene at 1280x720, about 56% more GPU work.",
        "No anti-aliasing.", "The game's own anti-aliasing.",
        "Not used at 1280x720.", "Bilinear: the cheapest, softer than the others.",
        "Lanczos when the picture is enlarged, bilinear when it is reduced.",
        "AMD FSR 1 when the picture is enlarged, bilinear when it is reduced.",
        "Saves these settings and starts the game.",
        "Up / Down: choose · A: open · Left / Right: change · +: start the game\n"
        "Hold A while the game starts to open this menu again.",
        "Reset settings", "Puts every setting back to its default (the language stays).",
        "AMD FSR 3.1: temporal anti-aliasing; it also upscales a lower resolution to the output.",
        "Done by FSR 3.1 (Anti-aliasing).",
        "Handheld settings", "Docked settings",
        "Graphics used when the game starts in handheld mode.",
        "Graphics used when the game starts docked (on a TV).",
        " The console is in this mode now.",
        "Experimental: the 3D scene at 1600x900, about 2.4x the GPU work of 1024x576. Best on a 1080p TV.",
        "Experimental: the 3D scene at 1920x1080, about 3.5x the GPU work of 1024x576. Best on a 1080p TV.",
        "Bilinear at this resolution.",
        " Used only when the console's TV resolution is 1080p.",
        "Bilinear, used only when the console's TV resolution is 720p (the picture is scaled down).",
        "Up / Down: choose \xC2\xB7 Left / Right: change \xC2\xB7 B: back",
        "Dynamic resolution", "On",
        "Lowers the 3D resolution in gameplay (down to the % shown) to keep the frame rate. Off: never.",
    },
    {   // Deutsch
        "Einstellungen", "Sprache", "Bildintervall", "Auflösung", "Kantenglättung",
        "Hochskalierung", "Spiel starten", "Aus", "Bilinear", "%d  (%d fps bei 60 Hz)",
        "Texte und Menüs des Spiels.",
        "Bildschirmaktualisierungen pro Bild: 1 = jede Aktualisierung. Vorgerenderte Filme laufen immer mit 30 fps.",
        "Die originale 3D-Auflösung des Spiels, auf die Ausgabe hochskaliert.",
        "Experimentell: die 3D-Szene in 1280x720, etwa 56 % mehr GPU-Last.",
        "Keine Kantenglättung.", "Die eigene Kantenglättung des Spiels.",
        "Bei 1280x720 nicht verwendet.", "Bilinear: am sparsamsten, weicher als die anderen.",
        "Lanczos beim Vergrößern des Bildes, bilinear beim Verkleinern.",
        "AMD FSR 1 beim Vergrößern des Bildes, bilinear beim Verkleinern.",
        "Speichert diese Einstellungen und startet das Spiel.",
        "Oben / Unten: auswählen · A: öffnen · Links / Rechts: ändern · +: Spiel starten\n"
        "A beim Spielstart gedrückt halten, um dieses Menü erneut zu öffnen.",
        "Einstellungen zurücksetzen", "Setzt alle Einstellungen auf ihre Standardwerte zurück (die Sprache bleibt).",
        "AMD FSR 3.1: temporale Kantenglättung; skaliert eine niedrigere Auflösung auch auf die Ausgabe hoch.",
        "Übernimmt FSR 3.1 (Kantenglättung).",
        "Handheld-Einstellungen", "TV-Modus-Einstellungen",
        "Grafik, wenn das Spiel im Handheld-Modus startet.",
        "Grafik, wenn das Spiel im TV-Modus (angedockt) startet.",
        " Die Konsole ist gerade in diesem Modus.",
        "Experimentell: die 3D-Szene in 1600x900, etwa die 2,4-fache GPU-Last von 1024x576. Am besten an einem 1080p-Fernseher.",
        "Experimentell: die 3D-Szene in 1920x1080, etwa die 3,5-fache GPU-Last von 1024x576. Am besten an einem 1080p-Fernseher.",
        "Bei dieser Auflösung bilinear.",
        " Nur wenn die TV-Auflösung der Konsole 1080p ist.",
        "Bilinear, nur wenn die TV-Auflösung der Konsole 720p ist (das Bild wird verkleinert).",
        "Oben / Unten: auswählen \xC2\xB7 Links / Rechts: ändern \xC2\xB7 B: zurück",
        "Dynamische Auflösung", "An",
        "Senkt die 3D-Auflösung im Spiel (bis zum gezeigten %), um die Bildrate zu halten. Aus: nie.",
    },
    {   // Français
        "Paramètres", "Langue", "Intervalle d'image", "Résolution", "Anticrénelage",
        "Mise à l'échelle", "Lancer le jeu", "Désactivé", "Bilinéaire", "%d  (%d i/s à 60 Hz)",
        "Les textes et les menus du jeu.",
        "Rafraîchissements de l'écran par image : 1 = à chaque rafraîchissement. Les vidéos précalculées sont "
        "toujours lues à 30 i/s.",
        "La résolution 3D d'origine du jeu, mise à l'échelle de la sortie.",
        "Expérimental : la scène 3D en 1280x720, environ 56 % de travail GPU en plus.",
        "Pas d'anticrénelage.", "L'anticrénelage d'origine du jeu.",
        "Non utilisée en 1280x720.", "Bilinéaire : le moins coûteux, plus doux que les autres.",
        "Lanczos quand l'image est agrandie, bilinéaire quand elle est réduite.",
        "AMD FSR 1 quand l'image est agrandie, bilinéaire quand elle est réduite.",
        "Enregistre ces paramètres et lance le jeu.",
        "Haut / Bas : choisir · A : ouvrir · Gauche / Droite : modifier · + : lancer le jeu\n"
        "Maintenez A au lancement du jeu pour rouvrir ce menu.",
        "Réinitialiser les paramètres",
        "Remet tous les paramètres à leur valeur par défaut (la langue est conservée).",
        "AMD FSR 3.1 : anticrénelage temporel ; agrandit aussi une résolution plus basse à celle de sortie.",
        "Assurée par FSR 3.1 (Anticrénelage).",
        "Paramètres mode portable", "Paramètres mode TV",
        "Graphismes utilisés quand le jeu démarre en mode portable.",
        "Graphismes utilisés quand le jeu démarre en mode TV (sur la station d'accueil).",
        " La console est dans ce mode actuellement.",
        "Expérimental : la scène 3D en 1600x900, environ 2,4 fois le travail GPU du 1024x576. Idéal sur un téléviseur 1080p.",
        "Expérimental : la scène 3D en 1920x1080, environ 3,5 fois le travail GPU du 1024x576. Idéal sur un téléviseur 1080p.",
        "Bilinéaire à cette résolution.",
        " Seulement si la résolution TV de la console est 1080p.",
        "Bilinéaire, seulement si la résolution TV de la console est 720p (l'image est réduite).",
        "Haut / Bas : choisir \xC2\xB7 Gauche / Droite : modifier \xC2\xB7 B : retour",
        "Résolution dynamique", "Activée",
        "Baisse la résolution 3D en jeu (jusqu'au % indiqué) pour tenir la fréquence d'images. Désactivée : jamais.",
    },
    {   // Español
        "Ajustes", "Idioma", "Intervalo de fotogramas", "Resolución", "Suavizado de bordes",
        "Escalado", "Iniciar el juego", "Desactivado", "Bilineal", "%d  (%d fps a 60 Hz)",
        "Los textos y menús del juego.",
        "Refrescos de pantalla por fotograma: 1 = cada refresco. Los vídeos prerrenderizados siempre se reproducen "
        "a 30 fps.",
        "La resolución 3D original del juego, escalada a la de salida.",
        "Experimental: la escena 3D a 1280x720, alrededor de un 56 % más de carga de GPU.",
        "Sin suavizado de bordes.", "El suavizado de bordes propio del juego.",
        "No se usa a 1280x720.", "Bilineal: el más ligero, más suave que los demás.",
        "Lanczos al ampliar la imagen, bilineal al reducirla.",
        "AMD FSR 1 al ampliar la imagen, bilineal al reducirla.",
        "Guarda estos ajustes e inicia el juego.",
        "Arriba / Abajo: elegir · A: abrir · Izquierda / Derecha: cambiar · +: iniciar el juego\n"
        "Mantén pulsado A al iniciar el juego para volver a abrir este menú.",
        "Restablecer ajustes", "Devuelve todos los ajustes a sus valores predeterminados (el idioma se mantiene).",
        "AMD FSR 3.1: suavizado temporal; también escala una resolución menor a la de salida.",
        "Lo hace FSR 3.1 (Suavizado de bordes).",
        "Ajustes modo portátil", "Ajustes modo TV",
        "Gráficos usados cuando el juego se inicia en modo portátil.",
        "Gráficos usados cuando el juego se inicia en modo TV (en la base).",
        " La consola está ahora en este modo.",
        "Experimental: la escena 3D a 1600x900, unas 2,4 veces la carga de GPU de 1024x576. Mejor en un televisor 1080p.",
        "Experimental: la escena 3D a 1920x1080, unas 3,5 veces la carga de GPU de 1024x576. Mejor en un televisor 1080p.",
        "Bilineal a esta resolución.",
        " Solo si la resolución de TV de la consola es 1080p.",
        "Bilineal, solo si la resolución de TV de la consola es 720p (la imagen se reduce).",
        "Arriba / Abajo: elegir \xC2\xB7 Izquierda / Derecha: cambiar \xC2\xB7 B: volver",
        "Resolución dinámica", "Activada",
        "Baja la resolución 3D en el juego (hasta el % indicado) para mantener los fps. Desactivada: nunca.",
    },
    {   // Italiano
        "Impostazioni", "Lingua", "Intervallo fotogrammi", "Risoluzione", "Anti-aliasing",
        "Upscaling", "Avvia il gioco", "Disattivato", "Bilineare", "%d  (%d fps a 60 Hz)",
        "I testi e i menu del gioco.",
        "Aggiornamenti dello schermo per fotogramma: 1 = ogni aggiornamento. I filmati pre-renderizzati sono "
        "sempre a 30 fps.",
        "La risoluzione 3D originale del gioco, con upscaling a quella di uscita.",
        "Sperimentale: la scena 3D a 1280x720, circa il 56% di lavoro GPU in più.",
        "Nessun anti-aliasing.", "L'anti-aliasing originale del gioco.",
        "Non usato a 1280x720.", "Bilineare: il più leggero, più morbido degli altri.",
        "Lanczos quando l'immagine viene ingrandita, bilineare quando viene ridotta.",
        "AMD FSR 1 quando l'immagine viene ingrandita, bilineare quando viene ridotta.",
        "Salva queste impostazioni e avvia il gioco.",
        "Su / Giù: scegli · A: apri · Sinistra / Destra: cambia · +: avvia il gioco\n"
        "Tieni premuto A all'avvio del gioco per riaprire questo menu.",
        "Ripristina impostazioni", "Riporta tutte le impostazioni ai valori predefiniti (la lingua resta invariata).",
        "AMD FSR 3.1: anti-aliasing temporale; scala anche una risoluzione inferiore a quella di uscita.",
        "Lo esegue FSR 3.1 (Anti-aliasing).",
        "Impostazioni modalità portatile", "Impostazioni modalità TV",
        "Grafica usata quando il gioco si avvia in modalità portatile.",
        "Grafica usata quando il gioco si avvia in modalità TV (nella base).",
        " La console è ora in questa modalità.",
        "Sperimentale: la scena 3D a 1600x900, circa 2,4 volte il lavoro GPU di 1024x576. Ideale su un televisore 1080p.",
        "Sperimentale: la scena 3D a 1920x1080, circa 3,5 volte il lavoro GPU di 1024x576. Ideale su un televisore 1080p.",
        "Bilineare a questa risoluzione.",
        " Solo se la risoluzione TV della console è 1080p.",
        "Bilineare, solo se la risoluzione TV della console è 720p (l'immagine viene ridotta).",
        "Su / Giù: scegli \xC2\xB7 Sinistra / Destra: cambia \xC2\xB7 B: indietro",
        "Risoluzione dinamica", "Attivata",
        "Abbassa la risoluzione 3D in gioco (fino alla % indicata) per mantenere il frame rate. Disattivata: mai.",
    },
};

// fnxf_xiii (2026-10-28): the achievement pages' text, in kLanguages' order; a language pack's [port] ach_* keys over
// its base language's.
struct AchStrings {
  const char *row, *note_row, *title, *unlocked, *all, *note_unlocked, *note_all, *none, *no_data, *footer_page,
      *footer_list;
  // fnxf_xiii (2026-10-28): when an unlocked one was unlocked, in the console's time zone: {D} day, {M} month,
  // {Y} year, {h} hour, {m} minute (two digits each, the year four).
  const char *date;
};
constexpr AchStrings kAchStrings[] = {
    {"Achievements", "The achievements unlocked so far, and the whole list.", "Achievements",
     "Unlocked achievements", "All achievements", "The achievements you have unlocked.",
     "Every achievement in the game (the locked ones dimmed).", "No achievements unlocked yet.",
     "Start the game once to see its achievements here.", "Up / Down: choose \xC2\xB7 A: open \xC2\xB7 B: back",
     "Up / Down: scroll \xC2\xB7 B: back", "{M}/{D}/{Y}  {h}:{m}"},
    {"Erfolge", "Die bisher freigeschalteten Erfolge und die vollst\xC3\xA4ndige Liste.", "Erfolge",
     "Freigeschaltete Erfolge", "Alle Erfolge", "Die Erfolge, die du freigeschaltet hast.",
     "Alle Erfolge des Spiels (die gesperrten abgeblendet).", "Noch keine Erfolge freigeschaltet.",
     "Starte das Spiel einmal, um seine Erfolge hier zu sehen.",
     "Oben / Unten: ausw\xC3\xA4hlen \xC2\xB7 A: \xC3\xB6" "ffnen \xC2\xB7 B: zur\xC3\xBC" "ck",
     "Oben / Unten: bl\xC3\xA4ttern \xC2\xB7 B: zur\xC3\xBC" "ck", "{D}.{M}.{Y}  {h}:{m}"},
    {"Succ\xC3\xA8s", "Les succ\xC3\xA8s d\xC3\xA9verrouill\xC3\xA9s jusqu'ici, et la liste compl\xC3\xA8te.",
     "Succ\xC3\xA8s", "Succ\xC3\xA8s d\xC3\xA9verrouill\xC3\xA9s", "Tous les succ\xC3\xA8s",
     "Les succ\xC3\xA8s que vous avez d\xC3\xA9verrouill\xC3\xA9s.",
     "Tous les succ\xC3\xA8s du jeu (ceux \xC3\xA0 d\xC3\xA9verrouiller en gris\xC3\xA9).",
     "Aucun succ\xC3\xA8s d\xC3\xA9verrouill\xC3\xA9 pour l'instant.",
     "Lancez le jeu une fois pour voir ses succ\xC3\xA8s ici.",
     "Haut / Bas : choisir \xC2\xB7 A : ouvrir \xC2\xB7 B : retour", "Haut / Bas : d\xC3\xA9" "filer \xC2\xB7 B : retour",
     "{D}/{M}/{Y}  {h}:{m}"},
    {"Logros", "Los logros desbloqueados hasta ahora y la lista completa.", "Logros", "Logros desbloqueados",
     "Todos los logros", "Los logros que has desbloqueado.", "Todos los logros del juego (los bloqueados, atenuados).",
     "A\xC3\xBAn no has desbloqueado ning\xC3\xBAn logro.", "Inicia el juego una vez para ver aqu\xC3\xAD sus logros.",
     "Arriba / Abajo: elegir \xC2\xB7 A: abrir \xC2\xB7 B: volver", "Arriba / Abajo: desplazar \xC2\xB7 B: volver",
     "{D}/{M}/{Y}  {h}:{m}"},
    {"Obiettivi", "Gli obiettivi sbloccati finora e l'elenco completo.", "Obiettivi", "Obiettivi sbloccati",
     "Tutti gli obiettivi", "Gli obiettivi che hai sbloccato.", "Tutti gli obiettivi del gioco (quelli bloccati in grigio).",
     "Nessun obiettivo ancora sbloccato.", "Avvia il gioco una volta per vedere qui i suoi obiettivi.",
     "Su / Gi\xC3\xB9: scegli \xC2\xB7 A: apri \xC2\xB7 B: indietro", "Su / Gi\xC3\xB9: scorri \xC2\xB7 B: indietro", "{D}/{M}/{Y}  {h}:{m}"},
};
static_assert(sizeof(kStrings) / sizeof(kStrings[0]) == sizeof(kLanguages) / sizeof(kLanguages[0]),
              "a Strings for each language");

// fnxf_xiii (2026-10-25): the Language row's choices: the game's languages, then the language packs.
struct Choice {
  std::string key;   // the settings file's value: kLanguages' key, or the pack's name (its zip's)
  std::string name;  // in its own language
  uint32_t xbox = 1; // user_language (kLanguages' type)
  std::string pack;  // fnxf_xiii_lang ("" for the game's own)
  Strings strings{};
  AchStrings ach{};  // fnxf_xiii (2026-10-28)
  int base = 0;      // kLanguages' index of the game's language under it
};
std::vector<fnxf_xiii::LangPack> g_packs;  // the Strings' text points into these
std::vector<Choice> g_choices;

// A pack's text for a format string: only when it has the same conversions as the base's (a wrong one would crash).
bool SameConversions(const char *a, const char *b) {
  auto count = [](const char *s) {
    int n = 0;
    for (; *s; ++s)
      if (*s == '%')
        n += s[1] == 'd' ? 1 : 100;
    return n;
  };
  return count(a) == count(b);
}

void BuildChoices() {
  g_choices.clear();
  for (int i = 0; i < kLanguageCount; i++)
    g_choices.push_back({kLanguages[i].key, kLanguages[i].name, kLanguages[i].xbox, "", kStrings[i], kAchStrings[i], i});
  g_packs = fnxf_xiii::ListLangPacks();
  for (const fnxf_xiii::LangPack &p : g_packs) {
    int base = 0;
    for (int i = 0; i < kLanguageCount; i++)
      if (p.base == kLanguages[i].key)
        base = i;
    Choice c{p.name, p.display_name, kLanguages[base].xbox, p.name, kStrings[base], kAchStrings[base], base};
    struct Field {
      const char *key;
      const char *Strings::*member;
    };
    static constexpr Field kFields[] = {
        {"menu_title", &Strings::title}, {"menu_language", &Strings::language},
        {"menu_interval", &Strings::interval}, {"menu_resolution", &Strings::resolution},
        {"menu_aa", &Strings::aa}, {"menu_upscaler", &Strings::upscaler}, {"menu_start", &Strings::start},
        {"menu_off", &Strings::off}, {"menu_bilinear", &Strings::bilinear},
        {"menu_interval_value", &Strings::interval_value}, {"menu_note_language", &Strings::note_language},
        {"menu_note_interval", &Strings::note_interval}, {"menu_note_res_576", &Strings::note_res_576},
        {"menu_note_res_720", &Strings::note_res_720}, {"menu_note_aa_off", &Strings::note_aa_off},
        {"menu_note_aa_msaa", &Strings::note_aa_msaa}, {"menu_note_up_720", &Strings::note_up_720},
        {"menu_note_up_bilinear", &Strings::note_up_bilinear}, {"menu_note_up_lanczos", &Strings::note_up_lanczos},
        {"menu_note_up_fsr1", &Strings::note_up_fsr1}, {"menu_note_start", &Strings::note_start},
        {"menu_footer", &Strings::footer}, {"menu_reset", &Strings::reset}, {"menu_note_reset", &Strings::note_reset},
        {"menu_note_aa_fsr3", &Strings::note_aa_fsr3}, {"menu_note_up_fsr3", &Strings::note_up_fsr3},
        {"menu_handheld", &Strings::handheld}, {"menu_docked", &Strings::docked},
        {"menu_note_handheld", &Strings::note_handheld}, {"menu_note_docked", &Strings::note_docked},
        {"menu_current", &Strings::current}, {"menu_note_res_900", &Strings::note_res_900},
        {"menu_note_res_1080", &Strings::note_res_1080}, {"menu_note_up_hi", &Strings::note_up_hi},
        {"menu_note_up_tv", &Strings::note_up_tv}, {"menu_note_up_down_tv", &Strings::note_up_down_tv},
        {"menu_sub_footer", &Strings::sub_footer},
        {"menu_dynres", &Strings::dynres}, {"menu_on", &Strings::on}, {"menu_note_dynres", &Strings::note_dynres},
    };
    for (const Field &f : kFields) {
      auto it = p.port.find(f.key);
      if (it == p.port.end() || it->second.empty())
        continue;
      if (f.member == &Strings::interval_value && !SameConversions(it->second.c_str(), c.strings.interval_value))
        continue;
      c.strings.*f.member = it->second.c_str();
    }
    // fnxf_xiii (2026-10-28): the achievement pages' text.
    struct AchField {
      const char *key;
      const char *AchStrings::*member;
    };
    static constexpr AchField kAchFields[] = {
        {"menu_achievements", &AchStrings::row}, {"menu_note_achievements", &AchStrings::note_row},
        {"ach_title", &AchStrings::title}, {"ach_unlocked", &AchStrings::unlocked}, {"ach_all", &AchStrings::all},
        {"ach_note_unlocked", &AchStrings::note_unlocked}, {"ach_note_all", &AchStrings::note_all},
        {"ach_none", &AchStrings::none}, {"ach_no_data", &AchStrings::no_data},
        {"ach_footer", &AchStrings::footer_page}, {"ach_list_footer", &AchStrings::footer_list},
        {"ach_date", &AchStrings::date},
    };
    for (const AchField &f : kAchFields) {
      auto it = p.port.find(f.key);
      if (it != p.port.end() && !it->second.empty())
        c.ach.*f.member = it->second.c_str();
    }
    g_choices.push_back(c);
  }
}

std::string SettingsPath() {
  return fnxf_xiii::renut_gpu::NextToExecutable("fnxf_xiii_settings.ini").string();
}

std::string Trim(const std::string &s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t'))
    a++;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n'))
    b--;
  return s.substr(a, b - a);
}

int IndexOf(const char *const *keys, int count, const std::string &v, int fallback) {
  for (int i = 0; i < count; i++)
    if (v == keys[i])
      return i;
  return fallback;
}

// False when the file does not exist.
bool Load(Settings &s) {
  FILE *f = std::fopen(SettingsPath().c_str(), "r");
  if (!f)
    return false;
  char line[256];
  while (std::fgets(line, sizeof(line), f)) {
    const std::string l = Trim(line);
    if (l.empty() || l[0] == '#' || l[0] == ';')
      continue;
    const size_t eq = l.find('=');
    if (eq == std::string::npos)
      continue;
    std::string key = Trim(l.substr(0, eq));
    const std::string value = Trim(l.substr(eq + 1));
    if (key == "language") {
      for (int i = 0; i < int(g_choices.size()); i++)
        if (value == g_choices[i].key)
          s.language = i;
      continue;
    }
    // fnxf_xiii (2026-11-07): "handheld_aa", "docked_aa", ...; a key without the prefix (the files before the two
    // pages) sets both modes.
    int first = kHandheld, last = kDocked;
    for (int m = 0; m < 2; m++) {
      const std::string prefix = std::string(kModeKeys[m]) + "_";
      if (key.compare(0, prefix.size(), prefix) == 0) {
        key = key.substr(prefix.size());
        first = last = m;
      }
    }
    for (int m = first; m <= last; m++) {
      Profile &p = s.mode[m];
      if (key == "interval") {
        p.interval = std::clamp(std::atoi(value.c_str()), 1, 4);
      } else if (key == "resolution") {
        p.resolution = IndexOf(kResolutions, kResolutionCount, value, 0);
      } else if (key == "aa") {
        p.aa = value == "smaa" ? 1 : IndexOf(kAaKeys, kAaCount, value, p.aa);  // "smaa": the removed option (MSAA 2x)
      } else if (key == "upscaler") {
        p.upscaler = value == "fsr1" ? 2 : 1;  // fnxf_xiii (2026-11-08): "bilinear" and "off" (older files) are Lanczos
      } else if (key == "dynres") {
        p.dynres = DynresFromText(value);  // fnxf_xiii (2026-11-07; a percent since 2026-11-08)
      }
    }
  }
  std::fclose(f);
  return true;
}

void Save(const Settings &s) {
  const std::string path = SettingsPath();
  const std::string tmp = path + ".tmp";
  FILE *f = std::fopen(tmp.c_str(), "w");
  if (!f) {
    REXLOG_WARN("[menu] could not write {}", path);
    return;
  }
  std::fprintf(f,
               "# fnxf_xiii boot menu settings. Hold A while the game starts to open the\n"
               "# menu again; delete this file to see it at the next start.\n"
               "language = %s\n",
               g_choices[s.language].key.c_str());
  for (int m = 0; m < 2; m++) {
    const Profile &p = s.mode[m];
    const char *k = kModeKeys[m];
    const std::string dynres = p.dynres ? std::to_string(p.dynres) : std::string("off");
    std::fprintf(f, "%s_interval = %d\n%s_resolution = %s\n%s_aa = %s\n%s_upscaler = %s\n%s_dynres = %s\n", k,
                 p.interval, k, kResolutions[p.resolution], k, kAaKeys[p.aa], k, kUpscalerKeys[p.upscaler], k,
                 dynres.c_str());
  }
  std::fclose(f);
  std::remove(path.c_str());
  if (std::rename(tmp.c_str(), path.c_str()) != 0)
    REXLOG_WARN("[menu] could not write {}", path);
}

// fnxf_xiii (2026-11-07): whether the Upscaler row means something: not with FSR 3.1 (it upscales itself); at 1024x576
// always; at 1280x720 and 1600x900 docked only, where a 1080p TV gets a 1920x1080 output the picture is upscaled to
// (1280x720: by the scan-out, backend/present.cpp; 1600x900: by the game's upscale draw); at 1920x1080 never.
// fnxf_xiii (2026-11-08): and with dynamic resolution at every resolution (the lowered picture is enlarged).
bool UpscalerUsable(const Profile &p, int mode) {
  return p.aa != 4 &&
         (p.resolution == 0 || p.dynres != 0 || (mode == 1 && (p.resolution == 1 || p.resolution == 2)));
}

// fnxf_xiii (2026-11-07): the console's mode as the game should take it: Reverse-NX's when its overlay rules, else the
// console's.
bool DockedNow() {
  if (REXCVAR_GET(fnxf_xiii_diag_output) == 2)
    return true;  // the test of a docked 1080p output in hand: the Docked page's settings too
  rex::ui::switch_saltynx::Start();  // finds Reverse-NX's block (nothing without SaltyNX; again later is harmless)
  return rex::ui::switch_saltynx::BaseMode(appletGetOperationMode() == AppletOperationMode_Console);
}

void Apply(const Settings &settings) {
  const bool docked = DockedNow();
  const Profile &s = settings.mode[docked ? kDocked : kHandheld];
  const Choice &language = g_choices[settings.language];
  REXCVAR_SET(user_language, language.xbox);
  REXCVAR_SET(fnxf_xiii_lang, language.pack);  // fnxf_xiii (2026-10-25): a language pack, or none
  REXCVAR_SET(fnxf_xiii_frame_interval, REXCVAR_GET(fnxf_xiii_diag_game_frame_lock) ? 0 : int32_t(s.interval));
  REXCVAR_SET(fnxf_xiii_render_720p, false);
  REXCVAR_SET(fnxf_xiii_render_height, kResolutionHeights[s.resolution]);  // fnxf_xiii (2026-11-07): up to 1920x1080
  REXCVAR_SET(fnxf_xiii_diag_msaa_off, s.aa == 0 || s.aa == kAaFsr3);  // FSR 3.1 reads single-sampled targets
  REXCVAR_SET(fnxf_xiii_fsr3, int32_t(s.aa == kAaFsr3));
  REXCVAR_SET(fnxf_xiii_msaa_samples, kAaSamples[s.aa]);
  // The Upscaler where it means something (UpscalerUsable), else bilinear (a 1:1 or downscaling draw: render/draw.cpp
  // uses the bilinear shader for those anyway).
  const int32_t filter = !UpscalerUsable(s, docked ? kDocked : kHandheld) ? 4
                         : s.upscaler == 2 ? 3 : 1;
  REXCVAR_SET(fnxf_xiii_renut_upscale_filter, filter);
  REXCVAR_SET(fnxf_xiii_drs, int32_t(s.dynres != 0));  // fnxf_xiii (2026-11-07): render/dynamic_resolution.cpp
  if (s.dynres)
    REXCVAR_SET(fnxf_xiii_drs_min, int32_t(s.dynres));  // fnxf_xiii (2026-11-08): the lowest height the player chose
  REXLOG_INFO("[menu] settings ({} mode): language {} (Xbox language {}), frame interval {}, resolution {}, "
              "anti-aliasing {}, upscaler {} (fnxf_xiii_renut_upscale_filter {}), dynamic resolution {}",
              docked ? "docked" : "handheld", language.key, language.xbox, s.interval, kResolutions[s.resolution],
              kAa[s.aa],
              s.aa == kAaFsr3 ? "FSR 3.1" : !UpscalerUsable(s, docked ? kDocked : kHandheld) ? "bilinear" : kUpscalers[s.upscaler],
              filter, s.dynres ? fmt::format("down to {}%", s.dynres) : std::string("off"));
}

// fnxf_xiii (2026-10-28): the achievement lists' data (see the top of the file).
struct AchEntry {
  uint32_t id = 0, gamerscore = 0;
  std::map<std::string, std::string> text;  // "label_english", "description_german", "locked_french", ...
};
struct AchData {
  bool loaded = false;
  std::vector<AchEntry> list;  // by id
  std::map<uint32_t, uint64_t> unlocked;  // id -> when (a Windows FILETIME, UTC: the runtime's unlock file)
  std::map<std::string, std::map<uint32_t, std::map<std::string, std::string>>> packs;  // pack -> id -> label, ...
};
std::filesystem::path g_user_root;
AchData g_ach;

std::string ReadText(const std::filesystem::path &path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream text;
  if (in)
    text << in.rdbuf();
  return text.str();
}

void LoadAchievements() {
  if (g_ach.loaded)
    return;
  g_ach.loaded = true;
  if (g_user_root.empty())
    return;
  const std::filesystem::path dir = g_user_root / "achievements";
  std::string title_id;
  try {
    const std::string text = ReadText(dir / "names.toml");
    if (text.empty())
      return;
    const toml::table table = toml::parse(text);
    title_id = table["title_id"].value_or<std::string>("");
    if (const toml::array *list = table["achievements"].as_array()) {
      for (const toml::node &node : *list) {
        const toml::table *t = node.as_table();
        if (!t)
          continue;
        AchEntry e;
        e.id = uint32_t((*t)["id"].value_or<int64_t>(0));
        e.gamerscore = uint32_t((*t)["gamerscore"].value_or<int64_t>(0));
        for (const auto &[key, value] : *t)
          if (const auto *str = value.as_string())
            e.text[std::string(key.str())] = str->get();
        if (e.id)
          g_ach.list.push_back(std::move(e));
      }
    }
  } catch (const toml::parse_error &e) {
    REXLOG_WARN("[menu] {}: {}", (dir / "names.toml").string(), e.what());
  }
  std::sort(g_ach.list.begin(), g_ach.list.end(), [](const AchEntry &a, const AchEntry &b) { return a.id < b.id; });
  if (title_id.empty())
    return;
  try {
    const std::string text = ReadText(dir / (title_id + ".toml"));
    if (!text.empty()) {
      const toml::table table = toml::parse(text);
      if (const toml::table *unlocked = table["unlocked"].as_table())
        for (const auto &[key, value] : *unlocked) {
          const toml::table *t = value.as_table();
          const int64_t when = t ? (*t)["filetime"].value_or<int64_t>(0) : 0;
          g_ach.unlocked[uint32_t(std::strtoul(std::string(key.str()).c_str(), nullptr, 10))] = uint64_t(when);
        }
    }
  } catch (const toml::parse_error &e) {
    REXLOG_WARN("[menu] the unlocked achievements: {}", e.what());
  }
  REXLOG_INFO("[menu] {} achievements, {} unlocked", g_ach.list.size(), g_ach.unlocked.size());
}

// A language pack's achievements.toml (label, description, unachieved_description by id), read once a pack.
const std::map<uint32_t, std::map<std::string, std::string>> &PackAchievements(const std::string &pack) {
  auto [it, added] = g_ach.packs.try_emplace(pack);
  if (!added || pack.empty())
    return it->second;
  const std::vector<uint8_t> file = fnxf_xiii::ReadPackFile(pack, "achievements.toml");
  if (file.empty())
    return it->second;
  try {
    const toml::table table = toml::parse(std::string_view(reinterpret_cast<const char *>(file.data()), file.size()));
    if (const toml::array *list = table["achievements"].as_array()) {
      for (const toml::node &node : *list) {
        const toml::table *t = node.as_table();
        if (!t)
          continue;
        const uint32_t id = uint32_t((*t)["id"].value_or<int64_t>(0));
        for (const char *key : {"label", "description", "unachieved_description"}) {
          std::string v = (*t)[key].value_or<std::string>("");
          if (id && !v.empty())
            it->second[id][key] = std::move(v);
        }
      }
    }
  } catch (const toml::parse_error &e) {
    REXLOG_WARN("[menu] {}'s achievements.toml: {}", pack, e.what());
  }
  return it->second;
}

// An achievement's text in the menu's language: the pack's, else its base language's, else English.
std::string AchText(const AchEntry &e, const Choice &c, const char *field, const char *pack_field) {
  if (!c.pack.empty()) {
    const auto &pack = PackAchievements(c.pack);
    auto a = pack.find(e.id);
    if (a != pack.end()) {
      auto v = a->second.find(pack_field);
      if (v != a->second.end())
        return v->second;
    }
  }
  for (const char *lang : {kLanguages[c.base].key, "english"}) {
    auto v = e.text.find(std::string(field) + "_" + lang);
    if (v != e.text.end() && !v->second.empty())
      return v->second;
  }
  return {};
}

// fnxf_xiii (2026-10-28): a FILETIME (100 ns since 1601, UTC) as the language's date and time in the console's time
// zone (its location setting); "" for none.
std::string UnlockDate(uint64_t filetime, const char *format) {
  constexpr uint64_t kEpochDelta = 116'444'736'000'000'000ULL;  // 1601 to 1970, in 100 ns
  if (filetime <= kEpochDelta || !format)
    return {};
  const uint64_t posix = (filetime - kEpochDelta) / 10'000'000ULL;
  int Y, M, D, h, m;
  TimeCalendarTime cal{};
  TimeCalendarAdditionalInfo info{};
  if (R_SUCCEEDED(timeToCalendarTimeWithMyRule(posix, &cal, &info))) {
    Y = cal.year, M = cal.month, D = cal.day, h = cal.hour, m = cal.minute;
  } else {
    const time_t t = time_t(posix);
    struct tm tm {};
    gmtime_r(&t, &tm);
    Y = tm.tm_year + 1900, M = tm.tm_mon + 1, D = tm.tm_mday, h = tm.tm_hour, m = tm.tm_min;
  }
  char buf[8];
  std::string out;
  for (const char *p = format; *p; ++p) {
    if (p[0] == '{' && p[1] && p[2] == '}') {
      const char k = p[1];
      if (k == 'Y')
        std::snprintf(buf, sizeof(buf), "%04d", Y);
      else
        std::snprintf(buf, sizeof(buf), "%02d", k == 'M' ? M : k == 'D' ? D : k == 'h' ? h : k == 'm' ? m : 0);
      out += buf;
      p += 2;
      continue;
    }
    out += *p;
  }
  return out;
}

std::string IntervalText(const Strings &t, int n) {
  char text[96];
  std::snprintf(text, sizeof(text), t.interval_value, n, 60 / n);
  return text;
}

enum Row { kRowLanguage, kRowHandheld, kRowDocked, kRowAchievements, kRowReset, kRowStart, kRowCount };
// fnxf_xiii (2026-11-07): the rows of a mode's settings page.
enum SubRow { kSubInterval, kSubResolution, kSubAa, kSubUpscaler, kSubDynres };
int SubRowCount(int) { return 5; }

// Returns false when the software was closed from HOME.
bool ShowMenu(Settings &s, PadState &pad) {
  PlFontData font{};
  const bool pl = R_SUCCEEDED(plInitialize(PlServiceType_User));
  if (pl)
    plGetSharedFontByType(&font, PlSharedFontType_Standard);
  bool keep_going = true;
  {
    PrepassScreen screen;
    // fnxf_xiii (2026-11-07): 1920x1080 docked on a 1080p TV, switched as the console is docked or not (below).
    const char *const kArt = "sdmc:/switch/fnxf_xiii/assets/nxeart";
    bool hd = ScreenWants1080p();
    screen.Init(kArt, static_cast<const uint8_t *>(font.address), font.size, hd ? 1.5f : 1.0f);
    ScreenFramebuffer sfb;
    bool have_fb = sfb.Open(hd);
    Framebuffer &fb = sfb.fb;
    int selected = kRowLanguage;  // the first row (not "Start the game": A held at start opens the menu)
    // fnxf_xiii (2026-11-07): a mode's settings page open (kHandheld / kDocked), -1 none; its selected row.
    int sub = -1, sub_selected = 0;
    int docked_now = DockedNow() ? kDocked : kHandheld;
    // fnxf_xiii (2026-10-28): 0 the settings, 1 the achievements page (its two lists), 2 a list (list_all: every one).
    int level = 0, page_selected = 0, list_selected = 0;
    bool list_all = false;
    bool dirty = true;
    int drawn = 0;  // both buffers drawn after a change
    u64 held_since = 0, last_repeat = 0;
    u64 held_buttons = 0;
    const u64 freq = armGetSystemTickFreq();
    for (;;) {
      if (!appletMainLoop()) {
        keep_going = false;
        break;
      }
      // fnxf_xiii (2026-11-07): docked or not now (the pages' notes), and the screen's size with it.
      {
        const int mode_now = rex::ui::switch_saltynx::BaseMode(appletGetOperationMode() == AppletOperationMode_Console)
                                 ? kDocked
                                 : kHandheld;
        if (mode_now != docked_now) {
          docked_now = mode_now;
          dirty = true;
        }
        const bool want = ScreenWants1080p();
        if (want != hd) {
          hd = want;
          sfb.Close();
          screen.Init(kArt, static_cast<const uint8_t *>(font.address), font.size, hd ? 1.5f : 1.0f);
          have_fb = sfb.Open(hd);
          dirty = true;
        }
      }
      padUpdate(&pad);
      u64 down = padGetButtonsDown(&pad);
      const u64 buttons = padGetButtons(&pad);
      // Held direction: repeats after 0.4 s, 8 a second.
      constexpr u64 kDirs = HidNpadButton_AnyUp | HidNpadButton_AnyDown | HidNpadButton_AnyLeft | HidNpadButton_AnyRight;
      const u64 now = armGetSystemTick();
      if ((buttons & kDirs) != held_buttons) {
        held_buttons = buttons & kDirs;
        held_since = last_repeat = now;
      } else if (held_buttons && now - held_since > freq * 4 / 10 && now - last_repeat > freq / 8) {
        last_repeat = now;
        down |= held_buttons;
      }
      // fnxf_xiii (2026-11-07): a mode's settings page: Up / Down, Left / Right, B back (+ starts the game).
      if (sub >= 0) {
        if (down & HidNpadButton_Plus)
          break;
        Profile &p = s.mode[sub];
        const int n = SubRowCount(sub);
        if (down & HidNpadButton_AnyUp)
          sub_selected = (sub_selected + n - 1) % n;
        if (down & HidNpadButton_AnyDown)
          sub_selected = (sub_selected + 1) % n;
        const int step = (down & HidNpadButton_AnyRight) ? 1 : (down & HidNpadButton_AnyLeft) ? -1 : 0;
        if (step) {
          switch (sub_selected) {
          case kSubInterval: p.interval = (p.interval - 1 + 4 + step) % 4 + 1; break;
          case kSubResolution: p.resolution = (std::min(p.resolution, kResolutionMenuCount - 1) + kResolutionMenuCount + step) %
                                             kResolutionMenuCount; break;
          case kSubAa: p.aa = (p.aa + kAaCount + step) % kAaCount; break;
          case kSubUpscaler:
            if (UpscalerUsable(p, sub))
              p.upscaler = p.upscaler == 2 ? 1 : 2;  // fnxf_xiii (2026-11-08): Lanczos or FSR 1
            break;
          case kSubDynres:  // fnxf_xiii (2026-11-07; the lowest height since 2026-11-08)
            p.dynres = kDynresChoices[(DynresIndex(p.dynres) + kDynresCount + step) % kDynresCount];
            break;
          default: break;
          }
        }
        if (down & HidNpadButton_B)
          sub = -1;
        if (down & (HidNpadButton_B | kDirs))
          dirty = true;
      } else
      // fnxf_xiii (2026-10-28): the achievement pages: B back, Up / Down through the page's rows or the list.
      if (level > 0) {
        const int vstep = (down & HidNpadButton_AnyDown) ? 1 : (down & HidNpadButton_AnyUp) ? -1 : 0;
        if (level == 1) {
          if (vstep)
            page_selected ^= 1;
          if (down & HidNpadButton_A) {
            level = 2;
            list_all = page_selected == 1;
            list_selected = 0;
          }
        } else if (vstep) {
          int n = 0;
          for (const AchEntry &e : g_ach.list)
            n += list_all || g_ach.unlocked.count(e.id) ? 1 : 0;
          if (n)
            list_selected = (list_selected + n + vstep) % n;
        }
        if (down & HidNpadButton_B)
          level--;
        if (down & (HidNpadButton_A | HidNpadButton_B) || vstep)
          dirty = true;
      } else {
      if (down & HidNpadButton_AnyUp) {
        selected = (selected + kRowCount - 1) % kRowCount;
        dirty = true;
      }
      if (down & HidNpadButton_AnyDown) {
        selected = (selected + 1) % kRowCount;
        dirty = true;
      }
      // A starts the game only on "Start the game" (on a setting it does nothing). + starts from anywhere.
      if ((down & HidNpadButton_Plus) || ((down & HidNpadButton_A) && selected == kRowStart))
        break;
      // fnxf_xiii (2026-11-07): a mode's settings page.
      if ((down & HidNpadButton_A) && (selected == kRowHandheld || selected == kRowDocked)) {
        sub = selected == kRowHandheld ? kHandheld : kDocked;
        sub_selected = 0;
        dirty = true;
      }
      // fnxf_xiii (2026-10-28): the achievements page.
      if ((down & HidNpadButton_A) && selected == kRowAchievements) {
        LoadAchievements();
        level = 1;
        page_selected = 0;
        dirty = true;
      }
      // fnxf_xiii (2026-10-25): "Reset settings": every setting back to its default; the language stays (the menu
      // would otherwise switch to English under the player).
      if ((down & HidNpadButton_A) && selected == kRowReset) {
        const int language = s.language;
        s = Settings{};
        s.language = language;
        dirty = true;
      }
      const int step = (down & HidNpadButton_AnyRight) ? 1 : (down & HidNpadButton_AnyLeft) ? -1 : 0;
      if (step) {
        switch (selected) {
        case kRowLanguage: {
          const int n = int(g_choices.size());
          s.language = (s.language + n + step) % n;
          break;
        }
        default: break;
        }
        dirty = true;
      }
      }  // level 0
      if (dirty) {
        dirty = false;
        drawn = 0;
      }
      if (have_fb && drawn < 2 && sub >= 0) {
        // fnxf_xiii (2026-11-07): a mode's settings page.
        const Strings &t = g_choices[s.language].strings;
        const Profile &p = s.mode[sub];
        MenuText m;
        m.title = sub == kDocked ? t.docked : t.handheld;
        m.rows.push_back({t.interval, IntervalText(t, p.interval), true});
        m.rows.push_back({t.resolution, kResolutions[p.resolution], true});
        m.rows.push_back({t.aa, p.aa == 0 ? t.off : kAa[p.aa], true});
        const char *upscaler = kUpscalers[p.upscaler == 2 ? 2 : 1];
        const bool usable = UpscalerUsable(p, sub);
        m.rows.push_back({t.upscaler,
                          p.aa == kAaFsr3 ? kAa[kAaFsr3]
                          : usable        ? upscaler
                          : p.resolution == 1 ? t.off
                                              : t.bilinear,
                          usable});
        // fnxf_xiii (2026-11-07): off, or the range from its lowest height (2026-11-08).
        m.rows.push_back({t.dynres, p.dynres ? std::to_string(p.dynres) + "% \xE2\x80\x93 100%" : std::string(t.off), true});
        m.selected = sub_selected;
        switch (sub_selected) {
        case kSubInterval: m.note = t.note_interval; break;
        case kSubResolution:
          m.note = p.resolution == 3 ? t.note_res_1080 : p.resolution == 2 ? t.note_res_900
                   : p.resolution ? t.note_res_720 : t.note_res_576;
          break;
        case kSubAa: m.note = p.aa == 0 ? t.note_aa_off : p.aa == kAaFsr3 ? t.note_aa_fsr3 : t.note_aa_msaa; break;
        case kSubUpscaler:
          if (p.aa == kAaFsr3)
            m.note = t.note_up_fsr3;
          else if (!usable)
            m.note = p.resolution == 1 ? t.note_up_720 : sub == kDocked ? t.note_up_down_tv : t.note_up_hi;
          else
            m.note = std::string(p.upscaler == 2 ? t.note_up_fsr1 : t.note_up_lanczos) +
                     (p.resolution != 0 && !p.dynres ? t.note_up_tv : "");
          break;
        case kSubDynres: m.note = t.note_dynres; break;
        default: break;
        }
        m.footer = t.sub_footer;
        u32 stride = 0;
        auto *pixels = static_cast<uint32_t *>(framebufferBegin(&fb, &stride));
        screen.DrawMenu(pixels, stride / 4, m);
        framebufferEnd(&fb);
        drawn++;
      } else if (have_fb && drawn < 2 && level > 0) {
        const Choice &c = g_choices[s.language];
        const AchStrings &a = c.ach;
        MenuText m;
        m.plain = true;
        m.arrows = false;
        uint32_t points = 0, all_points = 0;
        for (const AchEntry &e : g_ach.list) {
          all_points += e.gamerscore;
          if (g_ach.unlocked.count(e.id))
            points += e.gamerscore;
        }
        const std::string count = std::to_string(g_ach.unlocked.size()) + " / " + std::to_string(g_ach.list.size()) +
                                  "  (" + std::to_string(points) + " / " + std::to_string(all_points) + "G)";
        if (level == 1) {
          m.title = a.title;
          m.rows.push_back({a.unlocked, g_ach.list.empty() ? "" : count, true});
          m.rows.push_back({a.all, g_ach.list.empty() ? "" : std::to_string(g_ach.list.size()), true});
          m.selected = page_selected;
          m.note = g_ach.list.empty() ? a.no_data : page_selected ? a.note_all : a.note_unlocked;
          m.footer = a.footer_page;
        } else {
          m.title = std::string(list_all ? a.all : a.unlocked) + (g_ach.list.empty() ? "" : "  \xC2\xB7  " + count);
          std::vector<const AchEntry *> shown;
          for (const AchEntry &e : g_ach.list)
            if (list_all || g_ach.unlocked.count(e.id))
              shown.push_back(&e);
          // 15 rows at a time, the selected one kept in view.
          constexpr int kVisible = 15;
          const int n = int(shown.size());
          const int first = std::clamp(list_selected - kVisible / 2, 0, std::max(n - kVisible, 0));
          for (int i = first; i < std::min(n, first + kVisible); i++) {
            const AchEntry &e = *shown[i];
            auto when = g_ach.unlocked.find(e.id);
            const bool got = when != g_ach.unlocked.end();
            std::string value = std::to_string(e.gamerscore) + "G";
            const std::string date = got ? UnlockDate(when->second, a.date) : std::string();
            if (!date.empty())
              value += "     " + date;  // fnxf_xiii (2026-10-28): when it was unlocked
            m.rows.push_back({AchText(e, c, "label", "label"), value, got});
          }
          m.selected = list_selected - first;
          if (n == 0) {
            m.note = g_ach.list.empty() ? a.no_data : a.none;
          } else {
            const AchEntry &e = *shown[list_selected];
            std::string note = g_ach.unlocked.count(e.id) ? "" : AchText(e, c, "locked", "unachieved_description");
            m.note = note.empty() ? AchText(e, c, "description", "description") : note;
          }
          m.footer = a.footer_list;
        }
        u32 stride = 0;
        auto *pixels = static_cast<uint32_t *>(framebufferBegin(&fb, &stride));
        screen.DrawMenu(pixels, stride / 4, m);
        framebufferEnd(&fb);
        drawn++;
      } else if (have_fb && drawn < 2) {
        const Strings &t = g_choices[s.language].strings;
        MenuText m;
        m.title = t.title;
        m.rows.push_back({t.language, g_choices[s.language].name, true});
        m.rows.push_back({t.handheld, "", true});  // fnxf_xiii (2026-11-07)
        m.rows.push_back({t.docked, "", true});
        m.rows.push_back({g_choices[s.language].ach.row, "", true});  // fnxf_xiii (2026-10-28)
        m.rows.push_back({t.reset, "", true});
        m.rows.push_back({t.start, "", true});
        m.selected = selected;
        switch (selected) {
        case kRowLanguage: m.note = t.note_language; break;
        case kRowHandheld:
          m.note = std::string(t.note_handheld) + (docked_now == kHandheld ? t.current : "");
          break;
        case kRowDocked:
          m.note = std::string(t.note_docked) + (docked_now == kDocked ? t.current : "");
          break;
        case kRowAchievements: m.note = g_choices[s.language].ach.note_row; break;
        case kRowReset: m.note = t.note_reset; break;
        default: m.note = t.note_start; break;
        }
        m.footer = t.footer;
        u32 stride = 0;
        auto *pixels = static_cast<uint32_t *>(framebufferBegin(&fb, &stride));
        screen.DrawMenu(pixels, stride / 4, m);
        framebufferEnd(&fb);
        drawn++;
      } else {
        svcSleepThread(16'000'000);
      }
    }
    sfb.Close();
  }
  if (pl)
    plExit();
  return keep_going;
}

}  // namespace

bool ScreenWants1080p() {
  const int32_t mode = REXCVAR_GET(fnxf_xiii_diag_output);
  if (mode == 1)
    return false;
  if (mode == 2)
    return true;
  static bool asked = false, last = false;
  static u64 asked_at = 0;
  const u64 now = armGetSystemTick();
  if (asked && now - asked_at < armGetSystemTickFreq() / 2)
    return last;
  asked = true;
  asked_at = now;
  const bool real_docked = appletGetOperationMode() == AppletOperationMode_Console;
  const bool docked = rex::ui::switch_saltynx::BaseMode(real_docked);
  bool v = false;
  if (docked) {
    // As the game's output (graphics_options.cpp DisplayIs1080p): the display resolution when really docked;
    // Reverse-NX's "docked" in hand: 1920x1080.
    s32 w = 0, h = 0;
    v = !real_docked || R_FAILED(appletGetDefaultDisplayResolution(&w, &h)) || w <= 0 || w >= 1920;
  }
  if (v != last)
    REXLOG_INFO("[menu] screen {}", v ? "1920x1080" : "1280x720");
  last = v;
  return v;
}

// fnxf_xiii (2026-11-08): the size the status monitor shows (its RES row), which starts at 1280x720 until the renderer
// presents: the menu's own framebuffer while it is up.
extern "C" void RexSwitchPerfResolution(unsigned w, unsigned h);  // sdk/src/ui/switch_perf.cpp

bool ScreenFramebuffer::Open(bool want_1080p) {
  Close();
  NWindow *win = nwindowGetDefault();
  const u32 w = want_1080p ? 1920 : 1280, h = want_1080p ? 1080 : 720;
  nwindowSetDimensions(win, w, h);
  open = R_SUCCEEDED(framebufferCreate(&fb, win, w, h, PIXEL_FORMAT_RGBA_8888, 2));
  if (open && R_FAILED(framebufferMakeLinear(&fb))) {
    framebufferClose(&fb);
    open = false;
  }
  hd = open && want_1080p;
  if (open)
    RexSwitchPerfResolution(w, h);
  return open;
}

void ScreenFramebuffer::Close() {
  if (open)
    framebufferClose(&fb);
  open = false;
  fb = Framebuffer{};
  nwindowSetDimensions(nwindowGetDefault(), 1280, 720);  // as the window was made (display_sync.cpp)
}

// fnxf_xiii (2026-11-01): the Language row's choice for the console's own language, for a first start (no settings file):
// a language pack named by the language's code ("ru" for lang/ru.zip), else the game's language of that code, else
// English. The console's languages without either (Japanese, Chinese, Korean, Dutch, Portuguese, Russian with no pack)
// fall back to English.
static int SystemLanguageChoice() {
  u64 code = 0;
  if (R_FAILED(setInitialize()))
    return 0;
  const bool ok = R_SUCCEEDED(setGetSystemLanguage(&code));
  setExit();
  if (!ok)
    return 0;
  char tag[9] = {};
  std::memcpy(tag, &code, 8);  // "en-US", "fr", "es-419", ...
  std::string lang(tag);
  lang = lang.substr(0, lang.find('-'));
  // The code as it is, not libnx's SetLanguage list: a language a later system adds ("pl") finds its pack too.
  auto lower = [](std::string v) {
    for (char &ch : v)
      ch = char(std::tolower(static_cast<unsigned char>(ch)));
    return v;
  };
  lang = lower(lang);
  static constexpr struct { const char *code, *key; } kCodes[] = {
      {"en", "english"}, {"de", "german"}, {"fr", "french"}, {"es", "spanish"}, {"it", "italian"}};
  int found = 0;
  for (int i = 0; i < int(g_choices.size()); i++)
    if (!g_choices[i].pack.empty() && lower(g_choices[i].pack) == lang)
      found = i;
  if (!found)
    for (const auto &c : kCodes)
      if (lang == c.code)
        for (int i = 0; i < int(g_choices.size()); i++)
          if (g_choices[i].pack.empty() && g_choices[i].key == c.key)
            found = i;
  REXLOG_INFO("[menu] console language {}: {}", tag, g_choices[found].key);
  return found;
}

void RunBootMenu(const std::filesystem::path &user_data_root) {
  g_user_root = user_data_root;  // fnxf_xiii (2026-10-28): the achievement lists
  BuildChoices();  // fnxf_xiii (2026-10-25): with the language packs
  Settings s;
  const bool have_file = Load(s);
  if (!have_file)
    s.language = SystemLanguageChoice();  // fnxf_xiii (2026-11-01)
  // A held at start: watched for 0.5 s (single player: one controller).
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  PadState pad;
  padInitializeDefault(&pad);
  bool held = false;
  const u64 start = armGetSystemTick(), window = armGetSystemTickFreq() / 2;
  while (!held && armGetSystemTick() - start < window) {
    padUpdate(&pad);
    held = padGetButtons(&pad) & HidNpadButton_A;
    if (!held)
      svcSleepThread(10'000'000);
  }
  REXLOG_INFO("[menu] A {} at start", held ? "held" : "not held");
  if (!have_file || held || REXCVAR_GET(fnxf_xiii_diag_boot_menu)) {
    REXLOG_INFO("[menu] {}", have_file ? "opened (A held at start)" : "first start: no settings file");
    if (!ShowMenu(s, pad)) {
      REXLOG_INFO("[menu] closed from HOME");
      ExitBeforeGame();
    }
    Save(s);
  }
  Apply(s);
}

}  // namespace renut::nxs
