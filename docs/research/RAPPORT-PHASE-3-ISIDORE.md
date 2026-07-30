# RAPPORT DE RECHERCHE — Isidore, Le Bibliothécaire

==================================================

**Demande** : Rapport de recherche complet pour guider Célestin (L'Artisan) dans le développement de la Phase 3 de DanceHAP — plugin OBS (C++17, Qt6) qui composite 3 DLayers en temps réel (fond HAP + webcam matting + overlay alpha) à partir d'un show file `.dhp` JSON. Phase 3 = parser show file + source composite + dock minimal + hotkeys.

**Date** : 2026-07-30
**Destinataire** : Célestin (L'Artisan), via Alfred (orchestrateur)
**Sources** : Code source OBS (master branch), GitHub API, repos de référence, code DanceHAP existant, ADRs 009-013

---

## 1. CONTEXTE EXISTANT

### 1.1 Projets/repos locaux pertinents

- **`/root/dancehap`** — repo principal DanceHAP (branch `main`, v0.6.0)
- **`/root/dancehap/docs/plans/DANCEHAP-V2-DRAFT.md`** — plan architecture v2 validé par Jean-Luc le 30/07/2026
- **`/root/dancehap/ARCHITECTURE.md`** — cahier v1.0 (4 briques modulaires, maintenant complété par ADR-010)
- **`/root/dancehap/docs/adr/`** — 8 ADRs (001-008 historiques + 009-013 nouveaux pour Phase 3)

### 1.2 Code existant réutilisable (Phases 1-2 livrées)

Le repo contient déjà (dans `src/`) :

| Fichier | Rôle | Réutilisable Phase 3 ? |
|---------|------|----------------------|
| `hap_clip_source.cpp/.hpp` | Source OBS `dancehap_hap_clip` qui lit un .mov HAP (vidéo+audio). ClipPlayer intégré. | ✅ **Base pour DLayer 1/3** — le `video_render`/`video_tick` pattern est le modèle |
| `clip_player.cpp/.hpp` | Moteur de playback HAP (state machine + FPS pacing + audio master clock ADR-007 + loop/EOF). OBS-independent (pimpl). | ✅ **Réutilisable tel quel** pour chaque clip des DLayers 1 et 3. Double buffering à ajouter pour crossfade |
| `hap_decoder.cpp/.hpp` | Décodeur HAP (Snappy + DXT5→RGBA software + upload GPU). Stub/real modes. | ✅ Réutilisé via ClipPlayer |
| `hap_demuxer.cpp/.hpp` | Demuxer container MOV/MP4 (FFmpeg/stub). Détecte HAP variant + audio. | ✅ Réutilisé via ClipPlayer |
| `audio_decoder.cpp/.hpp` | Décodeur audio AAC→PCM float (stub sinus + FFmpeg real). | ✅ Réutilisé via ClipPlayer |
| `ai_matte_filter.cpp/.hpp` | Filtre OBS `dancehap_ai_matte` — matting via MatteEngine (texrender+stagesurface+process_filter_begin/end pattern). 7 modèles. | ✅ **Base pour DLayer 2 matting** — mais actuellement c'est un *filttre* sur une source OBS externe. Phase 3 doit intégrer la logique dans la source composite interne |
| `matte_engine.cpp/.hpp` | Engine ONNX Runtime (Strategy pattern, 7 modèles, async worker, DirectML/CoreML/CPU). | ✅ Réutilisable tel quel pour DLayer 2 |
| `obs_compat.hpp` / `obs_stub.cpp` | Compat shim pour compiler avec/sans OBS headers (stub mode pour tests). | ✅ **À étendre** pour nouvelles fonctions OBS (hotkeys, dock, video capture) |
| `plugin.cpp` | Module entry point (`obs_module_load` register hap_clip_source + ai_matte_filter). | ✅ **À étendre** pour register la nouvelle source composite + dock |
| `cmake/FindLibObs.cmake` | CMake find module pour libobs. | ✅ Existant |

### 1.3 Schémas/architecture en place

- **Plan v2** (`docs/plans/DANCEHAP-V2-DRAFT.md`) : architecture 2-composants (éditeur + plugin). 3 DLayers dans une source composite OBS unique. Dock minimal (play/stop/timecode/markers). Show file `.dhp` JSON comme contrat.
- **ADRs 009-013** (validés 30/07/2026) :
  - **ADR-009** : Show file format `.dhp` (JSON, versionnable, chemins absolus, matting statique, markers pour navigation)
  - **ADR-010** : Architecture 2-composants (éditeur standalone + plugin OBS, complète ADR-002)
  - **ADR-011** : Capture webcam **interne** DanceHAP (pas de référence à source OBS externe — le plugin ouvre le device directement)
  - **ADR-012** : Interpolation **B-spline cubique** pour keyframes d'opacité (degré 3, clamping aux extrémités, handles optionnels, implémentation custom légère)
  - **ADR-013** : Crossfade configurable entre clips HAP (double buffer de frames, blend alpha lerp, `crossfade_in`/`crossfade_out` par clip)

### 1.4 Stack technique actuelle

- C++17, OBS API ≥ 31, Qt6, CMake ≥ 3.22, GoogleTest 1.14
- ONNX Runtime ≥ 1.17 (DirectML Win / CoreML macOS / CPU fallback)
- FFmpeg (via obs-deps), Snappy (FetchContent 1.2.1)
- Tests : 134 tests GoogleTest en stub mode (sans OBS/FFmpeg/Snappy/ONNX)
- CI : GitHub Actions matrix Win+macOS

### 1.5 Patterns OBS déjà maîtrisés (leçons des Phases 1-2)

Le brief `dancehap-brief` documente 30+ anti-patternes OBS appris à grands frais. Les plus critiques pour Phase 3 :

- **Pattern filtre sync OBS** (ADR-008) : `texrender + stagesurface + process_filter_begin/end`, JAMAIS `obs_source_get_frame()` sur un filtre sync
- **Règle graphics-thread** : tout `gs_*` doit tourner sur le render thread (`video_render`), jamais depuis `video_tick`
- **Pas de `OBS_SOURCE_CUSTOM_DRAW`** sans charger son propre `gs_effect_t`
- **`gs_effect_create_from_file()`** requiert le contexte graphics actif (charger dans `update()` ou `video_render`, pas `create()`)
- **Installation user-level** : `C:\ProgramData\obs-studio\plugins\<name>\` (pas system-level)
- **Stub mode ne catche pas** les noms/signatures de fonction OBS faux → toujours étendre `obs_stub.cpp` + `obs_compat.hpp` en même temps que le code plugin

---

## 2. RECHERCHE TECHNIQUE

### 2.1 Parser JSON C++ pour show file

#### 2.1.1 Comparaison des bibliothèques

| Lib | Stars | Licence | Header-only | C++17 | API | Perf | Intégration CMake |
|-----|-------|---------|-------------|-------|-----|------|-------------------|
| **nlohmann/json** | 50.2k | MIT | Oui (single header) | ✅ | STL-like (`j["key"]`, `j.value()`) | Bonne (~2x slower que RapidJSON en parse, largement suffisant pour un .dhp de quelques Ko) | `FetchContent` ou `find_package(nlohmann_json)` — target `nlohmann_json::nlohmann_json` |
| RapidJSON | 14k | MIT | Oui | ✅ | SAX + DOM, plus verbeux | Excellente (SIMD) | `FetchContent` ou `find_package(RapidJSON)` |
| simdjson | 8k | Apache-2.0 | Non (lib) | ✅ | DOM, API spécifique | La plus rapide (SIMD) | `FetchContent` — plus lourd |
| boost::property_tree | — | Boost | Non | ✅ | Arborescente, verbose | Médiocre | `find_package(Boost)` — trop lourd pour un plugin |

#### 2.1.2 Recommandation : nlohmann/json

**Pourquoi nlohmann/json** :
1. **MIT licence** (compatible ADR-005, contrairement à simdjson Apache-2.0 qui est aussi compatible mais plus lourd)
2. **Header-only single file** — zéro dépendance build, zéro friction CI (contrairement à RapidJSON qui a des deps)
3. **API intuitive STL-like** — `json j = json::parse(str); auto name = j["show"]["name"].get<string>();` parfait pour un parser de show file
4. **Gestion d'erreur native** — exceptions avec messages détaillés (`parse_error`, `type_error`), `j.value("key", default)` pour accès safe
5. **Déjà le standard de facto** dans l'écosystème OBS/C++ moderne
6. **Intégration CMake FetchContent** triviale (3 lignes) — DanceHAP utilise déjà ce pattern pour Snappy

#### 2.1.3 Intégration CMake

Pattern **FetchContent** (recommandé, déjà utilisé pour Snappy dans `CMakeLists.txt:120`) :

```cmake
include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
)
FetchContent_MakeAvailable(nlohmann_json)
# Usage: target_link_libraries(dancehap PRIVATE nlohmann_json::nlohmann_json)
```

Alternative `find_package` (si nlohmann_json installé sur le système de build) :
```cmake
find_package(nlohmann_json 3.12.0 REQUIRED)
target_link_libraries(dancehap PRIVATE nlohmann_json::nlohmann_json)
```

**Recommandation** : FetchContent pour consistance avec le pattern Snappy existant et pour que le build fonctionne out-of-the-box sans pré-installation.

#### 2.1.4 Pattern de parsing/validation pour le show file

Le show file `.dhp` (ADR-009) a cette structure :

```json
{
  "version": "2.0",
  "show": { "name": "...", "duration": 3600.0, "created": "2026-07-30" },
  "webcam": { "device": "default", "resolution": "1280x720", "fps": 30 },
  "matting": { "model": "rvm_mobilenetv3", "threshold": 0.5, ... },
  "dlayers": {
    "dlayer1_background": { "clips": [...], "opacity_keyframes": [...] },
    "dlayer2_live": { "opacity_keyframes": [...] },
    "dlayer3_overlay": { "clips": [...], "opacity_keyframes": [...] }
  },
  "audio_tracks": [ { "id": "...", "file": "...", "start": 0.0, "volume": 0.8 } ],
  "markers": [ { "time": 0.0, "name": "Patience" } ]
}
```

**Pattern recommandé** — struct C++ + fonction de parsing avec validation :

```cpp
// show_file.hpp
struct ShowClip {
    std::string id;
    std::string file;      // chemin absolu
    double start;          // seconds
    double duration;       // seconds
    bool loop;
    double crossfade_in;   // seconds, 0 = coup sec
    double crossfade_out;  // seconds
};

struct Keyframe {
    double time;
    double value;          // 0.0-1.0
    std::optional<std::array<double,2>> handle_left;   // [dx, dy] tangente
    std::optional<std::array<double,2>> handle_right;
};

struct DLayer {
    std::vector<ShowClip> clips;           // vide pour dlayer2_live
    std::vector<Keyframe> opacity_keyframes;
};

struct ShowFile {
    std::string version;
    std::string show_name;
    double show_duration;
    std::string webcam_device;
    int webcam_width, webcam_height, webcam_fps;
    MatteModelConfig matting;             // réutiliser MatteModelConfig existant
    DLayer dlayer1, dlayer2, dlayer3;
    std::vector<AudioTrack> audio_tracks;
    std::vector<Marker> markers;
};

// Parsing avec validation
struct ParseError {
    std::string field;
    std::string message;
};

std::expected<ShowFile, std::vector<ParseError>> parse_show_file(const std::string& json_str);
// ou std::variant<ShowFile, ParseError> si C++23 <expected> non disponible
// En C++17 : retourner std::variant<ShowFile, ParseError> ou utiliser exceptions
```

**Validation à faire au parsing** :
1. `version` == "2.0" (rejeter autres versions avec message clair)
2. Chemins de fichiers **absolus** et **existants** (`std::filesystem::exists()`)
3. `duration` > 0, `start` >= 0, `crossfade_in/out` >= 0
4. Keyframes triés par `time` croissante, `value` dans [0.0, 1.0]
5. `dlayer2_live` ne doit PAS avoir de `clips` (feed live)
6. Chevauchement crossfade : `crossfade_out` clip A + `crossfade_in` clip B ne dépasse pas la durée restante (validation soft — warning, pas erreur fatale, car l'éditeur Phase 4 fera la validation stricte)
7. `matting.model` dans la liste des 7 modèles supportés (réutiliser `resolveModelType()` de Phase 2.6)

**Gestion d'erreur** : nlohmann/json lève des exceptions (`nlohmann::json::parse_error`, `nlohmann::json::type_error`). Pattern recommandé — wrapper dans un try/catch et convertir en `ParseError` structuré :

```cpp
ShowFile parse_show_file(const std::string& path) {
    std::ifstream f(path);
    nlohmann::json j = nlohmann::json::parse(f);  // peut lancer parse_error

    ShowFile show;
    show.version = j.at("version").get<std::string>();
    if (show.version != "2.0")
        throw std::runtime_error("Unsupported show file version: " + show.version);

    show.show_name = j.at("show").at("name").get<std::string>();
    show.show_duration = j.at("show").at("duration").get<double>();
    // ... etc, avec .at() pour les champs requis et .value() pour les optionnels
    return show;
}
```

**Tests** : le parser doit être testé en stub mode (pas besoin d'OBS). Créer des `.dhp` de test dans `tests/assets/` (valid + invalid variants). Le pattern est identique aux tests existants.

#### 2.1.5 Chemins absolus vs relatifs

ADR-009 stipule des chemins absolus dans le show file. **Caveat** : portabilité entre machines limitée. Pour Phase 3, on valide que le chemin est absolu (`std::filesystem::path::is_absolute()`) et existe. Phase 4 (éditeur) pourra ajouter une option "relative to show file directory" si Jean-Luc le demande.

---

### 2.2 OBS source composite pattern

#### 2.2.1 Comment créer une source OBS qui composite plusieurs layers

DanceHAP Phase 3 doit exposer **une seule source OBS** (`OBS_SOURCE_TYPE_INPUT`) qui composite 3 DLayers en interne. C'est différent des 4 briques séparées de l'architecture v1 (ADR-002).

**API OBS clé** (de `libobs/obs-source.h`, lignes 222-380) :

```c
struct obs_source_info {
    const char *id;                    // "dancehap_composite"
    enum obs_source_type type;         // OBS_SOURCE_TYPE_INPUT
    uint32_t output_flags;             // OBS_SOURCE_VIDEO | OBS_SOURCE_COMPOSITE | OBS_SOURCE_CUSTOM_DRAW
    // OBS_SOURCE_COMPOSITE (1<<6) : indique que la source a des enfants
    const char *(*get_name)(void *type_data);
    void *(*create)(obs_data_t *settings, obs_source_t *source);
    void (*destroy)(void *data);
    uint32_t (*get_width)(void *data);
    uint32_t (*get_height)(void *data);
    void (*get_defaults)(obs_data_t *settings);
    obs_properties_t *(*get_properties)(void *data);
    void (*update)(void *data, obs_data_t *settings);
    void (*activate)(void *data);
    void (*deactivate)(void *data);
    void (*video_tick)(void *data, float seconds);
    void (*video_render)(void *data, gs_effect_t *effect);
    // ... audio callbacks
};
```

**Flags recommandés** :
- `OBS_SOURCE_VIDEO` (1<<0) — la source produit de la vidéo
- `OBS_SOURCE_CUSTOM_DRAW` (1<<3) — **nécessaire** car on fait un compositing multi-layer custom (on ne peut pas juste faire `gs_effect_set_texture(image, tex)` + `gs_draw_sprite` comme une source simple)
- `OBS_SOURCE_COMPOSITE` (1<<6) — indique que la source peut avoir des enfants (utile si on veut que OBS gère le cycle de vie des sources enfants)
- `OBS_SOURCE_DO_NOT_DUPLICATE` (1<<7) — on ne veut pas que OBS duplique cette source (elle gère son propre état)

**⚠️ ATTENTION — leçon bug 7 du brief DanceHAP** : `OBS_SOURCE_CUSTOM_DRAW` fait que OBS passe un effect NULL à `video_render`. Il faut charger son propre `gs_effect_t` via `gs_effect_create_from_file()` dans `update()` (avec `obs_enter_graphics()`/`obs_leave_graphics()`), JAMAIS dans `create()` (pas de contexte graphics).

#### 2.2.2 Pattern de compositing multi-layer dans video_render

La source composite doit, dans `video_render` (render thread, contexte graphics actif) :

```cpp
void dancehap_composite_render(void *data, gs_effect_t *effect) {
    auto *ctx = static_cast<CompositeContext*>(data);

    // 1. DLayer 1 (background) — récupérer la texture du ClipPlayer actif
    gs_texture_t *bg_tex = ctx->dlayer1_player->getCurrentTexture();
    if (bg_tex) {
        gs_effect_set_texture(gs_effect_get_param_by_name(ctx->composite_effect, "bg_image"), bg_tex);
    }

    // 2. Crossfade DLayer 1 — si 2 clips actifs pendant le crossfade
    if (ctx->dlayer1_crossfade_active) {
        gs_texture_t *bg_tex2 = ctx->dlayer1_player_b->getCurrentTexture();
        gs_effect_set_texture(gs_effect_get_param_by_name(ctx->composite_effect, "bg_image2"), bg_tex2);
        gs_effect_set_float(gs_effect_get_param_by_name(ctx->composite_effect, "crossfade_t"), ctx->dlayer1_crossfade_t);
    }

    // 3. DLayer 2 (webcam + matting) — texture de la webcam après matting
    gs_texture_t *webcam_tex = ctx->webcam_capturer->getMattedTexture();
    gs_effect_set_texture(gs_effect_get_param_by_name(ctx->composite_effect, "webcam_image"), webcam_tex);

    // 4. DLayer 3 (overlay) — texture du ClipPlayer overlay
    gs_texture_t *overlay_tex = ctx->dlayer3_player->getCurrentTexture();
    gs_effect_set_texture(gs_effect_get_param_by_name(ctx->composite_effect, "overlay_image"), overlay_tex);

    // 5. Opacité par layer (B-spline évaluée en video_tick)
    gs_effect_set_float(gs_effect_get_param_by_name(ctx->composite_effect, "bg_opacity"), ctx->dlayer1_opacity);
    gs_effect_set_float(gs_effect_get_param_by_name(ctx->composite_effect, "webcam_opacity"), ctx->dlayer2_opacity);
    gs_effect_set_float(gs_effect_get_param_by_name(ctx->composite_effect, "overlay_opacity"), ctx->dlayer3_opacity);

    // 6. Draw avec le technique du composite effect
    gs_technique_t *tech = gs_effect_get_technique(ctx->composite_effect, "Draw");
    size_t passes = gs_technique_begin(tech);
    for (size_t i = 0; i < passes; i++) {
        gs_technique_begin_pass(tech, i);
        gs_draw_sprite(nullptr, 0, ctx->width, ctx->height);
        gs_technique_end_pass(tech);
    }
    gs_technique_end(tech);
}
```

Le **composite effect shader** (`.effect` file) fait le blending :

```hlsl
// data/effects/composite.effect
uniform float4x4 ViewProj;
uniform texture2d bg_image;
uniform texture2d bg_image2;      // pour crossfade
uniform texture2d webcam_image;
uniform texture2d overlay_image;
uniform float crossfade_t;        // 0..1 pendant crossfade DLayer 1
uniform float bg_opacity;
uniform float webcam_opacity;
uniform float overlay_opacity;
uniform bool crossfade_active;

sampler_state textureSampler { Filter=Linear; AddressU=Clamp; AddressV=Clamp; };

float4 composite_ps(VertData v_in) : TARGET {
    float4 bg = bg_image.Sample(textureSampler, v_in.uv);
    float4 bg2 = bg_image2.Sample(textureSampler, v_in.uv);
    float4 webcam = webcam_image.Sample(textureSampler, v_in.uv);
    float4 overlay = overlay_image.Sample(textureSampler, v_in.uv);

    // Crossfade DLayer 1
    float4 bg_final = crossfade_active ? lerp(bg, bg2, crossfade_t) : bg;
    bg_final.a *= bg_opacity;

    // Compositing : bg → webcam → overlay (back to front)
    float4 result = bg_final;
    result = lerp(result, float4(webcam.rgb, webcam.a * webcam_opacity), webcam.a * webcam_opacity);
    result = lerp(result, overlay, overlay.a * overlay_opacity);

    return result;
}

technique Draw {
    pass {
        vertex_shader = vs_transform(v_in);
        pixel_shader = composite_ps(v_in);
    }
}
```

#### 2.2.3 video_tick — logique de timing et évaluation B-spline

```cpp
void dancehap_composite_tick(void *data, float seconds) {
    auto *ctx = static_cast<CompositeContext*>(data);
    if (!ctx->playing) return;

    ctx->show_time += seconds;

    // 1. Évaluer l'opacité de chaque DLayer via B-spline
    ctx->dlayer1_opacity = ctx->bspline_dlayer1.evaluate(ctx->show_time);
    ctx->dlayer2_opacity = ctx->bspline_dlayer2.evaluate(ctx->show_time);
    ctx->dlayer3_opacity = ctx->bspline_dlayer3.evaluate(ctx->show_time);

    // 2. Tick des ClipPlayers (DLayer 1 et 3) — décodage CPU (pas de gs_* ici !)
    ctx->dlayer1_player->tick(seconds);
    ctx->dlayer3_player->tick(seconds);

    // 3. Gestion crossfade DLayer 1
    // Détermine si on est dans une zone de crossfade et gère le double buffer
    ctx->updateCrossfadeState();

    // 4. Tick webcam capturer (DLayer 2) — capture + matting async
    ctx->webcam_capturer->tick(seconds);

    // 5. Audio routing
    ctx->routeAudio();
}
```

**⚠️ Règle graphics-thread** : `video_tick` ne fait AUCUN appel `gs_*`. Le décodage et l'évaluation B-spline sont CPU-only. L'upload GPU se fait dans `video_render` ou dans le `ClipPlayer::uploadToGpu()` appelé depuis `video_render`.

#### 2.2.4 Comment une source OBS capture une webcam en interne

Voir §2.5 ci-dessous (capture webcam interne).

#### 2.2.5 Plugins OBS de référence pour le compositing multi-layer

| Projet | URL | Pertinence |
|--------|-----|-----------|
| **obs-studio/obs-source-transition.c** | https://github.com/obsproject/obs-studio/blob/master/libobs/obs-source-transition.c | **RÉFÉRENCE ABSOLUE** pour le crossfade entre 2 sources. Utilise `gs_texrender[2]` pour rendre les 2 sources séparément, puis blend dans `video_render`. 1072 lignes. |
| **obs-studio/plugins/obs-transitions/transition-fade.c** | https://github.com/obsproject/obs-studio/blob/master/plugins/obs-transitions/transition-fade.c | Transition fade simple — `obs_transition_video_render2()` + callbacks `mix_a`/`mix_b` pour le blend audio/video. 148 lignes, parfait comme exemple minimal. |
| **exeldro/obs-shaderfilter** | https://github.com/exeldro/obs-shaderfilter | 760 ⭐. Filtre OBS qui applique un shader arbitraire à une source. Montre comment charger un `.effect` custom et l'appliquer dans `video_render`. |
| **royshil/obs-backgroundremoval** | https://github.com/royshil/obs-backgroundremoval | 4440 ⭐. **RÉFÉRENCE ABSOLUE** pour le matting OBS (déjà étudié en Phase 2). Montre le pattern `texrender + stagesurface + process_filter_begin/end` + async worker ONNX. |
| **obs-studio/plugins/obs-transitions/** | https://github.com/obsproject/obs-studio/tree/master/plugins/obs-transitions | Toutes les transitions natives OBS (cut, fade, fade-to-color, luma-wipe, slide, stinger). `transition-stinger.c` montre comment gérer un média pendant une transition. |

**Leçon clé de `obs-source-transition.c`** : OBS gère le crossfade entre 2 sources via un système de `gs_texrender` double. Chaque source est rendue dans son propre texrender, puis un callback de blending (`mix_a`/`mix_b`) combine les 2 textures. Pour DanceHAP, on peut simplifier : comme on contrôle les ClipPlayers nous-mêmes, on peut faire le double buffer directement dans le composite effect shader (voir §2.2.2).

---

### 2.3 B-spline runtime evaluation

#### 2.3.1 Algorithme B-spline cubique

ADR-012 stipule une **B-spline cubique (degré 3)** avec clamping aux extrémités. L'algorithme :

1. **Knot vector** : pour une B-spline clamped (qui passe par les points d'extrémité), on duplique les knots aux bornes. Pour N keyframes et degré p=3, le knot vector a N+p+1 éléments, avec les p+1 premiers et derniers knots égaux.

2. **Évaluation** : pour un paramètre `t` (le temps), trouver le span de knots qui le contient, puis appliquer la formule de Cox-de Boor :

```
N[i,0](t) = 1 si t_i <= t < t_{i+1}, sinon 0
N[i,p](t) = (t - t_i)/(t_{i+p} - t_i) * N[i,p-1](t) + (t_{i+p+1} - t)/(t_{i+p+1} - t_{i+1}) * N[i+1,p-1](t)
S(t) = sum_i N[i,p](t) * P_i   (où P_i sont les keyframe values)
```

3. **Clamping (endpoint interpolation)** : pour que la courbe passe exactement par le premier et le dernier keyframe, on utilise un knot vector "clamped" :
   - N keyframes : P_0, P_1, ..., P_{N-1}
   - Degré p = 3
   - Knot vector : [0, 0, 0, 0, t_1, t_2, ..., t_{N-4}, T, T, T, T] (où T = durée totale)
   - Les N+p+1 = N+4 knots, avec les 4 premiers = 0 et les 4 derniers = T

#### 2.3.2 Implémentation C++ légère recommandée

**Pas de dépendance externe** (ADR-012). L'algorithme de Cox-de Boor est simple (~60 lignes). On n'a pas besoin d'une bibliothèque complète.

**Repos GitHub de référence** (recherche `bspline language:C++`) :
- **bgrimstad/splinter** (444 ⭐) — https://github.com/bgrimstad/splinter — bibliothèque C++ pour B-spline multivariée, mais trop lourde pour notre besoin
- **chen0040/cpp-spline** (144 ⭐) — https://github.com/chen0040/cpp-spline — spline interpolation C++, header-only
- **thibauts/b-spline** (317 ⭐) — https://github.com/thibauts/b-spline — JS, mais algorithme clair et portable

**Recommandation** : implémentation custom ~80 lignes, dans `src/bspline.hpp/.cpp`. L'algorithme est bien documenté (Wikipedia "B-spline", The NURBS Book de Piegl & Tiller).

**Code de base recommandé** :

```cpp
// bspline.hpp
#pragma once
#include <vector>
#include <cmath>
#include <algorithm>

class BSpline {
public:
    struct Keyframe {
        double time;
        double value;           // 0.0-1.0
        // Handles optionnels pour B-spline non-uniforme (Phase 4)
        // double handle_left[2];   // [dx, dy]
        // double handle_right[2];
    };

    void setKeyframes(const std::vector<Keyframe>& kfs);
    double evaluate(double t) const;  // t en seconds

private:
    std::vector<Keyframe> keyframes_;
    std::vector<double> knots_;       // knot vector clamped

    void buildKnotVector();
    int findSpan(double t) const;
    double basisFunction(int i, int p, double t) const;
};
```

```cpp
// bspline.cpp
void BSpline::setKeyframes(const std::vector<Keyframe>& kfs) {
    keyframes_ = kfs;
    std::sort(keyframes_.begin(), keyframes_.end(),
              [](const auto& a, const auto& b) { return a.time < b.time; });
    buildKnotVector();
}

void BSpline::buildKnotVector() {
    // B-spline cubique clamped (degré 3)
    // N keyframes → N+4 knots, 4 premiers = t_min, 4 derniers = t_max
    const int p = 3;  // degré
    const int n = keyframes_.size();
    if (n < 2) return;

    knots_.resize(n + p + 1);
    double t_min = keyframes_.front().time;
    double t_max = keyframes_.back().time;

    // Clamping aux extrémités
    for (int i = 0; i <= p; i++) {
        knots_[i] = t_min;
        knots_[n + p - i] = t_max;  // hmm, careful with indexing
    }
    // En fait pour clamped : les p+1 premiers knots = t_min, les p+1 derniers = t_max
    // knots_[0..p] = t_min, knots_[n..n+p] = t_max
    // Les knots intermédiaires (knots_[p+1 .. n-1]) sont uniformes ou basés sur les keyframe times
    for (int i = 0; i <= p; i++)
        knots_[i] = t_min;
    for (int i = n; i <= n + p; i++)
        knots_[i] = t_max;

    // Knots intermédiaires : uniforme ou paramétrisation chord-length
    // Simple : uniforme
    int num_internal = n - p - 1;  // = n - 4 pour p=3
    if (num_internal > 0) {
        for (int i = 1; i <= num_internal; i++) {
            knots_[p + i] = t_min + (double)i / (num_internal + 1) * (t_max - t_min);
        }
    }
}

int BSpline::findSpan(double t) const {
    // Binary search pour trouver le span de knots contenant t
    int n = keyframes_.size();
    int p = 3;
    if (t >= knots_[n]) return n - 1;  // dernier span
    if (t <= knots_[p]) return p;      // premier span

    int low = p, high = n;
    int mid = (low + high) / 2;
    while (t < knots_[mid] || t >= knots_[mid + 1]) {
        if (t < knots_[mid]) high = mid;
        else low = mid;
        mid = (low + high) / 2;
    }
    return mid;
}

double BSpline::basisFunction(int i, int p, double t) const {
    // Cox-de Boor récursif
    if (p == 0) {
        return (knots_[i] <= t && t < knots_[i + 1]) ? 1.0 : 0.0;
    }
    double left = 0.0, right = 0.0;
    double denom_left = knots_[i + p] - knots_[i];
    if (denom_left > 0)
        left = (t - knots_[i]) / denom_left * basisFunction(i, p - 1, t);
    double denom_right = knots_[i + p + 1] - knots_[i + 1];
    if (denom_right > 0)
        right = (knots_[i + p + 1] - t) / denom_right * basisFunction(i + 1, p - 1, t);
    return left + right;
}

double BSpline::evaluate(double t) const {
    if (keyframes_.empty()) return 1.0;  // default opaque
    if (keyframes_.size() == 1) return keyframes_[0].value;
    if (t <= keyframes_.front().time) return keyframes_.front().value;
    if (t >= keyframes_.back().time) return keyframes_.back().value;

    int span = findSpan(t);
    double result = 0.0;
    for (int i = span - 3; i <= span; i++) {
        if (i >= 0 && i < (int)keyframes_.size()) {
            result += basisFunction(i, 3, t) * keyframes_[i].value;
        }
    }
    return std::clamp(result, 0.0, 1.0);
}
```

**Optimisation** : la récursion `basisFunction` est O(p²) par évaluation. Pour p=3, c'est négligeable (9 multiplications). On peut pré-calculer les basis functions en table, mais pour une évaluation par frame à 30fps, c'est inutile. Si on veut optimiser, utiliser l'algorithme itératif de de Boor (non-récursif, O(p)) :

```cpp
// Version optimisée (de Boor algorithm)
double BSpline::evaluate(double t) const {
    int span = findSpan(t);
    int p = 3;
    // Les 4 control points influents
    double N[4] = {1, 0, 0, 0};
    for (int j = 1; j <= p; j++) {
        for (int k = j; k >= 1; k--) {
            double d = knots_[span + k] - knots_[span + k - j];
            N[k] = (d > 0) ? (N[k-1] + (knots_[span + k] - t) / d * (N[k] - N[k-1])) : 0;
        }
        double d = knots_[span] - knots_[span - j];
        N[0] = (d > 0) ? (t - knots_[span - j]) / d * N[0] : 0;
    }
    double result = 0;
    for (int i = 0; i <= p; i++)
        result += N[i] * keyframes_[span - p + i].value;
    return std::clamp(result, 0.0, 1.0);
}
```

#### 2.3.3 Clamping des extrémités (endpoint interpolation)

Le knot vector "clamped" (les p+1 premiers knots = t_min, les p+1 derniers = t_max) garantit que la B-spline passe exactement par le premier et le dernier keyframe. C'est le comportement voulu pour l'opacité : à t=0 on a la valeur du premier keyframe, à t=fin on a la valeur du dernier.

**Edge cases** :
- **1 seul keyframe** : retourner sa valeur (constante)
- **2 keyframes** : B-spline = Bézier = linéaire (la B-spline de degré 3 avec 2 points clampés est une droite)
- **t hors bornes** : clamer au premier/dernier keyframe (ne pas extrapoler)

#### 2.3.4 Handles de tangente (Phase 4, pré-recherche)

ADR-012 mentionne des `handle_left`/`handle_right` optionnels pour une B-spline non-uniforme. En Phase 3, on ignore les handles (B-spline uniforme clamped). En Phase 4, l'éditeur permettra d'éditer les handles — cela correspond à une B-spline avec un knot vector non-uniforme (les knots sont espacés selon les handles). L'algorithme reste le même, seul le knot vector change.

---

### 2.4 Crossfade entre deux clips vidéo décodés

#### 2.4.1 Double buffering de frames

ADR-013 stipule : pendant le crossfade, les deux clips sont décodés simultanément. Le `ClipPlayer` existant gère un seul clip. Il faut gérer **2 ClipPlayers actifs** par DLayer pendant la transition.

**Pattern recommandé** :

```cpp
struct DLayerState {
    std::unique_ptr<ClipPlayer> player_a;   // clip sortant
    std::unique_ptr<ClipPlayer> player_b;   // clip entrant
    double crossfade_t;                      // 0..1
    bool crossfade_active;
    double crossfade_duration;

    // Pendant le crossfade : tick les 2 players, blend dans le shader
    // Hors crossfade : un seul player actif (player_a = clip courant)
};
```

Dans `video_tick` :
```cpp
void CompositeContext::updateCrossfadeState() {
    // DLayer 1
    auto& dl1 = dlayer1;
    if (dl1.crossfade_active) {
        dl1.player_a->tick(seconds);
        dl1.player_b->tick(seconds);
        dl1.crossfade_t += seconds / dl1.crossfade_duration;
        if (dl1.crossfade_t >= 1.0) {
            // Crossfade terminé : player_a devient player_b, player_b libéré
            dl1.player_a = std::move(dl1.player_b);
            dl1.player_b = nullptr;
            dl1.crossfade_active = false;
            dl1.crossfade_t = 0;
        }
    } else {
        // Mode normal : un seul player
        dl1.player_a->tick(seconds);
        // Détecter si on entre dans une zone de crossfade
        checkCrossfadeStart(dl1);
    }
}
```

#### 2.4.2 Comment OBS gère les transitions entre sources

**Référence : `libobs/obs-source-transition.c`** (1072 lignes)

OBS utilise un système de `gs_texrender` double pour les transitions :
- `transition->transition_texrender[0]` — rend la source A
- `transition->transition_texrender[1]` — rend la source B
- Puis un callback de blending combine les 2 textures dans `video_render`

```c
// obs-source-transition.c (simplifié)
void obs_transition_video_render2(obs_source_t *transition,
    void (*callback)(void *data, gs_texture_t *a, gs_texture_t *b, float t, ...),
    void *data)
{
    // Rend source A dans texrender[0]
    gs_texrender_reset(transition->transition_texrender[0]);
    gs_texrender_begin(transition->transition_texrender[0], cx, cy);
    obs_source_video_render(transition->transition_sources[0]);
    gs_texrender_end(transition->transition_texrender[0]);

    // Rend source B dans texrender[1]
    gs_texrender_reset(transition->transition_texrender[1]);
    gs_texrender_begin(transition->transition_texrender[1], cx, cy);
    obs_source_video_render(transition->transition_sources[1]);
    gs_texrender_end(transition->transition_texrender[1]);

    // Blend via callback
    gs_texture_t *tex_a = gs_texrender_get_texture(transition->transition_texrender[0]);
    gs_texture_t *tex_b = gs_texrender_get_texture(transition->transition_texrender[1]);
    callback(data, tex_a, tex_b, transition->transition_time, ...);
}
```

**Transition fade** (`plugins/obs-transitions/transition-fade.c`, 148 lignes) :

```c
static void fade_callback(void *data, gs_texture_t *a, gs_texture_t *b, float t, ...) {
    // Simple lerp alpha : output = a * (1-t) + b * t
    gs_effect_set_texture(image_a, a);
    gs_effect_set_texture(image_b, b);
    gs_effect_set_float(progress, t);
    // Draw avec shader qui fait lerp(a, b, t)
}

static void fade_video_render(void *data, gs_effect_t *effect) {
    obs_transition_video_render2(fade->source, fade_callback, NULL);
}
```

**Leçon pour DanceHAP** : on n'utilise pas l'API `obs_transition_*` (c'est pour les sources de type TRANSITION, pas INPUT). On fait notre propre double buffering dans le `CompositeContext`, et le blending se fait dans le composite effect shader (voir §2.2.2). Mais la logique de `obs-source-transition.c` est la référence à comprendre pour le pattern.

---

### 2.5 Capture webcam interne dans un plugin OBS

#### 2.5.1 Le défi ADR-011

ADR-011 stipule que DanceHAP gère sa **propre capture webcam interne** — pas de référence à une source OBS externe. C'est la partie la plus risquée de Phase 3 car c'est inhabituel dans l'écosystème OBS.

#### 2.5.2 Option A — Créer une source webcam OBS interne invisible

OBS permet de créer des sources privées (`obs_source_create_private`) qui n'apparaissent pas dans l'UI mais produisent des frames. On peut créer une source webcam privée et consommer ses frames.

```cpp
// Créer une source webcam de capture privée (n'apparaît pas dans l'UI OBS)
obs_data_t *webcam_settings = obs_data_create();
obs_data_set_string(webcam_settings, "device_id", show.webcam_device.c_str());
obs_source_t *webcam_source = obs_source_create_private(
    "dshow_input",        // Windows DirectShow source id
    "dancehap_webcam_internal",
    webcam_settings
);
obs_data_release(webcam_settings);

// Récupérer les frames (async source)
struct obs_source_frame *frame = obs_source_get_frame(webcam_source);
if (frame) {
    // frame->data[0] = BGRA pixels
    // frame->width, frame->height, frame->linesize
    // Appliquer le matting (MatteEngine) sur ce frame
    // ...
    obs_source_release_frame(webcam_source, frame);
}
```

**Platform-specific source IDs** :
- **Windows** : `"dshow_input"` (DirectShow) — `plugins/win-dshow/win-dshow.cpp`
- **macOS** : `"av_capture_input"` (AVFoundation) — `plugins/mac-avcapture/`

**⚠️ ATTENTION — leçon bug 10 du brief** : `obs_source_get_frame()` est l'API pour les sources **async** (comme les webcams DShow). C'est **correct** ici car la webcam est une source async. Mais attention au `linesize` (stride de ligne) — peut être > `width*4` à cause du padding GPU. Toujours copier ligne par ligne.

**Avantages** :
- Réutilise le code de capture webcam déjà écrit par OBS (DirectShow/AVFoundation)
- Pas besoin de gérer l'API native (DirectShow est complexe)
- Le format de sortie est standardisé (`obs_source_frame` BGRA)

**Inconvénients** :
- Dépend des source IDs `"dshow_input"` / `"av_capture_input"` qui peuvent changer entre versions OBS
- La source privée n'apparaît pas dans l'UI, mais elle consomme des ressources
- Il faut `obs_source_addref`/`obs_source_release` proprement

**Test** : vérifier que `obs_source_create_private("dshow_input", ...)` fonctionne réellement. C'est une API supportée mais peu utilisée. Référence : `obs-studio/libobs/obs.c` — `obs_source_create_private` est la même chose que `obs_source_create` mais sans enregistrer la source dans la liste visible.

#### 2.5.3 Option B — FFmpeg/libavdevice pour ouvrir le device directement

Si l'option A ne fonctionne pas, on peut utiliser FFmpeg/libavdevice (déjà disponible via obs-deps) pour ouvrir le device webcam directement :

```cpp
// Windows (DirectShow via FFmpeg)
AVFormatContext *fmt_ctx = nullptr;
AVDictionary *opts = nullptr;
av_dict_set(&opts, "video_size", "1280x720", 0);
av_dict_set(&opts, "framerate", "30", 0);
avformat_open_input(&fmt_ctx, "video=HD Pro Webcam C920", nullptr, &opts);
// Input format: av_find_input_format("dshow") sur Windows
// Sur macOS: av_find_input_format("avfoundation"), device "default"

// Lire les frames
AVPacket pkt;
while (av_read_frame(fmt_ctx, &pkt) >= 0) {
    // Décode la frame → BGRA
    // Applique le matting
    // Upload en texture
}
```

**Avantages** :
- Pas de dépendance sur les source IDs OBS
- Contrôle total du cycle de vie
- Cross-platform via libavdevice (dshow sur Windows, avfoundation sur macOS)

**Inconvénients** :
- Plus de code à écrire (gestion device, format negotiation, decode)
- FFmpeg est déjà bundlé avec OBS mais l'API libavdevice n'est pas toujours exposée
- Plus fragile que de réutiliser le code OBS

#### 2.5.4 Comment obs-backgroundremoval gère la webcam

**Référence** : `royshil/obs-backgroundremoval/src/background-filter.cpp` (830 lignes)

obs-backgroundremoval est un **filtre** (`OBS_SOURCE_TYPE_FILTER`), pas une source. Il s'applique sur une source webcam **existante** que l'utilisateur a créée dans OBS. Le filtre :
1. Dans `video_render` : capture la source parent via `gs_texrender` + `gs_stagesurface` (pattern sync filter, ADR-008)
2. Dans `video_tick` : `try_lock` l'input buffer, clone l'image, lance l'inférence async
3. Dans `video_render` : applique le masque via `obs_source_process_filter_begin/end` + effect shader custom

**Pour DanceHAP, c'est différent** : on ne veut pas un filtre sur une source externe, on veut une capture interne. Donc le pattern de obs-backgroundremoval s'applique à la partie **matting** (déjà implémentée en Phase 2), mais pas à la **capture webcam**.

#### 2.5.5 Recommandation : Option A (source webcam OBS privée)

**Recommandation** : utiliser `obs_source_create_private("dshow_input"/"av_capture_input", ...)` pour créer une source webcam invisible, puis `obs_source_get_frame()` pour récupérer les frames async.

**Rationale** :
1. Réutilise le code de capture webcam d'OBS (DirectShow sur Windows, AVFoundation sur macOS) — testé, maintenu, gère les device enumeration, format negotiation, reconnexion
2. `obs_source_get_frame()` est l'API correcte pour les sources async (contrairement aux filtres sync où c'est un bug — le brief documente ça)
3. Le format de sortie (`obs_source_frame` BGRA) est directement compatible avec `MatteEngine`
4. Moins de code à maintenir vs FFmpeg/libavdevice direct

**Caveats** :
- Vérifier que les source IDs `"dshow_input"` (Win) et `"av_capture_input"` (macOS) sont stables. Les trouver dans `plugins/win-dshow/` et `plugins/mac-avcapture/` du repo OBS.
- Gérer le cycle de vie : `obs_source_addref` au create, `obs_source_release` au destroy
- La source privée ne doit pas être ajoutée à une scène — elle vit dans le contexte de la source composite

---

### 2.6 Qt6 dock OBS minimal

#### 2.6.1 Comment créer un dock OBS avec Qt6

**API OBS** (`frontend/api/obs-frontend-api.h`, lignes 155-157) :

```c
/* takes QWidget for widget */
EXPORT bool obs_frontend_add_dock_by_id(const char *id, const char *title, void *widget);
EXPORT void obs_frontend_remove_dock(const char *id);
/* takes QDockWidget for dock */
EXPORT bool obs_frontend_add_custom_qdock(const char *id, void *dock);
```

`obs_frontend_add_dock_by_id` prend un `QWidget*` (cast en `void*`) et l'ajoute comme dock dans la fenêtre principale d'OBS. OBS wrappe le QWidget dans un `QDockWidget` automatiquement.

#### 2.6.2 Pattern pour un dock minimal

```cpp
// dancehap_dock.hpp
#pragma once
#include <QWidget>

class QLabel;
class QPushButton;
class QSlider;

class DanceHAPDock : public QWidget {
    Q_OBJECT
public:
    explicit DanceHAPDock(QWidget *parent = nullptr);
    ~DanceHAPDock();

    void setShowName(const QString &name);
    void setTimecode(double current, double total);
    void setMarkers(const QStringList &markers);
    void setDLayerIndicators(bool d1, bool d2, bool d3);

signals:
    void loadShowFileRequested(const QString &path);
    void reloadRequested();
    void playRequested();
    void stopRequested();
    void markerJumpRequested(int markerIndex);

private slots:
    void onLoadClicked();
    void onPlayClicked();
    void onStopClicked();

private:
    QLabel *show_name_label_;
    QLabel *timecode_label_;
    QPushButton *load_btn_;
    QPushButton *reload_btn_;
    QPushButton *play_btn_;
    QPushButton *stop_btn_;
    // ... markers buttons
};
```

```cpp
// dancehap_dock.cpp
#include "dancehap_dock.hpp"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QFileDialog>
#include <QFileInfo>

DanceHAPDock::DanceHAPDock(QWidget *parent) : QWidget(parent) {
    auto *main_layout = new QVBoxLayout(this);

    // Show name
    show_name_label_ = new QLabel("Show: (no show loaded)", this);
    main_layout->addWidget(show_name_label_);

    // Load/Reload buttons
    auto *file_layout = new QHBoxLayout();
    load_btn_ = new QPushButton("Load .dhp...", this);
    reload_btn_ = new QPushButton("Reload", this);
    file_layout->addWidget(load_btn_);
    file_layout->addWidget(reload_btn_);
    main_layout->addLayout(file_layout);

    // Timecode
    timecode_label_ = new QLabel("00:00 / 00:00", this);
    timecode_label_->setAlignment(Qt::AlignCenter);
    main_layout->addWidget(timecode_label_);

    // Transport
    auto *transport_layout = new QHBoxLayout();
    play_btn_ = new QPushButton("▶ Play", this);
    stop_btn_ = new QPushButton("■ Stop", this);
    transport_layout->addWidget(play_btn_);
    transport_layout->addWidget(stop_btn_);
    main_layout->addLayout(transport_layout);

    // Markers (ajoutés dynamiquement)
    // ... marker buttons dans une grid

    connect(load_btn_, &QPushButton::clicked, this, &DanceHAPDock::onLoadClicked);
    connect(play_btn_, &QPushButton::clicked, this, &DanceHAPDock::onPlayClicked);
    connect(stop_btn_, &QPushButton::clicked, this, &DanceHAPDock::onStopClicked);
}

void DanceHAPDock::onLoadClicked() {
    QString path = QFileDialog::getOpenFileName(
        this, "Load DanceHAP Show File", QString(), "DanceHAP Show (*.dhp)");
    if (!path.isEmpty()) {
        emit loadShowFileRequested(path);
        show_name_label_->setText("Show: " + QFileInfo(path).baseName());
    }
}
```

#### 2.6.3 Enregistrement du dock dans le plugin

```cpp
// plugin.cpp — dans obs_module_load()
#include "dancehap_dock.hpp"
#include <obs-frontend-api.h>

static DanceHAPDock *dock = nullptr;

bool obs_module_load(void) {
    // ... register sources ...

    // Create and add dock
    dock = new DanceHAPDock();
    obs_frontend_add_dock_by_id("dancehap_dock", "DanceHAP", dock);

    return true;
}

void obs_module_unload(void) {
    if (dock) {
        obs_frontend_remove_dock("dancehap_dock");
        delete dock;
        dock = nullptr;
    }
}
```

**⚠️ Qt6 avec OBS** : OBS est linké avec Qt6. Le plugin doit utiliser les **mêmes** headers/libs Qt6 qu'OBS. CMake :
```cmake
find_package(Qt6 COMPONENTS Widgets)
target_link_libraries(dancehap PRIVATE Qt6::Widgets)
```

**⚠️ MOC** : comme `DanceHAPDock` utilise `Q_OBJECT`, il faut le moc :
```cmake
set_target_properties(dancehap PROPERTIES AUTOMOC ON)
```

#### 2.6.4 Charger un fichier .dhp via QFileDialog

Voir `onLoadClicked()` ci-dessus. Le `QFileDialog::getOpenFileName` ouvre le dialog natif, filtre sur `*.dhp`, et émet un signal `loadShowFileRequested(path)` qui sera connecté à la source composite pour charger le show file.

---

### 2.7 OBS hotkey API

#### 2.7.1 Enregistrement de hotkeys

**API OBS** (`libobs/obs-hotkey.h`, lignes 145-187) :

```c
// Enregistrer une hotkey frontend (globale)
EXPORT obs_hotkey_id obs_hotkey_register_frontend(
    const char *name,           // identifiant unique
    const char *description,    // description visible dans OBS Settings → Hotkeys
    obs_hotkey_func func,        // callback appelé quand la hotkey est pressée
    void *client_data           // data passé au callback
);

// Enregistrer une paire de hotkeys (ex: Play/Stop)
typedef bool (*obs_hotkey_active_func)(void *data, obs_hotkey_pair_id id, ...);
EXPORT obs_hotkey_pair_id obs_hotkey_pair_register_frontend(
    const char *name0, const char *description0,  // hotkey 1 (ex: "Play")
    const char *name1, const char *description1,  // hotkey 2 (ex: "Stop")
    obs_hotkey_func func0, obs_hotkey_func func1,
    void *data0, void *data1
);

// Désenregistrer
EXPORT void obs_hotkey_unregister(obs_hotkey_id id);
```

**Type du callback** :
```c
typedef void (*obs_hotkey_func)(void *data, obs_hotkey_t *hotkey, bool pressed);
// pressed = true quand la touche est pressée, false quand relâchée
```

#### 2.7.2 Pattern pour mapper 1 hotkey par marker

Le show file `.dhp` contient des markers : `{ "time": 0.0, "name": "Patience" }`. Il faut enregistrer une hotkey par marker, plus Play/Stop.

```cpp
// Dans la source composite
struct HotkeyContext {
    std::vector<obs_hotkey_id> marker_hotkeys;
    obs_hotkey_id play_hotkey;
    obs_hotkey_id stop_hotkey;
    CompositeContext *composite;
};

void register_hotkeys(obs_source_t *source, const ShowFile& show) {
    auto *hk = new HotkeyContext{.composite = ctx};

    // Play/Stop — utiliser obs_hotkey_register_source (lié à la source)
    hk->play_hotkey = obs_hotkey_register_source(source,
        "DanceHAP.Play", "DanceHAP: Play Show",
        [](void *data, obs_hotkey_t *hotkey, bool pressed) {
            if (pressed) static_cast<CompositeContext*>(data)->play();
        },
        ctx);

    hk->stop_hotkey = obs_hotkey_register_source(source,
        "DanceHAP.Stop", "DanceHAP: Stop Show",
        [](void *data, obs_hotkey_t *hotkey, bool pressed) {
            if (pressed) static_cast<CompositeContext*>(data)->stop();
        },
        ctx);

    // 1 hotkey par marker
    for (size_t i = 0; i < show.markers.size(); i++) {
        std::string name = "DanceHAP.Marker." + std::to_string(i);
        std::string desc = "DanceHAP: Jump to " + show.markers[i].name;
        obs_hotkey_id id = obs_hotkey_register_source(source,
            name.c_str(), desc.c_str(),
            [i](void *data, obs_hotkey_t *hotkey, bool pressed) {
                if (pressed) static_cast<CompositeContext*>(data)->jumpToMarker(i);
            },
            ctx);
        hk->marker_hotkeys.push_back(id);
    }
}

void unregister_hotkeys(HotkeyContext *hk) {
    for (auto id : hk->marker_hotkeys)
        obs_hotkey_unregister(id);
    obs_hotkey_unregister(hk->play_hotkey);
    obs_hotkey_unregister(hk->stop_hotkey);
    delete hk;
}
```

**⚠️ Important** : `obs_hotkey_register_source` (lié à la source) est préférable à `obs_hotkey_register_frontend` (global) car les hotkeys sont automatiquement désenregistrées quand la source est détruite. Mais les hotkeys frontend sont visibles dans Settings → Hotkeys même si la source n'existe pas. Pour DanceHAP, `obs_hotkey_register_source` est le bon choix car les hotkeys dépendent du show file chargé.

**Sauvegarde/restauration des bindings** : OBS sauvegarde les bindings hotkey automatiquement via `obs_hotkey_save`/`obs_hotkey_load`. Le plugin n'a rien à faire — les bindings sont persistés dans le profil OBS.

#### 2.7.3 Quand enregistrer les hotkeys

Les hotkeys dépendent du show file (nombre de markers = nombre de hotkeys). Il faut :
1. Au `create()` de la source : enregistrer Play/Stop (toujours présents)
2. Au `load_show_file()` : désenregistrer les anciennes marker hotkeys, enregistrer les nouvelles

---

### 2.8 Projets GitHub de référence

#### 2.8.1 Plugins OBS multi-layer / compositing

| Projet | URL | Pertinence Phase 3 |
|--------|-----|-------------------|
| **obs-studio/obs-source-transition.c** | https://github.com/obsproject/obs-studio/blob/master/libobs/obs-source-transition.c | ⭐⭐⭐ Référence pour crossfade entre 2 sources. Pattern `gs_texrender` double + callback de blend. |
| **obs-studio/plugins/obs-transitions/** | https://github.com/obsproject/obs-studio/tree/master/plugins/obs-transitions | ⭐⭐⭐ Transitions natives (fade, cut, slide, stinger). `transition-fade.c` = crossfade minimal. |
| **royshil/obs-backgroundremoval** | https://github.com/royshil/obs-backgroundremoval (4440 ⭐) | ⭐⭐⭐ Référence pour matting OBS + pattern sync filter + async worker. Déjà étudié Phase 2. |
| **exeldro/obs-shaderfilter** | https://github.com/exeldro/obs-shaderfilter (760 ⭐) | ⭐⭐ Montre comment charger un `.effect` custom et l'appliquer dans `video_render`. |
| **obs-studio/plugins/win-dshow/** | https://github.com/obsproject/obs-studio/tree/master/plugins/win-dshow | ⭐⭐ Référence pour capture webcam Windows (DirectShow). `win-dshow.cpp` = 57Ko. |
| **obs-studio/plugins/mac-avcapture/** | https://github.com/obsproject/obs-studio/tree/master/plugins/mac-avcapture | ⭐⭐ Référence pour capture webcam macOS (AVFoundation). `OBSAVCapture.m` = 60Ko. |

#### 2.8.2 Timeline editors Qt6 (Phase 4, pré-recherche)

| Projet | URL | Pertinence Phase 4 |
|--------|-----|-------------------|
| **asnunes/QTimeLine** | https://github.com/asnunes/QTimeLine (41 ⭐) | ⭐⭐ Timeline widget PyQt5 pour video edition. Adaptable en Qt6/C++. |
| **hasielhassan/QtEditorialTimelineWidget** | https://github.com/hasielhassan/QtEditorialTimelineWidget (30 ⭐) | ⭐⭐ Non-linear editor timeline widget pour Qt. |
| **skyrpex/QxTimeLineEditor** | https://github.com/skyrpex/QxTimeLineEditor (10 ⭐) | ⭐ Timeline editor générique pour Qt. |
| **jeremyabel/Prism** | https://github.com/jeremyabel/Prism (10 ⭐) | ⭐ Timeline clip editor basé sur Qt. |

**Note pour Phase 4** : aucun de ces projets n'est un "timeline compositor" complet (clips + keyframes + curve editor + audio). Ce seront des `QGraphicsScene`/`QGraphicsView` custom à construire. Ces repos servent d'inspiration pour le widget timeline de base.

#### 2.8.3 Implémentations B-spline C++ lightweight

| Projet | URL | Stars | Pertinence |
|--------|-----|-------|------------|
| **bgrimstad/splinter** | https://github.com/bgrimstad/splinter | 444 | Bibliothèque B-spline multivariée. Trop lourde mais algorithme de référence. |
| **chen0040/cpp-spline** | https://github.com/chen0040/cpp-spline | 144 | Spline interpolation C++ header-only. |
| **DannyRuijters/CubicInterpolationCUDA** | https://github.com/DannyRuijters/CubicInterpolationCUDA | 136 | B-spline cubique GPU (CUDA). Algorithme pré-filtré. |

**Recommandation** : implémentation custom (~80 lignes, voir §2.3.2). L'algorithme de Cox-de Boor est trivial et bien documenté. Aucune de ces bibliothèques n'est assez légère pour justifier l'ajout d'une dépendance.

---

## 3. SOURCES

### 3.1 Documentation officielle

- **OBS Studio API docs** : https://obsproject.com/docs
  - `obs-source.h` — `obs_source_info` struct, `OBS_SOURCE_*` flags, `obs_source_create`/`obs_source_create_private`
  - `obs-frontend-api.h` — `obs_frontend_add_dock_by_id`, `obs_frontend_add_event_callback`
  - `obs-hotkey.h` — `obs_hotkey_register_source`, `obs_hotkey_register_frontend`
- **OBS Studio source code** : https://github.com/obsproject/obs-studio
  - `libobs/obs-source.c` — rendering pipeline, `obs_source_main_render`, `obs_source_default_render`
  - `libobs/obs-source-transition.c` — transition/crossfade implementation (1072 lignes)
  - `libobs/obs-scene.h` — scene API
  - `plugins/obs-transitions/transition-fade.c` — crossfade minimal (148 lignes)
  - `plugins/win-dshow/win-dshow.cpp` — capture webcam Windows (57Ko)
  - `plugins/mac-avcapture/OBSAVCapture.m` — capture webcam macOS (60Ko)
- **nlohmann/json** : https://github.com/nlohmann/json (50.2k ⭐)
  - README §CMake — FetchContent pattern, `find_package` pattern
  - API docs — `json::parse()`, `j.at()`, `j.value()`, exception types
- **B-spline math** :
  - Wikipedia "B-spline" — algorithme de Cox-de Boor
  - The NURBS Book (Piegl & Tiller) — référence absolue pour les B-splines

### 3.2 Tutoriels/articles

- **OBS plugin development** :
  - https://obsproject.com/docs/reference-settings.html — obs_data API
  - https://obsproject.com/docs/reference-sources.html — obs_source_info
  - https://obsproject.com/wiki/Plugins-Guide — guide général
- **nlohmann/json integration** :
  - https://github.com/nlohmann/json#embedded-fetchcontent — pattern FetchContent
  - https://github.com/nlohmann/json#cmake — find_package pattern

### 3.3 Projets GitHub de référence (URLs précises)

- https://github.com/obsproject/obs-studio/blob/master/libobs/obs-source-transition.c
- https://github.com/obsproject/obs-studio/blob/master/libobs/obs-source.c
- https://github.com/obsproject/obs-studio/blob/master/plugins/obs-transitions/transition-fade.c
- https://github.com/obsproject/obs-studio/tree/master/plugins/win-dshow
- https://github.com/obsproject/obs-studio/tree/master/plugins/mac-avcapture
- https://github.com/royshil/obs-backgroundremoval
- https://github.com/royshil/obs-backgroundremoval/blob/main/src/background-filter.cpp
- https://github.com/exeldro/obs-shaderfilter
- https://github.com/nlohmann/json
- https://github.com/bgrimstad/splinter (B-spline lib)
- https://github.com/chen0040/cpp-spline (spline interpolation)
- https://github.com/asnunes/QTimeLine (timeline Qt, Phase 4)

### 3.4 Code DanceHAP existant à réutiliser

- `/root/dancehap/src/clip_player.cpp` — moteur de playback HAP (réutilisable pour DLayer 1/3)
- `/root/dancehap/src/ai_matte_filter.cpp` — pattern sync filter OBS + matting (réutilisable pour DLayer 2)
- `/root/dancehap/src/matte_engine.cpp` — engine ONNX 7 modèles (réutilisable pour DLayer 2)
- `/root/dancehap/src/obs_compat.hpp` + `/root/dancehap/src/obs_stub.cpp` — compat shim (à étendre)
- `/root/dancehap/CMakeLists.txt` — pattern FetchContent (Snappy), OBJECT library, stub mode

---

## 4. RISQUES ET CAVEATS

### 4.1 RISQUE HAUT — Capture webcam interne (ADR-011)

**Risque** : `obs_source_create_private("dshow_input", ...)` est une API supportée mais rarement utilisée. Il est possible que la source webcam privée ne produise pas de frames, ou qu'OBS refuse de créer une source DShow en mode privé.

**Mitigation** :
1. **Tester tôt** — faire un spike minimal qui crée une source webcam privée et vérifie qu'elle produit des frames (`obs_source_get_frame()` retourne non-NULL)
2. **Fallback** : si l'option A échoue, utiliser FFmpeg/libavdevice (option B, §2.5.3)
3. **Référence** : `obs-studio/libobs/obs.c` — `obs_source_create_private` est la même implémentation que `obs_source_create` mais avec `is_private=true`. Les sources DShow gèrent ce flag.

**Caveat cross-platform** : les source IDs différent entre Windows (`"dshow_input"`) et macOS (`"av_capture_input"`). Il faut `#ifdef _WIN32` / `#elif __APPLE__` pour choisir le bon ID.

### 4.2 RISQUE MOYEN — Composite effect shader multi-texture

**Risque** : le composite effect shader (§2.2.2) utilise 4 textures (`bg_image`, `bg_image2`, `webcam_image`, `overlay_image`). OBS gère normalement une seule texture `image` via le default effect. Un shader custom avec 4 textures est inhabituel — il faut vérifier que `gs_effect_set_texture` sur des params custom fonctionne correctement.

**Mitigation** :
1. **Charger l'effect dans `update()`** avec `obs_enter_graphics()` (leçon bug 9 du brief)
2. **Pas de `OBS_SOURCE_CUSTOM_DRAW` sans charger son propre effect** (leçon bug 7 du brief)
3. **Tester le shader isolément** avant l'intégration (créer une source de test qui affiche 2 textures blended)

### 4.3 RISQUE MOYEN — Performance du double décodage HAP pendant le crossfade

**Risque** : ADR-013 stipule que les 2 clips sont décodés simultanément pendant le crossfade. Le décodage HAP (Snappy + DXT5→RGBA software) consomme du CPU. 2 décodages en parallèle peuvent saturer le CPU sur les machines modestes.

**Mitigation** :
1. **Mesurer** : benchmark du décodage HAP sur Hephaistos (machine cible)
2. **Thread de décodage** : si le décodage est trop lent, déléguer à un thread worker (comme l'async worker ONNX de Phase 2.5b)
3. **Limitation** : si le CPU sature, réduire la résolution du clip sortant pendant le crossfade (le viewer ne verra pas la différence pendant un fondu)

### 4.4 RISQUE MOYEN — Stub mode pour les nouvelles fonctions OBS

**Risque** : Phase 3 introduit de nouvelles fonctions OBS (`obs_frontend_add_dock_by_id`, `obs_hotkey_register_source`, `obs_source_create_private`, etc.). Le stub mode (`obs_stub.cpp`) ne les implémente pas encore → les tests en stub mode ne compileront pas ou ne valideront pas les signatures.

**Mitigation** (leçon documentée dans le brief) :
1. **Étendre `obs_compat.hpp`** avec les déclarations des nouvelles fonctions
2. **Étendre `obs_stub.cpp`** avec des stubs qui matchent les signatures OBS réelles (types de retour inclus)
3. **Vérifier les signatures** dans les headers OBS réels avant d'écrire le stub
4. **Le stub mode ne catch pas les noms de fonction faux** — un build CI Win+macOS est le seul test de vérité

### 4.5 RISQUE FAIBLE — B-spline evaluation

**Risque** : l'algorithme de Cox-de Boor récursif peut être lent si mal implémenté. Mais pour p=3 et une évaluation par frame à 30fps, c'est négligeable (9 multiplications).

**Mitigation** : utiliser la version itérative de de Boor (§2.3.2) si la récursion pose problème. Tests unitaires sur les edge cases (1 keyframe, 2 keyframes, t hors bornes).

### 4.6 CAVEAT — nlohmann/json et les exceptions

nlohmann/json utilise des exceptions par défaut (`parse_error`, `type_error`, `out_of_range`). Dans un plugin OBS, les exceptions non catchées peuvent crasher OBS. Il faut wrapper tout parsing dans un try/catch et logger les erreurs au lieu de propager.

Alternative : `nlohmann::json::parse(str, nullptr, true)` avec le 3e paramètre `allow_exceptions=false` retourne un JSON invalide au lieu de lancer. Puis vérifier `j.is_discarded()`.

### 4.7 CAVEAT — Qt6 link avec OBS

Le plugin DanceHAP doit linker avec les **mêmes** Qt6 libs qu'OBS. Si OBS est build avec Qt 6.x et le plugin avec Qt 6.y, il peut y avoir des ABI breaks. CMake :
```cmake
find_package(Qt6 COMPONENTS Widgets)
target_link_libraries(dancehap PRIVATE Qt6::Widgets)
set_target_properties(dancehap PROPERTIES AUTOMOC ON)
```
La CI OBS fournit Qt6 via obs-deps. Vérifier que le `find_package(Qt6)` trouve les bons headers.

### 4.8 CAVEAT — Chemins absolus dans le show file

ADR-009 stipule des chemins absolus. Si Jean-Luc déplace le dossier de clips, tous les chemins cassent. **Mitigation** : le dock pourrait offrir une option "relocate clips" (chercher les fichiers manquants). Mais ce n'est pas dans le scope Phase 3 — l'éditeur Phase 4 gérera ça.

### 4.9 CAVEAT — Hotkeys dynamiques et OBS Settings UI

Les hotkeys de markers sont créées dynamiquement (1 par marker, nombre variable). Dans OBS Settings → Hotkeys, elles apparaîtront sous "DanceHAP: Jump to Patience", "DanceHAP: Jump to Shine", etc. Si le show file change (markers ajoutés/supprimés), les anciennes hotkeys deviennent orphelines. **Mitigation** : désenregistrer toutes les marker hotkeys avant d'enregistrer les nouvelles au reload du show file.

---

## 5. RECOMMANDATION

### 5.1 Approche suggérée pour Célestin

**Ordre de développement recommandé** (du moins risqué au plus risqué) :

#### Étape 1 — Parser show file + tests (risque faible, foundation)
1. Ajouter nlohmann/json via FetchContent dans `CMakeLists.txt`
2. Créer `src/show_file.hpp/.cpp` — structs C++ + fonction `parse_show_file(path)` avec validation
3. Créer `tests/unit/test_show_file.cpp` — tests sur des `.dhp` valides et invalides
4. Créer des `.dhp` de test dans `tests/assets/`
5. **Livrable** : parser testé en stub mode, validé par CI

#### Étape 2 — B-spline runtime + tests (risque faible, isolé)
1. Créer `src/bspline.hpp/.cpp` — B-spline cubique clamped (de Boor algorithm, ~80 lignes)
2. Créer `tests/unit/test_bspline.cpp` — tests sur keyframes (linéaire, courbe, edge cases)
3. **Livrable** : B-spline testé en stub mode

#### Étape 3 — Source composite skeleton (risque moyen)
1. Créer `src/dancehap_composite.hpp/.cpp` — `obs_source_info` avec `OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW`
2. Implémenter `create`/`destroy`/`get_width`/`get_height`/`get_name`/`get_defaults`/`get_properties`
3. Implémenter `update` — charger le show file, initialiser les ClipPlayers, la webcam capturer, le MatteEngine
4. Implémenter `video_tick` — timing, évaluation B-spline, tick des ClipPlayers
5. Implémenter `video_render` — compositing via composite effect shader
6. Créer `data/effects/composite.effect` — shader multi-texture
7. Étendre `obs_stub.cpp` pour les nouvelles fonctions OBS
8. **Livrable** : source qui compile et s'enregistre (stub + CI)

#### Étape 4 — Intégration ClipPlayer + crossfade (risque moyen)
1. Intégrer les ClipPlayers existants dans la source composite (DLayer 1 et 3)
2. Implémenter le double buffering pour le crossfade (ADR-013)
3. Implémenter la gestion des markers (saut à un timecode)
4. **Livrable** : DLayer 1 et 3 fonctionnels (sans webcam)

#### Étape 5 — Capture webcam interne + matting (risque haut)
1. **Spike test** : créer une source webcam privée et vérifier qu'elle produit des frames
2. Créer `src/webcam_capturer.hpp/.cpp` — wrapper autour de `obs_source_create_private`
3. Intégrer le MatteEngine existant (Phase 2) sur les frames webcam
4. Brancher DLayer 2 dans la source composite
5. **Livrable** : DLayer 2 fonctionnel (webcam + matting)

#### Étape 6 — Dock Qt6 minimal (risque faible)
1. Créer `src/dancehap_dock.hpp/.cpp` — QWidget avec play/stop/timecode/load/markers
2. Enregistrer le dock dans `plugin.cpp` via `obs_frontend_add_dock_by_id`
3. Connecter les signaux dock → source composite
4. **Livrable** : dock fonctionnel

#### Étape 7 — Hotkeys (risque faible)
1. Enregistrer Play/Stop + 1 hotkey par marker via `obs_hotkey_register_source`
2. Gérer le reload (désenregistrer/reenregistrer)
3. **Livrable** : hotkeys fonctionnelles

#### Étape 8 — Audio routing (risque faible)
1. Router l'audio des ClipPlayers (déjà implémenté Phase 1.5) + audio_tracks du show file
2. **Livrable** : audio complet

#### Étape 9 — Intégration + tests + smoke
1. Tests unitaires complets (stub mode)
2. CI Win+macOS verte
3. Smoke test OBS sur Hephaistos
4. **Livrable** : Phase 3 livrée

### 5.2 Anti-patternes à éviter absolument

1. ❌ **Ne pas étendre `obs_stub.cpp`** en même temps que le code plugin → build stub fail
2. ❌ **Appeler `gs_*` depuis `video_tick`** → écran noir silencieux (leçon bug 2)
3. ❌ **`OBS_SOURCE_CUSTOM_DRAW` sans charger son propre effect** → vidéo transparente (leçon bug 7)
4. ❌ **`gs_effect_create_from_file()` dans `create()`** → shader non chargé (leçon bug 9)
5. ❌ **`obs_source_get_frame()` sur un filtre sync** → déformation + freeze (leçon bug 10) — mais c'est OK pour une source async (webcam)
6. ❌ **Commencer par la webcam** (risque haut) sans avoir la foundation (parser, B-spline, source skeleton) — commencer par le moins risqué

### 5.3 Tests recommandés

| Test | Mode | Description |
|------|------|-------------|
| `test_show_file.cpp` | Stub | Parser .dhp valide + invalide, validation chemins, keyframes |
| `test_bspline.cpp` | Stub | Évaluation B-spline (linéaire, courbe, 1-2 keyframes, t hors bornes) |
| `test_composite_source.cpp` | Stub | Source composite registration, state machine, crossfade logic |
| `test_crossfade.cpp` | Stub | Double buffer, blend math, transition timing |
| `test_dock.cpp` | Stub | Dock signals, load file path |
| `test_hotkeys.cpp` | Stub | Hotkey registration count, marker mapping |

### 5.4 Estimation grossière

| Étape | Complexité | Estimation |
|-------|-------------|------------|
| 1. Parser show file | Faible | 2-3h |
| 2. B-spline runtime | Faible | 1-2h |
| 3. Source composite skeleton | Moyenne | 4-6h |
| 4. ClipPlayer + crossfade | Moyenne | 3-4h |
| 5. Webcam capture + matting | Haute | 6-8h |
| 6. Dock Qt6 | Faible | 2-3h |
| 7. Hotkeys | Faible | 1-2h |
| 8. Audio routing | Faible | 2h |
| 9. Intégration + tests + smoke | Moyenne | 4-6h |
| **Total** | | **25-34h** |

### 5.5 Ressources critiques à consulter avant de coder

1. **LIRE AVANT DE CODER LA SOURCE COMPOSITE** : `libobs/obs-source.c` — comprendre comment OBS appelle `video_render` et avec quels arguments (`obs_source_main_render`, `obs_source_default_render`)
2. **LIRE AVANT DE CODER LE CROSSFADE** : `plugins/obs-transitions/transition-fade.c` (148 lignes) — le pattern crossfade minimal
3. **LIRE AVANT DE CODER LA WEBCAM** : `plugins/win-dshow/win-dshow.cpp` (au moins les 200 premières lignes) — comprendre comment OBS gère la capture DirectShow
4. **LIRE AVANT DE CODER LE MATTING DANS LA SOURCE** : `royshil/obs-backgroundremoval/src/background-filter.cpp` (déjà étudié Phase 2, mais revoir pour l'intégration dans une source composite vs filtre)
5. **LIRE AVANT DE CODER LE DOCK** : `frontend/api/obs-frontend-api.h` — les fonctions `obs_frontend_add_dock_by_id`, `obs_frontend_add_event_callback`

---

**Fin du rapport.**

Célestin, tu as maintenant toute la matière pour démarrer. Commence par l'étape 1 (parser show file) — c'est le moins risqué et la foundation de tout le reste. N'hésite pas à poser des questions complémentaires via Alfred si un point technique n'est pas clair.

*Isidore, Le Bibliothécaire — 30/07/2026*