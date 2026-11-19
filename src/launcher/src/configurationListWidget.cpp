#include "configurationListWidget.h"

#include "additionalContentDialog.h"
#include "cheatFile.h"
#include "common/archive.h"
#include "compatibilityDatabase.h"
#include "configuration.h"
#include "configurationEditDialog.h"
#include "configurationItem.h"
#include "gameContent.h"
#include "gameViews.h"
#include "inputMappingDialog.h"
#include "mainDialog.h"
#include "patchesDialog.h"
#include "trophyViewerDialog.h"

#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QCursor>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QIcon>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QKeySequence>
#include <QLineEdit>
#include <QListView>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPalette>
#include <QPointer>
#include <QProcess>
#include <QProgressDialog>
#include <QPromise>
#include <QPushButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QSaveFile>
#include <QSet>
#include <QShortcut>
#include <QTimer>
#include <QVBoxLayout>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QStyle>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QtConcurrentRun>
#include <QtCore>

#ifdef __linux__
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#endif

#include <atomic>
#include <memory>
#include <vector>

#include "ui_configuration_list_widget.h"

constexpr char CONF_FILE_NAME[]    = "Kyty.ini";
constexpr char CONF_ORG_NAME[]     = "Kyty";
constexpr char CONF_APP_NAME[]     = "Kyty";
constexpr char CONF_SECTION_NAME[] = "GameConfigurations";
constexpr char CONF_LAUNCHER[]     = "Launcher";
constexpr char CONF_GAME_DIR[]     = "game_dir";
constexpr char CONF_GAME_DIRS[]    = "game_dirs";
constexpr char CONF_ADDONS_DIR[]   = "addons_dir";
constexpr char CONF_ADDCONT[]      = "AdditionalContent";
constexpr char CONF_GLOBAL[]       = "GlobalConfiguration";
constexpr char SAVE_DATA_DIR[]     = "_SaveData";

static QString NormalizeGameDirectory(const QString& dir) {
	const auto trimmed = dir.trimmed();
	if (trimmed.isEmpty()) {
		return {};
	}

	return QDir::cleanPath(QDir(trimmed).absolutePath());
}

static QString PathKey(const QString& path) {
	auto normalized = NormalizeGameDirectory(path);
	if (normalized.isEmpty()) {
		normalized = QDir::cleanPath(path.trimmed());
	}
	if (normalized.isEmpty()) {
		return {};
	}

	auto canonical = QFileInfo(normalized).canonicalFilePath();
	if (canonical.isEmpty()) {
		canonical = normalized;
	}
	canonical = QDir::cleanPath(canonical);

#ifdef __linux__
	return canonical;
#else
	return canonical.toCaseFolded();
#endif
}

static QStringList NormalizeGameDirectories(const QStringList& dirs) {
	QStringList   dirs_ret;
	QSet<QString> seen;

	for (const auto& dir: dirs) {
		const auto normalized = NormalizeGameDirectory(dir);
		const auto key        = PathKey(normalized);
		if (normalized.isEmpty() || key.isEmpty() || seen.contains(key)) {
			continue;
		}

		seen.insert(key);
		dirs_ret.append(normalized);
	}

	return dirs_ret;
}

static QStringList SettingsStringList(const QVariant& value) {
	auto list = value.toStringList();
	if (!list.isEmpty()) {
		return list;
	}

	const auto text = value.toString();
	return text.isEmpty() ? QStringList() : QStringList({text});
}

static void AddSaveDataDir(QStringList* dirs, QSet<QString>* seen, const QString& root,
                           const QString& title_id) {
	const auto path = QDir(root).filePath(
	    QStringLiteral("%1/%2").arg(QString::fromLatin1(SAVE_DATA_DIR), title_id));
	QDir dir(path);
	if (!dir.exists()) {
		return;
	}

	const auto absolute  = dir.absolutePath();
	auto       canonical = QFileInfo(absolute).canonicalFilePath();
	if (canonical.isEmpty()) {
		canonical = absolute;
	}
	canonical = QDir::cleanPath(canonical);

	if (!seen->contains(canonical)) {
		seen->insert(canonical);
		dirs->append(absolute);
	}
}

static QStringList GetSaveDataDirs(const Configuration& info) {
	QStringList dirs;
	if (info.title_id.trimmed().isEmpty()) {
		return dirs;
	}

	QSet<QString> seen;
	QStringList   roots({QDir::currentPath(), QCoreApplication::applicationDirPath()});

	QDir current_parent(QDir::currentPath());
	if (current_parent.cdUp()) {
		roots.append(current_parent.absolutePath());
	}

	QDir app_parent(QCoreApplication::applicationDirPath());
	if (app_parent.cdUp()) {
		roots.append(app_parent.absolutePath());
	}

	for (const auto& root: roots) {
		AddSaveDataDir(&dirs, &seen, root, info.title_id.trimmed());
	}

	return dirs;
}

