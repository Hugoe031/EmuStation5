#ifndef CONFIGURATION_LIST_WIDGET_H
#define CONFIGURATION_LIST_WIDGET_H

#include "configuration.h"

#include <QByteArray>
#include <QFutureWatcher>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <memory>

class ConfigurationItem;
class CompatibilityDatabase;
class ControllerPreview;
class QAction;
class QActionGroup;
class QEvent;
class QLabel;
class QMenu;
class QPushButton;
class QResizeEvent;
class QTimer;
class QTreeWidgetItem;

namespace Ui {
class ConfigurationListWidget;
} // namespace Ui

class ConfigurationListWidget: public QWidget {
	Q_OBJECT

public:
	explicit ConfigurationListWidget(QWidget* parent = nullptr);
	~ConfigurationListWidget() override;

	void SetRunEnabled(bool flag) { m_run_enabled = flag; }
	[[nodiscard]] bool IsRunEnabled() const { return m_run_enabled; }
	void SetRuntimeDirectory(const QString& directory) { m_runtime_directory = directory; }
	void SetControllerPreview(ControllerPreview* preview) { m_controller_preview = preview; }

	[[nodiscard]] const ConfigurationItem* GetSelectedItem() const { return m_selected_item; }
	ConfigurationItem*                     GetSelectedItem() { return m_selected_item; }

	[[nodiscard]] const QString& GetSettingsFile() const { return m_settings_file; }
	[[nodiscard]] const QString& GetGlobalControllerColor() const {
		return m_global_info.controller.color;
	}
	[[nodiscard]] std::unique_ptr<Configuration>
	CreateConfiguration(const ConfigurationItem& item) const;

	bool EnsureGameDirectory();
	void ScanGameDirectory();
	void ViewTrophies();
	void ViewTrophyOverview();

	// Launcher-view preferences surfaced in the main settings dialog.
	void                      SetLauncherPreferences(bool grid_view, int cover_size,
	                                                 const QString& settings_file);
	[[nodiscard]] bool        GetGridView() const;
	[[nodiscard]] int         GetCoverSize() const;
	[[nodiscard]] const QString& GetLauncherSettingsFile() const;

signals:

	void Run();
	void Select();
	void PreviewControllerColor(const QString& color);
	void SearchQueryChanged(const QString& text);

protected:
	void changeEvent(QEvent* event) override;
	void resizeEvent(QResizeEvent* event) override;
	bool eventFilter(QObject* watched, QEvent* event) override;

public slots:
	void WriteSettings();
	void ReadSettings();
	void SetSearchQuery(const QString& text);
	void edit_global_settings();

protected slots:

	void edit_configuration();
	void delete_configuartion();
	void edit_input_mapping();
	void list_itemDoubleClicked(QTreeWidgetItem* witem, int column);
	void open_game_folder();
	void remove_save_data();
	void compress_game();
	void filter_configurations(const QString& text);

private:
	// A single scan candidate resolved by the background worker. Only plain data
	// crosses the thread boundary — no GUI objects.
	struct ScanResult {
		QString   base;
		QString   game_path;
		QString   legacy_game_path;
		QString   fallback;
		bool      archive = false;
		QByteArray param_data;
		QByteArray icon_data;
	};

	void               SelectItem(QTreeWidgetItem* witem);
	void               ShowContextMenu(QTreeWidgetItem* witem);
	void               ShowListColumnMenu(const QPoint& pos);
	void               ResetListColumnDefaults();
	void               PersistHeaderStateSoon();
	void               ApplyCompatibility();
	void               UpdateToolbarIcons();
	void               UpdateEmptyState();
	void               ApplyScanResults(const QList<ScanResult>& results);
	void               ApplySort(int column, Qt::SortOrder order);
	void               UpdateSortMenu();
	void               ImportGameSettings(QWidget* parent);
	void               ExportGameSettings(QWidget* parent) const;
	[[nodiscard]] bool HasValidGameDirectory() const;

	ConfigurationItem*            m_selected_item = nullptr;
	bool                          m_run_enabled   = true;
	Ui::ConfigurationListWidget*  m_ui            = nullptr;
	QAction*                      m_search_action = nullptr;
	QString                       m_settings_file;
	QString                       m_launcher_settings_file;
	QString                       m_runtime_directory;
	QStringList                   m_game_dirs;
	QString                       m_addons_dir;
	QMap<QString, QStringList>    m_addcont; // User add-ons by title ID.
	Configuration                 m_global_info;
	QMap<QString, Configuration*> m_custom_infos;
	CompatibilityDatabase*        m_compatibility = nullptr;
	ControllerPreview*            m_controller_preview = nullptr;
	// Absolutely-positioned overlay shown when the views would otherwise be blank
	// (no games installed, or no matches for the current search query).
	QWidget*                      m_empty_state     = nullptr;
	QLabel*                       m_empty_icon      = nullptr;
	QLabel*                       m_empty_title     = nullptr;
	QLabel*                       m_empty_subtitle  = nullptr;
	QPushButton*                  m_empty_action    = nullptr;
	bool                          m_empty_is_search = false;
	// Async scan state: the directory walk runs on a worker thread; the GUI thread
	// only applies finished results. m_scan_dirty coalesces rescan requests that
	// arrive while a scan is in flight.
	QFutureWatcher<QList<ScanResult>>* m_scan_watcher       = nullptr;
	QMap<QString, ConfigurationItem*>  m_pending_running;
	QString                            m_pending_selected_path;
	bool                               m_pending_sorting_enabled = false;
	int                                m_pending_sort_column     = 0;
	Qt::SortOrder                      m_pending_sort_order      = Qt::AscendingOrder;
	bool                               m_scan_in_progress        = false;
	bool                               m_scan_dirty              = false;
	// Grid sort control state, persisted in the launcher settings group.
	// m_sort_column uses the GAME_*_COLUMN ids from gameViews.h.
	int                                m_sort_column             = 0;
	Qt::SortOrder                      m_sort_order              = Qt::AscendingOrder;
	QActionGroup*                      m_sort_column_group       = nullptr;
	QActionGroup*                      m_sort_direction_group    = nullptr;
	// Debounce timer for header writes so column drags don't rewrite the ini
	// on every sectionResized signal.
	QTimer*                            m_header_persist_timer    = nullptr;
};

#endif // CONFIGURATION_LIST_WIDGET_H
