//=============================================================================
// PSP test EBOOT for the SHAR (SRR2) engine port.
//
// Brings up the full new stack end-to-end on hardware/emulator:
//   radcore (thread/memory/time/file + PSP Memory-Stick drive)
//     -> pddi (GL/EGL device, display, render context)
//       -> Pure3D tContext (via tPlatform) + minimal chunk loaders
//         -> loads a real game .p3d ("test.p3d") off the Memory Stick.
//
// Visual result (so it can be judged on PPSSPP / hardware without a debugger):
//   * pulsing GREEN  = engine came up AND a .p3d loaded through the chunk loader
//   * pulsing RED    = engine came up but the load failed (bad/missing chunks)
//   * pulsing BLUE   = no test.p3d found / load returned a hard fail
// A steady pulse at all proves radcore + pddi + tContext + the frame loop run.
//
// Drawing the loaded tGeometry (camera/shader/vertex-format work) is the next
// iteration; the render loop below is structured so that slots straight in.
//=============================================================================

#include <pspkernel.h>
#include <pspsysmem.h>   // sceKernelMaxFreeMemSize — real heap ceiling at boot
#include <pspdebug.h>
#include <pspctrl.h>
#include <stdio.h>
#include <unistd.h>
#include <math.h>     // sinf — radsound HAL test tone
#include <malloc.h>   // mallinfo() — real heap high-water for the OOM probe

static void mlog(const char* s)
{
    FILE* f = pspDiagFopen("ms0:/shar_main.log", "a");
    if (f) { fputs(s, f); fputc('\n', f); fclose(f); }
}

// Breadcrumb log for the Start-New-Game world path (transition/load/render
// steps). Gated behind PSP_DIAG_LOG like the rest (pspDiagFopen is a no-op when
// diagnostics are off); set PSP_DIAG_LOG=1 to trace the world bring-up.
static void wlog(const char* s)
{
    FILE* f = pspDiagFopen("ms0:/shar_world.log", "a");
    if (f) { fputs(s, f); fputc('\n', f); fclose(f); }
}

// Frontend main-menu selection index (0..5), read by FePure3dObject::Render to
// show only the selected item's glow object (mirrors the original menu). Driven
// by the D-pad below. Default 4 = New Game (Homer glow), the retail default.
int g_pspSelectedGlow = 4;

#include <radmemory.hpp>
#include <radthread.hpp>
#include <radtime.hpp>
#include <radfile.hpp>

#include <radload/radload.hpp>

#include <radsound_hal.hpp>
#include <radsound.hpp>          // radsound core: clip / clipplayer / rsd datasource

#include <pddi/pddi.hpp>

#include <p3d/platform/psp/platform.hpp>
#include <p3d/context.hpp>
#include <p3d/utility.hpp>
#include <p3d/loadmanager.hpp>
#include <p3d/inventory.hpp>
#include <p3d/geometry.hpp>
#include <p3d/shader.hpp>
#include <p3d/texture.hpp>
#include <p3d/drawable.hpp>
#include <p3d/view.hpp>
#include <p3d/pointcamera.hpp>
#include <p3d/matrixstack.hpp>
#include <p3d/scenegraph/scenegraph.hpp>
#include <p3d/anim/skeleton.hpp>
#include <p3d/anim/polyskin.hpp>
#include <p3d/anim/compositedrawable.hpp>
#include <p3d/anim/animate.hpp>          // tAnimationLoader, tFrameControllerLoader
#include <p3d/anim/multicontroller.hpp>  // tMultiControllerLoader
#include <p3d/camera.hpp>                // tCameraLoader (camset framing)
#include <p3d/light.hpp>                 // tLightLoader, tLightGroupLoader
#include <p3d/billboardobject.hpp>       // tBillboardQuadGroupLoader (glows)
#include <p3d/locator.hpp>               // tLocatorLoader
#include <p3d/texturefont.hpp>
#include <p3d/imagefont.hpp>
#include <p3d/png.hpp>
#include <p3d/bmp.hpp>
#include <p3d/targa.hpp>
#include <p3d/sprite.hpp>
#include <p3d/image.hpp>

// Scrooby frontend/UI engine (menus)
#include <App.h>
#include <Project.h>
#include <Screen.h>
#include <Page.h>
#include <Text.h>
#include <Group.h>
#include <Layer.h>
#include <Pure3dObject.h>
#include "FeProject.h"   // to force-clear the frontend's "loading" flag (see PH_MENU)
#include "dsgloaders.h"  // ENTITY_DSG / WORLD_SPHERE_DSG unwrappers (level-1 world)
#include <p3d/anim/multicontroller.hpp>
#include <math.h>

// Main-menu label text (one multi-string Scrooby::Text; SetIndex picks which
// menu-item string is shown). Fetched once the MainMenu screen resolves; driven
// by the D-pad below. The game's CGuiMenu does this in the real frontend, but the
// harness doesn't run the GameFlow, so we replicate the small piece we need.
static Scrooby::Text* g_menuText = NULL;

// Camera intro (mirrors CGuiScreenIntroTransition). CamAndSet ("camset") is the
// room set + the shared menu camera + one long baked animation; the intro camera
// fly-in is the frame window [721..770] of that animation. We play it once then
// hold the idle pose (the pose frame 770 ends on) by not advancing further.
static Scrooby::Pure3dObject* g_camset  = NULL;
static Scrooby::Layer*        g_tvFrame = NULL;   // TV bezel overlay; fades in on intro
static int   g_introState = 0;   // 0=await controller, 1=playing intro, 2=idle/done
static const float FE_INTRO_START = 721.0f;
static const float FE_INTRO_END   = 770.0f;

// Iris wipe ("round black fade"): a full-screen 3D mask object "3dIris" on page
// "IrisCover" driven by the "IrisController" animation. Frame 0 and NumFrames =
// fully open (screen visible); the midpoint = fully closed (black). On menu open
// we start closed (mid) and play to the end so the iris opens to reveal the menu.
static Scrooby::Pure3dObject* g_iris      = NULL;
static Scrooby::Layer*        g_irisLayer = NULL;
static tMultiController*       g_irisMC    = NULL;
static float g_irisFrames = 0.0f;
static int   g_irisState  = 0;   // 0=inactive/absent, 1=revealing, 2=done(hidden)

// Procedural iris ("circle fade") reveal — the console frontend has no iris
// asset (no IrisCover page), so the backend draws a growing-circle black mask
// (pguDrawIrisMask) synced to the intro camera move. g_irisOpen: 0=pinhole
// (closed) .. 1=fully revealed. g_irisRunning gates the overlay draw.
extern "C" void pguDrawIrisMask( float openFraction );
static float g_irisOpen    = 0.0f;
static bool  g_irisRunning = false;
static int   g_irisFrames2 = 0;   // safety frame counter (force-open if intro stalls)

// Menu-item text size (fits between the L/R selection arrows) and TV-frame bezel
// fit factors (X = right edge, larger Y = bottom edge, since design Y 0..480 is
// squashed into 272px). Applied absolutely every frame (see the render loop) —
// a one-shot scale at menu setup was wiped when the Scrooby resources finished
// loading and reset the drawables' matrices.
static const float MENU_TEXT_SCALE = 0.6f;
static const float TVFRAME_FIT_X   = 1.010f;
static const float TVFRAME_FIT_Y   = 1.030f;

// ---- Frontend Homer "gag" cycle ------------------------------------------
// The retail main menu (CGuiScreenMainMenu::UpdateGags) periodically has a
// character perform a gag animation, then returns to idle. The 6 non-Homer gag
// characters (Grandpa/Moleman/Frink/Barney/Nick/Snake/Maggie) are skipped on
// PSP for RAM (FeResourceManager keeps only gaghomer.p3d), so we reproduce the
// Homer gag: idle loop, then one of Homer's gag sub-animations played once, then
// back to idle. Frame ranges mirror the retail HOMER_GAG_ANIMATION table; the
// idle is Homer's neutral loop. Scrooby never advances object animations in this
// harness (FeApp::DrawFrame only Display()s), so we advance Homer's controller
// ourselves each frame — exactly like the camera intro drives camset.
struct HomerGagRange { float start, end; const char* name; };
static const HomerGagRange kHomerGags[] = {
    {   0.0f,  75.0f, "scratch-head" },
    {  75.0f, 145.0f, "scratch-bum"  },
    { 145.0f, 215.0f, "yawn"         },
    { 215.0f, 355.0f, "nightmare"    },
    { 355.0f, 475.0f, "stretch"      },
};
static const int   kNumHomerGags   = (int)(sizeof(kHomerGags) / sizeof(kHomerGags[0]));
static const float HOMER_IDLE_START = 0.0f;
static const float HOMER_IDLE_END   = 60.0f;

static Scrooby::Pure3dObject* g_homer     = NULL;   // gaghomer (Gag0): the GAG poses
static tMultiController*      g_homerMC    = NULL;
static float    g_homerFrames = 0.0f;   // total frames in Homer's gag clip (~476)

// Sleeping Homer (homer.p3d "Homer" on 3dFE): the 61-frame SLEEP idle. The menu
// swaps visibility between this (idle/sleeping, most of the time) and g_homer
// (gaghomer) which does an occasional gag — gaghomer has no neutral/sleep frame.
static Scrooby::Pure3dObject* g_homerSleep   = NULL;
static tMultiController*      g_homerSleepMC = NULL;
static float    g_homerSleepFrames = 0.0f;

static int      g_gagState  = 0;         // 0=await mc, 1=idle loop, 2=playing gag
static float    g_gagTimer  = 0.0f;      // ms elapsed in the current idle
static float    g_gagNextMs = 5000.0f;   // idle time before the next gag (first sooner)
static unsigned g_gagRng    = 0x12345678;// LCG state for gag/timing choices

static inline unsigned GagRand()         // cheap LCG (no rand()/srand needed)
{
    g_gagRng = g_gagRng * 1103515245u + 12345u;
    return (g_gagRng >> 16) & 0x7fff;
}

// Gag playback speed. The clips played back at 1.0 looked too fast, so run them
// (Homer + the other characters) at this relative speed.
static const float GAG_SPEED = 0.5f;

// The OTHER frontend gag characters — Gag1..Gag7 on the "3dFEGags" page
// (Grandpa/Moleman/Frink/Barney/Nick/Snake/Maggie). They now load (un-skipped in
// FeResourceManager) and the menu shows one at a time: reveal its layer, play
// its animation once, hide it, wait, then the next — a second cycle running
// alongside the always-present Homer. Their multicontrollers attach lazily on
// the object's first Render, so we must show a gag BEFORE its controller exists
// and poll for it; a gag that never resolves (didn't load / no RAM) is skipped.
static Scrooby::Page*         g_gagPage = NULL;
static Scrooby::Layer*        g_otherLayer[8] = {0};   // index by GagN (1..7)
static Scrooby::Pure3dObject* g_otherObj[8]   = {0};
static tMultiController*      g_otherMC   = NULL;
static int   g_otherCur   = -1;      // gag index currently on screen (1..7) or -1
static int   g_otherNext  = 1;       // next candidate index to try (1..7, wraps)
static int   g_otherState = 0;       // 0=waiting, 1=starting(poll mc), 2=playing
static int   g_otherPoll  = 0;       // frames spent waiting for the mc to attach
static float g_otherTimer = 0.0f;
static float g_otherNextMs = 9000.0f;// first other-gag ~9 s in

// Set true to render the frontend menu (Scrooby) instead of a model .p3d.
static const bool kMenuMode = true;

// Set true to play a 440Hz sine through the radsound HAL at startup — the
// audible smoke test for the sceAudio software-mixer backend.
static const bool kTestTone = false;

