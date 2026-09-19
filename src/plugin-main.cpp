#include <obs-module.h>
#include <obs-frontend-api.h>

#include <QAction>
#include <QObject>

#include "hud-window.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("clatasha-hud", "en-US")

static ClatashaHudWindow *g_hud = nullptr;
static QAction *g_toolsAction = nullptr;

MODULE_EXPORT const char *obs_module_description(void)
{
    return "Native local-only HUD for OBS Studio";
}

static void ensure_hud()
{
    if (!g_hud) {
        g_hud = new ClatashaHudWindow();
    }
}

static void toggle_hud()
{
    ensure_hud();

    if (g_hud->isVisible()) {
        g_hud->hide();
    } else {
        g_hud->show();
        g_hud->raise();
    }
}

static void on_frontend_event(enum obs_frontend_event event, void *)
{
    switch (event) {
    case OBS_FRONTEND_EVENT_FINISHED_LOADING:
        ensure_hud();
        g_hud->show();
        break;

    case OBS_FRONTEND_EVENT_EXIT:
        if (g_hud) {
            g_hud->close();
            delete g_hud;
            g_hud = nullptr;
        }
        break;

    default:
        break;
    }
}

bool obs_module_load(void)
{
    blog(LOG_INFO, "[Clatasha HUD] Loading plugin");

    obs_frontend_add_event_callback(on_frontend_event, nullptr);

    g_toolsAction = static_cast<QAction *>(
        obs_frontend_add_tools_menu_qaction(obs_module_text("ClatashaHUD.Menu")));

    if (g_toolsAction) {
        QObject::connect(g_toolsAction, &QAction::triggered, []() { toggle_hud(); });
    }

    return true;
}

void obs_module_unload(void)
{
    obs_frontend_remove_event_callback(on_frontend_event, nullptr);

    if (g_toolsAction) {
        QObject::disconnect(g_toolsAction, nullptr, nullptr, nullptr);
        g_toolsAction = nullptr;
    }

    if (g_hud) {
        g_hud->close();
        delete g_hud;
        g_hud = nullptr;
    }

    blog(LOG_INFO, "[Clatasha HUD] Plugin unloaded");
}
