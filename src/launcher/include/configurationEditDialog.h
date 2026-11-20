#ifndef CONFIGURATION_EDIT_DIALOG_H
#define CONFIGURATION_EDIT_DIALOG_H

#include <QDialog>
#include <QString>
#include <QStringList>
class QByteArray;
class QCheckBox;
class QComboBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QMoveEvent;
class QResizeEvent;
class QSlider;
class QToolButton;
class QSettings;
class QWidget;

class Configuration;
namespace Ui {
class ConfigurationEditDialog;
} // namespace Ui

class ConfigurationEditDialog: public QDialog {
	Q_OBJECT

public:
	explicit ConfigurationEditDialog(Configuration& info, QWidget* parent = nullptr);
	~ConfigurationEditDialog() override;

	static void WriteSettings(QSettings& s);
	static void ReadSettings(QSettings& s);

	void                      SetGlobalSettings(const QStringList& dirs, const QString& addons_dir);
	void                      SetLauncherPreferences(bool grid_view, int cover_size,
	                                                 const QString& settings_file);
	[[nodiscard]] QString     GetAddonsDirectory() const;
	[[nodiscard]] QStringList GetGameDirectories() const;
	[[nodiscard]] bool        GetGridView() const;
	[[nodiscard]] int         GetCoverSize() const;

signals:
	void PreviewControllerColor(const QString& color);
	void ImportGameSettings();
	void ExportGameSettings();

private:
	Ui::ConfigurationEditDialog* m_ui = nullptr;
	Configuration&               m_info;
	QGroupBox*                   m_game_dirs_group        = nullptr;
	QListWidget*                 m_game_dirs_list         = nullptr;
	QLineEdit*                   m_addons_dir_edit        = nullptr;
	QToolButton*                 m_remove_game_dir_button = nullptr;
	QComboBox*                   m_launcher_theme         = nullptr;
	QCheckBox*                   m_launcher_updates       = nullptr;
	QCheckBox*                   m_launcher_close_terminal = nullptr;
	QCheckBox*                   m_launcher_detached      = nullptr;
	QCheckBox*                   m_launcher_grid_view     = nullptr;
	QSlider*                     m_launcher_cover_size    = nullptr;
	QLabel*                      m_launcher_settings_file = nullptr;
	int                          m_initial_theme          = 1;
	bool                         m_global_settings        = false;
	bool                         m_geometry_restored      = false;

protected:
	void Init(const Configuration& info);
	void InitGameDirectories();
	void InitLauncherSettings();
	void CommitLauncherSettings();
	void RevertLauncherTheme();
	void AddGameDirectoryItem(const QString& dir);
	void MakeTabsScrollable();
	void GrowToFitContents();
	void ShowTab(QWidget* page);

	void done(int r) override;
	void moveEvent(QMoveEvent* event) override;
	void resizeEvent(QResizeEvent* event) override;

	static QByteArray g_last_geometry;

	/*slots:*/

	void save();
	void clear();
	void add_game_directory();
	void remove_selected_game_directories();
	void update_game_directory_buttons();
};

#endif // CONFIGURATION_EDIT_DIALOG_H