ConfigurationListWidget::ConfigurationListWidget(QWidget* parent)
    : QWidget(parent), m_ui(new Ui::ConfigurationListWidget) {
	m_compatibility = new CompatibilityDatabase(
	    QCoreApplication::arguments().contains(QStringLiteral("--local")), this);
	m_ui->setupUi(this);

	m_ui->refresh_action->setShortcuts(QKeySequence::Refresh);
	m_ui->refresh_button->setDefaultAction(m_ui->refresh_action);
	addAction(m_ui->refresh_action);

	UpdateToolbarIcons();
	m_ui->global_settings_button->setToolTip(tr("Edit global settings and game folders"));
	m_ui->input_mapping_button->setToolTip(tr("Edit global input mapping"));

	m_ui->delete_button->setEnabled(false);
	m_ui->edit_button->setEnabled(false);

	m_ui->cfgs_list->setContextMenuPolicy(Qt::CustomContextMenu);
	m_ui->cfgs_list->setAllColumnsShowFocus(true);
	m_ui->cfgs_list->setTextElideMode(Qt::ElideMiddle);
	m_ui->cfgs_list->setIconSize(QSize(48, 48));
	m_ui->cfgs_list->setItemDelegateForColumn(GAME_NAME_COLUMN,
	                                          new GameListNameDelegate(m_ui->cfgs_list));
	m_ui->cfgs_list->setUniformRowHeights(true);
	m_ui->cfgs_list->setSortingEnabled(true);
	ResetListColumnDefaults();
	m_ui->cfgs_list->sortItems(GAME_NAME_COLUMN, Qt::AscendingOrder);
	SetupGameGrid(m_ui->cfgs_list, m_ui->cfgs_grid);
	// Columns are reorderable and user-hideable from the header's context
	// menu; both the order and the visibility persist in the settings.
	auto* list_header = m_ui->cfgs_list->header();
	list_header->setSectionsMovable(true);
	list_header->setContextMenuPolicy(Qt::CustomContextMenu);
	connect(list_header, &QHeaderView::customContextMenuRequested, this,
	        [this](const QPoint& pos) { ShowListColumnMenu(pos); });
	connect(list_header, &QHeaderView::sectionResized, this,
	        [this](int, int, int) { PersistHeaderStateSoon(); });
	connect(list_header, &QHeaderView::sectionMoved, this,
	        [this](int, int, int) { PersistHeaderStateSoon(); });
	m_ui->cfgs_grid->setContextMenuPolicy(Qt::CustomContextMenu);

	connect(m_ui->refresh_action, &QAction::triggered, this,
	        &ConfigurationListWidget::ScanGameDirectory);
	connect(m_ui->global_settings_button, &QToolButton::clicked, this,
	        &ConfigurationListWidget::edit_global_settings);
	connect(m_ui->input_mapping_button, &QToolButton::clicked, this,
	        &ConfigurationListWidget::edit_input_mapping);
	connect(m_ui->edit_button, &QToolButton::clicked, this,
	        &ConfigurationListWidget::edit_configuration);
	connect(m_ui->delete_button, &QToolButton::clicked, this,
	        &ConfigurationListWidget::delete_configuartion);
	connect(m_ui->trophy_overview_button, &QToolButton::clicked, this,
	        &ConfigurationListWidget::ViewTrophyOverview);
	connect(m_ui->cfgs_list, &QTreeWidget::currentItemChanged, this,
	        &ConfigurationListWidget::SelectItem);
	connect(m_ui->cfgs_list, &QTreeWidget::itemDoubleClicked, this,
	        &ConfigurationListWidget::list_itemDoubleClicked);
	connect(m_ui->cfgs_list, &QTreeWidget::customContextMenuRequested, this,
	        [this](const QPoint& pos) { ShowContextMenu(m_ui->cfgs_list->itemAt(pos)); });
	connect(m_ui->cfgs_grid, &QListView::doubleClicked, this, [this](const QModelIndex& index) {
		list_itemDoubleClicked(m_ui->cfgs_list->itemFromIndex(index), GAME_NAME_COLUMN);
	});
	connect(m_ui->cfgs_grid, &QListView::customContextMenuRequested, this,
	        [this](const QPoint& pos) {
		        ShowContextMenu(m_ui->cfgs_list->itemFromIndex(m_ui->cfgs_grid->indexAt(pos)));
	        });
	connect(m_ui->grid_view_button, &QToolButton::toggled, this, [this](bool) { UpdateToolbarIcons(); });
	connect(m_ui->grid_view_button, &QToolButton::toggled, this, [this](bool grid) {
		m_ui->grid_size_slider->setVisible(grid);
		auto* view = grid ? static_cast<QAbstractItemView*>(m_ui->cfgs_grid) : m_ui->cfgs_list;
		m_ui->game_views->setCurrentWidget(view);
		view->setCurrentIndex(view->currentIndex().siblingAtColumn(GAME_NAME_COLUMN));
		view->scrollTo(view->currentIndex());
		view->setFocus();
	});
	connect(m_ui->grid_view_button, &QToolButton::clicked, this,
	        &ConfigurationListWidget::WriteSettings);

	// Grid sort control: mirrors the list's column sorting so the grid can be
	// reordered without leaving card view. The list and grid share a model, so
	// one sortItems call reorders both.
	{
		auto* sort_menu        = new QMenu(this);
		m_sort_column_group    = new QActionGroup(sort_menu);
		m_sort_direction_group = new QActionGroup(sort_menu);
		m_sort_column_group->setExclusive(true);
		m_sort_direction_group->setExclusive(true);
		const struct {
			int         column;
			const char* label;
		} column_actions[] = {
		    {GAME_NAME_COLUMN, "Game name"},
		    {GAME_SERIAL_COLUMN, "Serial"},
		    {GAME_VERSION_COLUMN, "Game version"},
		    {GAME_FIRMWARE_VERSION_COLUMN, "Firmware version"},
		    {GAME_SIZE_COLUMN, "Size"},
		    {GAME_STATUS_COLUMN, "Status"},
		    {GAME_COMMENT_COLUMN, "Comments"},
		};
		for (const auto& entry: column_actions) {
			auto* action = sort_menu->addAction(tr(entry.label));
			action->setCheckable(true);
			action->setData(entry.column);
			m_sort_column_group->addAction(action);
			connect(action, &QAction::triggered, this, [this, action]() {
				ApplySort(action->data().toInt(), m_sort_order);
			});
		}
		sort_menu->addSeparator();
		for (const auto [label, order]: {std::pair{"Ascending", Qt::AscendingOrder},
		                                 std::pair{"Descending", Qt::DescendingOrder}}) {
			auto* action = sort_menu->addAction(tr(label));
			action->setCheckable(true);
			action->setData(static_cast<int>(order));
			m_sort_direction_group->addAction(action);
			connect(action, &QAction::triggered, this, [this, order]() {
				ApplySort(m_sort_column, order);
			});
		}
		m_ui->sort_button->setMenu(sort_menu);
		m_ui->sort_button->setPopupMode(QToolButton::InstantPopup);
	}

	// Keyboard activation: the views are focusable, so Return launches the
	// current game just like double-click does. WidgetWithChildrenShortcut
	// keeps the binding off the search field and dialogs.
	const auto launch_current = [this](ConfigurationItem* item) {
		if (item != nullptr) {
			list_itemDoubleClicked(item, GAME_NAME_COLUMN);
		}
	};
	for (const auto key: {Qt::Key_Return, Qt::Key_Enter}) {
		connect(new QShortcut(QKeySequence(key), m_ui->cfgs_list, nullptr, nullptr,
		                      Qt::WidgetWithChildrenShortcut),
		        &QShortcut::activated, this, [this, launch_current]() {
			    launch_current(
			        static_cast<ConfigurationItem*>(m_ui->cfgs_list->currentItem()));
		        });
		connect(new QShortcut(QKeySequence(key), m_ui->cfgs_grid, nullptr, nullptr,
		                      Qt::WidgetWithChildrenShortcut),
		        &QShortcut::activated, this, [this, launch_current]() {
			    launch_current(static_cast<ConfigurationItem*>(
			        m_ui->cfgs_list->itemFromIndex(m_ui->cfgs_grid->currentIndex())));
		        });
	}

	// Explicit tab order: toolbar -> search -> views -> empty-state action.
	setTabOrder(m_ui->refresh_button, m_ui->global_settings_button);
	setTabOrder(m_ui->global_settings_button, m_ui->input_mapping_button);
	setTabOrder(m_ui->input_mapping_button, m_ui->edit_button);
	setTabOrder(m_ui->edit_button, m_ui->delete_button);
	setTabOrder(m_ui->delete_button, m_ui->trophy_overview_button);
	setTabOrder(m_ui->trophy_overview_button, m_ui->sort_button);
	setTabOrder(m_ui->sort_button, m_ui->grid_size_slider);
	setTabOrder(m_ui->grid_size_slider, m_ui->grid_view_button);
	setTabOrder(m_ui->grid_view_button, m_ui->search_line_edit);
	setTabOrder(m_ui->search_line_edit, m_ui->cfgs_list);
	setTabOrder(m_ui->cfgs_list, m_ui->cfgs_grid);

	connect(m_ui->cfgs_list->model(), &QAbstractItemModel::layoutChanged, this, [this]() {
		filter_configurations(m_ui->search_line_edit->text());
	});
	connect(m_ui->search_line_edit, &QLineEdit::textChanged, this,
	        &ConfigurationListWidget::filter_configurations);
	m_ui->search_line_edit->installEventFilter(this);
	// Empty-state overlay: shown when the views would otherwise be blank
	// (no games configured) or nothing matches the search query.
	m_empty_state = new QWidget(this);
	m_empty_state->setObjectName(QStringLiteral("empty_state_overlay"));
	auto* overlay_layout = new QVBoxLayout(m_empty_state);
	overlay_layout->setAlignment(Qt::AlignCenter);
	overlay_layout->setContentsMargins(24, 24, 24, 24);
	overlay_layout->setSpacing(10);
	m_empty_icon = new QLabel(m_empty_state);
	m_empty_icon->setObjectName(QStringLiteral("empty_state_icon"));
	m_empty_icon->setAlignment(Qt::AlignCenter);
	m_empty_title = new QLabel(m_empty_state);
	m_empty_title->setObjectName(QStringLiteral("empty_state_title"));
	m_empty_title->setAlignment(Qt::AlignCenter);
	m_empty_subtitle = new QLabel(m_empty_state);
	m_empty_subtitle->setObjectName(QStringLiteral("empty_state_subtitle"));
	m_empty_subtitle->setAlignment(Qt::AlignCenter);
	m_empty_subtitle->setWordWrap(true);
	m_empty_action = new QPushButton(m_empty_state);
	m_empty_action->setObjectName(QStringLiteral("empty_state_action"));
	m_empty_action->setCursor(Qt::PointingHandCursor);
	overlay_layout->addWidget(m_empty_icon);
	overlay_layout->addSpacing(4);
	overlay_layout->addWidget(m_empty_title);
	overlay_layout->addWidget(m_empty_subtitle);
	overlay_layout->addSpacing(4);
	overlay_layout->addWidget(m_empty_action, 0, Qt::AlignCenter);
	connect(m_empty_action, &QPushButton::clicked, this, [this]() {
		if (m_empty_is_search) {
			m_ui->search_line_edit->clear();
			m_ui->search_line_edit->setFocus();
		} else {
			edit_global_settings();
		}
	});
	m_empty_state->setAccessibleName(tr("Empty library overlay"));
	m_empty_state->hide();
	// Tabbing out of the views reaches the overlay's action button (skipped
	// automatically while the overlay is hidden).
	setTabOrder(m_ui->cfgs_grid, m_empty_action);

	connect(m_compatibility, &CompatibilityDatabase::Updated, this,
	        &ConfigurationListWidget::ApplyCompatibility);

	ReadSettings();
	m_ui->grid_size_slider->setVisible(m_ui->grid_view_button->isChecked());
	const int grid_size = m_ui->grid_size_slider->value();
	m_ui->cfgs_grid->setIconSize(QSize(grid_size, grid_size));
	connect(m_ui->grid_size_slider, &QSlider::valueChanged, this, [this](int width) {
		m_ui->cfgs_grid->setIconSize(QSize(width, width));
		if (!m_ui->grid_size_slider->isSliderDown()) {
			WriteSettings();
		}
	});
	connect(m_ui->grid_size_slider, &QSlider::sliderReleased, this,
	        &ConfigurationListWidget::WriteSettings);
	// Header-driven sorting is user input too: persist it and keep the grid
	// menu's checkmarks in sync. This connects only after ReadSettings so the
	// constructor's initial sortItems does not rewrite settings.
	connect(m_ui->cfgs_list->header(), &QHeaderView::sortIndicatorChanged, this,
	        [this](int column, Qt::SortOrder order) {
		    m_sort_column = column;
		    m_sort_order  = order;
		    UpdateSortMenu();
		    WriteSettings();
	        });
	if (m_compatibility->IsLocal()) {
		m_compatibility->Load();
	}
	m_scan_watcher = new QFutureWatcher<QList<ScanResult>>(this);
	connect(m_scan_watcher, &QFutureWatcher<QList<ScanResult>>::finished, this,
	        [this]() { ApplyScanResults(m_scan_watcher->result()); });
	ScanGameDirectory();
	if (!m_compatibility->IsLocal()) {
		m_compatibility->Load();
	}
	// Position the overlay only after layouts have been activated.
	QTimer::singleShot(0, this, [this]() { UpdateEmptyState(); });
}

