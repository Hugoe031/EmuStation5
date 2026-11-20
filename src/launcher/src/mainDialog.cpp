#include "mainDialog.h"

#include "cheatFile.h"
#include "configuration.h"
#include "configurationItem.h"
#include "configurationListWidget.h"
#include "controllerPreview.h"
#include "gameContent.h"
#include "launcherTheme.h"
#include "titleSound.h"
#include "updateChecker.h"

#include <algorithm>
#include <QAction>
#include <QApplication>
#include <QByteArray>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QAbstractButton>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QGuiApplication>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QObject>
#include <QPainter>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QSaveFile>
#include <QScreen>
#include <QSlider>
#include <QSettings>
#include <QShortcut>
#include <QShowEvent>
#include <QStatusBar>
#include <QStringList>
#include <QStyleHints>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVariant>
#include <QVBoxLayout>
#include <QtCore>

#include <cstdint>

#include "ui_main_dialog.h"

#if defined(_WIN32)
#include <windows.h> // IWYU pragma: keep
#endif

// IWYU pragma: no_include <minwindef.h>
// IWYU pragma: no_include <processthreadsapi.h>
// IWYU pragma: no_include <winbase.h>

class QWidget;

#if defined(_WIN32)
constexpr char  EMULATOR_EXE[] = "kyty_emulator.exe";
constexpr char  CMD_EXE[]      = "cmd.exe";
constexpr DWORD CMD_X_CHARS    = 175;
constexpr DWORD CMD_Y_CHARS    = 1000;
#else
constexpr char EMULATOR_EXE[] = "kyty_emulator";
#endif
constexpr char SETTINGS_MAIN_DIALOG[]        = "MainDialog";
constexpr char SETTINGS_MAIN_LAST_GEOMETRY[] = "geometry";
constexpr char SETTINGS_CHECK_UPDATES[]      = "check_updates_on_startup";
constexpr char SETTINGS_CLOSE_TERMINAL[]     = "close_terminal";
constexpr char SETTINGS_DETACHED_CONSOLE[]   = "detached_console";
constexpr char SETTINGS_THEME[]              = "theme";

class MainDialogPrivate: public QObject {
	Q_OBJECT

public:
	explicit MainDialogPrivate(QObject* parent = nullptr): QObject(parent) {}
	~MainDialogPrivate() override;

	void Setup(MainDialog* main_dialog);

	/*slots:*/

	void Update();
	void FindInterpreter();
	void Run();
	void UpdateStatusPaths();

	[[nodiscard]] const QString& GetInterpreter() const { return m_interpreter; }

	static void            WriteSettings(QSettings& s);
	static void            ReadSettings(QSettings& s);
	static LauncherSettings GetLauncherSettings();
	static void            SetLauncherSettings(const LauncherSettings& settings);

private:
	void SetupMenus();
	void SetupStatusBar();
	void RefreshGameCount();

	static QByteArray g_last_geometry;
	static bool       g_check_updates_on_startup;
	static bool       g_close_terminal;
	static bool       g_detached_console;
	static int        g_theme;

	Ui::MainDialog*   m_ui          = {nullptr};
	MainDialog*       m_main_dialog = nullptr;
	UpdateChecker*    m_update_checker = nullptr;
	QLabel*           m_status_games = nullptr;
	QLabel*           m_status_emulator = nullptr;
	QLabel*           m_status_settings = nullptr;
	QString           m_status_settings_path;
	QString           m_status_emulator_path;
	ControllerPreview m_controller_preview;
	TitleSound        m_title_sound;
	QString           m_interpreter;

	int m_running_games = 0;
};

QByteArray MainDialogPrivate::g_last_geometry;
bool       MainDialogPrivate::g_check_updates_on_startup = true;
bool       MainDialogPrivate::g_close_terminal           = false;
#if defined(_WIN32)
bool MainDialogPrivate::g_detached_console = true;
#else
bool MainDialogPrivate::g_detached_console = false;
#endif
int MainDialogPrivate::g_theme = 1;

MainDialog::MainDialog(QWidget* parent): QMainWindow(parent), m_p(new MainDialogPrivate(this)) {
	m_p->Setup(this);
}

MainDialogPrivate::~MainDialogPrivate() {
	delete m_ui;
}

void MainDialogPrivate::UpdateStatusPaths() {
	auto* bar = m_main_dialog->statusBar();
	// Layouts are not activated (and the bar has no meaningful width yet) before the
	// window is shown — fall back to a sane static budget for that first pass.
	const int width    = bar != nullptr && bar->width() >= 200 ? bar->width() : 0;
	const int reserved = (m_status_games != nullptr ? m_status_games->sizeHint().width() + 48 : 48);
	const int budget   = width > 0 ? std::max(120, (width - reserved) / 2) : 420;

	const QFontMetrics metrics(m_status_settings->font());
	const auto elide   = [&metrics, &budget](QLabel* label, const QString& prefix, const QString& path) {
		const int elide_width = std::max(80, budget - metrics.horizontalAdvance(prefix));
		label->setText(prefix + metrics.elidedText(path, Qt::ElideMiddle, elide_width));
		label->setToolTip(path);
	};

	if (!m_status_settings_path.isEmpty()) {
		elide(m_status_settings, tr("Settings: "), m_status_settings_path);
	}
	if (!m_status_emulator_path.isEmpty()) {
		elide(m_status_emulator, tr("Emulator: "), m_status_emulator_path);
	}
}

