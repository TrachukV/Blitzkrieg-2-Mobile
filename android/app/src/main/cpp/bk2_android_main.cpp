#include "bk2_android_audio_backend.h"
#include "bk2_android_audio_output.h"
#include "bk2_android_database.h"
#include "bk2_android_legacy_game_runtime.h"
#include "bk2_android_menu_runtime.h"
#include "bk2_android_platform.h"
#include "bk2_android_single_player_runtime.h"
#include "bk2_android_vfs.h"
#include "bk2_port_paths.h"
#include "bk2_render_backend.h"

#include <game-activity/native_app_glue/android_native_app_glue.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace {

void HandleAppCommand(android_app* app, int32_t command) {
    auto& platform = bk2::android::PlatformRuntime::instance();
    switch (command) {
        case APP_CMD_INIT_WINDOW:
            if (app->window != nullptr) {
                platform.set_lifecycle_state(bk2::android::LifecycleState::WindowReady);
                if (bk2::android::InitializeLegacyGfx(app->window)) {
                    bk2::android::RefreshSinglePlayerRenderResources();
                    platform.log_info(
                            bk2::android::RenderBackendDiagnosticReport());
                } else {
                    platform.log_warn(
                            bk2::android::RenderBackendDiagnosticReport());
                }
            }
            break;
        case APP_CMD_TERM_WINDOW:
            bk2::android::ShutdownLegacyGfx();
            platform.set_lifecycle_state(bk2::android::LifecycleState::Started);
            platform.log_info("Android window terminated.");
            break;
        case APP_CMD_WINDOW_RESIZED:
        case APP_CMD_CONTENT_RECT_CHANGED:
            if (app->window != nullptr) {
                bk2::android::ResizeLegacyGfx(
                        static_cast<uint32_t>(
                                ANativeWindow_getWidth(app->window)),
                        static_cast<uint32_t>(
                                ANativeWindow_getHeight(app->window)));
            }
            break;
        case APP_CMD_WINDOW_REDRAW_NEEDED:
            bk2::android::RenderLegacyGfxFrame();
            break;
        case APP_CMD_GAINED_FOCUS:
            platform.set_lifecycle_state(bk2::android::LifecycleState::Focused);
            bk2::android::AudioBackend().set_paused(false);
            bk2::android::AudioOutput().resume();
            platform.log_info("App gained focus.");
            platform.log_info(bk2::android::AudioOutput().diagnostic_report());
            break;
        case APP_CMD_LOST_FOCUS:
            platform.set_lifecycle_state(bk2::android::LifecycleState::Started);
            bk2::android::AudioOutput().pause();
            bk2::android::AudioBackend().set_paused(true);
            platform.log_info("App lost focus.");
            platform.log_info(bk2::android::AudioOutput().diagnostic_report());
            break;
        case APP_CMD_PAUSE:
            platform.set_lifecycle_state(bk2::android::LifecycleState::Paused);
            bk2::android::AudioOutput().pause();
            bk2::android::AudioBackend().set_paused(true);
            platform.log_info("App paused.");
            platform.log_info(bk2::android::AudioOutput().diagnostic_report());
            break;
        case APP_CMD_STOP:
            platform.set_lifecycle_state(bk2::android::LifecycleState::Stopped);
            bk2::android::AudioOutput().pause();
            bk2::android::AudioBackend().set_paused(true);
            platform.log_info("App stopped.");
            break;
        case APP_CMD_DESTROY:
            platform.set_lifecycle_state(bk2::android::LifecycleState::Destroying);
            bk2::android::ShutdownLegacyGfx();
            bk2::android::AudioOutput().shutdown();
            platform.log_info("App destroy requested.");
            break;
        default:
            break;
    }
}

struct TouchCameraState {
    bool tracking = false;
    int pointer_count = 0;
    float center_x = 0.0f;
    float center_y = 0.0f;
    float start_x = 0.0f;
    float start_y = 0.0f;
    float distance = 0.0f;
    bool moved = false;
    bool panning = false;
    bool selecting = false;
    uint64_t down_millis = 0;
};

TouchCameraState g_touch_camera;

bool g_menu_input_active = false;