ConfigurationListWidget::~ConfigurationListWidget() {
	qDeleteAll(m_custom_infos);
	delete m_ui;
}

void ConfigurationListWidget::changeEvent(QEvent* event) {
	QWidget::changeEvent(event);
	if (event->type() == QEvent::ApplicationPaletteChange || event->type() == QEvent::PaletteChange) {
		UpdateToolbarIcons();
		UpdateEmptyState();
	}
}

void ConfigurationListWidget::resizeEvent(QResizeEvent* event) {
	QWidget::resizeEvent(event);
	UpdateEmptyState();
}

void ConfigurationListWidget::UpdateEmptyState() {
	if (m_empty_state == nullptr) {
		return;
	}

	if (m_scan_in_progress) {
		// Rows are being rebuilt by an in-flight scan; don't flash the overlay.
		m_empty_state->hide();
		return;
	}

	const int  total   = m_ui->cfgs_list->topLevelItemCount();
	const auto query   = m_ui->search_line_edit->text().trimmed();
	int        visible = 0;
	for (int index = 0; index < total; ++index) {
		const auto* item = m_ui->cfgs_list->topLevelItem(index);
		if (item != nullptr && !item->isHidden()) {
			++visible;
		}
	}

	m_empty_is_search = total > 0 && !query.isEmpty() && visible == 0;
	const bool show = total == 0 || m_empty_is_search;

	// Keep the overlay glued to the game views so it never detaches on resize.
	const QRect rect = m_ui->game_views->geometry();
	if (rect.width() > 0 && rect.height() > 0) {
		m_empty_state->setGeometry(rect);
	}

	if (!show) {
		m_empty_state->hide();
		return;
	}

	m_empty_state->raise();
	const bool dark = palette().color(QPalette::Window).lightness() < 128;
	QColor muted = dark ? QColor(0x8a, 0x8a, 0x8a) : QColor(0xa8, 0xa8, 0xa8);

	if (m_empty_is_search) {
		// search.svg is a monochrome stroke icon — tint it to the muted color.
		auto pixmap = QIcon(QStringLiteral(":/icons/search.svg")).pixmap(72, 72);
		QPainter painter(&pixmap);
		painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
		painter.fillRect(pixmap.rect(), muted);
		painter.end();
		m_empty_icon->setPixmap(pixmap);
	} else {
		// emustation5.png is the full-color brand tile — keep it as-is.
		m_empty_icon->setPixmap(QIcon(QStringLiteral(":/icons/emustation5.png")).pixmap(96, 96));
	}

	if (m_empty_is_search) {
		m_empty_title->setText(tr("No matches"));
		m_empty_subtitle->setText(tr("No games match \"%1\". Try a different title or serial.").arg(query));
		m_empty_action->setText(tr("Clear Search"));
	} else {
		m_empty_title->setText(tr("No games yet"));
		m_empty_subtitle->setText(tr("Add a folder containing your games\nand EmuStation5 will list them here."));
		m_empty_action->setText(tr("Add Game Folder…"));
	}

	m_empty_state->show();
}

void ConfigurationListWidget::ApplySort(int column, Qt::SortOrder order) {
	m_sort_column = column;
	m_sort_order  = order;
	m_ui->cfgs_list->setSortingEnabled(true);
	m_ui->cfgs_list->sortItems(column, order);
	UpdateSortMenu();
}

void ConfigurationListWidget::UpdateSortMenu() {
	if (m_sort_column_group == nullptr || m_sort_direction_group == nullptr) {
		return;
	}
	for (QAction* action: m_sort_column_group->actions()) {
		action->setChecked(action->data().toInt() == m_sort_column);
	}
	for (QAction* action: m_sort_direction_group->actions()) {
		action->setChecked(action->data().toInt() == static_cast<int>(m_sort_order));
	}
}

void ConfigurationListWidget::ResetListColumnDefaults() {
	auto* header  = m_ui->cfgs_list->header();
	const int count = m_ui->cfgs_list->columnCount();
	for (int col = 0; col < count; ++col) {
		m_ui->cfgs_list->setColumnHidden(col, false);
	}
	for (int col = 0; col < count; ++col) {
		header->moveSection(header->visualIndex(col), col);
	}
	m_ui->cfgs_list->setColumnWidth(GAME_NAME_COLUMN, 320);
	m_ui->cfgs_list->setColumnWidth(GAME_SERIAL_COLUMN, 110);
	m_ui->cfgs_list->setColumnWidth(GAME_VERSION_COLUMN, 120);
	m_ui->cfgs_list->setColumnWidth(GAME_FIRMWARE_VERSION_COLUMN, 150);
	m_ui->cfgs_list->setColumnWidth(GAME_SIZE_COLUMN, 100);
	m_ui->cfgs_list->setColumnWidth(GAME_PATH_COLUMN, 320);
	m_ui->cfgs_list->setColumnWidth(GAME_STATUS_COLUMN, 150);
	m_ui->cfgs_list->setColumnWidth(GAME_COMMENT_COLUMN, 240);
}

void ConfigurationListWidget::ShowListColumnMenu(const QPoint& pos) {
	auto*       header = m_ui->cfgs_list->header();
	QMenu       menu(this);
	const auto* header_item = m_ui->cfgs_list->headerItem();
	// The Game column stays visible — it anchors the grid sort and the
	// double-click-to-launch behavior.
	for (int col = 1; col < m_ui->cfgs_list->columnCount(); ++col) {
		const QString title = header_item != nullptr ? header_item->text(col)
		                                             : QString::number(col);
		auto* action = menu.addAction(title);
		action->setCheckable(true);
		action->setChecked(!m_ui->cfgs_list->isColumnHidden(col));
		connect(action, &QAction::triggered, this, [this, col, action]() {
			m_ui->cfgs_list->setColumnHidden(col, !action->isChecked());
			WriteSettings();
		});
	}
	menu.addSeparator();
	connect(menu.addAction(tr("Reset Columns to Defaults")), &QAction::triggered, this, [this]() {
		ResetListColumnDefaults();
		WriteSettings();
	});
	menu.exec(header->mapToGlobal(pos));
}

void ConfigurationListWidget::PersistHeaderStateSoon() {
	if (m_header_persist_timer == nullptr) {
		m_header_persist_timer = new QTimer(this);
		m_header_persist_timer->setSingleShot(true);
		m_header_persist_timer->setInterval(400);
		connect(m_header_persist_timer, &QTimer::timeout, this,
		        &ConfigurationListWidget::WriteSettings);
	}
	m_header_persist_timer->start();
}

bool ConfigurationListWidget::eventFilter(QObject* watched, QEvent* event) {
	if (watched == m_ui->search_line_edit && event->type() == QEvent::FocusIn) {
		// The user typed a query — select it on focus so they can immediately replace it.
		if (!m_ui->search_line_edit->text().isEmpty()) {
			m_ui->search_line_edit->selectAll();
		}
		// ... or clear a stuck selection if they tabbed past.
	}
	return QWidget::eventFilter(watched, event);
}

void ConfigurationListWidget::UpdateToolbarIcons() {
	const auto color = palette().color(QPalette::Window).lightness() < 128 ? QColor(Qt::white)
	                                                                      : QColor(Qt::black);
	const auto set_icon = [&color](QToolButton* button, const QString& resource) {
		auto pixmap = QIcon(resource).pixmap(button->iconSize(), button->devicePixelRatioF());
		QPainter painter(&pixmap);
		painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
		painter.fillRect(pixmap.rect(), color);
		button->setIcon(QIcon(pixmap));
	};

	// The search field icon should stay a muted glyph, not the same brightness as the toolbar.
	auto muted = color;
	if (color == QColor(Qt::white)) {
		muted = QColor(0x8f, 0x8f, 0x8f);
	}
	auto pixmap = QIcon(QStringLiteral(":/icons/search.svg")).pixmap(QSize(20, 20));
	QPainter painter(&pixmap);
	painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
	painter.fillRect(pixmap.rect(), muted);
	painter.end();
	QIcon search_icon(pixmap);
	if (m_search_action == nullptr) {
		m_search_action = new QAction(search_icon, tr("Search"), this);
		m_ui->search_line_edit->addAction(m_search_action, QLineEdit::LeadingPosition);
	} else {
		m_search_action->setIcon(search_icon);
	}

	set_icon(m_ui->refresh_button, QStringLiteral(":/icons/refresh.svg"));
	m_ui->refresh_action->setIcon(m_ui->refresh_button->icon());
	set_icon(m_ui->global_settings_button, QStringLiteral(":/icons/global-settings.svg"));
	set_icon(m_ui->input_mapping_button, QStringLiteral(":/icons/input-mapping.svg"));
	set_icon(m_ui->edit_button, QStringLiteral(":/icons/edit-configuration.svg"));
	set_icon(m_ui->delete_button, QStringLiteral(":/icons/remove-configuration.svg"));
	set_icon(m_ui->trophy_overview_button, QStringLiteral(":/icons/trophy.svg"));
	set_icon(m_ui->grid_view_button, QStringLiteral(":/icons/grid-view.svg"));
	set_icon(m_ui->sort_button, QStringLiteral(":/icons/sort.svg"));
	if (m_ui->grid_view_button->isChecked()) {
		// The checked grid toggle is accent-filled in both themes; repaint the
		// glyph white so it stays legible on the accent background.
		auto pixmap = QIcon(QStringLiteral(":/icons/grid-view.svg"))
		                 .pixmap(m_ui->grid_view_button->iconSize(),
		                         m_ui->grid_view_button->devicePixelRatioF());
		QPainter painter(&pixmap);
		painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
		painter.fillRect(pixmap.rect(), Qt::white);
		painter.end();
		m_ui->grid_view_button->setIcon(QIcon(pixmap));
	}
}