// Clamp a restored frame to the currently connected screens. QWidget::restoreGeometry()
// faithfully re-applies whatever coordinates were saved — if a monitor was unplugged, the
// DPI changed, or the previous session saved an off-screen sentinel (e.g. -32000), the
// launcher comes up invisible. The restore path also does not enforce the widget's
// minimum size, so a degenerate persisted frame (0×0, or smaller than minimumSize) is
// repaired here even when it technically fits on a screen. Returns true if the frame
// had to be moved or resized.
static bool ClampWindowToVisibleScreens(QWidget* window) {
	const QRect frame    = window->frameGeometry();
	const QSize min_size = window->minimumSize();
	const bool  size_ok  = frame.width() >= min_size.width() &&
	                       frame.height() >= min_size.height();

	if (size_ok) {
		const auto screens = QGuiApplication::screens();
		for (const QScreen* screen: screens) {
			if (screen->availableGeometry().intersects(frame)) {
				return false;
			}
		}
	}
	if (!size_ok) {
		// On-screen but smaller than the minimum: grow in place and nudge the
		// origin back inside the screen if the enlargement overflows.
		const auto* screen = QGuiApplication::screenAt(frame.center());
		if (screen != nullptr) {
			const QRect avail_screen = screen->availableGeometry();
			const QSize size = window->size().expandedTo(min_size).boundedTo(
			    avail_screen.size());
			QPoint pos = frame.topLeft();
			if (pos.x() + size.width() - 1 > avail_screen.right() ||
			    pos.y() + size.height() - 1 > avail_screen.bottom()) {
				pos = QPoint(avail_screen.right() - size.width() + 1,
				             avail_screen.bottom() - size.height() + 1);
			}
			pos.setX(std::max(pos.x(), avail_screen.left()));
			pos.setY(std::max(pos.y(), avail_screen.top()));
			window->resize(size);
			window->move(pos);
			return true;
		}
	}

	const QScreen* primary = QGuiApplication::primaryScreen();
	if (primary == nullptr) {
		return false;
	}

	const QRect avail = primary->availableGeometry();
	QSize       size  = window->size().boundedTo(avail.size());
	size              = size.expandedTo(window->minimumSize());
	if (size.width() > avail.width() || size.height() > avail.height()) {
		size = avail.size();
	}

	window->resize(size);
	window->move(avail.topLeft()
	             + QPoint((avail.width() - size.width()) / 2, (avail.height() - size.height()) / 2));
	return true;
}

void MainDialogPrivate::Setup(MainDialog* main_dialog) {
	m_ui = new Ui::MainDialog;
	m_ui->setupUi(main_dialog);
	m_ui->brand_mark->setPixmap(QIcon(QStringLiteral(":/icons/emustation5.png")).pixmap(QSize(44, 44)));
	m_ui->header_search->setVisible(false);
	m_ui->widget->SetControllerPreview(&m_controller_preview);

	m_main_dialog    = main_dialog;
	m_update_checker = new UpdateChecker(main_dialog);
	m_ui->check_updates_link->setVisible(UpdateChecker::IsSupported());
#if defined(__APPLE__)
	m_ui->check_updates_link->setVisible(false);
#endif

	SetupMenus();
	SetupStatusBar();
	LauncherTheme::Apply(g_theme);

	connect(main_dialog, &MainDialog::Start, this, &MainDialogPrivate::FindInterpreter,
	        Qt::QueuedConnection);
	connect(m_ui->widget, &ConfigurationListWidget::Select, this, [this]() {
		Update();
		const auto* item = m_ui->widget->GetSelectedItem();
		m_title_sound.SetTitle(m_running_games == 0 && item != nullptr ? item->GetInfo().basedir
		                                                               : QString());
	});
	connect(m_ui->widget, &ConfigurationListWidget::PreviewControllerColor, this,
	        [this](const QString& color) {
		        if (m_running_games == 0) {
			        m_controller_preview.SetColor(color);
		        }
	        });
	connect(m_ui->widget, &ConfigurationListWidget::Run, this, &MainDialogPrivate::Run);
	connect(m_ui->header_search, &QLineEdit::textChanged, m_ui->widget,
	        &ConfigurationListWidget::SetSearchQuery);
	connect(m_ui->widget, &ConfigurationListWidget::SearchQueryChanged, m_ui->header_search,
	        &QLineEdit::setText);
	connect(m_ui->check_updates_link, &QLabel::linkActivated, this,
	        [this](const QString&) { m_update_checker->Check(true); });
	connect(m_update_checker, &UpdateChecker::CheckingChanged, m_ui->check_updates_link,
	        &QLabel::setDisabled);
	connect(m_update_checker, &UpdateChecker::InstallingChanged, this, &MainDialogPrivate::Update);
	connect(main_dialog, &MainDialog::Resize, [this]() {
		// Never persist a degenerate frame (0×0 or smaller than the minimum):
		// restoreGeometry() would apply it verbatim and the launcher would
		// come up invisible on the next run.
		const QSize current = m_main_dialog->size();
		const QSize minimum = m_main_dialog->minimumSize();
		if (current.width() >= minimum.width() && current.height() >= minimum.height()) {
			g_last_geometry = m_main_dialog->saveGeometry();
		}
		m_ui->widget->WriteSettings();
		UpdateStatusPaths();
	});

	m_status_settings_path = m_ui->widget->GetSettingsFile();
	UpdateStatusPaths();

	// Keep the launcher reachable even if the saved position is off-screen (stale
	// monitor configuration or an off-screen sentinel persisted in the settings).
	m_main_dialog->restoreGeometry(g_last_geometry);
	if (ClampWindowToVisibleScreens(m_main_dialog)) {
		g_last_geometry = m_main_dialog->saveGeometry();
		// Persist the recovered geometry so the settings file self-heals instead of
		// handing the dead position to the next launch.
		m_ui->widget->WriteSettings();
	}

	Update();
	RefreshGameCount();
}

