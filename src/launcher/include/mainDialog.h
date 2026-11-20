#ifndef MAIN_DIALOG_H
#define MAIN_DIALOG_H

#include <QMainWindow>
#include <QString>

class QWidget;
class QProcess;
class MainDialogPrivate;
class QSettings;
class QResizeEvent;
class QShowEvent;

class Configuration;

struct LauncherSettings {
	int  theme                    = 1;
	bool check_updates_on_startup = true;
	bool close_terminal           = false;
	bool detached_console         = true;
};

class MainDialog: public QMainWindow {
	Q_OBJECT

signals:
	void Start();
	void Resize();

public:
	explicit MainDialog(QWidget* parent = nullptr);
	~MainDialog() override = default;

	void RunInterpreter(QProcess* process, const Configuration& info, bool close_terminal,
	                    bool detached);

	static void            WriteSettings(QSettings& s);
	static void            ReadSettings(QSettings& s);
	static LauncherSettings GetLauncherSettings();
	static void            SetLauncherSettings(const LauncherSettings& settings);

	void resizeEvent(QResizeEvent* event) override;
	void showEvent(QShowEvent* event) override;

private:
	MainDialogPrivate* m_p = nullptr;
};

#endif // MAIN_DIALOG_H