void ResetTouchCameraState() {
    g_touch_camera = TouchCameraState{};
}

void QueueTouchSelectionOverlay() {
    if (!g_touch_camera.selecting ||
        !bk2::android::RenderBackend().is_ready()) {
        return;
    }
    const float viewport_width = static_cast<float>(
            bk2::android::RenderBackend().width());
    const float viewport_height = static_cast<float>(
            bk2::android::RenderBackend().content_height());
    const float left = std::clamp(
            std::min(g_touch_camera.start_x, g_touch_camera.center_x),
            0.0f,
            viewport_width);
    const float right = std::clamp(
            std::max(g_touch_camera.start_x, g_touch_camera.center_x),
            0.0f,
            viewport_width);
    const float top = std::clamp(
            std::min(g_touch_camera.start_y, g_touch_camera.center_y),
            0.0f,
            viewport_height);
    const float bottom = std::clamp(
            std::max(g_touch_camera.start_y, g_touch_camera.center_y),
            0.0f,
            viewport_height);
    if (right - left < 1.0f || bottom - top < 1.0f) {
        return;
    }
    constexpr uint32_t kSelectionFillArgb = 0x243fdb67u;
    constexpr uint32_t kSelectionBorderArgb = 0xff72e681u;
    const float border = std::max(2.0f, viewport_width / 1200.0f);
    auto& backend = bk2::android::RenderBackend();
    backend.queue_solid_rect(
            left,
            top,
            right - left,
            bottom - top,
            kSelectionFillArgb);
    backend.queue_solid_rect(
            left,
            top,
            right - left,
            border,
            kSelectionBorderArgb);
    backend.queue_solid_rect(
            left,
            bottom - border,
            right - left,
            border,
            kSelectionBorderArgb);
    backend.queue_solid_rect(
            left,
            top,
            border,
            bottom - top,
            kSelectionBorderArgb);
    backend.queue_solid_rect(
            right - border,
            top,
            border,
            bottom - top,
            kSelectionBorderArgb);
}

// Routes a pointer through the original menu screen instead of the
// battlefield while no mission is running.
void PollMenuInput(android_input_buffer* input_buffer) {
    for (uint64_t i = 0; i < input_buffer->motionEventsCount; ++i) {
        const GameActivityMotionEvent& event = input_buffer->motionEvents[i];
        const int action = event.action & AMOTION_EVENT_ACTION_MASK;
        if (event.pointerCount == 0) {
            continue;
        }
        const float x = GameActivityPointerAxes_getX(&event.pointers[0]);
        const float y = GameActivityPointerAxes_getY(&event.pointers[0]);
        const uint32_t width = bk2::android::RenderBackend().width();
        const uint32_t height = bk2::android::RenderBackend().height();
        if (action == AMOTION_EVENT_ACTION_DOWN) {
            bk2::android::PressOriginalMenu(x, y, width, height);
        } else if (action == AMOTION_EVENT_ACTION_UP) {
            const std::string reaction =
                    bk2::android::ReleaseOriginalMenu(x, y, width, height);
            bk2::android::RunOriginalMenuReaction(reaction);
        } else if (action == AMOTION_EVENT_ACTION_CANCEL) {
            bk2::android::CancelOriginalMenuPress();
        }
    }
    android_app_clear_motion_events(input_buffer);

    // The system Back gesture pops the menu stack, which is what a phone
    // player expects from the shipped Back buttons.
    for (uint64_t i = 0; i < input_buffer->keyEventsCount; ++i) {
        const GameActivityKeyEvent& event = input_buffer->keyEvents[i];
        if (event.action == AKEY_EVENT_ACTION_UP &&
            event.keyCode == AKEYCODE_BACK) {
            bk2::android::RunOriginalMenuReaction("back");
        }
    }
    android_app_clear_key_events(input_buffer);
}