void MainDialogPrivate::SetupMenus() {
	auto* menubar = m_main_dialog->menuBar();
	menubar->setNativeMenuBar(false);
	// The classic menu bar is replaced by the navigation rail and its More menu.
	// Its actions are kept alive (the More menu reuses them); shortcuts are rebound
	// below because hidden menu actions do not receive key presses.
	menubar->hide();

	const auto invoke = [this](const char* slot) {
		QMetaObject::invokeMethod(m_ui->widget, slot, Qt::DirectConnection);
	};
	const auto show_search = [this]() {
		m_ui->header_search->setVisible(true);
		m_ui->header_search->setFocus();
		m_ui->header_search->selectAll();
	};

	auto* file = m_ui->menu_File;
	auto* add_game_folder = file->addAction(tr("Add Game Folder..."));
	connect(add_game_folder, &QAction::triggered, m_ui->widget,
	        &ConfigurationListWidget::edit_global_settings);

	auto* refresh = file->addAction(tr("Refresh Game List"));
	refresh->setShortcut(QKeySequence::Refresh);
	connect(refresh, &QAction::triggered, m_ui->widget,
	        &ConfigurationListWidget::ScanGameDirectory);

	file->addSeparator();
	auto* quit = file->addAction(tr("Exit"));
	quit->setShortcut(QKeySequence::Quit);
	connect(quit, &QAction::triggered, m_main_dialog, &QWidget::close);

	auto* emulation = m_ui->menu_Emulation;
	auto* start = emulation->addAction(tr("Start"));
	start->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_R));
	connect(start, &QAction::triggered, m_ui->widget, &ConfigurationListWidget::Run);

	auto* configure_global = emulation->addAction(tr("Configure..."));
	connect(configure_global, &QAction::triggered, m_ui->widget,
	        &ConfigurationListWidget::edit_global_settings);

	auto* input_mapping = emulation->addAction(tr("Input Mapping..."));
	connect(input_mapping, &QAction::triggered, this,
	        [invoke]() { invoke("edit_input_mapping"); });

	auto* configure_game = emulation->addAction(tr("Configure Game..."));
	connect(configure_game, &QAction::triggered, this,
	        [invoke]() { invoke("edit_configuration"); });

	emulation->addSeparator();
	auto* view_trophies = emulation->addAction(tr("View Trophies"));
	connect(view_trophies, &QAction::triggered, m_ui->widget,
	        &ConfigurationListWidget::ViewTrophies);

	auto* trophy_overview = emulation->addAction(tr("Trophy Overview"));
	connect(trophy_overview, &QAction::triggered, m_ui->widget,
	        &ConfigurationListWidget::ViewTrophyOverview);

	auto* view = m_ui->menu_View;
	auto* focus_search = view->addAction(tr("Focus Search"));
	focus_search->setShortcut(QKeySequence::Find);
	connect(focus_search, &QAction::triggered, this, [show_search]() { show_search(); });

	auto* grid_action = view->addAction(tr("Grid View"));
	grid_action->setCheckable(true);
	grid_action->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_G));
	if (auto* grid_button =
	        m_ui->widget->findChild<QToolButton*>(QStringLiteral("grid_view_button"))) {
		grid_action->setChecked(grid_button->isChecked());
		connect(grid_action, &QAction::toggled, grid_button, &QToolButton::setChecked);
		connect(grid_button, &QToolButton::toggled, grid_action, &QAction::setChecked);
	}

	// Grid icon size, mirroring the toolbar slider (singleStep = 16 px).
	auto* increase_icon = view->addAction(tr("Increase Grid Icon Size"));
	increase_icon->setShortcuts(
	    {QKeySequence(Qt::CTRL | Qt::Key_Equal), QKeySequence(Qt::CTRL | Qt::Key_Plus)});
	auto* decrease_icon = view->addAction(tr("Decrease Grid Icon Size"));
	decrease_icon->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Minus));
	const auto adjust_grid_icon_size = [this](int steps) {
		if (auto* slider =
		        m_ui->widget->findChild<QSlider*>(QStringLiteral("grid_size_slider"))) {
			slider->setValue(slider->value() + steps * slider->singleStep());
		}
	};
	connect(increase_icon, &QAction::triggered, this, [adjust_grid_icon_size]() { adjust_grid_icon_size(1); });
	connect(decrease_icon, &QAction::triggered, this, [adjust_grid_icon_size]() { adjust_grid_icon_size(-1); });

	auto* tools = m_ui->menu_Tools;
	auto* check_updates = tools->addAction(tr("Check for Updates..."));
	check_updates->setVisible(UpdateChecker::IsSupported());
	connect(check_updates, &QAction::triggered, this,
	        [this]() { m_update_checker->Check(true); });

	auto* settings_folder = tools->addAction(tr("Open Settings Folder"));
	connect(settings_folder, &QAction::triggered, this, [this]() {
		QDesktopServices::openUrl(
		    QUrl::fromLocalFile(QFileInfo(m_ui->widget->GetSettingsFile()).absolutePath()));
	});

	tools->addSeparator();
	auto* settings = tools->addAction(tr("Settings..."));
	settings->setShortcut(QKeySequence::Preferences);
	connect(settings, &QAction::triggered, m_ui->widget,
	        &ConfigurationListWidget::edit_global_settings);

	auto* help = m_ui->menu_Help;
	auto* about = help->addAction(tr("About EmuStation5"));
	connect(about, &QAction::triggered, this, [this]() {
		QMessageBox::about(
		    m_main_dialog, tr("About EmuStation5"),
		    tr("<h3>EmuStation5</h3>"
		       "<p>A PlayStation 5 emulator for Windows and Linux.</p>"
		       "<p>%1</p>")
		        .arg(m_ui->label_Version->text()));
	});
	auto* about_qt = help->addAction(tr("About Qt"));
	connect(about_qt, &QAction::triggered, qApp, &QApplication::aboutQt);

	// PS5-style navigation rail. The existing monochrome glyphs are tinted to a
	// soft white so they read against the dark rail.
	const auto tinted_icon = [](const QString& resource) {
		auto pixmap = QIcon(resource).pixmap(QSize(28, 28));
		QPainter painter(&pixmap);
		painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
		painter.fillRect(pixmap.rect(), QColor(0xd6, 0xe2, 0xf2));
		painter.end();
		return QIcon(pixmap);
	};
	m_ui->nav_play_button->setIcon(tinted_icon(QStringLiteral(":/icons/grid-view.svg")));
	m_ui->nav_trophies_button->setIcon(tinted_icon(QStringLiteral(":/icons/trophy.svg")));
	m_ui->nav_settings_button->setIcon(tinted_icon(QStringLiteral(":/icons/global-settings.svg")));
	m_ui->nav_search_button->setIcon(tinted_icon(QStringLiteral(":/icons/search.svg")));
	m_ui->nav_more_button->setIcon(tinted_icon(QStringLiteral(":/icons/sort.svg")));

	connect(m_ui->nav_play_button, &QToolButton::clicked, this, [this]() {
		m_ui->nav_play_button->setChecked(true);
		if (auto* list = m_ui->widget->findChild<QTreeWidget*>(QStringLiteral("cfgs_list"))) {
			list->setFocus();
		}
	});
	connect(m_ui->nav_trophies_button, &QToolButton::clicked, m_ui->widget,
	        &ConfigurationListWidget::ViewTrophyOverview);
	connect(m_ui->nav_settings_button, &QToolButton::clicked, m_ui->widget,
	        &ConfigurationListWidget::edit_global_settings);
	connect(m_ui->nav_search_button, &QToolButton::clicked, this,
	        [show_search]() { show_search(); });
	auto* more_menu = new QMenu(m_ui->nav_more_button);
	for (QAction* menu_action: menubar->actions()) {
		auto* menu = menu_action->menu();
		if (menu != nullptr) {
			more_menu->addAction(menu_action);
		}
		// Preserve keyboard shortcuts now that the menu bar itself is hidden.
		if (menu == nullptr) {
			continue;
		}
		for (QAction* action: menu->actions()) {
			const QKeySequence keys = action->shortcut();
			if (keys.isEmpty()) {
				continue;
			}
			action->setShortcut(QKeySequence());
			auto* shortcut = new QShortcut(keys, m_main_dialog);
			connect(shortcut, &QShortcut::activated, action, &QAction::trigger);
		}
	}

	// The library toolbar now shows only Refresh / Input mapping / Delete.
	// Sorting remains available from the left rail's More menu.
	if (auto* sort_button =
	        m_ui->widget->findChild<QToolButton*>(QStringLiteral("sort_button"))) {
		if (auto* source = sort_button->menu()) {
			auto* sort_menu = new QMenu(tr("Sort Games"), m_main_dialog);
			for (QAction* action: source->actions()) {
				sort_menu->addAction(action);
			}
			more_menu->addMenu(sort_menu);
		}
	}
	m_ui->nav_more_button->setMenu(more_menu);
}