void ConfigurationListWidget::WriteSettings() {
	QFile                      file = QFile(QDir(".").absoluteFilePath(CONF_FILE_NAME));
	std::unique_ptr<QSettings> s;
	if (file.exists()) {
		s = std::make_unique<QSettings>(CONF_FILE_NAME, QSettings::IniFormat);
	} else {
#ifdef __linux__
		s = std::make_unique<QSettings>(QSettings::IniFormat, QSettings::UserScope, CONF_ORG_NAME,
		                                CONF_APP_NAME);
#else
		s = std::make_unique<QSettings>(QSettings::IniFormat, QSettings::SystemScope, CONF_ORG_NAME,
		                                CONF_APP_NAME);
#endif
	}

	MainDialog::WriteSettings(*s);
	ConfigurationEditDialog::WriteSettings(*s);

	s->beginGroup(CONF_LAUNCHER);
	m_game_dirs = NormalizeGameDirectories(m_game_dirs);
	s->setValue(CONF_GAME_DIRS, m_game_dirs);
	s->setValue(CONF_ADDONS_DIR, m_addons_dir);
	s->setValue("grid_view", m_ui->grid_view_button->isChecked());
	s->setValue("grid_icon_width", m_ui->grid_size_slider->value());
	s->setValue("grid_sort_column", m_sort_column);
	s->setValue("grid_sort_order", static_cast<int>(m_sort_order));
	s->setValue("list_header_state", m_ui->cfgs_list->header()->saveState());
	s->remove(CONF_GAME_DIR);
	s->endGroup();

	s->remove(CONF_ADDCONT);
	s->beginGroup(CONF_ADDCONT);
	for (auto it = m_addcont.constBegin(); it != m_addcont.constEnd(); ++it) {
		s->setValue(it.key(), it.value());
	}
	s->endGroup();

	s->remove(CONF_GLOBAL);
	s->beginGroup(CONF_GLOBAL);
	m_global_info.WriteSettings(s.get());
	m_global_info.controller.WriteSettings(s.get());
	s->endGroup();

	s->remove(CONF_SECTION_NAME);
	s->beginWriteArray(CONF_SECTION_NAME);
	int i = 0;
	for (auto it = m_custom_infos.constBegin(); it != m_custom_infos.constEnd(); ++it) {
		s->setArrayIndex(i++);
		it.value()->WriteSettings(s.get());
	}
	s->endArray();
}

void ConfigurationListWidget::ReadSettings() {
	QFile                      file = QFile(QDir(".").absoluteFilePath(CONF_FILE_NAME));
	std::unique_ptr<QSettings> s;
	if (file.exists()) {
		s = std::make_unique<QSettings>(CONF_FILE_NAME, QSettings::IniFormat);
	} else {
#ifdef __linux__
		s = std::make_unique<QSettings>(QSettings::IniFormat, QSettings::UserScope, CONF_ORG_NAME,
		                                CONF_APP_NAME);
#else
		s = std::make_unique<QSettings>(QSettings::IniFormat, QSettings::SystemScope, CONF_ORG_NAME,
		                                CONF_APP_NAME);
#endif
	}

	m_settings_file = s->fileName();

	MainDialog::ReadSettings(*s);
	ConfigurationEditDialog::ReadSettings(*s);

	s->beginGroup(CONF_LAUNCHER);
	m_ui->grid_view_button->setChecked(s->value("grid_view", false).toBool());
	m_ui->grid_size_slider->setValue(
	    s->value("grid_icon_width", m_ui->grid_size_slider->value()).toInt());
	m_sort_column = s->value("grid_sort_column", GAME_NAME_COLUMN).toInt();
	m_sort_order  = s->value("grid_sort_order", static_cast<int>(Qt::AscendingOrder)).toInt() ==
	                    static_cast<int>(Qt::DescendingOrder)
	                 ? Qt::DescendingOrder
	                 : Qt::AscendingOrder;
	m_ui->cfgs_list->setSortingEnabled(true);
	m_ui->cfgs_list->sortItems(m_sort_column, m_sort_order);
	UpdateSortMenu();
	// Restored after the programmatic sort so the user's column layout
	// (widths, order, visibility) wins over the compiled-in defaults.
	const auto header_state = s->value("list_header_state").toByteArray();
	if (!header_state.isEmpty()) {
		m_ui->cfgs_list->header()->restoreState(header_state);
	}
	m_game_dirs = NormalizeGameDirectories(SettingsStringList(s->value(CONF_GAME_DIRS)));
	if (m_game_dirs.isEmpty()) {
		m_game_dirs = NormalizeGameDirectories(SettingsStringList(s->value(CONF_GAME_DIR)));
	}
	m_addons_dir = NormalizeGameDirectory(s->value(CONF_ADDONS_DIR).toString());
	s->endGroup();

	m_addcont.clear();
	s->beginGroup(CONF_ADDCONT);
	for (const auto& title_id: s->childKeys()) {
		m_addcont.insert(title_id, SettingsStringList(s->value(title_id)));
	}
	s->endGroup();

	s->beginGroup(CONF_GLOBAL);
	if (!s->childKeys().isEmpty()) {
		m_global_info.ReadSettings(s.get());
	}
	m_global_info.controller.ReadSettings(s.get());
	s->endGroup();

	qDeleteAll(m_custom_infos);
	m_custom_infos.clear();

	int size = s->beginReadArray(CONF_SECTION_NAME);

	for (int i = 0; i < size; i++) {
		s->setArrayIndex(i);
		auto* info = new Configuration;
		info->ReadSettings(s.get());
		info->custom_settings = true;
		if (!info->game_path.isEmpty()) {
			m_custom_infos.insert(info->game_path, info);
		} else {
			delete info;
		}
	}
	s->endArray();
}

void ConfigurationListWidget::ApplyCompatibility() {
	const bool sorting_enabled = m_ui->cfgs_list->isSortingEnabled();
	const int  sort_column     = m_ui->cfgs_list->sortColumn();
	const auto sort_order      = m_ui->cfgs_list->header()->sortIndicatorOrder();
	m_ui->cfgs_list->setSortingEnabled(false);

	for (int index = 0; index < m_ui->cfgs_list->topLevelItemCount(); index++) {
		auto*       item = static_cast<ConfigurationItem*>(m_ui->cfgs_list->topLevelItem(index));
		const auto& title_id = item->GetInfo().title_id;
		const auto* entry    = m_compatibility->Find(title_id);
		item->GetInfo().game_status =
		    entry != nullptr ? entry->status : Configuration::GameStatus::Unknown;
		item->GetInfo().game_comment = entry != nullptr ? entry->comment : QString();

		const QSignalBlocker blocker(item->GetStatusCombo());
		item->Update();
		item->SetCompatibilityEditable(m_compatibility->IsLocal() && !title_id.trimmed().isEmpty());
	}

	m_ui->cfgs_list->setSortingEnabled(sorting_enabled);
	if (sorting_enabled) {
		m_ui->cfgs_list->sortItems(sort_column, sort_order);
	}
}

std::unique_ptr<Configuration>
ConfigurationListWidget::CreateConfiguration(const ConfigurationItem& item) const {
	auto        info   = std::make_unique<Configuration>();
	const auto* custom = m_custom_infos.value(item.GetInfo().game_path);
	info->CopyGameInfoFrom(item.GetInfo());
	info->CopyEmulatorSettingsFrom(custom != nullptr ? *custom : m_global_info);
	info->controller = m_global_info.controller;
	if (custom != nullptr && !custom->elf.isEmpty()) {
		info->elf = custom->elf;
	}
	info->host_input_mapping = m_global_info.host_input_mapping;
	info->addons_dir         = m_addons_dir;
	info->addcont            = m_addcont.value(item.GetInfo().title_id.trimmed().toUpper());
	return info;
}

struct GameMetadata {
	QString title_name;
	QString title_id;
	QString gameVersion;
	QString firmwareVer;
	bool    pro_supported = false;
};

static QString GetJsonString(const QJsonObject& obj, const QString& key) {
	return obj.value(key).toString().trimmed();
}

