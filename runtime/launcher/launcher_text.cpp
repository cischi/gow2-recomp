#include "launcher/launcher_text.h"

namespace launcher {
namespace {

constexpr UiText kItalian{
    "Schermo", "Modalit\xC3\xA0", "Finestra", "Senza bordi", "Schermo intero", "Risoluzione", "Predefinita",
    "Quella del monitor", "Ridimensionabile", "Adattamento", "Bande nere", "Stira", "V-Sync",
    "Immagine", "Upscaler", "Nessuno", "gow2_dlss.dll assente: build senza l'SDK NVIDIA", "Anti-aliasing",
    "FXAA estremo", "L'upscaler sostituisce FXAA", "Nitidezza", "Nitidezza (RCAS di FSR)", "DLSS non ha la nitidezza",
    "Risoluzione interna", "Shader migliorati",
    "Filtro anisotropico", "Del gioco", "Nitidezza delle texture viste di taglio (pavimenti); vale subito",
    "Gioco", "Lingua",
    "Predefiniti", "Gioca", "Esci", "Applica", "Chiudi",
    "Si parte dai valori predefiniti: premendo Gioca il file verr\xC3\xA0 riscritto.",
    "Premendo Gioca il file viene riscritto: i commenti non vengono conservati.",
    "Valgono dal prossimo avvio: ", "Salvataggio non riuscito: ",
    "Upscaler", "Risoluzione interna", "Shader migliorati", "Lingua", "Frame rate",
    "Qualità ombre", "Ombre dinamiche",
    "Frame rate", "30 (originale)", "Frequenza del monitor", "Illimitato",
    "Limite dei fotogrammi del gioco. 30 \xC3\xA8 l'originale. Sopra la frequenza del monitor serve V-Sync spento. "
    "Vale dal prossimo avvio. Patch Unlock FPS: illusion, boma.",
    "VRR (G-Sync/FreeSync)", "Automatico", "S\xC3\xAC", "No",
    "Con G-Sync o FreeSync attivi, limita i fotogrammi appena sotto la frequenza del monitor (es. 157 a 165 Hz). "
    "Automatico: rilevato con NVAPI sulle GPU NVIDIA; con FreeSync scegliere S\xC3\xAC.",
    "Contatore di fps", "Mostra il contatore di fps del gioco (patch di Sowa_95)",
    "Senza motion blur", "Toglie la sfocatura di movimento (patch di illusion)",
    "Senza lens flare", "Toglie i riflessi dell'obiettivo contro il sole (patch di boma)",
    "Senza bloom", "Toglie l'alone luminoso delle zone chiare (patch di boma)",
    "Qualit\xC3\xA0 ombre", "Bassa", "Originale", "Alta",
    "Risoluzione delle ombre dinamiche: 256, 800 (originale) o 2048. Alta rende i bordi delle ombre un po' pi\xC3\xB9 "
    "netti. Vale dal prossimo avvio. Patch del controllo di Coalesced: Sowa_95, boma.",
    "Ombre dinamiche",
    "Ombre dei personaggi e degli oggetti in movimento. Spente, spariscono. Vale dal prossimo avvio. "
    "Patch del controllo di Coalesced: Sowa_95, boma.",
    "Frame generation",
    "AMD FSR 3: un frame generato fra due reali, circa il doppio dei frame a schermo. Frame rate conta i frame reali; "
    "con la frame generation restano sotto met\xC3\xA0 della frequenza del monitor. Aggiunge circa un frame di latenza. "
    "Lo schermo intero diventa borderless. Accesa vale dal prossimo avvio; spenta vale subito.",
    "Non disponibile: manca amd_fidelityfx_framegeneration_dx12.dll accanto a gow2.exe",
    "Frame generation",
    "Spenta",
    "NVIDIA DLSS: un frame generato fra due reali. Serve una RTX 40 o successiva. Circa un frame di latenza in più. "
    "Il preset (A, B, C) si sceglie dalla NVIDIA App, non dal gioco. Lo schermo intero diventa borderless. "
    "Accesa vale dal prossimo avvio; spenta vale subito.",
    "Non disponibile: mancano gow2_dlss.dll o nvngx_dlssg.dll accanto a gow2.exe",
    "Spenta perché OptiScaler è installato: la frame generation si sceglie dal menu di OptiScaler.",
    "Formato",
    "16:9 \xC3\xA8 l'originale. 21:9 allarga il campo visivo (scena 1280x550; HUD e men\xC3\xB9 interi, filmati con bande "
    "ai lati). 16:10 mostra pi\xC3\xB9 immagine sopra e sotto (scena 1280x800; HUD un po' pi\xC3\xB9 grande, alcuni men\xC3\xB9 "
    "stirati in verticale). Tutti e due tengono l'MSAA 2x del gioco. In una finestra di altra forma l'immagine ha "
    "bande nere. Patch di boma e TudorHorse. Vale dal prossimo avvio.",
    "Formato",
    "HDR",
    "Uscita HDR10: le luci che il gioco tagliava (cielo, sole sulla pietra, luci) salgono oltre il bianco carta, fino alla "
    "luminosit\xC3\xA0 di picco. Men\xC3\xB9, HUD e filmati restano al bianco carta. Serve l'HDR di Windows acceso su questo "
    "schermo. Vale dal prossimo avvio.",
    "Non disponibile: l'HDR di Windows \xC3\xA8 spento su questo schermo (Impostazioni > Sistema > Schermo > Usa HDR)",
    "Luminosit\xC3\xA0 di picco",
    "Luce pi\xC3\xB9 forte, in nit. Auto usa il valore dichiarato dallo schermo (1000 se ignoto). Vale dal prossimo avvio.",
    "Bianco carta",
    "Luminosit\xC3\xA0 del bianco di men\xC3\xB9, HUD e immagine normale, in nit. Auto usa la luminosit\xC3\xA0 dei contenuti SDR "
    "di Windows (203 se ignota). Vale dal prossimo avvio.",
    "Auto (%u nit)",
    "HDR",
    "Scala interfaccia",
    "Dimensione del pannello rispetto a quella automatica (100%), che lo fa stare nello schermo. Sopra il 100% il "
    "pannello scorre. Vale subito.",
    "Preset DLSS",
    "Preset del modello DLSS. Default lascia la scelta a NVIDIA. K e' il transformer di seconda generazione "
    "(migliore qualita'), M e L sono pi\xC3\xB9 veloci su RTX 40. Richiede DLSS. Vale dal prossimo avvio.",
    "Preset DLSS",
    "Predefinito",
    "Modalit\xC3\xA0 DLSS",
    "Automatica la ricava dalla risoluzione interna; le altre decidono la risoluzione interna per l'uscita "
    "della finestra. Ogni voce mostra render e uscita. Richiede DLSS. Vale dal prossimo avvio.",
    "Modalit\xC3\xA0 DLSS",
    "Automatica",
    "Qualit\xC3\xA0",
    "Bilanciato",
    "Prestazioni",
    "Ultra prestazioni",
    "Non raggiungibile a %ux%u: il render pu\xC3\xB2 essere solo 1x, 2x o 3x.",
    "Decisa dalla modalit\xC3\xA0 DLSS.",
    "Deprecato da NVIDIA (vecchio modello CNN).",
    "Simile a K: meno ghosting, pi\xC3\xB9 sfarfallio.",
    "Consigliato per DLAA, Qualit\xC3\xA0 e Bilanciato.",
    "Default per Ultra prestazioni.",
    "Default per Prestazioni.",
    "Rendering neurale",
    "Installa OptiScaler",
    "Sostituisci con la %s",
    "Scarica OptiScaler %s (fork F5, GPL-3) da GitHub e lo mette accanto a gow2.exe. Il rendering neurale richiede "
    "nvngx_dlssnr.dll, da procurarsi a parte. Il launcher si riavvia: le modifiche non salvate vanno perse.",
    " Sovrascrive l'OptiScaler presente.",
    "Apri OptiScaler",
    "OptiScaler installato",
    "Installato: riavvia il gioco",
    "Installazione...",
    "dxgi.dll di un altro programma: OptiScaler non si installa",
    "Il rendering neurale richiede nvngx_dlssnr.dll accanto a gow2.exe (nvngx_dlssnr.dll non fornita dal progetto).",
    "OptiScaler applica il rendering neurale solo con DLSS.",
    "Riavvio automatico non riuscito: riavvia a mano.",
    "Dati del gioco",
    "Dati del gioco pronti.",
    "Dati del gioco mancanti: scegli la tua ISO di Gears of War 2 (PAL Europa).",
    "Scegli ISO...",
    "Estrazione in corso...",
    "Annulla",
    "Il file non è un'immagine ISO Xbox 360 valida.",
    "L'ISO non contiene default.xex.",
    "Edizione non supportata: serve l'ISO PAL Europa di Gears of War 2.",
    "Spazio su disco insufficiente per estrarre l'ISO.",
    "Estrazione non riuscita: ",
    "Estrazione annullata.",
    "Edizione non supportata: i file estratti non corrispondono all'ISO PAL Europa.",
    "I dati del gioco sono già pronti. Estrarre l'ISO sopra quelli esistenti?",
    "Mostra console dei log", "Vale dal prossimo avvio.", "Console dei log",
    "Scarica NVIDIA DLSS", "Download in corso...",
    "Le DLL di NVIDIA DLSS vengono scaricate dal GitHub ufficiale di NVIDIA e sono coperte dalla licenza NVIDIA.",
    "DLL scaricate. DLSS è selezionabile.",
};

constexpr UiText kEnglish{
    "Display", "Mode", "Windowed", "Borderless", "Fullscreen", "Resolution", "Default", "Monitor resolution",
    "Resizable", "Scaling", "Letterbox", "Stretch", "V-Sync",
    "Image", "Upscaler", "None", "gow2_dlss.dll missing: built without the NVIDIA SDK", "Anti-aliasing",
    "FXAA extreme", "The upscaler replaces FXAA", "Sharpness", "Sharpness (FSR RCAS)", "DLSS has no sharpening",
    "Internal resolution", "Enhanced shaders",
    "Anisotropic filtering", "Game default", "Sharper textures at grazing angles (floors); applies immediately",
    "Game", "Language",
    "Defaults", "Play", "Quit", "Apply", "Close",
    "Starting from the defaults: pressing Play will rewrite the file.",
    "Pressing Play rewrites the file: comments are not kept.",
    "Take effect on next launch: ", "Save failed: ",
    "Upscaler", "Internal resolution", "Enhanced shaders", "Language", "Frame rate",
    "Shadow quality", "Dynamic shadows",
    "Frame rate", "30 (original)", "Monitor refresh rate", "Unlimited",
    "Frame limit of the game. 30 is the original. Above the monitor refresh rate turn V-Sync off. "
    "Applies at next start. Unlock FPS patch: illusion, boma.",
    "VRR (G-Sync/FreeSync)", "Auto", "On", "Off",
    "With G-Sync or FreeSync active, limits frames just below the monitor refresh rate (e.g. 157 at 165 Hz). "
    "Auto: detected through NVAPI on NVIDIA GPUs; with FreeSync choose On.",
    "FPS counter", "Shows the game's own fps counter (patch by Sowa_95)",
    "No motion blur", "Removes motion blur (patch by illusion)",
    "No lens flares", "Removes lens flares from the sun (patch by boma)",
    "No bloom", "Removes the glow around bright areas (patch by boma)",
    "Shadow quality", "Low", "Original", "High",
    "Resolution of dynamic shadows: 256, 800 (original) or 2048. High makes shadow edges slightly sharper. "
    "Applies at next start. Coalesced hash check patch: Sowa_95, boma.",
    "Dynamic shadows",
    "Shadows of characters and moving objects. Off removes them. Applies at next start. "
    "Coalesced hash check patch: Sowa_95, boma.",
    "Frame generation",
    "AMD FSR 3: one generated frame between two real ones, about twice the frames on screen. Frame rate counts the "
    "real frames; with frame generation they stay under half the monitor's refresh rate. Adds about one frame of "
    "latency. Fullscreen plays as borderless. Turning it on applies at next start; turning it off applies immediately.",
    "Not available: amd_fidelityfx_framegeneration_dx12.dll is missing next to gow2.exe",
    "Frame generation",
    "Off",
    "NVIDIA DLSS: one generated frame between two real ones. Needs an RTX 40 or later. Adds about one frame of latency. "
    "The preset (A, B, C) is chosen in the NVIDIA App, not in the game. Fullscreen plays as borderless. "
    "Turning it on applies at next start; turning it off applies immediately.",
    "Not available: gow2_dlss.dll or nvngx_dlssg.dll is missing next to gow2.exe",
    "Off because OptiScaler is installed: choose frame generation in the OptiScaler menu.",
    "Aspect ratio",
    "16:9 is the original. 21:9 widens the field of view (scene 1280x550; HUD and menus intact, videos with side "
    "bars). 16:10 shows more image above and below (scene 1280x800; HUD a little larger, some menus stretched "
    "vertically). Both keep the game's 2x MSAA. In a window of another shape the image gets black bars. Patches by "
    "boma and TudorHorse. Applies at next start.",
    "Aspect ratio",
    "HDR",
    "HDR10 output: highlights the game used to clip (sky, sun on stone, lights) go above paper white, up to the peak "
    "brightness. Menus, HUD and movies stay at paper white. Needs Windows HDR on for this display. Applies at next start.",
    "Not available: Windows HDR is off on this display (Settings > System > Display > Use HDR)",
    "Peak brightness",
    "Brightest highlight, in nits. Auto uses the value the display reports (1000 if unknown). Applies at next start.",
    "Paper white",
    "Brightness of white in menus, HUD and the normal image, in nits. Auto uses the Windows SDR content brightness (203 "
    "if unknown). Applies at next start.",
    "Auto (%u nit)",
    "HDR",
    "UI scale",
    "Panel size relative to the automatic size (100%), which fits it on screen. Above 100% the panel scrolls. "
    "Applies immediately.",
    "DLSS preset",
    "DLSS model preset. Default leaves the choice to NVIDIA. K is the 2nd generation transformer (best quality), "
    "M and L are faster on RTX 40. Requires DLSS. Applies from the next launch.",
    "DLSS preset",
    "Default",
    "DLSS mode",
    "Automatic derives it from the internal resolution; the others set the internal resolution for the window "
    "output. Each entry shows render and output. Requires DLSS. Applies from the next launch.",
    "DLSS mode",
    "Automatic",
    "Quality",
    "Balanced",
    "Performance",
    "Ultra performance",
    "Not reachable at %ux%u: render can only be 1x, 2x or 3x.",
    "Set by the DLSS mode.",
    "Deprecated by NVIDIA (old CNN model).",
    "Like K: less ghosting, more flicker.",
    "Recommended for DLAA, Quality and Balanced.",
    "Default for Ultra performance.",
    "Default for Performance.",
    "Neural rendering",
    "Install OptiScaler",
    "Replace with %s",
    "Downloads OptiScaler %s (F5 fork, GPL-3) from GitHub and puts it next to gow2.exe. Neural rendering needs "
    "nvngx_dlssnr.dll, to be obtained separately. The launcher restarts: unsaved changes are lost.",
    " Overwrites the installed OptiScaler.",
    "Open OptiScaler",
    "OptiScaler installed",
    "Installed: restart the game",
    "Installing...",
    "dxgi.dll from another program: OptiScaler is not installed",
    "Neural rendering needs nvngx_dlssnr.dll next to gow2.exe (nvngx_dlssnr.dll is not provided by the project).",
    "OptiScaler applies neural rendering only with DLSS.",
    "Automatic restart failed: restart manually.",
    "Game data",
    "Game data ready.",
    "Game data missing: choose your Gears of War 2 ISO (PAL Europe).",
    "Choose ISO...",
    "Extracting...",
    "Cancel",
    "The file is not a valid Xbox 360 ISO image.",
    "The ISO does not contain default.xex.",
    "Unsupported edition: the PAL Europe ISO of Gears of War 2 is required.",
    "Not enough disk space to extract the ISO.",
    "Extraction failed: ",
    "Extraction cancelled.",
    "Unsupported edition: the extracted files do not match the PAL Europe ISO.",
    "The game data is already ready. Extract the ISO over the existing data?",
    "Show log console", "Takes effect on next launch.", "Log console",
    "Download NVIDIA DLSS", "Downloading...",
    "The NVIDIA DLSS DLLs are downloaded from NVIDIA's official GitHub and are covered by NVIDIA's license.",
    "DLLs downloaded. DLSS is now selectable.",
};

}  // namespace

UiLanguage UiLanguageFor(std::string_view game_language, bool windows_italian) {
  if (game_language.empty()) return windows_italian ? UiLanguage::kItalian : UiLanguage::kEnglish;
  return game_language == "it" ? UiLanguage::kItalian : UiLanguage::kEnglish;
}

const UiText& Text(UiLanguage l) { return l == UiLanguage::kItalian ? kItalian : kEnglish; }

std::string RestartText(const std::vector<RestartField>& fields, UiLanguage l) {
  const UiText& t = Text(l);
  std::string out;
  for (const RestartField f : fields) {
    if (!out.empty()) out += ", ";
    switch (f) {
      case RestartField::kUpscaler: out += t.field_upscaler; break;
      case RestartField::kInternalResolution: out += t.field_internal_resolution; break;
      case RestartField::kEnhancedShaders: out += t.field_enhanced_shaders; break;
      case RestartField::kLanguage: out += t.field_language; break;
      case RestartField::kFrameRate: out += t.field_frame_rate; break;
      case RestartField::kShadowQuality: out += t.field_shadow_quality; break;
      case RestartField::kDynamicShadows: out += t.field_dynamic_shadows; break;
      case RestartField::kFrameGeneration: out += t.field_frame_generation; break;
      case RestartField::kAspect: out += t.field_aspect; break;
      case RestartField::kHdr: out += t.field_hdr; break;
      case RestartField::kDlssPreset: out += t.field_dlss_preset; break;
      case RestartField::kDlssMode: out += t.field_dlss_mode; break;
      case RestartField::kConsole: out += t.field_console; break;
    }
  }
  return out;
}

}  // namespace launcher