void PollInput(android_app* app) {
    android_input_buffer* input_buffer = android_app_swap_input_buffers(app);
    if (input_buffer == nullptr) {
        return;
    }
    if (g_menu_input_active) {
        PollMenuInput(input_buffer);
        return;
    }

    for (uint64_t i = 0; i < input_buffer->motionEventsCount; ++i) {
        const GameActivityMotionEvent& event = input_buffer->motionEvents[i];
        const int action = event.action & AMOTION_EVENT_ACTION_MASK;
        if (action == AMOTION_EVENT_ACTION_CANCEL) {
            ResetTouchCameraState();
            continue;
        }
        if (action == AMOTION_EVENT_ACTION_UP ||
            action == AMOTION_EVENT_ACTION_POINTER_UP) {
            if (action == AMOTION_EVENT_ACTION_UP &&
                g_touch_camera.tracking &&
                g_touch_camera.pointer_count == 1 &&
                event.pointerCount > 0) {
                const float x =
                        GameActivityPointerAxes_getX(&event.pointers[0]);
                const float y =
                        GameActivityPointerAxes_getY(&event.pointers[0]);
                const float content_height = static_cast<float>(
                        bk2::android::RenderBackend().content_height());
                if (g_touch_camera.selecting) {
                    bk2::android::HandleSinglePlayerSelectionRect(
                            g_touch_camera.start_x,
                            g_touch_camera.start_y,
                            x,
                            std::min(y, content_height - 1.0f),
                            bk2::android::RenderBackend().width(),
                            bk2::android::RenderBackend().content_height());
                } else if (!g_touch_camera.moved &&
                           y < content_height) {
                    bk2::android::HandleSinglePlayerTap(
                            x,
                            y,
                            bk2::android::RenderBackend().width(),
                            bk2::android::RenderBackend().content_height());
                }
            }
            ResetTouchCameraState();
            continue;
        }

        if (event.pointerCount == 1) {
            const float x =
                    GameActivityPointerAxes_getX(&event.pointers[0]);
            const float y =
                    GameActivityPointerAxes_getY(&event.pointers[0]);
            if (y >= static_cast<float>(
                        bk2::android::RenderBackend().content_height())) {
                ResetTouchCameraState();
                continue;
            }
            if (!g_touch_camera.tracking ||
                g_touch_camera.pointer_count != 1 ||
                action == AMOTION_EVENT_ACTION_DOWN) {
                g_touch_camera.tracking = true;
                g_touch_camera.pointer_count = 1;
                g_touch_camera.center_x = x;
                g_touch_camera.center_y = y;
                g_touch_camera.start_x = x;
                g_touch_camera.start_y = y;
                g_touch_camera.distance = 0.0f;
                g_touch_camera.moved = false;
                g_touch_camera.panning = false;
                g_touch_camera.selecting = false;
                g_touch_camera.down_millis =
                        bk2::android::PlatformRuntime::instance()
                                .monotonic_millis();
                continue;
            }

            if (action == AMOTION_EVENT_ACTION_MOVE) {
                const float total_delta_x = x - g_touch_camera.start_x;
                const float total_delta_y = y - g_touch_camera.start_y;
                if (total_delta_x * total_delta_x +
                            total_delta_y * total_delta_y >
                        16.0f * 16.0f) {
                    g_touch_camera.moved = true;
                }
                const uint64_t now_millis =
                        bk2::android::PlatformRuntime::instance()
                                .monotonic_millis();
                if (!g_touch_camera.panning &&
                    !g_touch_camera.selecting &&
                    g_touch_camera.moved) {
                    if (bk2::android::SinglePlayerTouchCommandMode() == 0 &&
                        now_millis >= g_touch_camera.down_millis &&
                        now_millis - g_touch_camera.down_millis >= 350) {
                        g_touch_camera.selecting = true;
                    } else {
                        g_touch_camera.panning = true;
                    }
                }
                if (g_touch_camera.panning) {
                    bk2::android::PanSinglePlayerCamera(
                            x - g_touch_camera.center_x,
                            y - g_touch_camera.center_y);
                }
                g_touch_camera.center_x = x;
                g_touch_camera.center_y = y;
            }
            continue;
        }

        if (event.pointerCount < 2) {
            if (action == AMOTION_EVENT_ACTION_POINTER_DOWN) {
                ResetTouchCameraState();
            }
            continue;
        }

        const float x0 =
                GameActivityPointerAxes_getX(&event.pointers[0]);
        const float y0 =
                GameActivityPointerAxes_getY(&event.pointers[0]);
        const float x1 =
                GameActivityPointerAxes_getX(&event.pointers[1]);
        const float y1 =
                GameActivityPointerAxes_getY(&event.pointers[1]);
        const float content_height = static_cast<float>(
                bk2::android::RenderBackend().content_height());
        if (y0 >= content_height || y1 >= content_height) {
            ResetTouchCameraState();
            continue;
        }
        const float center_x = (x0 + x1) * 0.5f;
        const float center_y = (y0 + y1) * 0.5f;
        const float distance = std::hypot(x1 - x0, y1 - y0);

        if (!g_touch_camera.tracking ||
            g_touch_camera.pointer_count != 2 ||
            action == AMOTION_EVENT_ACTION_POINTER_DOWN ||
            action == AMOTION_EVENT_ACTION_DOWN) {
            g_touch_camera.tracking = true;
            g_touch_camera.pointer_count = 2;
            g_touch_camera.center_x = center_x;
            g_touch_camera.center_y = center_y;
            g_touch_camera.distance = distance;
            g_touch_camera.moved = true;
            g_touch_camera.panning = true;
            g_touch_camera.selecting = false;
            continue;
        }

        if (action == AMOTION_EVENT_ACTION_MOVE) {
            bk2::android::PanSinglePlayerCamera(
                    center_x - g_touch_camera.center_x,
                    center_y - g_touch_camera.center_y);
            if (g_touch_camera.distance > 1.0f && distance > 1.0f) {
                bk2::android::ZoomSinglePlayerCamera(
                        distance / g_touch_camera.distance);
            }
            g_touch_camera.center_x = center_x;
            g_touch_camera.center_y = center_y;
            g_touch_camera.distance = distance;
        }
    }

    for (uint64_t i = 0; i < input_buffer->keyEventsCount; ++i) {
        const GameActivityKeyEvent& event = input_buffer->keyEvents[i];
        if (event.action != AKEY_EVENT_ACTION_DOWN) {
            continue;
        }
        switch (event.keyCode) {
            case AKEYCODE_A:
            case AKEYCODE_DPAD_LEFT:
                bk2::android::PanSinglePlayerCamera(-32.0f, 0.0f);
                break;
            case AKEYCODE_D:
            case AKEYCODE_DPAD_RIGHT:
                bk2::android::PanSinglePlayerCamera(32.0f, 0.0f);
                break;
            case AKEYCODE_W:
            case AKEYCODE_DPAD_UP:
                bk2::android::PanSinglePlayerCamera(0.0f, -32.0f);
                break;
            case AKEYCODE_S:
            case AKEYCODE_DPAD_DOWN:
                bk2::android::PanSinglePlayerCamera(0.0f, 32.0f);
                break;
            case AKEYCODE_PLUS:
            case AKEYCODE_EQUALS:
                bk2::android::ZoomSinglePlayerCamera(1.1f);
                break;
            case AKEYCODE_MINUS:
                bk2::android::ZoomSinglePlayerCamera(0.9f);
                break;
            case AKEYCODE_Q:
                bk2::android::RotateSinglePlayerCamera(-0.08f);
                break;
            case AKEYCODE_E:
                bk2::android::RotateSinglePlayerCamera(0.08f);
                break;
            case AKEYCODE_P:
            case AKEYCODE_SPACE:
                bk2::android::SetSinglePlayerPaused(
                        !bk2::android::IsSinglePlayerPaused());
                break;
#if !defined(NDEBUG)
            case AKEYCODE_V:
                bk2::android::HandleLegacyInputEvent("local_win");
                break;
            case AKEYCODE_F:
                bk2::android::HandleLegacyInputEvent(
                        "debug_force_combat");
                break;
            case AKEYCODE_T:
                bk2::android::HandleLegacyInputEvent(
                        "debug_combat_effect");
                break;
            case AKEYCODE_K:
                bk2::android::HandleLegacyInputEvent(
                        "debug_kill_attack_target");
                break;
            case AKEYCODE_L:
                bk2::android::HandleLegacyInputEvent(
                        "debug_toggle_lying");
                break;
            case AKEYCODE_M:
                bk2::android::HandleLegacyInputEvent(
                        "debug_kill_mechanized");
                break;
            case AKEYCODE_N:
                bk2::android::HandleLegacyInputEvent(
                        "debug_reinforcement_notification");
                break;
#endif
            default:
                break;
        }
    }

    android_app_clear_motion_events(input_buffer);
    android_app_clear_key_events(input_buffer);
}

}  // namespace