static QString GetLocalizedTitleName(const QJsonObject& root) {
	const auto localized = root.value(QStringLiteral("localizedParameters")).toObject();
	if (localized.isEmpty()) {
		return {};
	}

	const auto default_language = GetJsonString(localized, QStringLiteral("defaultLanguage"));
	if (!default_language.isEmpty()) {
		const auto title = GetJsonString(localized.value(default_language).toObject(),
		                                 QStringLiteral("titleName"));
		if (!title.isEmpty()) {
			return title;
		}
	}

	const auto english_title = GetJsonString(localized.value(QStringLiteral("en-US")).toObject(),
	                                         QStringLiteral("titleName"));
	if (!english_title.isEmpty()) {
		return english_title;
	}

	for (auto it = localized.constBegin(); it != localized.constEnd(); ++it) {
		const auto title = GetJsonString(it.value().toObject(), QStringLiteral("titleName"));
		if (!title.isEmpty()) {
			return title;
		}
	}

	return {};
}

static QString GetFirmwareVersion(const QJsonObject& root) {
	const auto encoded = GetJsonString(root, QStringLiteral("requiredSystemSoftwareVersion"));
	static const QRegularExpression version_pattern(
	    QStringLiteral("^0[xX]([0-9]{6})[0-9A-Fa-f]{10}$"));
	const auto match = version_pattern.match(encoded);
	if (!match.hasMatch()) {
		return {};
	}

	const auto digits = match.captured(1);
	bool       valid  = false;
	const int  major  = digits.left(2).toInt(&valid, 10);
	if (!valid) {
		return {};
	}

	QString    version = QStringLiteral("%1.%2").arg(major).arg(digits.mid(2, 2));
	const auto patch   = digits.mid(4, 2);
	if (patch != QStringLiteral("00")) {
		version += QStringLiteral(".") + patch;
	}

	return version;
}

static GameMetadata GetGameMetadata(const QByteArray& param_data, const QString& fallback) {
	GameMetadata ret;
	ret.title_name = fallback;

	if (param_data.isEmpty()) {
		return ret;
	}

	QJsonParseError parse_error;
	const auto      doc = QJsonDocument::fromJson(param_data, &parse_error);
	if (parse_error.error != QJsonParseError::NoError || !doc.isObject()) {
		return ret;
	}

	const auto root  = doc.object();
	const auto title = GetLocalizedTitleName(root);
	if (!title.isEmpty()) {
		ret.title_name = title;
	}

	ret.title_id = GetJsonString(root, QStringLiteral("titleId"));
	if (ret.title_id.isEmpty()) {
		const auto content_id = GetJsonString(root, QStringLiteral("contentId"));
		if (content_id.size() == 36 && content_id[6] == '-' && content_id[16] == '_') {
			ret.title_id = content_id.mid(7, 9);
		}
	}
	ret.gameVersion = GetJsonString(root, QStringLiteral("appVersion"));
	if (ret.gameVersion.isEmpty()) {
		ret.gameVersion = GetJsonString(root, QStringLiteral("contentVersion"));
	}
	ret.firmwareVer   = GetFirmwareVersion(root);
	ret.pro_supported = (root.value(QStringLiteral("attribute3")).toInteger() & (1 << 22)) != 0;

	return ret;
}

static void SetGameFiles(Configuration& info, const QString& game_dir, const QString& game_path,
                         const GameMetadata& metadata, bool archive) {
	const QFileInfo game(game_dir);

	info.game_path     = game_path;
	info.basedir       = archive ? game.absoluteFilePath() : QDir(game_dir).absolutePath();
	info.name          = metadata.title_name;
	info.title_id      = metadata.title_id;
	info.gameVersion   = metadata.gameVersion;
	info.firmwareVer   = metadata.firmwareVer;
	info.pro_supported = metadata.pro_supported;

	if (info.name.isEmpty()) {
		info.name = archive ? game.completeBaseName() : QDir(game_dir).dirName();
	}
}

static Configuration* FindCustomInfo(QMap<QString, Configuration*>* custom_infos,
                                     const QString& game_path, const QString& legacy_game_path) {
	auto custom = custom_infos->find(game_path);
	if (custom != custom_infos->end()) {
		return custom.value();
	}

	custom = custom_infos->find(legacy_game_path);
	if (custom == custom_infos->end()) {
		return nullptr;
	}

	auto* info = custom.value();
	custom_infos->erase(custom);
	info->game_path = game_path;
	custom_infos->insert(game_path, info);

	return info;
}

bool ConfigurationListWidget::EnsureGameDirectory() {
	if (HasValidGameDirectory()) {
		return true;
	}

	QMessageBox::information(this, tr("Game folders"),
	                         tr("Add at least one game folder in global settings."));
	edit_global_settings();

	return HasValidGameDirectory();
}

bool ConfigurationListWidget::HasValidGameDirectory() const {
	for (const auto& dir: m_game_dirs) {
		if (!dir.isEmpty() && QDir(dir).exists()) {
			return true;
		}
	}

	return false;
}

void ConfigurationListWidget::ScanGameDirectory() {
	// Serialized rescan requests: if a scan is requested while one is in flight,
	// run exactly one follow-up pass instead of starting a concurrent one.
	if (m_scan_in_progress) {
		m_scan_dirty = true;
		return;
	}
	m_scan_in_progress = true;

	// Commit an active inline editor before destroying its row or replacing its data.
	if (auto* focused = QApplication::focusWidget();
	    focused != nullptr && m_ui->cfgs_list->isAncestorOf(focused)) {
		m_ui->cfgs_list->setFocus();
	}

	m_pending_selected_path   = m_selected_item != nullptr
	                                ? m_selected_item->GetInfo().game_path
	                                : QString();
	m_pending_sorting_enabled = m_ui->cfgs_list->isSortingEnabled();
	m_pending_sort_column     = m_ui->cfgs_list->sortColumn();
	m_pending_sort_order      = m_ui->cfgs_list->header()->sortIndicatorOrder();
	const QSignalBlocker blocker(m_ui->cfgs_list);
	m_ui->cfgs_list->setSortingEnabled(false);
	m_selected_item = nullptr;

	// MainDialog tracks the running item, so keep its identity even if its
	// directory is no longer present. Other rows can be rebuilt from disk.
	for (int index = m_ui->cfgs_list->topLevelItemCount() - 1; index >= 0; index--) {
		auto* item = static_cast<ConfigurationItem*>(m_ui->cfgs_list->topLevelItem(index));
		if (item->IsRunning()) {
			m_pending_running.insert(PathKey(item->GetInfo().game_path), item);
		} else {
			delete item;
		}
	}

	struct ScanCandidate {
		QString base;
		QString game_path;
		QString legacy_game_path;
		QString fallback;
		bool    archive = false;
	};

	const QString         eboot_name = QStringLiteral("eboot.bin");
	QList<ScanCandidate>  candidates;
	const auto            append_archives = [&candidates](const QDir& directory, const QDir& root) {
		const auto entries = directory.entryInfoList(QDir::Files | QDir::NoSymLinks, QDir::Name);
		for (const auto& archive: entries) {
			const auto base = archive.absoluteFilePath();
			if (!Common::IsSupportedArchive(GameContent::ToPath(base))) {
				continue;
			}
			candidates.append({base, QDir::cleanPath(base), root.relativeFilePath(base),
			                   archive.completeBaseName(), true});
		}
	};

	for (const auto& root_path: m_game_dirs) {
		QDir root(root_path);
		if (root_path.isEmpty() || !root.exists()) {
			continue;
		}

		append_archives(root, root);
		QList<QDir> pending_dirs;
		const auto  root_subdirs =
		    root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
		for (const auto& subdir: root_subdirs) {
			pending_dirs.append(QDir(subdir.absoluteFilePath()));
		}

		while (!pending_dirs.isEmpty()) {
			QDir game_dir = pending_dirs.takeFirst();
			append_archives(game_dir, root);

			if (game_dir.exists(eboot_name)) {
				const QString game_path        = NormalizeGameDirectory(game_dir.absolutePath());
				const QString legacy_game_path = root.relativeFilePath(game_dir.absolutePath());
				candidates.append({game_dir.absolutePath(), game_path, legacy_game_path,
				                   game_dir.dirName(), false});
				continue;
			}

			const auto subdirs =
			    game_dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
			for (const auto& subdir: subdirs) {
				pending_dirs.append(QDir(subdir.absoluteFilePath()));
			}
		}
	}

	// All I/O and archive work happens here, off the UI thread. Only plain data
	// crosses back into ApplyScanResults.
	m_scan_watcher->setFuture(QtConcurrent::run([candidates, eboot_name]() -> QList<ScanResult> {
		QList<ScanResult> results;
		QSet<QString>     found;
		results.reserve(candidates.size());
		for (const auto& candidate: candidates) {
			const QString key = PathKey(candidate.game_path);
			if (key.isEmpty() || found.contains(key)) {
				continue;
			}
			// Keep the index and decompression cache alive through validation and reads,
			// after rejecting duplicate candidates from overlapping game folders.
			if (candidate.archive) {
				const auto reader = Common::OpenArchive(GameContent::ToPath(candidate.base));
				if (reader == nullptr || !GameContent::FileExists(candidate.base, eboot_name)) {
					continue;
				}
			}
			found.insert(key);

			ScanResult result;
			result.base             = candidate.base;
			result.game_path        = candidate.game_path;
			result.legacy_game_path = candidate.legacy_game_path;
			result.fallback         = candidate.fallback;
			result.archive          = candidate.archive;
			result.param_data =
			    GameContent::ReadFile(candidate.base, QStringLiteral("sce_sys/param.json"),
			                          GameContent::MaxMetadataSize);
			result.icon_data = GameContent::ReadFile(candidate.base, QStringLiteral("sce_sys/icon0.png"),
			                                         GameContent::MaxImageSize);
			results.append(std::move(result));
		}
		return results;
	}));
}