void MainDialogPrivate::SetupStatusBar() {
	auto* bar = m_main_dialog->statusBar();
	bar->setObjectName(QStringLiteral("launcher_status_bar"));
	bar->setSizeGripEnabled(false);

	m_status_games = new QLabel(tr("No games"), bar);
	m_status_games->setObjectName(QStringLiteral("status_games"));
	m_status_games->setAccessibleName(tr("Game count"));
	bar->addWidget(m_status_games);

	m_status_settings = new QLabel(bar);
	m_status_settings->setObjectName(QStringLiteral("status_settings"));
	m_status_settings->setAccessibleName(tr("Settings file location"));
	m_status_settings->setTextInteractionFlags(Qt::TextSelectableByMouse);
	bar->addPermanentWidget(m_status_settings);

	m_status_emulator = new QLabel(tr("Emulator: not found"), bar);
	m_status_emulator->setObjectName(QStringLiteral("status_emulator"));
	m_status_emulator->setAccessibleName(tr("Emulator executable location"));
	m_status_emulator->setTextInteractionFlags(Qt::TextSelectableByMouse);
	bar->addPermanentWidget(m_status_emulator);
}

void MainDialogPrivate::RefreshGameCount() {
	if (m_status_games == nullptr) {
		return;
	}
	const auto* list = m_ui->widget->findChild<QTreeWidget*>(QStringLiteral("cfgs_list"));
	const int   count = list != nullptr ? list->topLevelItemCount() : 0;
	m_status_games->setText(count == 1 ? tr("1 game") : tr("%1 games").arg(count));
}