// Set true to load the real game menu stingers (sound/{accept,scroll}.rsd) and
// play them on menu navigation / accept — the first real-asset audio through the
// radsound core (rsd datasource -> clip -> clipplayer -> HAL). Deploy
// content/sound/{accept,scroll}.rsd to ms0:/sound/ alongside the EBOOT.
static const bool kMenuSounds = true;

// Set true to stream the menu background music (sound/music/simpsons_theme.rsd,
// the Simpsons theme, PCM stereo 24kHz ~7.7MB) — too big to load resident, so it
// runs through the radsound streamplayer (small ring buffer refilled from disk).
static const bool kMusic = true;

// Menu stinger clip players (loaded async at startup; ready by the time the menu
// appears). Refs held for the harness lifetime.
static IRadSoundClipPlayer* g_scrollPlayer = NULL;
static IRadSoundClipPlayer* g_acceptPlayer = NULL;

// ---- Start New Game: load & render the Level-1 game world ---------------
// Accepting the menu (X) mirrors the retail New Game path (guiscreenmainmenu
// OnNewGameSelected -> GUI_MSG_QUIT_FRONTEND -> CONTEXT_LOADING_GAMEPLAY ->
// CONTEXT_GAMEPLAY): we tear the frontend down to reclaim RAM, stream in the
// level-1 world, and render it with a free-look camera at the player start.
// Worldsim/physics/character control are not ported yet, so this is a
// fly-through of the real, textured game world rather than playable gameplay.
enum HarnessMode { HM_MENU, HM_WORLD };
static HarnessMode g_mode = HM_MENU;
static bool  g_worldStartRequested = false;   // set when X is pressed on the menu

// Level-1 world files. L1_TERRA is the always-resident base (terrain, baked in
// world space); l1z1 is the Simpsons'-House starting zone whose buildings/props
// are ENTITY_DSG-wrapped meshes (unwrapped by PspEntityDSGLoader). SHAR bakes
// static geometry in world coordinates, so every mesh draws correctly at
// identity. Kept to two files for the ~22MB heap; append zones once we stream.
// The Simpsons'-House region per level.mfk = "l1z1;l1r1;l1r7" + the L1_TERRA
// base. Roads (l1r*) carry the drivable ground/road surface (as ENTITY_DSG
// meshes); without them the foreground ground is missing (sky shows through).
static const char* kWorldFiles[]  = { "art\\L1_TERRA.p3d", "art\\l1z1.p3d",
                                      "art\\l1r1.p3d", "art\\l1r7.p3d" };
static const int   kNumWorldFiles = (int)(sizeof(kWorldFiles) / sizeof(kWorldFiles[0]));

// Player start = the "Simpsons' House" teleport dest from level.mfk.
static const float WORLD_START_X = 220.0f, WORLD_START_Y = 3.5f, WORLD_START_Z = -172.0f;

static int   g_worldPhase    = 0;      // 0=idle, 1=loading, 2=ready
static int   g_worldFileIdx  = 0;
static bool  g_worldLoaded   = false;
static tLoadRequest* g_worldReq = NULL;
static int   g_worldFileBaseline = 0;  // entity count when current file began
static int   g_worldLastCount    = -1; // last observed entity count
static int   g_worldStable       = 0;  // frames the count has held steady
static int   g_worldFileFrames   = 0;  // frames spent on the current file (timeout)
static tView*        g_worldView = NULL;
static tPointCamera* g_worldCam  = NULL;
static rmt::Vector   g_worldCamPos;
static float         g_worldYaw  = 0.0f;

// The level's real render objects, built from its DSG chunks (see dsgloaders.h)
// and rendered with the game's ordering. g_worldEnt = static entities (the
// StaticEntityDSG::Display + mTranslucent split the game uses); g_worldSky =
// world-sphere/backdrop meshes drawn FIRST with depth-write off (as
// WorldRenderLayer does — the sky is not part of the culled scene).
static StaticEntityDSG* g_worldEnt[4096];
static int              g_worldEntCount = 0;
static tGeometry*       g_worldSky[64];
static int              g_worldSkyCount = 0;
void PspAddWorldEntity(StaticEntityDSG* dsg)
{
    if (g_worldEntCount < 4096) g_worldEnt[g_worldEntCount++] = dsg;
}
void PspAddSkyGeo(tGeometry* geo)
{
    if (g_worldSkyCount < 64) g_worldSky[g_worldSkyCount++] = geo;
}
// Ranked-translucent sort scratch (indices into g_worldEnt), sorted far->near.
static int   g_worldTrans[4096];
static int   g_worldTransCount = 0;

// Background music streamer (streamed, not resident) + its file data source.
static IRadSoundStreamPlayer*      g_musicStream    = NULL;
static IRadSoundRsdFileDataSource* g_musicDs        = NULL;
static IRadSoundHalAudioFormat*    g_musicFmt       = NULL;
static bool                        g_musicWasPlaying = false;

// Homer's frontend gag VOICE lines — one per menu gag animation, indexed to
// match kHomerGags below (the retail FE_GAGS_FOR_HOMER order: ScratchHead,
// ScratchBum, Yawn, Nightmare, Stretch). From nis.rcf (sound/nis/), PCM mono
// 24kHz. Playing the matching line when its animation starts keeps them in sync.
static const char* kGagSoundFiles[] = {
    "sound/nis/FE_Gag_Homer_ScratchHead.rsd",  // scratch-head
    "sound/nis/FE_Gag_Homer_ScratchBum.rsd",   // scratch-bum
    "sound/nis/FE_Gag_Homer_Yawn.rsd",         // yawn
    "sound/nis/FE_Gag_Homer_Nightmare.rsd",    // nightmare
    "sound/nis/FE_homer_stretch.rsd",          // stretch
};
static const int kNumGagSounds = (int)(sizeof(kGagSoundFiles) / sizeof(kGagSoundFiles[0]));
static IRadSoundClipPlayer* g_gagPlayers[ 5 ] = { NULL, NULL, NULL, NULL, NULL };

// The OTHER gag characters' voice lines, indexed by GagN (1..7) to match the
// retail FE_GAGS order (Gag1=Grandpa..Gag7=Maggie). These are longer (7-11s) and
// total ~2.4MB, so rather than hold them resident they STREAM one at a time
// through a shared mono streamer (only one other-gag plays at once). From nis.rcf.
static const char* kOtherGagFiles[ 8 ] = {
    NULL,                               // 0 = Homer (handled by resident clips)
    "sound/nis/FE_gag_grandpa.rsd",     // 1 Grandpa
    "sound/nis/FE_gag_moleman.rsd",     // 2 Moleman
    "sound/nis/FE_gag_frink.rsd",       // 3 Frink
    "sound/nis/FE_gag_barney.rsd",      // 4 Barney
    "sound/nis/FE_gag_nick.rsd",        // 5 Nick
    "sound/nis/FE_gag_snake.rsd",       // 6 Snake
    "sound/nis/FE_gag_maggie.rsd",      // 7 Maggie
};
static IRadSoundStreamPlayer*      g_otherGagStream = NULL;
static IRadSoundRsdFileDataSource* g_otherGagDs     = NULL;
static IRadSoundHalAudioFormat*    g_otherGagFmt    = NULL;

// Loads a .rsd file into a clip and returns a ready-to-play clip player. The
// load is async (pumped by radFileService + sound Service each frame), so the
// player is usable a few frames later. Mirrors SoundManager::prepareStartupSounds.
static IRadSoundClipPlayer* HarnessLoadClipPlayer( const char* fn, IRadSoundHalAudioFormat* clipFmt )
{
    IRadSoundRsdFileDataSource* ds = radSoundRsdFileDataSourceCreate( RADMEMORY_ALLOC_DEFAULT );
    ds->AddRef();
    ds->InitializeFromFileName( fn, false, 0, IRadSoundHalAudioFormat::Frames, clipFmt );

    IRadSoundClip* clip = radSoundClipCreate( RADMEMORY_ALLOC_DEFAULT );
    clip->AddRef();
    clip->Initialize( ds, radSoundHalSystemGet()->GetRootMemoryRegion(), false, fn );
    ds->Release();

    IRadSoundClipPlayer* player = radSoundClipPlayerCreate( RADMEMORY_ALLOC_DEFAULT );
    player->AddRef();
    player->SetClip( clip );   // player holds its own ref
    clip->Release();
    player->SetVolume( 1.0f );
    return player;
}

// (Re)starts the streamed menu music from the top. Called once at setup and
// again whenever the stream drains (EOS) — a simple loop for the ~80s theme.
// Re-creates the file data source (rewind) and re-points the persistent streamer.
static void HarnessStartMusic( void )
{
    if ( g_musicStream == NULL ) return;

    if ( g_musicDs != NULL )
    {
        g_musicStream->SetDataSource( NULL );
        g_musicDs->Release();
        g_musicDs = NULL;
    }

    g_musicDs = radSoundRsdFileDataSourceCreate( RADMEMORY_ALLOC_DEFAULT );
    g_musicDs->AddRef();
    g_musicDs->InitializeFromFileName( "sound/music/simpsons_theme.rsd", false, 0,
                                       IRadSoundHalAudioFormat::Frames, g_musicFmt );
    g_musicStream->SetDataSource( g_musicDs );
    g_musicStream->SetVolume( 0.6f );
    g_musicStream->Play();
    g_musicWasPlaying = false;
}

// Streams one other-character gag voice line (GagN, 1..7) through the shared
// gag streamer, played once (non-looping). Re-points the streamer at the new
// file; a still-playing previous line is cut (gags are seconds apart).
static void HarnessPlayOtherGag( int gi )
{
    if ( g_otherGagStream == NULL || gi < 1 || gi > 7 || kOtherGagFiles[ gi ] == NULL )
        return;

    g_otherGagStream->Stop();

    if ( g_otherGagDs != NULL )
    {
        g_otherGagStream->SetDataSource( NULL );
        g_otherGagDs->Release();
        g_otherGagDs = NULL;
    }

    g_otherGagDs = radSoundRsdFileDataSourceCreate( RADMEMORY_ALLOC_DEFAULT );
    g_otherGagDs->AddRef();
    g_otherGagDs->InitializeFromFileName( kOtherGagFiles[ gi ], false, 0,
                                          IRadSoundHalAudioFormat::Frames, g_otherGagFmt );
    g_otherGagStream->SetDataSource( g_otherGagDs );
    g_otherGagStream->SetVolume( 1.0f );
    g_otherGagStream->Play();
}

// Cached-room backdrop (implemented in the GL backend, pddi/gl/glcon.cpp).
extern "C" void pglDrawRoomBackdrop( void );

// Frame-time breakdown (ms) for PSP profiling.
float g_dbgBackdropMs = 0.0f, g_dbgDrawFrameMs = 0.0f, g_dbgEndFrameMs = 0.0f;
int g_pspDiagFrame = -1;  // >=0 => log each drawable child (first menu frames)

// FPS/bottleneck probe. Per-draw counters live in the sceGU backend (gucon.cpp).
// Every kPerfWindow menu frames we log averages to ms0:/shar_perf.log so we can
// see the real split (total frame ms vs DrawFrame ms vs draw-call count) and how
// much a gag character adds. Plain fopen once per window (~3s) is negligible.
extern int g_pspDrawBuffer;
extern int g_pspDrawStream;
extern int g_pspVerts;
// When true, skinned characters re-draw last frame's skinned verts instead of
// re-transforming — set on frames where the character animation didn't advance.
extern bool g_pspSkipReskin;
static const bool kPerfLog = true;
static const int  kPerfWindow = 60;
static int    s_perfFrames = 0;
static float  s_perfFrameMs = 0.0f, s_perfDrawMs = 0.0f, s_perfEndMs = 0.0f;
static int    s_perfDrawCalls = 0, s_perfGagFrames = 0, s_perfVerts = 0;