void ConfigurationListWidget::ApplyScanResults(const QList<ScanResult>& results) {
	m_scan_in_progress = false;

	const auto add_game = [this](const ScanResult& result) {
		const auto metadata = GetGameMetadata(result.param_data, result.fallback);
		auto info = std::make_unique<Configuration>();
		info->custom_settings =
		    FindCustomInfo(&m_custom_infos, result.game_path, result.legacy_game_path) != nullptr;

		SetGameFiles(*info, result.base, result.game_path, metadata, result.archive);
		const auto* compatibility = m_compatibility->Find(info->title_id);
		if (compatibility != nullptr) {
			info->game_status  = compatibility->status;
			info->game_comment = compatibility->comment;
		}

		const auto game_key   = PathKey(result.game_path);
		const auto running    = m_pending_running.find(game_key);
		if (running != m_pending_running.end()) {
			// Refresh metadata without losing the running row's identity.
			auto* item = running.value();
			const QSignalBlocker status_blocker(item->GetStatusCombo());
			item->SetIconData(result.icon_data);
			item->GetInfo().CopyGameInfoFrom(*info);
			item->Update(true);
			item->SetCompatibilityEditable(m_compatibility->IsLocal() &&
			                               !item->GetInfo().title_id.trimmed().isEmpty());
			return;
		}

		auto* item = new ConfigurationItem(std::move(info), m_ui->cfgs_list, result.icon_data);
		item->SetCompatibilityEditable(m_compatibility->IsLocal() &&
		                               !item->GetInfo().title_id.trimmed().isEmpty());
		connect(item->GetStatusCombo(), &QComboBox::currentIndexChanged, item,
		        [this, item](int /*index*/) {
			        if (!m_compatibility->IsLocal()) {
				        return;
			        }

			        const auto& title_id = item->GetInfo().title_id;
			        if (title_id.trimmed().isEmpty()) {
				        return;
			        }
			        item->GetInfo().game_status = static_cast<Configuration::GameStatus>(
			            item->GetStatusCombo()->currentData().toInt());
			        item->Update();
			        m_compatibility->SetStatus(title_id, item->GetInfo().game_status);
			        m_ui->cfgs_list->setCurrentItem(item);
			        SelectItem(item);
		        });
		connect(item->GetCommentEdit(), &QLineEdit::editingFinished, item, [this, item]() {
			if (!m_compatibility->IsLocal()) {
				return;
			}

			const auto& title_id = item->GetInfo().title_id;
			if (title_id.trimmed().isEmpty()) {
				return;
			}
			item->GetInfo().game_comment = item->GetCommentEdit()->text();
			item->Update();
			m_compatibility->SetComment(title_id, item->GetInfo().game_comment);
			m_ui->cfgs_list->setCurrentItem(item);
			SelectItem(item);
		});
	};

	for (const auto& result: results) {
		add_game(result);
	}
	m_pending_running.clear();

	m_ui->cfgs_list->setSortingEnabled(m_pending_sorting_enabled);
	if (m_pending_sorting_enabled) {
		m_ui->cfgs_list->sortItems(m_pending_sort_column, m_pending_sort_order);
	}
	filter_configurations(m_ui->search_line_edit->text());

	ConfigurationItem* selected_item = nullptr;
	for (int index = 0; index < m_ui->cfgs_list->topLevelItemCount(); index++) {
		auto* item = static_cast<ConfigurationItem*>(m_ui->cfgs_list->topLevelItem(index));
		if (!item->isHidden() && item->GetInfo().game_path == m_pending_selected_path) {
			selected_item = item;
			break;
		}
	}
	m_ui->cfgs_list->setCurrentItem(selected_item);
	SelectItem(selected_item);

	if (m_scan_dirty) {
		m_scan_dirty = false;
		ScanGameDirectory();
	}
}

void ConfigurationListWidget::edit_configuration() {
	auto* item = static_cast<ConfigurationItem*>(m_ui->cfgs_list->currentItem());
	if (item == nullptr) {
		return;
	}

	auto                    info = CreateConfiguration(*item);
	ConfigurationEditDialog dlg(*info, this);
	dlg.setWindowTitle(tr("Edit game config"));
	connect(&dlg, &ConfigurationEditDialog::PreviewControllerColor, this,
	        &ConfigurationListWidget::PreviewControllerColor);

	if (dlg.exec() == QDialog::Accepted) {
		info->custom_settings           = true;
		item->GetInfo().custom_settings = true;
		auto game_path                  = info->game_path;
		delete m_custom_infos.take(game_path);
		m_custom_infos.insert(game_path, info.release());
		WriteSettings();
		item->Update();
		SelectItem(item);
	} else {
		emit Select();
	}
}

void ConfigurationListWidget::delete_configuartion() {
	auto* item = static_cast<ConfigurationItem*>(m_ui->cfgs_list->currentItem());
	if (item == nullptr || !item->GetInfo().custom_settings) {
		return;
	}

	if (QMessageBox::Yes == QMessageBox::question(this, tr("Clear game config"),
	                                              tr("Clear this game's config and use global settings?"))) {
		delete m_custom_infos.take(item->GetInfo().game_path);
		item->GetInfo().custom_settings = false;
		WriteSettings();
		item->Update();
		SelectItem(item);
	}
}

void ConfigurationListWidget::edit_global_settings() {
	Configuration info;
	info.CopyEmulatorSettingsFrom(m_global_info);
	info.controller = m_global_info.controller;
	info.name = tr("Global settings");

	ConfigurationEditDialog dlg(info, this);
	dlg.setWindowTitle(tr("Global settings"));
	dlg.SetGlobalSettings(m_game_dirs, m_addons_dir);
	connect(&dlg, &ConfigurationEditDialog::PreviewControllerColor, this,
	        &ConfigurationListWidget::PreviewControllerColor);
	connect(&dlg, &ConfigurationEditDialog::ImportGameSettings, this,
	        [this, &dlg]() { ImportGameSettings(&dlg); });
	connect(&dlg, &ConfigurationEditDialog::ExportGameSettings, this,
	        [this, &dlg]() { ExportGameSettings(&dlg); });

	if (dlg.exec() == QDialog::Accepted) {
		m_global_info.CopyEmulatorSettingsFrom(info);
		m_global_info.controller     = info.controller;
		const auto game_dirs         = NormalizeGameDirectories(dlg.GetGameDirectories());
		const bool game_dirs_changed = game_dirs != m_game_dirs;
		m_game_dirs                  = game_dirs;
		m_addons_dir                 = NormalizeGameDirectory(dlg.GetAddonsDirectory());
		WriteSettings();
		if (game_dirs_changed) {
			ScanGameDirectory();
		}
	}
	SelectItem(m_ui->cfgs_list->currentItem());
}

static bool IsGameSettingsTitleId(const QString& title_id) {
	static const QRegularExpression pattern(QStringLiteral("\\APPSA[0-9]{5}\\z"));
	return pattern.match(title_id).hasMatch();
}