void MainDialogPrivate::FindInterpreter() {
	UpdateChecker::ShowPreviousResult(m_main_dialog);
	QDir search_dir(QApplication::applicationDirPath());
	m_interpreter = search_dir.absoluteFilePath(EMULATOR_EXE);

	if (!QFile::exists(m_interpreter)) {
		search_dir.cdUp();
		m_interpreter = search_dir.absoluteFilePath(EMULATOR_EXE);
	}

	bool found = QFile::exists(m_interpreter);
	m_ui->widget->SetRuntimeDirectory(QFileInfo(m_interpreter).absolutePath());

	if (found) {
		m_status_emulator_path = m_interpreter;
		UpdateStatusPaths();

		QProcess test;
		test.setProgram(m_interpreter);
		test.start();
		test.waitForFinished();

		auto output = QString(test.readAllStandardOutput());
		auto lines  = output.split(QRegularExpression("[\r\n]"), Qt::SkipEmptyParts);

		if (lines.count() >= 2) {
			const auto version =
			    lines.at(0).startsWith("exe_name") ? lines.at(1) : lines.at(0);
			m_ui->label_Version->setText(tr("Version: ") + version);
			m_ui->label_Version->setToolTip(version);
		} else {
			found = false;
		}
	}

	if (!found) {
		QMessageBox::critical(m_main_dialog, tr("Error"), tr("Can't find emulator"));
		QApplication::quit();
		return;
	}

	// Prompt for a game folder when none are configured, but keep the launcher
	// open if the user dismisses the dialog (quitting here can segfault during
	// nested modal shutdown / background compatibility load).
	m_ui->widget->EnsureGameDirectory();

	m_status_settings_path = m_ui->widget->GetSettingsFile();
	UpdateStatusPaths();

	Update();
	if (g_check_updates_on_startup) {
		m_update_checker->Check(false);
	}
}

static void AddFlag(QStringList& args, bool enabled, const char* name) {
	if (enabled) {
		args << name;
	}
}