// Fires when a Scrooby project finishes loading (records the project pointer).
struct HarnessLoadCB : public Scrooby::LoadProjectCallback
{
    volatile bool       done;
    Scrooby::Project*   proj;
    HarnessLoadCB() : done(false), proj(NULL) {}
    void OnProjectLoadComplete( Scrooby::Project* p ) { proj = p; done = true; }
};

// Frontend load phases: show bootup.p3d's loading screen while the large
// frontend.p3d streams in the background, then switch to the main menu.
enum FePhase { PH_BOOTUP, PH_FRONTEND, PH_MENU };
static FePhase        s_fePhase = PH_BOOTUP;
static HarnessLoadCB  s_bootCB;
static HarnessLoadCB  s_feCB;

#include <radmath/radmath.hpp>
#include <math.h>

PSP_MODULE_INFO("SHAR_P3D_TEST", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-1024);   // give all but 1MB of the user heap to malloc/radMemory

//-----------------------------------------------------------------------------
// Exit callback so the PSP HOME button quits cleanly.
//-----------------------------------------------------------------------------
static volatile int s_exit = 0;

static int ExitCallback(int, int, void*) { s_exit = 1; return 0; }
static int CallbackThread(SceSize, void*)
{
    int cbid = sceKernelCreateCallback("ExitCB", ExitCallback, 0);
    sceKernelRegisterExitCallback(cbid);
    sceKernelSleepThreadCB();
    return 0;
}
static void SetupCallbacks()
{
    int th = sceKernelCreateThread("update_thread", CallbackThread, 0x11, 0xFA0, 0, 0);
    if (th >= 0) sceKernelStartThread(th, 0, 0);
}