void ConfigurationListWidget::ImportGameSettings(QWidget* parent) {
	const auto path = QFileDialog::getOpenFileName(parent, tr("Import game configs"), {},
	                                               tr("JSON files (*.json)"));
	if (path.isEmpty()) {
		return;
	}
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) {
		QMessageBox::warning(parent, tr("Import failed"), file.errorString());
		return;
	}
	const auto document = QJsonDocument::fromJson(file.readAll());
	if (!document.isObject() || document.object().isEmpty()) {
		QMessageBox::warning(
		    parent, tr("Import failed"),
		    tr("Expected a JSON object containing game configs keyed by PPSA code."));
		return;
	}
	const auto settings = document.object();
	for (auto it = settings.constBegin(); it != settings.constEnd(); ++it) {
		Configuration info;
		QString       error;
		if (!IsGameSettingsTitleId(it.key()) || !it.value().isObject() ||
		    it.value().toObject().isEmpty() ||
		    !info.SetGameSettings(it.value().toObject(), error)) {
			QMessageBox::warning(parent, tr("Import failed"),
			                     tr("Invalid game config for %1. %2").arg(it.key(), error));
			return;
		}
	}
	struct ImportedGame {
		ConfigurationItem*             item;
		std::unique_ptr<Configuration> info;
	};
	std::vector<ImportedGame> imported;
	auto                      unmatched = settings.keys();
	for (int index = 0; index < m_ui->cfgs_list->topLevelItemCount(); ++index) {
		auto*      item     = static_cast<ConfigurationItem*>(m_ui->cfgs_list->topLevelItem(index));
		const auto title_id = item->GetInfo().title_id.trimmed().toUpper();
		if (!settings.contains(title_id)) {
			continue;
		}
		auto    info = CreateConfiguration(*item);
		QString error;
		if (!info->SetGameSettings(settings.value(title_id).toObject(), error)) {
			QMessageBox::warning(parent, tr("Import failed"),
			                     tr("Invalid game config for %1. %2").arg(title_id, error));
			return;
		}
		info->custom_settings = true;
		imported.push_back({item, std::move(info)});
		unmatched.removeAll(title_id);
	}
	auto message = imported.empty() ? tr("No matching games found in the library.")
	                                : tr("Configs will be updated for the following games:\n");
	for (const auto& game: imported) {
		message += tr("\n%1 — %2").arg(game.info->title_id.trimmed().toUpper(), game.info->name);
	}
	if (!unmatched.isEmpty()) {
		message += tr("\nGames not found: %1").arg(unmatched.join(QStringLiteral(", ")));
	}
	if (imported.empty()) {
		QMessageBox::information(parent, tr("Import game configs"), message);
		return;
	}
	QMessageBox confirmation(QMessageBox::Question, tr("Import game configs"), message,
	                         QMessageBox::Apply | QMessageBox::Cancel, parent);
	confirmation.setTextFormat(Qt::PlainText);
	confirmation.setDefaultButton(QMessageBox::Cancel);
	if (confirmation.exec() != QMessageBox::Apply) {
		return;
	}
	for (auto& [item, info]: imported) {
		const auto game_path = info->game_path;
		delete m_custom_infos.take(game_path);
		m_custom_infos.insert(game_path, info.release());
		item->GetInfo().custom_settings = true;
		item->Update();
	}
	WriteSettings();
}

void ConfigurationListWidget::ExportGameSettings(QWidget* parent) const {
	QMap<QString, const ConfigurationItem*> games;
	for (int index = 0; index < m_ui->cfgs_list->topLevelItemCount(); ++index) {
		const auto* item = static_cast<ConfigurationItem*>(m_ui->cfgs_list->topLevelItem(index));
		const auto& game = item->GetInfo();
		const auto  title_id = game.title_id.trimmed().toUpper();
		if (m_custom_infos.contains(game.game_path) && IsGameSettingsTitleId(title_id)) {
			games.insert(tr("%1 — %2 (%3)").arg(title_id, game.name, game.game_path), item);
		}
	}
	if (games.isEmpty()) {
		QMessageBox::information(parent, tr("Export game config"),
		                         tr("No game configs with a PPSA code were found."));
		return;
	}
	bool       accepted = false;
	const auto selected = QInputDialog::getItem(parent, tr("Export game config"), tr("Game:"),
	                                            games.keys(), 0, false, &accepted);
	if (!accepted) {
		return;
	}
	const auto& game     = games.value(selected)->GetInfo();
	const auto  title_id = game.title_id.trimmed().toUpper();
	const auto  path =
	    QFileDialog::getSaveFileName(parent, tr("Export game config"),
	                                 title_id + QStringLiteral(".json"), tr("JSON files (*.json)"));
	if (path.isEmpty()) {
		return;
	}
	const auto settings =
	    QJsonObject::fromVariantMap(m_custom_infos.value(game.game_path)->GameSettings());
	const auto data =
	    QJsonDocument(QJsonObject {{title_id, settings}}).toJson(QJsonDocument::Indented);
	QSaveFile file(path);
	if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
		QMessageBox::warning(parent, tr("Export failed"), file.errorString());
	}
}

void ConfigurationListWidget::edit_input_mapping() {
	InputMappingDialog dialog(m_global_info.host_input_mapping, m_controller_preview, this);
	if (dialog.exec() == QDialog::Accepted) {
		m_global_info.host_input_mapping = dialog.Mapping();
		WriteSettings();
	}
}

void ConfigurationListWidget::ViewTrophies() {
	auto* current = static_cast<ConfigurationItem*>(m_ui->cfgs_list->currentItem());
	auto* item    = current != nullptr ? current : m_selected_item;
	if (item == nullptr) {
		return;
	}

	const auto config = CreateConfiguration(*item);
	TrophyViewerDialog::ShowForGame(config.get(), m_runtime_directory, this);
}

void ConfigurationListWidget::ViewTrophyOverview() {
	std::vector<std::unique_ptr<Configuration>> configurations;
	std::vector<const Configuration*>            games;
	configurations.reserve(static_cast<size_t>(m_ui->cfgs_list->topLevelItemCount()));
	games.reserve(static_cast<size_t>(m_ui->cfgs_list->topLevelItemCount()));
	for (int index = 0; index < m_ui->cfgs_list->topLevelItemCount(); ++index) {
		const auto* item =
		    static_cast<const ConfigurationItem*>(m_ui->cfgs_list->topLevelItem(index));
		configurations.push_back(CreateConfiguration(*item));
		games.push_back(configurations.back().get());
	}
	TrophyViewerDialog::ShowOverview(games, m_runtime_directory, this);
}

void ConfigurationListWidget::open_game_folder() {
	auto* item = static_cast<ConfigurationItem*>(m_ui->cfgs_list->currentItem());
	if (item == nullptr) {
		return;
	}

	const auto base       = item->GetInfo().basedir;
	const bool is_archive = GameContent::IsArchive(base);
	const QDir game_dir(is_archive ? QFileInfo(base).absolutePath() : base);
	if (!game_dir.exists()) {
		QMessageBox::warning(this, tr("Open game folder"), tr("Game folder does not exist."));
		return;
	}

	const auto open_directory = [this, game_dir] {
		if (!QDesktopServices::openUrl(QUrl::fromLocalFile(game_dir.absolutePath()))) {
			QMessageBox::warning(this, tr("Open game folder"), tr("Could not open game folder."));
		}
	};
	if (is_archive) {
		const auto path = QFileInfo(base).absoluteFilePath();
#if defined(_WIN32)
		QProcess explorer;
		explorer.setProgram("explorer.exe");
		explorer.setNativeArguments(
		    QStringLiteral("/select,\"%1\"").arg(QDir::toNativeSeparators(path)));
		if (explorer.startDetached()) {
			return;
		}
#elif defined(__APPLE__)
		if (QProcess::startDetached("/usr/bin/open", {"-R", path})) {
			return;
		}
#elif defined(__linux__)
		auto request = QDBusMessage::createMethodCall("org.freedesktop.FileManager1",
		                                              "/org/freedesktop/FileManager1",
		                                              "org.freedesktop.FileManager1", "ShowItems");
		request << QStringList {QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded)}
		        << QString();
		auto* watcher = new QDBusPendingCallWatcher(
		    QDBusConnection::sessionBus().asyncCall(request, 5000), this);
		connect(watcher, &QDBusPendingCallWatcher::finished, this,
		        [open_directory](QDBusPendingCallWatcher* call) {
			        call->deleteLater();
			        if (call->isError()) {
				        open_directory();
			        }
		        });
		return;
#endif
	}
	open_directory();
}

void ConfigurationListWidget::remove_save_data() {
	auto* item = static_cast<ConfigurationItem*>(m_ui->cfgs_list->currentItem());
	if (item == nullptr) {
		return;
	}

	const auto save_data_dirs = GetSaveDataDirs(item->GetInfo());
	if (save_data_dirs.isEmpty()) {
		QMessageBox::information(this, tr("Remove save data"),
		                         tr("No save data folder found for this game."));
		return;
	}

	const auto title =
	    !item->GetInfo().name.isEmpty() ? item->GetInfo().name : item->GetInfo().title_id;
	const auto text =
	    tr("Remove save data for \"%1\"?\n\nThis will delete:\n%2\n\nThis cannot be undone.")
	        .arg(title, save_data_dirs.join(QLatin1Char('\n')));

	if (QMessageBox::Yes != QMessageBox::question(this, tr("Remove save data"), text)) {
		return;
	}

	QStringList failed_dirs;
	for (const auto& path: save_data_dirs) {
		QDir dir(path);
		if (dir.exists() && !dir.removeRecursively()) {
			failed_dirs.append(path);
		}
	}

	if (!failed_dirs.isEmpty()) {
		QMessageBox::warning(this, tr("Remove save data"),
		                     tr("Could not remove:\n%1").arg(failed_dirs.join(QLatin1Char('\n'))));
	}
}