static QStringList CreateEmulatorArgs(const Configuration& info) {
	QStringList args;
	auto        r = Configuration::NormalizeResolution(info.screen_resolution).split('x');

	if (r.size() != 2) {
		return {};
	}

	args << "--screen-width" << r.at(0);
	args << "--screen-height" << r.at(1);
	args << "--user-name" << info.user_name;
	args << "--user-id" << QString::number(info.user_id);
	if (!info.audio_input_device.isEmpty()) {
		args << "--mic" << info.audio_input_device;
	}
	if (!info.controller.color.isEmpty()) {
		args << "--controller-color" << info.controller.color;
	}
	args << "--controller-volume" << QString::number(info.controller.speaker_volume);
	args << "--controller-vibration" << QString::number(info.controller.vibration_intensity);
	args << "--keyboard-player" << QString::number(info.controller.keyboard_player);
	args << "--present-mode" << EnumToText(info.present_mode);
	if (info.gpu_index >= 0) {
		args << "--gpu" << QString::number(info.gpu_index);
	}
	AddFlag(args, info.fullscreen_enabled, "--fullscreen");
	AddFlag(args, info.hdr_enabled, "--hdr");
	AddFlag(args, info.hide_cursor_enabled, "--hide-cursor");
	AddFlag(args, info.stretch_enabled, "--stretch");
	AddFlag(args, info.readback_linear_images, "--readback-linear-images");
	AddFlag(args, info.readback_tiled_images, "--readback-tiled-images");
	AddFlag(args, info.sync_raw_image_buffers, "--sync-raw-image-buffers");
	AddFlag(args, !info.trophy_enabled, "--no-trophy-notifications");
	args << "--trophy-duration" << QString::number(info.trophy_duration_seconds);
	AddFlag(args, info.perf_overlay_enabled, "--perf-overlay");
	AddFlag(args, info.skip_notice_screen, "--skip-notice-screen");
	AddFlag(args, info.network_connected, "--network-connected");
	AddFlag(args, info.psn_emulation_enabled, "--emulate-psn");
	AddFlag(args, info.tessellation_enabled, "--tessellation");
	AddFlag(args, info.occlusion_queries_enabled, "--occlusion-queries");
	AddFlag(args, info.shader_cache_enabled, "--shader-cache");
	args << "--vblank-frequency" << QString::number(info.vblank_frequency);
	args << "--console-language" << QString::number(info.console_language);
	AddFlag(args, info.vulkan_validation_enabled, "--vulkan-validation");
	AddFlag(args, info.shader_validation_enabled, "--shader-validation");
	args << "--shader-log-direction" << EnumToText(info.shader_log_direction);
	args << "--shader-log-folder" << info.shader_log_folder;
	AddFlag(args, info.command_buffer_dump_enabled, "--command-buffer-dump");
	args << "--command-buffer-dump-folder" << info.command_buffer_dump_folder;
	args << "--printf-direction" << EnumToText(info.printf_direction);
	args << "--printf-output-file" << info.printf_output_file;
	AddFlag(args, info.profiler_enabled, "--profile");
	AddFlag(args, info.pro_enabled, "--trinity");
	AddFlag(args, info.amd_cpu_enabled, "--amd-cpu");
	AddFlag(args, info.guest_tsc_enabled, "--guest-tsc");
#if defined(_WIN32)
	AddFlag(args, info.red_zone_protection_enabled, "--redzone");
#endif
	for (const auto& binding: info.host_input_mapping) {
		args << "--keymap" << binding;
	}
	AddFlag(args, info.renderdoc_enabled, "--rd");

	QString game = info.basedir;
	if (!info.elf.isEmpty() && !GameContent::IsArchive(info.basedir)) {
		game = QDir(info.basedir).filePath(info.elf);
	}
	args << "--game" << game;
	if (!info.addons_dir.isEmpty()) {
		args << "--addcont-dir" << info.addons_dir;
	}
	for (const auto& entry: info.addcont) {
		args << "--addcont" << entry;
	}

	const auto patch_plan = Cheats::PlanPath(info.title_id);
	if (QFileInfo::exists(patch_plan)) {
		args << "--game-patch" << patch_plan;
	}

	return args;
}

#ifdef __linux__
// Runs the command in bash, then waits for a key (with close_on_clean_exit, only after an error).
// The command goes in as positional parameters ("$@"), so it needs no quoting.
static QStringList BashCommand(const QString& interpreter, const QStringList& args,
                               bool close_on_clean_exit) {
	const QString run = close_on_clean_exit ? "\"$@\" && exit" : "\"$@\"";
	return QStringList {"bash", "-c", run + "; echo Press any key...; read -n1", "kyty",
	                    interpreter} +
	       args;
}

// Find a terminal and its command separator.
static bool FindTerminal(QString* program, QStringList* prefix) {
	struct TerminalSpec {
		const char* executable;
		const char* separator; // nullptr when the command follows immediately
	};

	static const TerminalSpec candidates[] = {
	    {"x-terminal-emulator", "-e"},
	    {"gnome-terminal", "--"},
	    {"konsole", "-e"},
	    {"xfce4-terminal", "-x"},
	    {"mate-terminal", "--"},
	    {"tilix", "-e"},
	    {"alacritty", "-e"},
	    {"kitty", nullptr},
	    {"foot", nullptr},
	    {"wezterm", "-e"},
	    {"urxvt", "-e"},
	    {"xterm", "-e"},
	};

	const auto try_candidate = [program, prefix](const QString& executable, const char* separator) {
		const auto resolved = QStandardPaths::findExecutable(executable);
		if (resolved.isEmpty()) {
			return false;
		}
		*program = resolved;
		prefix->clear();
		if (separator != nullptr) {
			*prefix << QString::fromLatin1(separator);
		}
		return true;
	};

	if (const auto from_env = qEnvironmentVariable("TERMINAL"); !from_env.isEmpty()) {
		// Reuse the known separator for an explicit terminal.
		const auto  env_name  = QFileInfo(from_env).fileName();
		const char* separator = "-e";
		for (const auto& candidate: candidates) {
			if (env_name == QLatin1String(candidate.executable)) {
				separator = candidate.separator;
				break;
			}
		}
		if (try_candidate(from_env, separator)) {
			return true;
		}
	}

	for (const auto& candidate: candidates) {
		if (try_candidate(QString::fromLatin1(candidate.executable), candidate.separator)) {
			return true;
		}
	}

	return false;
}
#endif