extern "C" void android_main(android_app* app) {
    app->onAppCmd = HandleAppCommand;

    auto& platform = bk2::android::PlatformRuntime::instance();
    platform.log_info("Blitzkrieg 2 Android bootstrap started.");
    bk2::android::AudioBackend().init(44100, 96);
    bk2::android::AudioOutput().start();

    const bk2::android::PortPaths paths = bk2::android::GetPortPaths();
    bk2::android::InitializeLegacyVfs();
    if (!paths.files_dir.empty()) {
        platform.log_info(std::string("Data root: ") + paths.data_root());
        platform.log_info(std::string("Save root: ") + paths.save_root());
        platform.log_info(std::string("Log root: ") + paths.log_root());
    }
    if (bk2::android::InitializeLegacyDatabase()) {
        bk2::android::LoadOriginalMenuScreen(
                "UI/Game/Menu/MainMenu_WindowScreen.xdb");
        platform.log_info(bk2::android::OriginalMenuReport());
    }
    if (bk2::android::IsLegacyDatabaseOpen() &&
        bk2::android::InitializeSinglePlayerRuntime()) {
        platform.log_info(bk2::android::SinglePlayerRuntimeReport());
    } else {
        platform.log_warn(bk2::android::SinglePlayerRuntimeReport());
    }

    const bool menu_active =
            !bk2::android::IsSinglePlayerRuntimeReady() &&
            bk2::android::IsOriginalMenuReady();
    g_menu_input_active = menu_active;
    bool first_render_frame_logged = false;
    bool mission_script_tick_logged = false;
    uint64_t last_tick_millis = platform.monotonic_millis();
    const uint64_t runtime_start_millis = last_tick_millis;
    while (!app->destroyRequested) {
        android_poll_source* source = nullptr;
        int events = 0;
        while (ALooper_pollOnce(0, nullptr, &events, reinterpret_cast<void**>(&source)) >= 0) {
            if (source != nullptr) {
                source->process(app, source);
            }
            if (app->destroyRequested) {
                break;
            }
        }

        PollInput(app);
        const uint64_t now_millis = platform.monotonic_millis();
        const uint32_t elapsed_millis = static_cast<uint32_t>(
                std::min<uint64_t>(now_millis - last_tick_millis, 100));
        last_tick_millis = now_millis;
        if (platform.lifecycle_state() ==
            bk2::android::LifecycleState::Focused) {
            bk2::android::TickSinglePlayerRuntime(elapsed_millis);
            if (!mission_script_tick_logged &&
                now_millis - runtime_start_millis >= 1500) {
                platform.log_info(
                        bk2::android::SinglePlayerRuntimeReport());
                mission_script_tick_logged = true;
            }
        }
        bk2::android::AudioOutput().service();
        if (bk2::android::RenderBackend().is_ready()) {
            // Without a selected mission the shell shows the original menu
            // screen instead of an empty battlefield.
            if (menu_active) {
                bk2::android::RenderOriginalMenu(
                        bk2::android::RenderBackend().width(),
                        bk2::android::RenderBackend().height());
            }
            QueueTouchSelectionOverlay();
            bk2::android::RenderLegacyGfxFrame();
            if (!first_render_frame_logged &&
                bk2::android::RenderBackend().frame_count() > 0) {
                platform.log_info(bk2::android::RenderBackendDiagnosticReport());
                first_render_frame_logged = true;
            }
        }

        platform.sleep_millis(16);
    }

    platform.log_info("Blitzkrieg 2 Android bootstrap stopped.");
    bk2::android::ShutdownSinglePlayerRuntime();
    bk2::android::ShutdownLegacyDatabase();
    bk2::android::ShutdownLegacyVfs();
    bk2::android::ShutdownLegacyGfx();
    bk2::android::AudioOutput().shutdown();
    bk2::android::AudioBackend().shutdown();
}