void ConfigurationListWidget::compress_game() {
	auto* item = static_cast<ConfigurationItem*>(m_ui->cfgs_list->currentItem());
	if (item == nullptr) {
		return;
	}

	const auto base    = item->GetInfo().basedir;
	const auto archive = base + QStringLiteral(".zar");
	const auto title   = tr("Compress to ZAR");
	if (QFileInfo::exists(archive)) {
		QMessageBox::warning(this, title,
		                     tr("%1 already exists.").arg(QDir::toNativeSeparators(archive)));
		return;
	}

	// Cancel through a flag, not the future, so a pack that already succeeded keeps its result.
	auto  canceled = std::make_shared<std::atomic_bool>(false);
	auto* progress = new QProgressDialog(
	    tr("Compressing %1...").arg(QDir::toNativeSeparators(base)), tr("Cancel"), 0, 1000, this);
	auto* watcher  = new QFutureWatcher<bool>(this);
	// Stay modal through finalization and fsync, which follow the last progress step.
	progress->setAutoReset(false);
	connect(progress, &QProgressDialog::canceled, this, [canceled] { *canceled = true; });
	connect(watcher, &QFutureWatcher<bool>::progressValueChanged, progress,
	        &QProgressDialog::setValue);
	connect(watcher, &QFutureWatcher<bool>::finished, this,
	        [this, progress, watcher, canceled, base, archive, title] {
		        // finished can arrive inside the modal dialog's setValue event processing.
		        progress->deleteLater();
		        watcher->deleteLater();
		        if (!watcher->result()) {
			        if (!*canceled) {
				        QMessageBox::warning(
				            this, title,
				            tr("Could not compress %1.").arg(QDir::toNativeSeparators(base)));
			        }
			        return;
		        }
		        if (QMessageBox::Yes ==
		                QMessageBox::question(this, title,
		                                      tr("Created %1.\n\nDelete the original folder %2?")
		                                          .arg(QDir::toNativeSeparators(archive),
		                                               QDir::toNativeSeparators(base)),
		                                      QMessageBox::Yes | QMessageBox::No, QMessageBox::No) &&
		            !QDir(base).removeRecursively()) {
			        QMessageBox::warning(
			            this, title, tr("Could not remove %1.").arg(QDir::toNativeSeparators(base)));
		        }
		        ScanGameDirectory();
	        });
	progress->open();
	watcher->setFuture(QtConcurrent::run(
	    [directory = GameContent::ToPath(base), path = GameContent::ToPath(archive),
	     canceled](QPromise<bool>& promise) {
		    promise.setProgressRange(0, 1000);
		    // A pack cancelled while queued must not touch a path a later pack may already own.
		    promise.addResult(
		        !*canceled &&
		        Common::PackArchive(directory, path, [&](uint64_t done, uint64_t total) {
			        promise.setProgressValue(static_cast<int>(done * 1000 / total));
			        return !*canceled;
		        }));
	    }));
}

void ConfigurationListWidget::filter_configurations(const QString& text) {
	const auto query              = text.trimmed();
	const bool has_query          = !query.isEmpty();
	bool       selection_is_shown = false;

	for (int item_index = 0; item_index < m_ui->cfgs_list->topLevelItemCount(); item_index++) {
		auto* item = m_ui->cfgs_list->topLevelItem(item_index);
		if (item == nullptr) {
			continue;
		}

		const bool match = !has_query ||
		                   item->text(GAME_NAME_COLUMN).contains(query, Qt::CaseInsensitive) ||
		                   item->text(GAME_SERIAL_COLUMN).contains(query, Qt::CaseInsensitive);
		item->setHidden(!match);
		m_ui->cfgs_grid->setRowHidden(item_index, !match);

		if (match && item == m_selected_item) {
			selection_is_shown = true;
		}
	}

	if (m_selected_item != nullptr && !selection_is_shown) {
		m_ui->cfgs_list->clearSelection();
		m_ui->cfgs_list->setCurrentItem(nullptr);
		SelectItem(nullptr);
	}

	UpdateEmptyState();
}

void ConfigurationListWidget::SelectItem(QTreeWidgetItem* witem) {
	auto* item = static_cast<ConfigurationItem*>(witem);
	if (item == nullptr) {
		m_selected_item = nullptr;
		m_ui->game_views->SetBackgroundImage({});
		m_ui->edit_button->setEnabled(false);
		m_ui->delete_button->setEnabled(false);
		emit Select();
		return;
	}

	m_ui->delete_button->setEnabled(!item->IsRunning() && item->GetInfo().custom_settings);
	m_ui->edit_button->setEnabled(!item->IsRunning());

	m_selected_item = item;
	m_ui->game_views->SetBackgroundImage(item->GetInfo().basedir);

	emit Select();
}

void ConfigurationListWidget::list_itemDoubleClicked(QTreeWidgetItem* witem, int /*column*/) {
	SelectItem(witem);
	if (m_run_enabled) {
		emit Run();
	}
}

void ConfigurationListWidget::ShowContextMenu(QTreeWidgetItem* witem) {
	auto* item = static_cast<ConfigurationItem*>(witem);

	if (item != nullptr) {
		m_ui->cfgs_list->setCurrentItem(item);
		SelectItem(item);
	}

	QMenu      menu;
	const auto save_data_dirs = item != nullptr ? GetSaveDataDirs(item->GetInfo()) : QStringList();
	const bool has_trophy_data =
	    item != nullptr && TrophyViewerDialog::HasTrophyData(&item->GetInfo());

	QAction* action_run = menu.addAction(tr("Run"), this, SIGNAL(Run()));
	QAction* action_open_folder =
	    menu.addAction(style()->standardIcon(QStyle::SP_DirOpenIcon), tr("Open game folder"), this,
	                   SLOT(open_game_folder()));
	QAction* action_view_trophies = menu.addAction(
	    style()->standardIcon(QStyle::SP_FileDialogContentsView), tr("View trophies..."));
	connect(action_view_trophies, &QAction::triggered, this,
	        &ConfigurationListWidget::ViewTrophies);
	QAction* action_patches = menu.addAction(tr("Cheats (experimental)..."));
	connect(action_patches, &QAction::triggered, this,
	        [this, item = QPointer<ConfigurationItem>(item)]() {
		        if (item != nullptr) {
			        auto* dialog = new PatchesDialog(item->GetInfo(), this);
			        dialog->show();
		        }
	        });
	action_patches->setVisible(item != nullptr &&
	                           Cheats::IsSupportedTitleId(item->GetInfo().title_id));
	QAction* action_addcont = menu.addAction(
	    style()->standardIcon(QStyle::SP_DirIcon), tr("Additional Content..."));
	connect(action_addcont, &QAction::triggered, this,
	        [this, item = QPointer<ConfigurationItem>(item)]() {
		        if (item != nullptr) {
			        const auto title_id = item->GetInfo().title_id.trimmed().toUpper();
			        auto*      dialog   = new AdditionalContentDialog(
			            item->GetInfo(), m_addons_dir, m_addcont.value(title_id),
			            [this, title_id](const QStringList& entries) {
				            if (entries.isEmpty()) {
					            m_addcont.remove(title_id);
				            } else {
					            m_addcont.insert(title_id, entries);
				            }
				            WriteSettings();
			            },
			            this);
			        dialog->show();
		        }
	        });
	action_addcont->setEnabled(item != nullptr && !item->GetInfo().title_id.trimmed().isEmpty());
	QAction* action_compress =
	    menu.addAction(tr("Compress to ZAR..."), this, SLOT(compress_game()));
	QAction* action_remove_save_data =
	    menu.addAction(style()->standardIcon(QStyle::SP_DialogDiscardButton),
	                   tr("Remove save data..."), this, SLOT(remove_save_data()));
	menu.addSeparator();
	QAction* action_edit =
	    menu.addAction(style()->standardIcon(QStyle::SP_FileIcon), tr("Edit game config..."),
	                   this, SLOT(edit_configuration()));
	QAction* action_delete =
	    menu.addAction(style()->standardIcon(QStyle::SP_DialogDiscardButton),
	                   tr("Clear game config"), this, SLOT(delete_configuartion()));

	if (item == nullptr) {
		menu.addSeparator();
		/*QAction *action_global = */ menu.addAction(
		    style()->standardIcon(QStyle::SP_FileDialogDetailedView), tr("Global settings..."),
		    this, SLOT(edit_global_settings()));
	}

	if (item != nullptr) {
		action_run->setDisabled(item->IsRunning());
		const auto& base = item->GetInfo().basedir;
		action_open_folder->setDisabled(!QDir(base).exists() && !GameContent::IsArchive(base));
		action_view_trophies->setDisabled(!has_trophy_data);
		action_compress->setDisabled(item->IsRunning() || !QDir(base).exists());
		action_remove_save_data->setDisabled(item->IsRunning() || save_data_dirs.isEmpty());
		action_edit->setDisabled(item->IsRunning());
		action_delete->setDisabled(item->IsRunning() || !item->GetInfo().custom_settings);
	} else {
		action_run->setDisabled(true);
		action_open_folder->setDisabled(true);
		action_view_trophies->setDisabled(true);
		action_compress->setDisabled(true);
		action_remove_save_data->setDisabled(true);
		action_edit->setDisabled(true);
		action_delete->setDisabled(true);
	}

	if (!m_run_enabled) {
		action_run->setDisabled(true);
	}

	menu.exec(QCursor::pos());
}