#if defined(_WIN32)
// cmd /K arguments that keep the console open (with close_on_clean_exit, only after an error).
// A quote is written as "" rather than \" so cmd's quote count stays even and operators inside
// arguments stay quoted.
static QString CmdCommand(const QString& interpreter, const QStringList& args,
                          bool close_on_clean_exit) {
	static const QRegularExpression quote_or_end(QStringLiteral(R"((\\*)("|\z))"));
	// The UCRT reads 2n backslashes before a quote as n.
	auto quoted = QStringList {interpreter} + args;
	quoted.replaceInStrings(quote_or_end, QStringLiteral(R"(\1\1\2\2)"));
	return QStringLiteral("/K \"\"") + quoted.join(QStringLiteral("\" \"")) +
	       (close_on_clean_exit ? "\" && exit\"" : "\"\"");
}
#endif

void MainDialog::RunInterpreter(QProcess* process, const Configuration& info, bool close_terminal,
                                bool detached) {
	const auto& interpreter = m_p->GetInterpreter();

	auto args = CreateEmulatorArgs(info);
	if (args.isEmpty()) {
		QMessageBox::critical(this, tr("Error"), tr("Invalid emulator configuration"));
		QApplication::quit();
		return;
	}

	const auto working_directory = QFileInfo(interpreter).absolutePath();

#if defined(_WIN32)
	if (detached) {
		// Run the emulator itself, without cmd.exe and without a console window. Its console
		// output goes to a file next to the emulator. Because no console is attached, closing a
		// terminal cannot terminate the game, and the detached QProcess below keeps it alive
		// when the launcher exits.
		process->setProgram(interpreter);
		process->setArguments(args);
		process->setWorkingDirectory(working_directory);
		process->setProcessChannelMode(QProcess::MergedChannels);
		process->setStandardOutputFile(
		    QDir(working_directory).filePath(QStringLiteral("_kyty_console.txt")),
		    QIODevice::Truncate);
		process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
			args->flags |= static_cast<uint32_t>(CREATE_NO_WINDOW);
		});
		process->start();
		if (!process->waitForStarted(5000)) {
			QMessageBox::critical(this, tr("Error"),
			                      tr("Failed to start:\n%1\n\n%2").arg(process->program(),
			                                                          process->errorString()));
			return;
		}
		// QProcess kills its child in its destructor, so drop the parent link: the game must
		// outlive the launcher. The process object is freed by the stateChanged handler once
		// the game exits, or by the OS if the launcher quits first.
		process->setParent(nullptr);
		return;
	}
#endif

	// Without a terminal, the emulator runs directly.
	process->setProgram(interpreter);
	process->setArguments(args);
#ifdef __linux__
	QString     terminal;
	QStringList terminal_prefix;
	if (FindTerminal(&terminal, &terminal_prefix)) {
		process->setProgram(terminal);
		process->setArguments(terminal_prefix + BashCommand(interpreter, args, close_terminal));
	}
#elif defined(_WIN32)
	// Use nativeArguments so Qt does not re-quote the /K command string.
	process->setProgram(CMD_EXE);
	process->setArguments({});
	process->setNativeArguments(
	    CmdCommand(QDir::toNativeSeparators(interpreter), args, close_terminal));
#endif
	process->setWorkingDirectory(working_directory);
#if defined(_WIN32)
	process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
		args->flags |= static_cast<uint32_t>(CREATE_NEW_CONSOLE);
		args->startupInfo->dwFlags &= ~static_cast<DWORD>(STARTF_USESTDHANDLES);
		args->startupInfo->dwFlags |= static_cast<DWORD>(STARTF_USECOUNTCHARS);
		args->startupInfo->dwXCountChars = CMD_X_CHARS;
		args->startupInfo->dwYCountChars = CMD_Y_CHARS;
		// args->startupInfo->dwFlags |= static_cast<DWORD>(STARTF_USEFILLATTRIBUTE);
		// args->startupInfo->dwFillAttribute =
		//     static_cast<DWORD>(BACKGROUND_BLUE) | static_cast<DWORD>(FOREGROUND_RED) |
		//     static_cast<DWORD>(FOREGROUND_INTENSITY);
	});
#endif
	process->start();
#if !defined(_WIN32)
	// Report immediate launch failures.
	if (!process->waitForStarted(5000)) {
		QMessageBox::critical(
		    this, tr("Error"),
		    tr("Failed to start:\n%1\n\n%2").arg(process->program(), process->errorString()));
		return;
	}
#endif
	process->waitForFinished(100);
}