//-----------------------------------------------------------------------------
int main(int argc, char* argv[])
{
    SetupCallbacks();

    // truncate logs from any previous run
    { FILE* f = pspDiagFopen("ms0:/shar_main.log",  "w"); if (f) fclose(f); }
    { FILE* f = pspDiagFopen("ms0:/shar_gl.log",    "w"); if (f) fclose(f); }
    { FILE* f = pspDiagFopen("ms0:/shar_load.log",  "w"); if (f) fclose(f); }
    { FILE* f = pspDiagFopen("ms0:/shar_thread.log","w"); if (f) fclose(f); }
    { FILE* f = pspDiagFopen("ms0:/shar_drive.log", "w"); if (f) fclose(f); }
    { FILE* f = pspDiagFopen("ms0:/shar_ftt.log",   "w"); if (f) fclose(f); }
    { FILE* f = pspDiagFopen("ms0:/shar_tex.log",   "w"); if (f) fclose(f); }
    { FILE* f = pspDiagFopen("ms0:/shar_chunks.log","w"); if (f) fclose(f); }
    { FILE* f = pspDiagFopen("ms0:/shar_sprite.log","w"); if (f) fclose(f); }
    { FILE* f = pspDiagFopen("ms0:/shar_fe.log",    "w"); if (f) fclose(f); }
    { FILE* f = pspDiagFopen("ms0:/shar_rm.log",    "w"); if (f) fclose(f); }
    { FILE* f = pspDiagFopen("ms0:/shar_gltex.log", "w"); if (f) fclose(f); }
    { FILE* f = pspDiagFopen("ms0:/shar_world.log", "w"); if (f) { fputs("boot\n", f); fclose(f); } }
    mlog("boot");

    // Real available user memory at boot — the true heap ceiling. On a CFW PSP
    // with plugins loaded (see seplugins/), the kernel/VSH/plugins consume much
    // of the 32 MB, so a 1000 may hand the app far less than the ~24 MB the bare
    // partition suggests. PSP_HEAP_SIZE_KB(-1024) then reserves all-but-1MB of
    // THIS. If the frontend load dies at ~this-many MB used, it's OOM at the
    // real ceiling (the post-hoc shar_oom.log can't survive a hard power-off).
    {
        char buf[128];
        sprintf(buf, "mem: MaxFree=%uKB TotalFree=%uKB",
                sceKernelMaxFreeMemSize() / 1024, sceKernelTotalFreeMemSize() / 1024);
        mlog(buf);
    }

    // Log what PPSSPP/PSP gives us — essential for diagnosing path issues.
    {
        char buf[256];
        if (argc > 0 && argv && argv[0])
        {
            sprintf(buf, "argv[0]=%s", argv[0]);
            mlog(buf);
        }
        else
        {
            mlog("argc=0 (no argv)");
        }
        char cwd[256];
        if (getcwd(cwd, sizeof(cwd)))
        {
            sprintf(buf, "cwd=%s", cwd);
            mlog(buf);
        }
        else
        {
            mlog("getcwd failed");
        }
    }

    // --- radcore foundation -------------------------------------------------
    radThreadInitialize();
    radMemoryInitialize();
    radTimeInitialize();
    radFileInitialize(50, 32, RADMEMORY_ALLOC_DEFAULT);
    radLoadInitialize();                             // creates the radLoad singleton (tLoadManager::AddHandler uses it)
    radDriveMount(NULL, RADMEMORY_ALLOC_DEFAULT);    // mount default drive (ms0:)

    // --- radsound HAL (sceAudio software mixer) -----------------------------
    // Brings the audio foundation online: the mixer thread starts here and
    // outputs silence until a voice plays. kTestTone drives a 440Hz sine
    // through the whole HAL path (format -> memory region -> buffer -> voice ->
    // mixer -> sceAudioSRC) to verify the backend audibly on PPSSPP / PSP.
    // The game's SoundManager (menu music, gags, actions) layers on top of this
    // in a later phase.
    radSoundHalSystemInitialize(RADMEMORY_ALLOC_DEFAULT);
    {
        IRadSoundHalSystem::SystemDescription desc;
        desc.m_MaxRootAllocations  = 64;
        desc.m_NumAuxSends         = 0;
        // Holds the resident menu-stinger + gag clips and the music stream's
        // ring buffer. Music itself streams from disk, so this stays small.
        desc.m_ReservedSoundMemory = 2 * 1024 * 1024;
        radSoundHalSystemGet()->Initialize(desc);
        mlog("radsound: HAL initialized (sceAudio mixer running)");
    }

    if (kTestTone)
    {
        const unsigned int rate       = 44100;                 // mixer output rate
        const unsigned int toneFrames = rate;                  // 1 second, looped
        const unsigned int channels   = 1;
        const unsigned int bits       = 16;

        IRadSoundHalAudioFormat* fmt = radSoundHalAudioFormatCreate(RADMEMORY_ALLOC_DEFAULT);
        fmt->AddRef();
        fmt->Initialize(IRadSoundHalAudioFormat::PCM, NULL, rate, channels, bits);

        unsigned int bytes = radSoundHalBufferCalculateMemorySize(
            IRadSoundHalAudioFormat::Bytes, toneFrames,
            IRadSoundHalAudioFormat::Frames, fmt);

        IRadMemoryObject* mem = NULL;
        radSoundHalSystemGet()->GetRootMemoryRegion()->CreateMemoryObject(&mem, bytes, "testtone");

        if (mem != NULL)
        {
            // Synthesize a 440Hz sine straight into sound memory (the mixer
            // reads these bytes directly — no datasource needed).
            short* pcm = (short*)mem->GetMemoryAddress();
            const float twoPiF = 6.2831853f * 440.0f / (float)rate;
            for (unsigned int i = 0; i < toneFrames; i++)
            {
                pcm[i] = (short)(sinf(twoPiF * (float)i) * 12000.0f);
            }

            IRadSoundHalBuffer* buf = radSoundHalBufferCreate(RADMEMORY_ALLOC_DEFAULT);
            buf->AddRef();
            buf->Initialize(fmt, mem, toneFrames, /*looping*/ true, /*streaming*/ false);

            IRadSoundHalVoice* voice = radSoundHalVoiceCreate(RADMEMORY_ALLOC_DEFAULT);
            voice->AddRef();
            voice->SetBuffer(buf);
            voice->SetVolume(0.7f);
            voice->Play();
            mlog("radsound: test tone playing (440Hz loop)");
            // Intentionally leaked for the harness lifetime (refs held).
        }
        fmt->Release();
    }

    // --- Real game menu stingers (accept.rsd / scroll.rsd) ------------------
    // The clip file format for the loose menu .rsd files is PCM mono 24kHz
    // (matches gClipFileAudioFormat in soundnucleus.cpp and the RSD4PCM header).
    if (kMenuSounds)
    {
        IRadSoundHalAudioFormat* clipFmt = radSoundHalAudioFormatCreate(RADMEMORY_ALLOC_DEFAULT);
        clipFmt->AddRef();
        clipFmt->Initialize(IRadSoundHalAudioFormat::PCM, NULL, 24000, 1, 16);

        g_scrollPlayer = HarnessLoadClipPlayer("sound/scroll.rsd", clipFmt);
        g_acceptPlayer = HarnessLoadClipPlayer("sound/accept.rsd", clipFmt);

        // Gag SFX clips (same PCM mono 24kHz format).
        for (int i = 0; i < kNumGagSounds; i++)
            g_gagPlayers[i] = HarnessLoadClipPlayer(kGagSoundFiles[i], clipFmt);

        clipFmt->Release();
        mlog("radsound: menu stinger + gag clips requested");
    }

    // --- Menu background music (streamed) ----------------------------------
    if (kMusic)
    {
        g_musicFmt = radSoundHalAudioFormatCreate(RADMEMORY_ALLOC_DEFAULT);
        g_musicFmt->AddRef();
        g_musicFmt->Initialize(IRadSoundHalAudioFormat::PCM, NULL, 24000, 2, 16);  // stereo

        g_musicStream = radSoundStreamPlayerCreate(RADMEMORY_ALLOC_DEFAULT);
        g_musicStream->AddRef();
        // ~1s stereo ring buffer, refilled from disk each Service().
        g_musicStream->Initialize(g_musicFmt, 1000, IRadSoundHalAudioFormat::Milliseconds,
                                  radSoundHalSystemGet()->GetRootMemoryRegion(), "fe_music");
        HarnessStartMusic();
        mlog("radsound: menu music streaming (simpsons_theme)");

        // Shared streamer for the other characters' (longer) gag voice lines.
        g_otherGagFmt = radSoundHalAudioFormatCreate(RADMEMORY_ALLOC_DEFAULT);
        g_otherGagFmt->AddRef();
        g_otherGagFmt->Initialize(IRadSoundHalAudioFormat::PCM, NULL, 24000, 1, 16);  // mono
        g_otherGagStream = radSoundStreamPlayerCreate(RADMEMORY_ALLOC_DEFAULT);
        g_otherGagStream->AddRef();
        g_otherGagStream->Initialize(g_otherGagFmt, 1000, IRadSoundHalAudioFormat::Milliseconds,
                                     radSoundHalSystemGet()->GetRootMemoryRegion(), "fe_othergag");
        mlog("radsound: other-character gag streamer ready");
    }

    // --- Pure3D platform + context (pddi device/display/context inside) -----
    tPlatform* platform = tPlatform::Create(NULL);

    tContextInitData init;
    init.xsize = 480;
    init.ysize = 272;
    init.bpp   = 32;

    tContext* ctx = platform->CreateContext(&init);
    mlog(ctx ? "context created" : "context NULL");

    // Files loaded in order. Shared character assets (global.p3d: the
    // char_swatches / eyeball texture atlas) MUST load before any model that
    // references them — shader->texture binding is resolved at load time, so the
    // atlas has to already be in the inventory. If global.p3d is absent the load
    // just cancels and we move on (harmless for non-character models).
    static const char* kFiles[]  = { "global.p3d", "test.p3d" };
    static const int   kNumFiles = (int)(sizeof(kFiles) / sizeof(kFiles[0]));
    int   fileIdx    = 0;
    int   flushCount = 0;
    bool  allLoaded  = false;
    bool  curFileDone = false;
    tLoadRequest* req = NULL;   // current file's async request

    // Start an async load for one file and wake the worker once. We build the
    // request by hand (p3d::loadAsync's SetCallback(NULL) null-derefs, and a
    // sync open deadlocks on WaitForCompletion), then pump it in the loop.
    auto startLoad = [&](const char* fn) -> tLoadRequest*
    {
        mlog(fn);
        radMemoryAllocator old = ::radMemorySetCurrentAllocator(RADMEMORY_ALLOC_TEMP);
        tFile* lf = p3d::openFile(fn, false);
        tLoadRequest* r = new tLoadRequest(lf);
        ::radMemorySetCurrentAllocator(old);
        r->SetAsync(true);
        p3d::loadManager->Load(r);
        p3d::loadManager->SwitchTask();   // wake the worker once
        return r;
    };

    if (ctx)
    {
        // --- minimal chunk loaders (NOT InstallDefaultLoaders, which would
        //     force-link the excluded subsystems) --------------------------
        tP3DFileHandler* p3dh = new tP3DFileHandler;
        p3d::loadManager->AddHandler(p3dh, "p3d");
        p3dh->AddHandler(new tGeometryLoader);
        p3dh->AddHandler(new tShaderLoader);
        p3dh->AddHandler(new tTextureLoader);
        p3dh->AddHandler(new Scenegraph::Loader);        // multi-mesh scene-graph models
        p3dh->AddHandler(new tSkeletonLoader);           // skeleton (joint bind pose)
        p3dh->AddHandler(new tPolySkinLoader);           // skinned meshes
        p3dh->AddHandler(new tCompositeDrawableLoader);  // characters + vehicles
        p3dh->AddHandler(new tTextureFontLoader);        // fonts (menu text)
        p3dh->AddHandler(new tImageFontLoader);
        p3dh->AddHandler(new tImageLoader);              // Texture::IMAGE
        p3dh->AddHandler(new tSpriteLoader);             // Texture::SPRITE (menu 2D images!)
        // Scene loaders — required by the frontend menu's 3D room scene.
        p3dh->AddHandler(new tCameraLoader);             // P3D_CAMERA (camset framing)
        p3dh->AddHandler(new tLightLoader);              // LIGHT (scene lighting)
        p3dh->AddHandler(new tLightGroupLoader);         // P3D_LIGHT_GROUP
        p3dh->AddHandler(new tBillboardQuadGroupLoader); // QUAD_GROUP (glow sprites)
        p3dh->AddHandler(new tLocatorLoader);            // LOCATOR (reference points)
        p3dh->AddHandler(new tAnimationLoader);          // ANIMATION (keyframe data)
        p3dh->AddHandler(new tFrameControllerLoader);    // FRAME_CONTROLLER
        p3dh->AddHandler(new tMultiControllerLoader);    // P3D_MULTI_CONTROLLER (drives FCs)

        // Image FILE handlers — the frontend loads sprites from standalone
        // .png/.bmp/.tga files (e.g. resource/images/gamelogo.png). Without
        // these, every menu image sprite is blank.
        p3d::loadManager->AddHandler(new tPNGHandler,   "png");
        p3d::loadManager->AddHandler(new tBMPHandler,   "bmp");
        p3d::loadManager->AddHandler(new tTargaHandler, "tga");

        // DSG unwrappers: a level zone stores its world geometry inside
        // ENTITY_DSG / WORLD_SPHERE_DSG chunks (game-specific), each wrapping a
        // Pure3D MESH. These handlers pull the inner tGeometry into the inventory
        // so the world renders through the same path as any other tGeometry.
        {
            tGeometryLoader* dsgGeo = new tGeometryLoader;
            p3dh->AddHandler(new PspEntityDSGLoader(dsgGeo));
            p3dh->AddHandler(new PspWorldSphereLoader(dsgGeo));
        }

        ctx->SetClearMask(PDDI_BUFFER_COLOUR | PDDI_BUFFER_DEPTH);
        ctx->SetClearDepth(1.0f);
        ctx->SetClearColour(tColour(24, 24, 40));

        if (kMenuMode)
        {
            // Scrooby::App::GetInstance() creates the frontend app and registers
            // the PROJECT + text-bible chunk handlers on our p3d file handler.
            mlog("scrooby: GetInstance");
            Scrooby::App* app = Scrooby::App::GetInstance();
            mlog(app ? "scrooby: app ok" : "scrooby: app NULL");
            // Two-project load: bootup first (its PROJECT chunk must parse
            // before the frontend's — loading frontend as the very first project
            // crashes in the Scrooby PROJECT-chunk handler), then frontend.
            app->LoadProject("art\\frontend\\scrooby\\bootup.p3d", &s_bootCB);
            mlog("scrooby: LoadProject(bootup) queued");
        }
        else
        {
            req = startLoad(kFiles[fileIdx]);   // begin with global.p3d
        }
    }
    // Controller: digital mode is enough for menu navigation (D-pad + buttons).
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);

    mlog("entering render loop");

    // --- scene state (built once the load finishes) -------------------------
    static const int MAX_GEOS = 512;
    tGeometry*   geos[ MAX_GEOS ];
    int          nGeos = 0;
    tView*       view  = NULL;
    tPointCamera* cam  = NULL;
    Scenegraph::Scenegraph* scene = NULL;   // set if the file has a scene graph
    tCompositeDrawable*     composite = NULL; // set if the file is a character/vehicle
    rmt::Vector  sceneCentre( 0, 0, 0 );
    float        sceneRadius = 10.0f;
    bool         sceneReady  = false;
    int          buildTries  = 0;

    // --- render loop --------------------------------------------------------
    unsigned frame = 0;
    float    orbitAng = 0.0f;   // model-viewer camera spin, time-integrated
    bool loggedDone = false;
    // Real per-frame delta (ms), so animation speed is decoupled from the
    // render rate: the harness must feed *elapsed* time to the Pure3D/Scrooby
    // controllers (they advance by deltaTime*fps), exactly like the desktop
    // game loop does with radTimeGetMilliseconds() (game.cpp:482). Feeding a
    // fixed 16 ms made animation track the frame rate (fast on PPSSPP @60,
    // slow-mo on real PSP @20). sceKernelGetSystemTimeWide() is microseconds.
    unsigned long long prevTimeUs = sceKernelGetSystemTimeWide();
    while (!s_exit)
    {
        unsigned long long nowTimeUs = sceKernelGetSystemTimeWide();
        float deltaMs = (float)(nowTimeUs - prevTimeUs) / 1000.0f;
        prevTimeUs = nowTimeUs;
        // Clamp hitches / debugger stalls (mirrors guisystem.cpp:508); also
        // guards the very first frame's bogus delta.
        if (deltaMs > 100.0f || deltaMs < 0.0f) deltaMs = 20.0f;

        // Character animation runs full-rate (smooth). The skip-reskin experiment
        // (advance/skin every other frame) gave NO fps gain on real hardware —
        // per-vertex skinning is not the bottleneck (it's GPU fillrate + per-draw
        // state), so it's disabled to keep the animation smooth.
        float charDelta = deltaMs;
        // Skip per-vertex CPU re-skinning every frame. The PSP skinned VBO is
        // built ONCE (bind pose) and never refilled, so the re-skin never reached
        // the GE — it was pure wasted work. Confirmed on hardware: fps improved
        // and the menu characters (driven by their joint/composite transforms)
        // still animate. NOTE: when the full game needs true vertex deformation,
        // this must be re-enabled together with a per-frame VBO refill.
        g_pspSkipReskin = true;

        // Service the sound HAL each frame (drives streamplayer refills etc.).
        radSoundHalSystemGet()->Service();
        radSoundHalSystemGet()->ServiceOncePerFrame();

        // Loop the background music: once it has actually started, a drop back to
        // not-playing means the ~80s stream hit EOS, so restart it from the top.
        // Only while in the menu — Start New Game stops the music for good.
        if (g_musicStream && g_mode == HM_MENU)
        {
            if (g_musicStream->IsPlaying())
                g_musicWasPlaying = true;
            else if (g_musicWasPlaying)
                HarnessStartMusic();
        }

        int pulse = (frame & 0x3F);
        if (frame & 0x40) pulse = 0x3F - pulse;   // triangle wave 0..63

        // --- Scrooby frontend menu path ------------------------------------
        if (kMenuMode)
        {
            // Drive the resource-manager loads (Scrooby queues tLoadRequests):
            radFileService();                 // file IO
            p3d::loadManager->SwitchTask();   // process loads + fire callbacks/dump
            sceKernelDelayThread(2000);

            Scrooby::App* app = Scrooby::App::GetInstance();

            // === Start New Game -> load & render the Level-1 game world =====
            if (g_worldStartRequested && g_mode == HM_MENU)
            {
                g_worldStartRequested = false;
                g_mode = HM_WORLD;
                mlog("world: New Game accepted -> loading level 1");
                wlog("STEP transition-begin");

                // Silence the menu audio. We do NOT tear the frontend down:
                // Scrooby's App::UnloadProject crashes on the PSP build while
                // freeing the frontend project's resources. Instead we just stop
                // drawing the menu (the HM_WORLD path continues before the menu
                // render). The world's DSG loaders build a fresh set of render
                // objects (g_worldEnt / g_worldSky) — the resident frontend meshes
                // are never in those lists, so nothing stray draws in the world.
                if (g_musicStream)    g_musicStream->Stop();
                if (g_otherGagStream) g_otherGagStream->Stop();
                g_worldEntCount = 0; g_worldSkyCount = 0;
                wlog("STEP audio-stopped (frontend kept resident)");
            }

            if (g_mode == HM_WORLD)
            {
                // --- drive the world file load sequence --------------------
                // The load request frees itself on completion (its internal
                // callback dumps objects into p3d::inventory then destroys the
                // request), so polling req->GetState() is unreliable — we caught
                // it going LOADING->freed. Instead we pump the loader and watch
                // the inventory geometry count: when it has grown past this
                // file's baseline and then held steady for a while, the file's
                // objects are all dumped and we advance to the next file.
                if (g_worldPhase == 0)
                {
                    wlog("STEP load-start file0");
                    g_worldFileIdx = 0; g_worldLoaded = false; g_worldPhase = 1;
                    g_worldFileBaseline = 0; g_worldLastCount = -1;
                    g_worldStable = 0; g_worldFileFrames = 0;
                    g_worldReq = startLoad(kWorldFiles[0]);
                    mlog("world: load start");
                    { char b[64]; sprintf(b, "STEP file0 req=%p", (void*)g_worldReq); wlog(b); }
                }
                else if (g_worldPhase == 1)
                {
                    // Pump the loader hard while the world streams (a raw request
                    // can otherwise be starved by the single SwitchTask/frame).
                    radFileService();
                    p3d::loadManager->SwitchTask();
                    radFileService();
                    p3d::loadManager->SwitchTask();

                    // The DSG loaders build render objects as chunks parse, so
                    // watch the entity+sky count: once it has grown past this
                    // file's baseline and held steady, the file is fully parsed.
                    int count = g_worldEntCount + g_worldSkyCount;

                    if (count == g_worldLastCount) g_worldStable++;
                    else { g_worldStable = 0; g_worldLastCount = count; }
                    g_worldFileFrames++;

                    static int s_hb = 0;
                    if ((s_hb++ % 30) == 0)
                    { char b[96]; sprintf(b, "STEP hb file%d ent=%d sky=%d base=%d stable=%d fr=%d",
                        g_worldFileIdx, g_worldEntCount, g_worldSkyCount, g_worldFileBaseline, g_worldStable, g_worldFileFrames); wlog(b); }

                    // File done when new entities appeared and settled (>=45
                    // steady frames), or a hard timeout (~1800 frames) so a file
                    // with no renderables can't wedge the sequence.
                    bool fileDone = (count > g_worldFileBaseline && g_worldStable >= 45)
                                    || (g_worldFileFrames > 1800);
                    if (fileDone)
                    {
                        g_worldFileIdx++;
                        if (g_worldFileIdx < kNumWorldFiles)
                        {
                            { char b[64]; sprintf(b, "STEP file%d start (base=%d)", g_worldFileIdx, count); wlog(b); }
                            g_worldFileBaseline = count;
                            g_worldLastCount = -1; g_worldStable = 0; g_worldFileFrames = 0;
                            g_worldReq = startLoad(kWorldFiles[g_worldFileIdx]);
                        }
                        else
                        {
                            g_worldLoaded = true; g_worldReq = NULL;
                            mlog("world: all files loaded"); wlog("STEP all-files-loaded");
                        }
                    }
                    if (g_worldLoaded)
                    {
                        g_worldCam = new tPointCamera; g_worldCam->AddRef();
                        g_worldCam->SetFOV(rmt::DegToRadian(65.0f), 480.0f / 272.0f);
                        g_worldCam->SetNearPlane(0.5f);
                        g_worldCam->SetFarPlane(3000.0f);
                        g_worldView = new tView; g_worldView->AddRef();
                        g_worldView->SetCamera(g_worldCam);
                        g_worldView->SetClearColour(tColour(96, 160, 224));   // sky
                        g_worldView->SetClearMask(PDDI_BUFFER_COLOUR | PDDI_BUFFER_DEPTH);
                        g_worldView->SetAmbientLight(tColour(255, 255, 255)); // no lights loaded
                        g_worldCamPos.Set(WORLD_START_X, WORLD_START_Y + 6.0f, WORLD_START_Z);
                        g_worldYaw = 0.0f;
                        ctx->SetClearMask(0);   // the view owns the clear
                        g_worldPhase = 2;
                        {
                            char b[64]; sprintf(b, "world: ready, %d entities %d sky", g_worldEntCount, g_worldSkyCount); mlog(b);
                            sprintf(b, "STEP scene-ready ent=%d sky=%d", g_worldEntCount, g_worldSkyCount); wlog(b);
                        }
                    }
                }

                // --- render (phase 2) or a loading pulse -------------------
                if (ctx && g_worldPhase == 2 && g_worldView)
                {
                    // Free-look camera: D-pad translates on the ground plane,
                    // shoulders turn, triangle/cross raise/lower.
                    SceCtrlData pad; sceCtrlPeekBufferPositive(&pad, 1);
                    float mv  = deltaMs * 0.05f;     // world units / ms
                    float rot = deltaMs * 0.0025f;   // radians / ms
                    if (pad.Buttons & PSP_CTRL_LTRIGGER) g_worldYaw -= rot;
                    if (pad.Buttons & PSP_CTRL_RTRIGGER) g_worldYaw += rot;
                    float fx = rmt::Sin(g_worldYaw), fz = rmt::Cos(g_worldYaw);
                    float rx = fz, rz = -fx;   // right = forward rotated -90
                    if (pad.Buttons & PSP_CTRL_UP)    { g_worldCamPos.x += fx * mv; g_worldCamPos.z += fz * mv; }
                    if (pad.Buttons & PSP_CTRL_DOWN)  { g_worldCamPos.x -= fx * mv; g_worldCamPos.z -= fz * mv; }
                    if (pad.Buttons & PSP_CTRL_LEFT)  { g_worldCamPos.x -= rx * mv; g_worldCamPos.z -= rz * mv; }
                    if (pad.Buttons & PSP_CTRL_RIGHT) { g_worldCamPos.x += rx * mv; g_worldCamPos.z += rz * mv; }
                    if (pad.Buttons & PSP_CTRL_TRIANGLE) g_worldCamPos.y += mv;
                    if (pad.Buttons & PSP_CTRL_CROSS)    g_worldCamPos.y -= mv;
                    g_worldCam->SetPosition(g_worldCamPos);
                    rmt::Vector tgt(g_worldCamPos.x + fx * 10.0f, g_worldCamPos.y, g_worldCamPos.z + fz * 10.0f);
                    g_worldCam->SetTarget(tgt);

                    static bool s_worldFirstDraw = true;
                    if (s_worldFirstDraw)
                    {
                        wlog("STEP first-render-begin");
                        FILE* gf = pspDiagFopen("ms0:/shar_wgeo.log", "w");
                        if (gf)
                        {
                            for (int i = 0; i < g_worldEntCount; i++)
                            {
                                rmt::Box3D bx; g_worldEnt[i]->GetBoundingBox(&bx);
                                const char* nm = g_worldEnt[i]->GetName();
                                fprintf(gf, "%-26s a=%d sz=(%.0f,%.0f,%.0f) c=(%.0f,%.0f,%.0f)\n",
                                        nm ? nm : "(null)", g_worldEnt[i]->mTranslucent ? 1 : 0,
                                        bx.high.x-bx.low.x, bx.high.y-bx.low.y, bx.high.z-bx.low.z,
                                        (bx.high.x+bx.low.x)*0.5f, (bx.high.y+bx.low.y)*0.5f, (bx.high.z+bx.low.z)*0.5f);
                            }
                            fclose(gf);
                        }
                    }

                    // Camera ref position + view direction, for translucent SetRank.
                    rmt::Vector camPos = g_worldCamPos;
                    rmt::Vector camDir(fx, 0.0f, fz);

                    ctx->BeginFrame();
                    g_worldView->BeginRender();

                    // Pass 0: the sky / world-sphere backdrop, drawn first with
                    // depth-write disabled so the scene always composites over it
                    // (mirrors WorldRenderLayer: the sphere is outside the tree).
                    if (g_worldSkyCount > 0)
                    {
                        p3d::pddi->SetZWrite(false);
                        for (int i = 0; i < g_worldSkyCount; i++)
                            g_worldSky[i]->Display();
                        p3d::pddi->SetZWrite(true);
                    }

                    // Pass 1: opaque static entities (real StaticEntityDSG::Display).
                    int drawn = 0;
                    g_worldTransCount = 0;
                    for (int i = 0; i < g_worldEntCount; i++)
                    {
                        if (g_worldEnt[i]->mTranslucent)
                        {
                            // Rank now (distance along view) and defer to pass 2.
                            g_worldEnt[i]->SetRank(camPos, camDir);
                            if (g_worldTransCount < 4096) g_worldTrans[g_worldTransCount++] = i;
                        }
                        else { g_worldEnt[i]->Display(); drawn++; }
                    }

                    // Pass 2: translucent entities, sorted far->near (insertion
                    // sort — the translucent set is small), so blending composites
                    // back-to-front like WorldScene::RenderTranslucent.
                    for (int a = 1; a < g_worldTransCount; a++)
                    {
                        int v = g_worldTrans[a], b = a;
                        while (b > 0 && g_worldEnt[g_worldTrans[b - 1]]->Rank() < g_worldEnt[v]->Rank())
                        { g_worldTrans[b] = g_worldTrans[b - 1]; b--; }
                        g_worldTrans[b] = v;
                    }
                    for (int i = 0; i < g_worldTransCount; i++) { g_worldEnt[g_worldTrans[i]]->Display(); drawn++; }

                    g_worldView->EndRender();
                    ctx->EndFrame(true);
                    if (s_worldFirstDraw)
                    { char b[80]; sprintf(b, "STEP first-render-ok ent=%d trans=%d sky=%d", drawn, g_worldTransCount, g_worldSkyCount); wlog(b); s_worldFirstDraw = false; }
                }
                else if (ctx)
                {
                    ctx->SetClearColour(tColour(0, 0, 40 + pulse * 3));   // loading pulse
                    ctx->BeginFrame();
                    ctx->EndFrame(true);
                }
                frame++;
                continue;
            }

            // Menu navigation: D-pad up/down moves the selection (0..5, wrapping),
            // which FePure3dObject::Render uses to show only that item's glow —
            // mirroring the original main menu. Edge-detected so one press = one
            // step. (Full Scrooby text-item highlight + Accept action come with the
            // GameFlow frontend port; this drives the glow selection for now.)
            if (s_fePhase == PH_MENU)
            {
                SceCtrlData pad;
                sceCtrlPeekBufferPositive(&pad, 1);
                static unsigned s_prevBtns = 0;
                unsigned pressed = pad.Buttons & ~s_prevBtns;   // rising edges
                // Navigate over the number of menu strings (falls back to 6 glows).
                int itemCount = (g_menuText && g_menuText->GetNumOfStrings() > 0)
                                ? g_menuText->GetNumOfStrings() : 6;
                if (pressed & PSP_CTRL_LEFT)
                    g_pspSelectedGlow = (g_pspSelectedGlow + itemCount - 1) % itemCount;
                if (pressed & PSP_CTRL_RIGHT)
                    g_pspSelectedGlow = (g_pspSelectedGlow + 1) % itemCount;

                // Real game menu SFX: scroll stinger on navigation, accept on X.
                if ((pressed & (PSP_CTRL_LEFT | PSP_CTRL_RIGHT | PSP_CTRL_UP | PSP_CTRL_DOWN))
                    && g_scrollPlayer)
                {
                    g_scrollPlayer->Stop();   // retrigger from the start
                    g_scrollPlayer->Play();
                }
                if ((pressed & PSP_CTRL_CROSS) && g_acceptPlayer)
                {
                    g_acceptPlayer->Stop();
                    g_acceptPlayer->Play();
                }
                s_prevBtns = pad.Buttons;

                // Accept (X) on the menu = Start New Game: request the world
                // transition (handled next frame, before the menu logic runs).
                if ((pressed & PSP_CTRL_CROSS) && g_mode == HM_MENU)
                    g_worldStartRequested = true;

                // Drive the label + highlight, mirroring CGuiMenu:
                //  - SetIndex  : show the selected item's string (text changes)
                //  - SetColour : yellow highlight (the shown item is the selection)
                // The size + throb animation is applied later, right before
                // DrawFrame (see MENU_TEXT_SCALE), because a scale set here is
                // overwritten before the menu draws.
                if (g_menuText)
                {
                    g_menuText->SetIndex(g_pspSelectedGlow);
                    g_menuText->SetColour(tColour(255, 255, 0));
                }

                // Camera intro state machine (mirrors CGuiScreenIntroTransition +
                // CGuiScreen::StartTransitionAnimation): play CamAndSet's baked
                // animation window [721..770] once — the menu fly-in — fading the
                // TV frame in over the last 20 frames, then hold the idle pose by
                // not advancing further. CamAndSet's controller is attached lazily
                // on its first Render, so g_camset->GetMultiController() is NULL for
                // the first frame; we start once it's available.
                if (g_camset)
                {
                    tMultiController* mc = g_camset->GetMultiController();
                    if (mc)
                    {
                        if (g_introState == 0)
                        {
                            mc->SetFrameRange(FE_INTRO_START, FE_INTRO_END);
                            mc->Reset();                 // frame 0 (== 721 absolute)
                            g_introState = 1;
                            mlog("scrooby: camera intro start [721..770]");
                        }
                        else if (g_introState == 1)
                        {
                            mc->Advance(deltaMs);
                            float total = mc->GetNumFrames();     // 770-721
                            float cur   = mc->GetFrame();
                            float remaining = total - cur;
                            if (g_tvFrame)
                            {
                                const float FADE = 20.0f;
                                float a = (remaining < FADE) ? (1.0f - remaining / FADE) : 0.0f;
                                if (a < 0.0f) a = 0.0f;
                                if (a > 1.0f) a = 1.0f;
                                a *= a;                  // ease-in, matches original
                                g_tvFrame->SetAlpha(a);
                            }
                            // Iris reveal tracks the intro camera progress: closed
                            // (pinhole) at the start, fully open when the camera
                            // settles — "the circle extends with the camera move".
                            if (g_irisRunning && total > 0.5f)
                            {
                                float f = cur / total;
                                if (f < 0.0f) f = 0.0f;
                                if (f > 1.0f) f = 1.0f;
                                g_irisOpen = f;
                            }
                            if (cur >= total - 0.5f)
                            {
                                if (g_tvFrame) g_tvFrame->SetAlpha(1.0f);
                                g_introState = 2;        // hold idle pose (stop advancing)
                                g_irisOpen = 1.0f;       // fully revealed
                                g_irisRunning = false;   // stop drawing the mask
                                mlog("scrooby: camera intro done -> idle");
                            }
                        }
                    }
                }

                // Iris reveal: the attached IrisController advances via the normal
                // Update path; once it opens fully (last frame), hide the iris
                // layer so the black mask is gone for the rest of the menu.
                if (g_irisState == 1 && g_irisMC)
                {
                    if (g_irisMC->LastFrameReached() ||
                        g_irisMC->GetFrame() >= g_irisFrames - 0.5f)
                    {
                        if (g_irisLayer) g_irisLayer->SetVisible(false);
                        g_irisState = 2;
                        mlog("scrooby: iris reveal done");
                    }
                }

                // Homer menu behaviour. Retail: Homer SLEEPS (homer.p3d, a 61-frame
                // loop) most of the time and occasionally does a gag (gaghomer.p3d,
                // a 476-frame sequence of 5 gags). We reproduce that by swapping
                // VISIBILITY between the two loaded Homer objects: g_homerSleep
                // (sleeping idle) shown while waiting, g_homer (gaghomer) shown for
                // the gag. Each plays its own animation. If homer.p3d didn't load
                // (g_homerSleep NULL) we fall back to holding gaghomer static.
                if (g_homer)
                {
                    if (g_gagState == 0)
                    {
                        // Controllers attach lazily on first Render, so both
                        // objects must be visible while we poll for them.
                        if (g_homerSleep) g_homerSleep->SetVisible(true);
                        tMultiController* mc  = g_homer->GetMultiController();
                        tMultiController* smc = g_homerSleep ? g_homerSleep->GetMultiController() : (tMultiController*)1;
                        if (mc && smc)
                        {
                            g_homerMC = mc;
                            g_homerFrames = mc->GetNumFrames();   // gag clip (~476)
                            mc->SetRelativeSpeed(GAG_SPEED);
                            mc->SetCycleMode(FORCE_NON_CYCLIC);
                            mc->SetFrameRange(0.0f, g_homerFrames);
                            mc->SetFrame(0.0f);
                            mc->Advance(0.0f);

                            if (g_homerSleep)
                            {
                                g_homerSleepMC = g_homerSleep->GetMultiController();
                                g_homerSleepFrames = g_homerSleepMC->GetNumFrames();  // sleep loop (~61)
                                g_homerSleepMC->SetRelativeSpeed(GAG_SPEED);
                                g_homerSleepMC->SetCycleMode(FORCE_CYCLIC);
                                g_homerSleepMC->SetFrameRange(0.0f, g_homerSleepFrames);
                                g_homerSleepMC->Reset();
                                // Start asleep: show the sleeper, hide the gag Homer.
                                g_homerSleep->SetVisible(true);
                                g_homer->SetVisible(false);
                            }
                            g_pspSkipReskin = false;   // first visible pose must skin
                            g_gagState = 1;
                            g_gagTimer = 0.0f;
                            g_gagNextMs = 5000.0f;   // first gag ~5 s in
                        }
                    }
                    else if (g_homerMC)
                    {
                        if (g_gagState == 1)           // sleeping: loop the sleep idle
                        {
                            if (g_homerSleepMC) g_homerSleepMC->Advance(charDelta);
                            g_gagTimer += deltaMs;
                            if (g_gagTimer >= g_gagNextMs)
                            {
                                g_gagTimer = 0.0f;
                                // Wake up: hide the sleeper, show the gag Homer,
                                // play a RANDOM gag (all 5 fit the 476-frame clip).
                                int idx = (int)(GagRand() % (unsigned)kNumHomerGags);
                                if (g_homerSleep) g_homerSleep->SetVisible(false);
                                g_homer->SetVisible(true);
                                g_homerMC->SetCycleMode(FORCE_NON_CYCLIC);
                                g_homerMC->SetFrameRange(kHomerGags[idx].start, kHomerGags[idx].end);
                                g_homerMC->SetFrame(0.0f);   // range-relative
                                g_homerMC->Advance(0.0f);
                                g_pspSkipReskin = false;     // new pose -> must skin this frame
                                g_gagState = 2;

                                // Play Homer's matching gag VOICE line (synced).
                                if (idx < kNumGagSounds && g_gagPlayers[idx])
                                {
                                    g_gagPlayers[idx]->Stop();
                                    g_gagPlayers[idx]->Play();
                                }
                            }
                        }
                        else if (g_gagState == 2)      // gag playing: advance to its end
                        {
                            g_homerMC->Advance(charDelta);
                            if (g_homerMC->LastFrameReached() ||
                                g_homerMC->GetFrame() >= g_homerMC->GetNumFrames() - 0.5f)
                            {
                                // Back to sleep: hide gag Homer, show the sleeper.
                                g_pspSkipReskin = false;     // pose/visibility change -> skin this frame
                                if (g_homerSleep)
                                {
                                    g_homer->SetVisible(false);
                                    g_homerSleep->SetVisible(true);
                                    if (g_homerSleepMC) g_homerSleepMC->Reset();
                                }
                                else
                                {
                                    // Fallback (no sleep model): hold gag frame 0.
                                    g_homerMC->SetFrameRange(0.0f, g_homerFrames);
                                    g_homerMC->SetFrame(0.0f);
                                    g_homerMC->Advance(0.0f);
                                }
                                g_gagState = 1;
                                g_gagNextMs = 7000.0f + (float)(GagRand() % 7000);  // 7-14 s asleep
                            }
                        }
                    }
                }

                // Other gag characters (Gag1..Gag7): one at a time, alongside
                // Homer. Reveal a candidate's layer, wait for its controller to
                // attach (it only does so once the object Renders, i.e. while
                // visible), play it once, then hide and move on. A candidate that
                // never attaches a controller (its .p3d didn't load) is skipped.
                if (g_gagPage)
                {
                    if (g_otherState == 0)              // waiting between gags
                    {
                        g_otherTimer += deltaMs;
                        if (g_otherTimer >= g_otherNextMs)
                        {
                            g_otherTimer = 0.0f;
                            // Pick a RANDOM character (1..7) rather than cycling
                            // them in order; avoid repeating the last one twice.
                            {
                                int pick = 1 + (int)(GagRand() % 7);
                                if (pick == g_otherCur) pick = 1 + (pick % 7);
                                g_otherCur = pick;
                            }
                            if (g_otherObj[g_otherCur] && g_otherLayer[g_otherCur])
                            {
                                g_otherLayer[g_otherCur]->SetVisible(true);  // Render -> attach mc
                                g_pspSkipReskin = false;   // newly shown -> skin it
                                g_otherMC = NULL;
                                g_otherPoll = 0;
                                g_otherState = 1;
                            }
                            // else: no object/layer for this slot; retry next tick
                        }
                    }
                    else if (g_otherState == 1)        // starting: poll for the mc
                    {
                        g_pspSkipReskin = false;   // attaching/first pose -> keep skinning
                        g_otherMC = g_otherObj[g_otherCur] ? g_otherObj[g_otherCur]->GetMultiController() : NULL;
                        if (g_otherMC)
                        {
                            g_otherMC->SetRelativeSpeed(GAG_SPEED);
                            g_otherMC->SetCycleMode(FORCE_NON_CYCLIC);
                            g_otherMC->SetFrame(0.0f);
                            g_otherState = 2;

                            // Play this character's gag voice line (streamed).
                            HarnessPlayOtherGag(g_otherCur);

                            FILE* lf = pspDiagFopen("ms0:/shar_main.log", "a");
                            if (lf) { fprintf(lf, "other gag: Gag%d playing (mc=%p frames=%.0f)\n",
                                      g_otherCur, (void*)g_otherMC, g_otherMC->GetNumFrames()); fclose(lf); }
                        }
                        else if (++g_otherPoll > 60)   // never attached -> not loaded, skip
                        {
                            if (g_otherLayer[g_otherCur]) g_otherLayer[g_otherCur]->SetVisible(false);
                            g_otherState = 0;
                            g_otherNextMs = 1500.0f;    // try the next candidate soon
                            FILE* lf = pspDiagFopen("ms0:/shar_main.log", "a");
                            if (lf) { fprintf(lf, "other gag: Gag%d not loaded, skipping\n", g_otherCur); fclose(lf); }
                        }
                    }
                    else if (g_otherState == 2 && g_otherMC)   // playing -> hide when done
                    {
                        g_otherMC->Advance(charDelta);
                        if (g_otherMC->LastFrameReached() ||
                            g_otherMC->GetFrame() >= g_otherMC->GetNumFrames() - 0.5f)
                        {
                            if (g_otherLayer[g_otherCur]) g_otherLayer[g_otherCur]->SetVisible(false);
                            g_otherMC = NULL; g_otherCur = -1;
                            g_otherState = 0;
                            g_otherNextMs = 8000.0f + (float)(GagRand() % 6000);  // 8-14 s
                        }
                    }
                }
            }

            // Phase machine:
            //  PH_BOOTUP   - bootup.p3d loading; DrawFrame shows it once ready.
            //  PH_FRONTEND - bootup screen stays up while frontend.p3d streams
            //                (DrawFrame drives ContinueLoading every frame now).
            //  PH_MENU     - frontend loaded; switch to it and go to MainMenu.
            if (s_fePhase == PH_BOOTUP && s_bootCB.done)
            {
                app->LoadProject("art\\frontend\\scrooby\\frontend.p3d", &s_feCB);
                s_fePhase = PH_FRONTEND;
                mlog("scrooby: bootup up -> streaming frontend");
            }
            else if (s_fePhase == PH_FRONTEND && s_feCB.done && s_feCB.proj)
            {
                app->SetProject(s_feCB.proj);         // current project -> frontend
                s_fePhase = PH_MENU;
                mlog("scrooby: frontend loaded -> PH_MENU");
            }

            // In PH_MENU, keep trying to switch to the MainMenu screen until it
            // resolves — the screen resource may not be ready the exact frame the
            // project's load-complete callback fires.
            if (s_fePhase == PH_MENU)
            {
                // The load-complete callback can report the wrong project (the
                // two-project handoff races at high frame rates), so don't trust
                // it — scan every loaded project for the one that actually has a
                // "MainMenu" screen and switch to it.
                static bool gotoMenuDone = false;
                if (!gotoMenuDone)
                {
                    for (int pi = 0; ; pi++)
                    {
                        Scrooby::Project* pr = app->GetProject(pi);
                        if (!pr) break;
                        Scrooby::Screen* mm = pr->GetScreen("MainMenu");
                        if (mm)
                        {
                            app->SetProject(pr);
                            // The frontend's load-complete callback fired for the
                            // wrong project (two-project race), so this project is
                            // still flagged "loading" and DrawFrame won't draw it.
                            // Force its loaded state now that its screens exist.
                            FeProject* fp = dynamic_cast<FeProject*>(pr);
                            if (fp) fp->OnResourceLoadComplete();
                            pr->GotoScreen(mm, NULL);
                            gotoMenuDone = true;
                            // DIAG (real-hardware fence bring-up): a PLAIN fopen
                            // (not the PSP_DIAG_LOG-gated pspDiagFopen) written at
                            // a definitely-reached point. If shar_probe.log shows
                            // up on the memstick, plain fopen("ms0:/..") works on
                            // real hardware and the empty fence log means the fence
                            // never draws; if it's ALSO absent, fopen itself isn't
                            // writing on real hardware (and no log ever will).
                            {
                                FILE* pf = fopen("ms0:/shar_probe.log", "w");
                                if (pf) { fprintf(pf, "menu reached; plain fopen to ms0 works\n"); fclose(pf); }
                            }
                            // Grab the multi-string menu label so we can change
                            // the selection text + highlight it (see input/pulse
                            // code). Same lookup the game uses:
                            // GetPage("MainMenu")->GetText("MainMenu").
                            Scrooby::Page* pg = mm->GetPage("MainMenu");
                            if (pg) g_menuText = pg->GetText("MainMenu");
                            // The asset carries both a console ("MainMenu") and a
                            // PC ("MainMenu_PC") label; the game hides the unused
                            // one. We use the console label, so hide the PC variant
                            // (otherwise it draws as a static white string behind).
                            if (pg)
                            {
                                Scrooby::Text* pcLabel = pg->GetText("MainMenu_PC");
                                if (pcLabel) pcLabel->SetVisible(false);
                            }
                            // The level-select sub-menu ("L1 Suburbs" etc.) is
                            // hidden by default in the retail game (it's gated
                            // behind level-selection/debug builds): the screen
                            // does levelPage->GetGroup("Menu")->SetVisible(false).
                            // The harness doesn't run that controller, so hide it
                            // here or it shows as static white text on the menu.
                            {
                                Scrooby::Page* lvlPg = mm->GetPage("Level");
                                if (lvlPg)
                                {
                                    Scrooby::Group* lvlMenu = lvlPg->GetGroup("Menu");
                                    if (lvlMenu) lvlMenu->SetVisible(false);
                                }
                            }
                            // Camera intro / static-set setup: grab CamAndSet
                            // (the room+camera+baked-anim object) and the TV frame
                            // overlay. The TV frame starts hidden and fades in over
                            // the tail of the intro camera move.
                            {
                                Scrooby::Page* pg3d = mm->GetPage("3dFE");
                                if (pg3d) g_camset = pg3d->GetPure3dObject("CamAndSet");
                                Scrooby::Page* pgTv = mm->GetPage("TVFrame");
                                if (pgTv) g_tvFrame = pgTv->GetLayer("TVFrame");
                                // Only hide the TV frame if we have CamAndSet to run
                                // the intro that fades it back in — otherwise it'd
                                // stay invisible forever.
                                if (g_tvFrame && g_camset) g_tvFrame->SetAlpha(0.0f);
                                else g_tvFrame = NULL;
                                // (TV-frame bezel fit is applied every frame in
                                // the render loop, not here — see TVFRAME_FIT_*.)
                                g_introState = 0;
                                // Homer gag object. On PSP the retail idle "Homer"
                                // (homer.p3d) is skipped, so the loaded gag-Homer
                                // ("Gag0" on 3dFEGags = gaghomer.p3d) is the visible
                                // menu Homer — grab it; fall back to the 3dFE "Homer"
                                // object if that layout is present instead.
                                {
                                    Scrooby::Page* pgGag = mm->GetPage("3dFEGags");
                                    if (pgGag) g_homer = pgGag->GetPure3dObject("Gag0");
                                    if (!g_homer)
                                    {
                                        Scrooby::Page* pg3d2 = mm->GetPage("3dFE");
                                        if (pg3d2) g_homer = pg3d2->GetPure3dObject("Homer");
                                    }
                                    // The sleeping idle Homer (homer.p3d) lives as
                                    // "Homer" on the 3dFE page. We swap visibility
                                    // between it and gaghomer (Gag0). If it didn't
                                    // load, g_homerSleep stays NULL and we fall back
                                    // to holding gaghomer static (old behaviour).
                                    if (pg3d) g_homerSleep = pg3d->GetPure3dObject("Homer");
                                    if (g_homerSleep == g_homer) g_homerSleep = NULL;  // same obj (fallback layout)
                                    g_homerSleepMC = NULL;
                                    g_homerMC = NULL;
                                    g_gagState = 0;
                                    g_gagTimer = 0.0f;
                                    g_gagNextMs = 5000.0f;   // first gag ~5 s in
                                    g_gagRng ^= (unsigned)(sceKernelGetSystemTimeWide() & 0xffffffff);

                                    // Other gag characters (Gag1..Gag7): grab
                                    // their objects + layers and hide them; the
                                    // cycle reveals one at a time. (Objects have
                                    // NULL drawables until their .p3d loads; the
                                    // cycle polls + skips any that never resolve.)
                                    g_gagPage = pgGag;
                                    if (g_gagPage)
                                    {
                                        int nLayers = g_gagPage->GetNumberOfLayers();
                                        for (int gi = 1; gi <= 7; gi++)
                                        {
                                            char nm[8]; sprintf(nm, "Gag%d", gi);
                                            g_otherObj[gi]   = g_gagPage->GetPure3dObject(nm);
                                            g_otherLayer[gi] = (gi < nLayers) ? g_gagPage->GetLayerByIndex(gi) : NULL;
                                            if (g_otherLayer[gi]) g_otherLayer[gi]->SetVisible(false);
                                        }
                                    }
                                    g_otherCur = -1; g_otherNext = 1; g_otherState = 0;
                                    g_otherTimer = 0.0f; g_otherNextMs = 9000.0f;
                                }
                                // Start the procedural circle-fade closed; it
                                // opens in step with the intro camera move. Only
                                // arm it when we have camset to run that intro,
                                // else the menu would sit under a black pinhole.
                                if (g_camset)
                                {
                                    g_irisRunning = true;
                                    g_irisOpen    = 0.0f;
                                    g_irisFrames2 = 0;
                                }
                            }
                            // Iris wipe setup (probe + drive). Fetch the IrisCover
                            // page/layer/object and the IrisController animation;
                            // if present, start the iris closed and reveal outward.
                            {
                                Scrooby::Page* pgIris = mm->GetPage("IrisCover");
                                FILE* lf = pspDiagFopen("ms0:/shar_iris.log", "a");
                                if (pgIris)
                                {
                                    g_irisLayer = pgIris->GetLayer("IrisCover");
                                    if (g_irisLayer) g_irisLayer->SetVisible(true);
                                    g_iris = pgIris->GetPure3dObject("3dIris");
                                    if (g_iris) g_iris->SetZBufferEnabled(false);
                                    g_irisMC = p3d::find<tMultiController>("IrisController");
                                    if (g_irisMC) g_irisFrames = g_irisMC->GetNumFrames();
                                    if (lf) fprintf(lf, "iris: page=%p layer=%p obj=%p mc=%p frames=%.1f\n",
                                        (void*)pgIris, (void*)g_irisLayer, (void*)g_iris,
                                        (void*)g_irisMC, g_irisFrames);
                                }
                                else if (lf) fprintf(lf, "iris: no IrisCover page\n");
                                if (lf) fclose(lf);

                                if (g_iris && g_irisMC && g_irisFrames > 1.0f)
                                {
                                    // Start fully closed (midpoint = black), then
                                    // play to the end so the iris opens to reveal.
                                    g_irisMC->SetCycleMode(FORCE_NON_CYCLIC);
                                    g_irisMC->SetFrameRange(g_irisFrames * 0.5f, g_irisFrames);
                                    g_irisMC->SetFrame(g_irisFrames * 0.5f);
                                    g_irisMC->SetRelativeSpeed(0.5f);
                                    g_iris->SetMultiController(g_irisMC);   // attach -> Update advances it
                                    g_irisState = 1;
                                }
                            }
                            if (g_menuText)
                            {
                                int n = g_menuText->GetNumOfStrings();
                                char mb[96];
                                sprintf(mb, "scrooby: menu label found, %d strings", n);
                                mlog(mb);
                                if (n > 0) g_pspSelectedGlow %= n;
                                g_menuText->SetIndex(g_pspSelectedGlow);
                                // (menu text is shrunk every frame in the render
                                // loop, not here — see MENU_TEXT_SCALE.)
                            }
                            char db[96];
                            sprintf(db, "scrooby: GotoScreen(MainMenu) on project %d (%p)", pi, (void*)pr);
                            mlog(db);
                            break;
                        }
                    }
                }
            }

            if (ctx)
            {
                if (s_fePhase == PH_BOOTUP && !s_bootCB.done)
                {
                    // The bootup screen itself hasn't parsed yet: pulse a colour
                    // so the very first fraction of a second isn't a dead screen.
                    int pv = (frame & 0x3F);
                    if (frame & 0x40) pv = 0x3F - pv;   // triangle 0..63
                    ctx->SetClearColour(tColour(8, 12, 24 + pv));
                }
                else
                {
                    // A Scrooby screen (bootup loading screen or the menu) is
                    // being drawn on top; clear to black behind it.
                    ctx->SetClearColour(tColour(0, 0, 0));
                }

                // The menu's 3D scene objects (FePure3dObject: Homer's living
                // room) render into the context's active tView, taking their own
                // camera from the 'camset' resource. Provide a base view+camera
                // so FePure3dObject::Render has one to work with.
                static tView* feView = NULL;
                if (feView == NULL)
                {
                    tPointCamera* c = new tPointCamera;
                    c->AddRef();
                    c->SetFOV(rmt::DegToRadian(60.0f), 480.0f / 272.0f);
                    c->SetNearPlane(0.1f);
                    c->SetFarPlane(2000.0f);
                    c->SetPosition(rmt::Vector(0, 0, -5));
                    c->SetTarget(rmt::Vector(0, 0, 0));
                    feView = new tView;
                    feView->AddRef();
                    feView->SetCamera(c);
                }
                p3d::context->SetView(feView);

                ctx->BeginFrame();            // clears (ctx clear mask/colour set)
                // Draw the cached 3D room as the frame backdrop (behind
                // everything) once it has been captured. The camset object then
                // skips its ~280-mesh live render; only animated objects (Homer)
                // draw live over this. Big FPS win for the menu.
                pglDrawRoomBackdrop();

                // Re-assert the menu-text + TV-frame transforms every frame, just
                // before they're drawn. Applying them once at menu setup had no
                // visible effect because the Scrooby resources finish loading and
                // relaying-out AFTER that point, resetting the drawables' matrices
                // (m_matrix -> identity) and wiping the one-shot scale. Reset then
                // Scale each frame gives a constant, non-compounding absolute scale
                // that survives those resets.
                if (s_fePhase == PH_MENU)
                {
                    if (g_menuText)
                    {
                        // Animated selection "throb": pulse the (shrunk) menu text
                        // so the highlighted item breathes like the retail menu.
                        // Must be applied HERE, right before DrawFrame — this is
                        // the per-frame transform that actually survives to the
                        // draw (the earlier navigation-block scale is overwritten
                        // before the menu renders, which is why it never showed).
                        long long ms = (long long)(sceKernelGetSystemTimeWide() / 1000);
                        float ph = (float)(ms % 600) / 600.0f;   // 600 ms period
                        float pulse = 1.0f + 0.08f * sinf(ph * 2.0f * 3.14159265f);
                        g_menuText->ResetTransformation();
                        g_menuText->ScaleAboutCenter(MENU_TEXT_SCALE * pulse);
                    }
                    if (g_tvFrame)
                    {
                        g_tvFrame->ResetTransformation();
                        g_tvFrame->Scale(TVFRAME_FIT_X, TVFRAME_FIT_Y, 1.0f);
                    }
                    static bool s_scaleLogged = false;
                    if (!s_scaleLogged)
                    {
                        FILE* lf = pspDiagFopen("ms0:/shar_main.log", "a");
                        if (lf)
                        {
                            fprintf(lf, "scale: menuText=%p tvFrame=%p textScale=%.2f\n",
                                    (void*)g_menuText, (void*)g_tvFrame, MENU_TEXT_SCALE);
                            fclose(lf);
                        }
                        s_scaleLogged = true;
                    }
                }

                // DrawFrame pumps ContinueLoading() until the project is loaded,
                // then draws the current screen (FeScreen sets its own 2D camera).
                unsigned long long tD0 = sceKernelGetSystemTimeWide();
                g_pspDrawBuffer = 0; g_pspDrawStream = 0; g_pspVerts = 0;
                Scrooby::App::GetInstance()->DrawFrame(deltaMs);
                unsigned long long tD1 = sceKernelGetSystemTimeWide();

                // Procedural iris "circle fade": draw the growing-circle black mask
                // over the fully-composited menu (3D scene + 2D UI), revealing it
                // from the centre outward as the intro camera flies in. Safety:
                // if the intro never advances the mask to open (e.g. the camset
                // controller never attaches), force it open after ~4 s so the
                // player is never left staring at a black pinhole.
                if (g_irisRunning)
                {
                    if (++g_irisFrames2 > 240) { g_irisOpen = 1.0f; g_irisRunning = false; }
                    pguDrawIrisMask(g_irisOpen);
                }
                unsigned long long tE0 = sceKernelGetSystemTimeWide();
                ctx->EndFrame(true);
                unsigned long long tE1 = sceKernelGetSystemTimeWide();

                // Accumulate FPS/bottleneck stats; log averages every window.
                if (kPerfLog)
                {
                    s_perfFrames++;
                    s_perfFrameMs   += deltaMs;
                    s_perfDrawMs    += (float)(tD1 - tD0) / 1000.0f;   // DrawFrame (incl. skinning)
                    s_perfEndMs     += (float)(tE1 - tE0) / 1000.0f;   // EndFrame / swap
                    s_perfDrawCalls  = g_pspDrawBuffer;   // VBO/DrawPrimBuffer draws
                    s_perfVerts      = g_pspDrawStream;   // immediate BeginPrims draws
                    if (g_gagState == 2 || g_otherState == 2) s_perfGagFrames++;
                    if (s_perfFrames >= kPerfWindow)
                    {
                        float n = (float)s_perfFrames;
                        FILE* pf = fopen("ms0:/shar_perf.log", "a");
                        if (pf)
                        {
                            fprintf(pf, "avgFrame=%.1fms (%.1ffps) DrawFrame=%.1fms EndFrame=%.1fms vboDraws=%d immDraws=%d immVerts=%d gagFrames=%d/%d\n",
                                    s_perfFrameMs / n, 1000.0f / (s_perfFrameMs / n),
                                    s_perfDrawMs / n, s_perfEndMs / n,
                                    s_perfDrawCalls, s_perfVerts, g_pspVerts, s_perfGagFrames, s_perfFrames);
                            fclose(pf);
                        }
                        s_perfFrames = 0; s_perfFrameMs = s_perfDrawMs = s_perfEndMs = 0.0f;
                        s_perfGagFrames = 0;
                    }
                }
            }


            frame++;
            continue;
        }

        // --- drive the load sequence (global.p3d, then test.p3d) ------------
        // Read the current file's state only until it is done; the completion
        // callback (fired by SwitchTask below) frees 'req', so polling it after
        // that is a use-after-free.
        if (req && !curFileDone)
        {
            radLoadState ls = req->GetState();
            if (ls == COMPLETE || ls == CANCELED) curFileDone = true;
        }

        // Pump file IO so the worker's reads complete (during loading).
        radFileService();

        // When the current file is done, pump SwitchTask a few frames so its
        // completion callback (tLoadRequest::InternalCallback::Done) Dumps the
        // loaded objects into p3d::inventory. Only THEN start the next file, so
        // the model's shaders can resolve textures the shared file just added.
        if (curFileDone && !allLoaded)
        {
            p3d::loadManager->SwitchTask();
            if (++flushCount >= 4)
            {
                fileIdx++;
                flushCount  = 0;
                curFileDone = false;
                if (fileIdx < kNumFiles)
                {
                    req = startLoad(kFiles[fileIdx]);   // next file (test.p3d)
                }
                else
                {
                    allLoaded = true;
                    req = NULL;
                    mlog("all files loaded");
                }
            }
        }
        // Yield CPU to the load worker (PSP is strict-priority).
        sceKernelDelayThread(2000);   // 2 ms

        // state: 1 = everything loaded, 2 = still loading
        int state = allLoaded ? 1 : 2;

        // --- one-time scene build once the inventory is populated -----------
        if (state == 1 && !sceneReady && ctx)
        {
            // Collect every tGeometry the chunk loaders put in the inventory.
            // The Dump callback may not have run yet on the first COMPLETE
            // frame, so retry until geometry appears (or we give up).
            nGeos = 0;
            tInventory::Iterator<tGeometry> it;
            for (tGeometry* g = it.First(); g && nGeos < MAX_GEOS; g = it.Next())
            {
                geos[ nGeos++ ] = g;
            }

            buildTries++;
            if (nGeos == 0 && buildTries < 120)
            {
                // inventory not populated yet — try again next frame
                goto after_scene_build;
            }
            { char b[48]; sprintf(b, "geometries found: %d (tries=%d)", nGeos, buildTries); mlog(b); }

            // If the file carries a scene graph, it positions the meshes at
            // their correct transforms (car body / doors / wheels). Prefer it
            // over the flat "draw every mesh at the origin" fallback.
            {
                tInventory::Iterator<Scenegraph::Scenegraph> sit;
                scene = sit.First();
            }
            { char b[48]; sprintf(b, "scenegraph: %s", scene ? "FOUND" : "none"); mlog(b); }

            // Characters + vehicles are CompositeDrawables (skeleton + props/skins).
            {
                tInventory::Iterator<tCompositeDrawable> cit;
                composite = cit.First();
            }
            { char b[48]; sprintf(b, "composite: %s", composite ? "FOUND" : "none"); mlog(b); }

            // Frame the camera to the union of the geometries' bounds. Prefer
            // the bounding box; fall back to the bounding sphere if the box is
            // degenerate (no BOX chunk in the mesh).
            rmt::Vector lo(  1e18f,  1e18f,  1e18f );
            rmt::Vector hi( -1e18f, -1e18f, -1e18f );
            bool haveBounds = false;
            for (int i = 0; i < nGeos; i++)
            {
                rmt::Box3D  bx; geos[i]->GetBoundingBox( &bx );
                rmt::Vector clo = bx.low, chi = bx.high;
                if ( chi.x - clo.x < 1e-4f && chi.y - clo.y < 1e-4f && chi.z - clo.z < 1e-4f )
                {
                    rmt::Sphere s; geos[i]->GetBoundingSphere( &s );
                    clo.Set( s.centre.x - s.radius, s.centre.y - s.radius, s.centre.z - s.radius );
                    chi.Set( s.centre.x + s.radius, s.centre.y + s.radius, s.centre.z + s.radius );
                }
                if ( clo.x < lo.x ) lo.x = clo.x;
                if ( clo.y < lo.y ) lo.y = clo.y;
                if ( clo.z < lo.z ) lo.z = clo.z;
                if ( chi.x > hi.x ) hi.x = chi.x;
                if ( chi.y > hi.y ) hi.y = chi.y;
                if ( chi.z > hi.z ) hi.z = chi.z;
                haveBounds = true;
            }
            if (haveBounds)
            {
                sceneCentre.Set( (lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f );
                rmt::Vector diag; diag.Sub( hi, sceneCentre );
                sceneRadius = diag.Magnitude();
            }
            if ( !(sceneRadius > 0.01f && sceneRadius < 1e7f) )   // guard degenerate bounds
            {
                sceneCentre.Set( 0, 0, 0 );
                sceneRadius = 10.0f;
            }
            {
                char b[96];
                sprintf(b, "scene centre=(%.2f,%.2f,%.2f) r=%.2f",
                        sceneCentre.x, sceneCentre.y, sceneCentre.z, sceneRadius);
                mlog(b);
            }

            cam = new tPointCamera;
            cam->AddRef();
            cam->SetFOV( rmt::DegToRadian(60.0f), 480.0f / 272.0f );
            cam->SetNearPlane( sceneRadius * 0.02f + 0.05f );
            cam->SetFarPlane ( sceneRadius * 8.0f  + 100.0f );

            view = new tView;
            view->AddRef();
            view->SetCamera( cam );
            view->SetClearColour( tColour(24, 24, 40) );          // dark slate => "rendering" state
            view->SetClearMask( PDDI_BUFFER_COLOUR | PDDI_BUFFER_DEPTH );
            view->SetAmbientLight( tColour(255, 255, 255) );      // full ambient (no lights loaded)

            ctx->SetClearMask( 0 );   // the view owns the clear now; avoid a double clear
            sceneReady = true;
            mlog("scene ready");
        }
    after_scene_build:

        // --- draw -----------------------------------------------------------
        if (ctx && sceneReady)
        {
            // Orbit the camera so the result is unmistakably 3D (proves depth,
            // projection and the view matrix are all live). Pull back a little
            // extra (1.8) since scene-graph assembly spreads parts wider than
            // their individual local bounding boxes suggest.
            // Was float(frame)*0.02f (≈1.2 rad/s at 60fps); integrate real time
            // so the orbit rate is the same on PSP (20fps) and PPSSPP (60fps).
            orbitAng += deltaMs * 0.001f * 1.2f;
            float ang  = orbitAng;
            float dist = sceneRadius / rmt::Sin( rmt::DegToRadian(30.0f) ) * 1.8f;   // fit half-FOV
            rmt::Vector pos;
            pos.Set( sceneCentre.x + rmt::Sin(ang) * dist,
                     sceneCentre.y + sceneRadius * 0.5f,
                     sceneCentre.z + rmt::Cos(ang) * dist );
            cam->SetPosition( pos );
            cam->SetTarget( sceneCentre );

            ctx->BeginFrame();
            view->BeginRender();
            if (composite)
            {
                // Character/vehicle: evaluates the skeleton bind pose, then draws
                // each rigid prop at its joint and each skinned mesh (CPU-skinned).
                composite->Display();
            }
            else if (scene)
            {
                // Traverses the transform hierarchy, drawing each mesh at its
                // correct place (body / doors / wheels assembled).
                scene->Display();
            }
            else
            {
                // No container: draw every mesh at the origin (static prop).
                for (int i = 0; i < nGeos; i++)
                    geos[i]->Display();
            }
            view->EndRender();
            ctx->EndFrame(true);
        }
        else if (ctx)
        {
            // Pre-load feedback: pulse by load state (blue=loading, red=failed).
            tColour c;
            if (state == 0) c.Set(64 + pulse * 3, 0, 0);
            else            c.Set(0, 0, 64 + pulse * 3);
            ctx->SetClearColour(c);
            ctx->BeginFrame();
            ctx->EndFrame(true);
        }

        if (frame == 1)  mlog("first frame rendered");
        if (state == 1 && !loggedDone) { mlog("load COMPLETE"); loggedDone = true; }
        if ((frame % 180) == 0)
        {
            char buf[64];
            sprintf(buf, "frame %u: state=%d nGeos=%d ready=%d", frame, state, nGeos, (int)sceneReady);
            mlog(buf);
        }
        frame++;
    }

    sceKernelExitGame();
    return 0;
}
