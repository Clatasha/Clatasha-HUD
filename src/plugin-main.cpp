#include <obs-module.h>
#include <obs-frontend-api.h>

#include <QAction>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QObject>
#include <QSlider>
#include <QVBoxLayout>

#include "hud-window.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("clatasha-hud", "en-US")

static ClatashaHudWindow *g_hud = nullptr;
static QAction *g_toolsAction = nullptr;
static QAction *g_settingsAction = nullptr;

MODULE_EXPORT const char *obs_module_description(void)
{
    return "Native local-only HUD for OBS Studio";
}

static void ensure_hud()
{
    if (!g_hud)
        g_hud = new ClatashaHudWindow();
}

static void toggle_hud()
{
    ensure_hud();

    if (g_hud->isVisible()) {
        g_hud->hide();
    } else {
        g_hud->show();
        g_hud->positionHud();
        g_hud->raise();
    }
}

static void show_settings()
{
    ensure_hud();

    const int originalOpacity = g_hud->opacityPercent();
    const QString originalLocation = g_hud->location();

    QWidget *parent = static_cast<QWidget *>(obs_frontend_get_main_window());
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("Clatasha HUD Settings"));
    dialog.setModal(true);
    dialog.setMinimumWidth(330);

    auto *opacitySlider = new QSlider(Qt::Horizontal, &dialog);
    opacitySlider->setRange(10, 100);
    opacitySlider->setValue(originalOpacity);

    auto *opacityValue = new QLabel(QStringLiteral("%1%").arg(originalOpacity), &dialog);
    auto *opacityRow = new QWidget(&dialog);
    auto *opacityLayout = new QHBoxLayout(opacityRow);
    opacityLayout->setContentsMargins(0, 0, 0, 0);
    opacityLayout->addWidget(opacitySlider, 1);
    opacityLayout->addWidget(opacityValue);

    auto *locationBox = new QComboBox(&dialog);
    locationBox->addItem(QStringLiteral("Top Right"), QStringLiteral("top-right"));
    locationBox->addItem(QStringLiteral("Top Left"), QStringLiteral("top-left"));
    locationBox->addItem(QStringLiteral("Bottom Right"), QStringLiteral("bottom-right"));
    locationBox->addItem(QStringLiteral("Bottom Left"), QStringLiteral("bottom-left"));

    const int currentLocation = locationBox->findData(originalLocation);
    locationBox->setCurrentIndex(currentLocation >= 0 ? currentLocation : 0);

    auto *form = new QFormLayout();
    form->addRow(QStringLiteral("Opacity"), opacityRow);
    form->addRow(QStringLiteral("HUD location"), locationBox);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);

    auto *layout = new QVBoxLayout(&dialog);
    layout->addLayout(form);
    layout->addSpacing(8);
    layout->addWidget(buttons);

    QObject::connect(opacitySlider, &QSlider::valueChanged, [&](int value) {
        opacityValue->setText(QStringLiteral("%1%").arg(value));
        g_hud->setOpacityPercent(value);
    });

    QObject::connect(locationBox, QOverload<int>::of(&QComboBox::currentIndexChanged), [&](int) {
        g_hud->setLocation(locationBox->currentData().toString());
    });

    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() == QDialog::Accepted) {
        g_hud->saveSettings();
    } else {
        g_hud->setOpacityPercent(originalOpacity);
        g_hud->setLocation(originalLocation);
    }
}

static void on_frontend_event(enum obs_frontend_event event, void *)
{
    switch (event) {
    case OBS_FRONTEND_EVENT_FINISHED_LOADING:
        ensure_hud();
        g_hud->show();
        g_hud->positionHud();
        break;

    case OBS_FRONTEND_EVENT_EXIT:
        if (g_hud) {
            g_hud->saveSettings();
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
    g_settingsAction = static_cast<QAction *>(
        obs_frontend_add_tools_menu_qaction(obs_module_text("ClatashaHUD.Settings")));

    if (g_toolsAction)
        QObject::connect(g_toolsAction, &QAction::triggered, []() { toggle_hud(); });
    if (g_settingsAction)
        QObject::connect(g_settingsAction, &QAction::triggered, []() { show_settings(); });

    return true;
}

void obs_module_unload(void)
{
    obs_frontend_remove_event_callback(on_frontend_event, nullptr);

    if (g_toolsAction) {
        QObject::disconnect(g_toolsAction, nullptr, nullptr, nullptr);
        g_toolsAction = nullptr;
    }

    if (g_settingsAction) {
        QObject::disconnect(g_settingsAction, nullptr, nullptr, nullptr);
        g_settingsAction = nullptr;
    }

    if (g_hud) {
        g_hud->saveSettings();
        g_hud->close();
        delete g_hud;
        g_hud = nullptr;
    }

    blog(LOG_INFO, "[Clatasha HUD] Plugin unloaded");
}