void MainDialog::WriteSettings(QSettings& s) {
	MainDialogPrivate::WriteSettings(s);
}

void MainDialog::ReadSettings(QSettings& s) {
	MainDialogPrivate::ReadSettings(s);
}

LauncherSettings MainDialog::GetLauncherSettings() {
	return MainDialogPrivate::GetLauncherSettings();
}

void MainDialog::SetLauncherSettings(const LauncherSettings& settings) {
	MainDialogPrivate::SetLauncherSettings(settings);
}

void MainDialog::resizeEvent(QResizeEvent* event) {
	emit Resize();
	QMainWindow::resizeEvent(event);
}

void MainDialog::showEvent(QShowEvent* event) {
	QMainWindow::showEvent(event);
	// The environment can park a just-created window off-screen (observed as -32000
	// at 200% DPI), which would leave the launcher invisible until manually restored.
	if (ClampWindowToVisibleScreens(this)) {
		emit Resize();
	}
	// The bar has its real width only once it is shown — re-elide the paths to fit.
	m_p->UpdateStatusPaths();
}

void MainDialogPrivate::WriteSettings(QSettings& s) {
	s.beginGroup(SETTINGS_MAIN_DIALOG);

	if (!g_last_geometry.isEmpty()) {
		s.setValue(SETTINGS_MAIN_LAST_GEOMETRY, g_last_geometry);
	}
	s.setValue(SETTINGS_CHECK_UPDATES, g_check_updates_on_startup);
	s.setValue(SETTINGS_CLOSE_TERMINAL, g_close_terminal);
	s.setValue(SETTINGS_DETACHED_CONSOLE, g_detached_console);
	s.setValue(SETTINGS_THEME, g_theme);

	s.endGroup();
}

void MainDialogPrivate::ReadSettings(QSettings& s) {
	s.beginGroup(SETTINGS_MAIN_DIALOG);

	g_last_geometry = s.value(SETTINGS_MAIN_LAST_GEOMETRY, g_last_geometry).toByteArray();
	g_check_updates_on_startup = s.value(SETTINGS_CHECK_UPDATES, true).toBool();
	g_close_terminal           = s.value(SETTINGS_CLOSE_TERMINAL, false).toBool();
#if defined(_WIN32)
	g_detached_console = s.value(SETTINGS_DETACHED_CONSOLE, true).toBool();
#else
	// The detached console mode is Windows-only.
	g_detached_console = false;
#endif
	g_theme = s.value(SETTINGS_THEME, 1).toInt();
	if (g_theme < 0 || g_theme > 2) {
		g_theme = 1;
	}

	s.endGroup();
}

LauncherSettings MainDialogPrivate::GetLauncherSettings() {
	return {g_theme, g_check_updates_on_startup, g_close_terminal, g_detached_console};
}

void MainDialogPrivate::SetLauncherSettings(const LauncherSettings& settings) {
	g_theme                    = settings.theme;
	g_check_updates_on_startup = settings.check_updates_on_startup;
	g_close_terminal           = settings.close_terminal;
	g_detached_console         = settings.detached_console;
	if (g_theme < 0 || g_theme > 2) {
		g_theme = 1;
	}
	LauncherTheme::Apply(g_theme);
}

void MainDialogPrivate::Run() {
	if (m_update_checker->IsInstalling() || !m_ui->widget->IsRunEnabled()) {
		return;
	}
	QPointer<ConfigurationItem> item = m_ui->widget->GetSelectedItem();
	if (item == nullptr) {
		return;
	}

	item->SetRunning(true);
	m_running_games++;
	m_controller_preview.SetSuspended(true);
	m_title_sound.SetTitle({});

	auto* process = new QProcess(this);
	connect(process, &QProcess::stateChanged, this,
	        [this, process, item](QProcess::ProcessState state) {
		        if (state == QProcess::NotRunning) {
			        if (item != nullptr) {
				        item->SetRunning(false);
			        }
			        m_running_games--;
			        process->deleteLater();
			        Update();
		        }
	        });
	auto info = m_ui->widget->CreateConfiguration(*item);
	m_main_dialog->RunInterpreter(process, *info, g_close_terminal, g_detached_console);

	Update();
}

void MainDialogPrivate::Update() {
	RefreshGameCount();
	const auto* item = m_ui->widget->GetSelectedItem();

	bool run_enabled = item != nullptr && !item->IsRunning() && !m_update_checker->IsInstalling();

	if (run_enabled) {
		const auto& info = item->GetInfo();
		auto        dir  = info.basedir;
		run_enabled      = !dir.isEmpty() && (QDir(dir).exists() || GameContent::IsArchive(dir));
	}

	m_ui->widget->SetRunEnabled(run_enabled);
	m_update_checker->SetGameRunning(m_running_games > 0);
	if (m_running_games > 0) {
		m_controller_preview.SetSuspended(true);
		return;
	}
	m_controller_preview.SetSuspended(false);
	m_controller_preview.SetColor(m_ui->widget->GetGlobalControllerColor());
}

#include "mainDialog.moc"
